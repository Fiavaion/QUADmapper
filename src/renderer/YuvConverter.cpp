#include "YuvConverter.hpp"
#include "Texture.hpp"
#include "core/GlObjectStats.hpp"
#include "core/Logger.hpp"
#include "core/MainThreadTimings.hpp"

#include <glad/glad.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace quadmapper {

namespace {

// One triangle covering the target; vUv runs 0-1 across it, so output row r samples
// picture row r (row 0 first), the orientation every other upload path uses.
constexpr const char* VERTEX_SOURCE = R"GLSL(
#version 330 core
out vec2 vUv;
void main() {
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr const char* FRAGMENT_SOURCE = R"GLSL(
#version 330 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uY;
uniform sampler2D uU;   // interleaved UV when uSemiPlanar, else U
uniform sampler2D uV;
uniform bool uSemiPlanar;
uniform mat3 uMatrix;
uniform vec3 uOffset;
uniform float uScale;
void main() {
    float y = texture(uY, vUv).r;
    vec2 c = uSemiPlanar ? texture(uU, vUv).rg : vec2(texture(uU, vUv).r, texture(uV, vUv).r);
    vec3 rgb = uMatrix * (vec3(y, c) * uScale - uOffset);
    fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)GLSL";

struct PlaneFormat {
    GLint internalFormat;
    GLenum format;
    GLenum type;
    int texelBytes;
};

PlaneFormat planeFormat(const YuvPicture& picture, int plane) {
    const bool wide = picture.bytesPerSample() == 2;
    const bool interleaved = picture.semiPlanar() && plane == 1;
    if (interleaved) {
        return wide ? PlaneFormat{GL_RG16, GL_RG, GL_UNSIGNED_SHORT, 4}
                    : PlaneFormat{GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2};
    }
    return wide ? PlaneFormat{GL_R16, GL_RED, GL_UNSIGNED_SHORT, 2}
                : PlaneFormat{GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1};
}

int planeWidth(const YuvPicture& picture, int plane) {
    return plane == 0 ? picture.width : picture.chromaWidth();
}

int planeHeight(const YuvPicture& picture, int plane) {
    return plane == 0 ? picture.height : picture.chromaHeight();
}

size_t planeRowBytes(const YuvPicture& picture, int plane) {
    return static_cast<size_t>(planeWidth(picture, plane)) * planeFormat(picture, plane).texelBytes;
}

size_t planeBytes(const YuvPicture& picture, int plane) {
    return planeRowBytes(picture, plane) * static_cast<size_t>(planeHeight(picture, plane));
}

// The planes, packed tight row by row, into the mapped buffer at the staged offsets.
void copyPlanes(const YuvUploadState::Staging& job) {
    const YuvPicture& picture = job.picture;
    for (int p = 0; p < picture.planeCount(); ++p) {
        const size_t rowBytes = planeRowBytes(picture, p);
        const int rows = planeHeight(picture, p);
        uint8_t* dst = job.mapped + job.offsets[p];
        const uint8_t* src = picture.planes[p];
        if (static_cast<size_t>(picture.strides[p]) == rowBytes) {
            std::memcpy(dst, src, rowBytes * rows);
        } else {
            for (int y = 0; y < rows; ++y) {
                std::memcpy(dst + rowBytes * y, src + static_cast<size_t>(picture.strides[p]) * y, rowBytes);
            }
        }
    }
}

}  // namespace

// Copies frames into mapped pixel buffers off the main thread (ADR-055 section 3). One
// thread is plenty: 16 x 30 fps of 1080p NV12 is ~22% of a core. Jobs are the sources'
// Staging slots; each stays alive until its copy is done (YuvUploadState waits first).
class PlaneCopier {
public:
    PlaneCopier() : thread_([this] { run(); }) {}
    ~PlaneCopier() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();  // after the queue drains: no job is left undone
    }
    PlaneCopier(const PlaneCopier&) = delete;
    PlaneCopier& operator=(const PlaneCopier&) = delete;

    void submit(YuvUploadState::Staging& job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job.done = false;
            queue_.push_back(&job);
        }
        wake_.notify_all();
    }

    void wait(const YuvUploadState::Staging& job) {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [&job] { return job.done; });
    }

    // Never waits: true once no copy writes into `job`'s mapping (a queued copy is
    // dropped, it never starts), false while its copy runs.
    bool drop(YuvUploadState::Staging& job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job.done) return true;
        for (auto it = queue_.begin(); it != queue_.end(); ++it) {
            if (*it == &job) {
                queue_.erase(it);
                job.done = true;
                return true;
            }
        }
        return false;
    }

    static std::function<void()>& hook() {
        static std::function<void()> h;
        return h;
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopping, and nothing left to copy
            YuvUploadState::Staging* job = queue_.front();
            queue_.pop_front();
            lock.unlock();
            if (hook()) hook()();
            copyPlanes(*job);
            lock.lock();
            job->done = true;
            finished_.notify_all();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable finished_;
    std::deque<YuvUploadState::Staging*> queue_;
    bool stop_ = false;
    std::thread thread_;  // last: starts once the members above exist
};

// --- YuvUploadState ---

YuvUploadState::YuvUploadState() {
    glGenTextures(3, planes_);
    for (int p = 0; p < 3; ++p) GlObjectStats::onTextureCreated();
    glGenBuffers(PBO_COUNT, pbos_);
    for (int b = 0; b < PBO_COUNT; ++b) GlObjectStats::onVertexBufferCreated();  // counted with the other GL buffers
}

YuvUploadState::~YuvUploadState() {
    settle();
    for (uint32_t& texture : planes_) {
        if (texture != 0) {
            glDeleteTextures(1, &texture);
            GlObjectStats::onTextureDestroyed();
        }
    }
    for (uint32_t& buffer : pbos_) {
        if (buffer != 0) {
            glDeleteBuffers(1, &buffer);
            GlObjectStats::onVertexBufferDestroyed();
        }
    }
}

bool YuvUploadState::idle() {
    return !staging_.mapped || !copier_ || copier_->drop(staging_);
}

void YuvUploadState::settle() {
    if (staging_.mapped) {
        if (copier_) copier_->wait(staging_);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[staging_.pbo]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        staging_.mapped = nullptr;
    }
    staging_.picture = YuvPicture{};
}

// --- YuvConverter ---

void YuvConverter::setCopyHookForTesting(std::function<void()> hook) {
    PlaneCopier::hook() = std::move(hook);
}

YuvConverter::~YuvConverter() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        GlObjectStats::onVertexArrayDestroyed();
    }
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        GlObjectStats::onFramebufferDestroyed();
    }
}

YuvConverter::Coefficients YuvConverter::coefficients(YuvMatrix matrix, bool fullRange,
                                                      YuvLayout layout) {
    double kr = 0.2126, kb = 0.0722;  // BT.709
    if (matrix == YuvMatrix::BT601) {
        kr = 0.299;
        kb = 0.114;
    } else if (matrix == YuvMatrix::BT2020) {
        kr = 0.2627;
        kb = 0.0593;
    }
    const double kg = 1.0 - kr - kb;

    const bool tenBit = layout == YuvLayout::P010 || layout == YuvLayout::YUV420P10;
    const double maxValue = tenBit ? 1023.0 : 255.0;
    const double step = tenBit ? 4.0 : 1.0;  // one 8-bit code value in this bit depth
    Coefficients c;
    // Samples arrive as 0-1 of their container; turn them into 0-1 of the bit depth.
    if (layout == YuvLayout::P010) c.scale = static_cast<float>(65535.0 / (64.0 * maxValue));
    if (layout == YuvLayout::YUV420P10) c.scale = static_cast<float>(65535.0 / maxValue);

    double yOffset = 0.0, yRange = 1.0, cRange = 1.0;
    const double cOffset = 128.0 * step / maxValue;
    if (!fullRange) {
        yOffset = 16.0 * step / maxValue;
        yRange = 219.0 * step / maxValue;
        cRange = 224.0 * step / maxValue;
    }
    c.offset = glm::vec3(static_cast<float>(yOffset), static_cast<float>(cOffset),
                         static_cast<float>(cOffset));

    // Rows of the R'G'B' <- Y'CbCr matrix, scaled by 1/range per input channel.
    const double rows[3][3] = {
        {1.0, 0.0, 2.0 * (1.0 - kr)},
        {1.0, -2.0 * kb * (1.0 - kb) / kg, -2.0 * kr * (1.0 - kr) / kg},
        {1.0, 2.0 * (1.0 - kb), 0.0},
    };
    const double inverseRange[3] = {1.0 / yRange, 1.0 / cRange, 1.0 / cRange};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            c.matrix[col][row] = static_cast<float>(rows[row][col] * inverseRange[col]);  // column-major
        }
    }
    return c;
}

bool YuvConverter::prepare() {
    const bool ok = ensureObjects();
    glUseProgram(0);  // ensureObjects() bound the program; outside begin()/end() nothing restores it
    return ok;
}

bool YuvConverter::ensureObjects() {
    if (failed_) return false;
    if (program_.isValid()) return true;
    ScopedMainThreadTiming timing("yuvShaderBuild");

    if (!program_.create(VERTEX_SOURCE, FRAGMENT_SOURCE)) {
        Logger::error("YuvConverter: the YUV -> RGBA shader failed to build; video stays black");
        failed_ = true;
        return false;
    }
    program_.bind();
    program_.setInt("uY", 0);
    program_.setInt("uU", 1);
    program_.setInt("uV", 2);

    glGenVertexArrays(1, &vao_);  // core profile: a draw needs a bound VAO, even with no attributes
    GlObjectStats::onVertexArrayCreated();
    glGenFramebuffers(1, &fbo_);
    GlObjectStats::onFramebufferCreated();
    copier_ = std::make_shared<PlaneCopier>();
    return true;
}

bool YuvConverter::ensurePlanes(const YuvPicture& picture, YuvUploadState& state) {
    if (state.layout_ == picture.layout && state.width_ == picture.width &&
        state.height_ == picture.height) {
        return true;
    }
    if (state.planes_[0] == 0 || state.pbos_[0] == 0) return false;

    // (Re)size the storage behind the state's names for this layout and size.
    state.settle();  // a buffer being filled for the old layout
    state.pboBytes_ = 0;
    state.nextPbo_ = 0;
    const int planeCount = picture.planeCount();
    for (int p = 0; p < planeCount; ++p) {
        const PlaneFormat fmt = planeFormat(picture, p);
        glBindTexture(GL_TEXTURE_2D, state.planes_[p]);
        glTexImage2D(GL_TEXTURE_2D, 0, fmt.internalFormat, planeWidth(picture, p),
                     planeHeight(picture, p), 0, fmt.format, fmt.type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        state.pboBytes_ += planeBytes(picture, p);
    }

    for (uint32_t buffer : state.pbos_) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(state.pboBytes_), nullptr,
                     GL_STREAM_DRAW);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    state.layout_ = picture.layout;
    state.width_ = picture.width;
    state.height_ = picture.height;
    return true;
}

// Orphan the next buffer of the ring (the GPU may still read the frame uploaded from
// it before) and map it for the copier to fill.
bool YuvConverter::mapStaging(YuvUploadState& state) {
    YuvUploadState::Staging& staged = state.staging_;
    staged.pbo = state.nextPbo_;
    state.nextPbo_ = (state.nextPbo_ + 1) % YuvUploadState::PBO_COUNT;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, state.pbos_[staged.pbo]);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(state.pboBytes_), nullptr,
                 GL_STREAM_DRAW);
    staged.mapped = static_cast<uint8_t*>(glMapBufferRange(
        GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(state.pboBytes_),
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);  // mapped buffers stay unbound: GL must not read them
    return staged.mapped != nullptr;
}

void YuvConverter::stageCopy(const YuvPicture& picture, YuvUploadState& state) {
    YuvUploadState::Staging& staged = state.staging_;
    size_t offset = 0;
    for (int p = 0; p < picture.planeCount(); ++p) {
        staged.offsets[p] = offset;
        offset += planeBytes(picture, p);
    }
    staged.picture = picture;
    copier_->submit(staged);
}

bool YuvConverter::uploadPlanes(const YuvPicture& picture, YuvUploadState& state) {
    state.copier_ = copier_;
    YuvUploadState::Staging& staged = state.staging_;
    const int planeCount = picture.planeCount();
    // The frame prefetch() staged: its bytes are (nearly) in the buffer already.
    bool staging = staged.mapped && staged.picture.owner && staged.picture.owner == picture.owner;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!staging) {
            // Another frame than the one staged (first frame, seek, dropped frames), or
            // the buffer lost its contents: copy this one now, on the copier.
            if (staged.mapped) {
                copier_->wait(staged);  // the unwanted copy still writes into the mapping
            } else if (!mapStaging(state)) {
                break;
            }
            stageCopy(picture, state);
        }
        copier_->wait(staged);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, state.pbos_[staged.pbo]);
        const bool intact = glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER) == GL_TRUE;
        staged.mapped = nullptr;
        if (intact) {
            for (int p = 0; p < planeCount; ++p) {
                const PlaneFormat fmt = planeFormat(picture, p);
                glBindTexture(GL_TEXTURE_2D, state.planes_[p]);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, planeWidth(picture, p), planeHeight(picture, p),
                                fmt.format, fmt.type, reinterpret_cast<const void*>(staged.offsets[p]));
            }
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            staged.picture = YuvPicture{};
            return true;
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        staging = false;  // contents lost (a display mode change): copy again
    }
    staged.picture = YuvPicture{};
    // Once: a failure that lasts would otherwise write a line for every frame of the show.
    static bool logged = false;
    if (!logged) {
        logged = true;
        Logger::warning("YuvConverter: could not map a pixel buffer; video frames are not uploaded while this lasts");
    }
    return false;
}

void YuvConverter::prefetch(const YuvPicture& next, YuvUploadState& state) {
    if (!copier_ || !next.isValid() || next.onGpu() || state.staging_.mapped || next.layout != state.layout_ ||
        next.width != state.width_ || next.height != state.height_) {
        return;
    }
    if (!saved_) saveState();  // mapping binds the unpack buffer
    state.copier_ = copier_;
    if (mapStaging(state)) stageCopy(next, state);
    if (!inBatch_) end();
}

void YuvConverter::begin() {
    inBatch_ = true;
    saved_ = false;
}

void YuvConverter::end() {
    if (saved_) restoreState();
    inBatch_ = false;
    saved_ = false;
}

bool YuvConverter::convert(const YuvPicture& picture, Texture& target, YuvUploadState& state) {
    if (!picture.isValid() || picture.onGpu() || !target.isValid() || target.getWidth() != picture.width ||
        target.getHeight() != picture.height) {
        return false;
    }
    if (!saved_) saveState();
    if (!ensureObjects() || !ensurePlanes(picture, state)) {
        if (!inBatch_) end();
        return false;
    }

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (!uploadPlanes(picture, state)) {
        if (!inBatch_) end();
        return false;
    }
    drawPass(picture, state.planes_, target);
    if (!inBatch_) end();
    return true;
}

bool YuvConverter::convertPlanes(const YuvPicture& picture, uint32_t luma, uint32_t chroma, Texture& target) {
    if (!picture.semiPlanar() || luma == 0 || chroma == 0 || !target.isValid() ||
        target.getWidth() != picture.width || target.getHeight() != picture.height) {
        return false;
    }
    if (!saved_) saveState();
    if (!ensureObjects()) {
        if (!inBatch_) end();
        return false;
    }
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    // Sampled as the uploaded planes are (ensurePlanes): linear, clamped. They have one
    // level already; the driver refuses GL_TEXTURE_MAX_LEVEL on a shared texture.
    for (uint32_t plane : {luma, chroma}) {
        glBindTexture(GL_TEXTURE_2D, plane);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    const uint32_t planes[3] = {luma, chroma, 0};
    drawPass(picture, planes, target);
    if (!inBatch_) end();
    return true;
}

void YuvConverter::drawPass(const YuvPicture& picture, const uint32_t planes[3], Texture& target) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.getId(), 0);
    glViewport(0, 0, picture.width, picture.height);

    const Coefficients c = coefficients(picture.matrix, picture.fullRange, picture.layout);
    program_.bind();
    program_.setMat3("uMatrix", c.matrix);
    program_.setVec3("uOffset", c.offset);
    program_.setFloat("uScale", c.scale);
    program_.setInt("uSemiPlanar", picture.semiPlanar() ? 1 : 0);

    for (int p = 0; p < 3; ++p) {
        glActiveTexture(GL_TEXTURE0 + p);
        glBindTexture(GL_TEXTURE_2D, p < picture.planeCount() ? planes[p] : 0);
    }
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

    // The RGBA texture is sampled minified by small quads and previews: refresh its mips,
    // as every full-texture update does.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, target.getId());
    glGenerateMipmap(GL_TEXTURE_2D);
}

void YuvConverter::saveState() {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &savedDrawFbo_);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &savedReadFbo_);
    glGetIntegerv(GL_VIEWPORT, savedViewport_);
    glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram_);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVao_);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture_);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &savedUnpackBuffer_);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlignment_);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &savedUnpackRowLength_);
    savedBlend_ = glIsEnabled(GL_BLEND) == GL_TRUE;
    savedScissor_ = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    savedCull_ = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    savedDepth_ = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
    saved_ = true;
}

void YuvConverter::restoreState() {
    auto setEnabled = [](GLenum cap, bool on) { on ? glEnable(cap) : glDisable(cap); };
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(savedDrawFbo_));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(savedReadFbo_));
    glViewport(savedViewport_[0], savedViewport_[1], savedViewport_[2], savedViewport_[3]);
    glUseProgram(static_cast<GLuint>(savedProgram_));
    glBindVertexArray(static_cast<GLuint>(savedVao_));
    // Leave the units the pass used unbound, as Texture's uploads do.
    for (int p = 2; p >= 0; --p) {
        glActiveTexture(GL_TEXTURE0 + p);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(static_cast<GLenum>(savedActiveTexture_));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(savedUnpackBuffer_));
    glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlignment_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, savedUnpackRowLength_);
    setEnabled(GL_BLEND, savedBlend_);
    setEnabled(GL_SCISSOR_TEST, savedScissor_);
    setEnabled(GL_CULL_FACE, savedCull_);
    setEnabled(GL_DEPTH_TEST, savedDepth_);
}

}  // namespace quadmapper
