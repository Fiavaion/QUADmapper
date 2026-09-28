#include "SourceManager.hpp"
#include "StaticImageSource.hpp"
#include "VideoFileSource.hpp"
#include "HapFileSource.hpp"
#include "CameraSource.hpp"
#include "procedural/ProceduralFactory.hpp"
#include "renderer/Texture.hpp"
#include "renderer/CompressedUpload.hpp"
#include "renderer/YuvConverter.hpp"
#include "renderer/DxInterop.hpp"
#include "renderer/GpuCapabilities.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"

#include <chrono>

#ifdef QUADMAPPER_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

namespace quadmapper {

namespace {

// Cheap codec probe: open the container, check whether its video stream is HAP,
// close. FFmpeg stays behind this .cpp (ADR-011); routes HAP clips to the
// demux-only HapFileSource instead of the FFmpeg-decode VideoFileSource (ADR-044).
bool isHapContainer(const std::filesystem::path& path) {
#ifdef QUADMAPPER_HAS_FFMPEG
    AVFormatContext* ctx = nullptr;
    const std::string pathStr = utf8FromPath(path);  // FFmpeg file names are UTF-8
    if (avformat_open_input(&ctx, pathStr.c_str(), nullptr, nullptr) < 0) {
        return false;
    }

    bool isHap = false;
    if (avformat_find_stream_info(ctx, nullptr) >= 0) {
        for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
            AVCodecParameters* cp = ctx->streams[i]->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->codec_id == AV_CODEC_ID_HAP) {
                isHap = true;
                break;
            }
        }
    }

    avformat_close_input(&ctx);
    return isHap;
#else
    (void)path;
    return false;
#endif
}

} // namespace

SourceManager::SourceManager()
    : yuvConverter_(std::make_unique<YuvConverter>())  // no GL work until the first video frame
    , compressedUploader_(std::make_unique<CompressedUploader>())  // nor until the first HAP frame
    , dxInterop_(std::make_unique<DxInterop>())        // nor any D3D11 work
{
    masterTimeline_.play(std::chrono::steady_clock::now());
}

SourceManager::~SourceManager() {
    clear();
}

UUID SourceManager::createImageSource(const std::filesystem::path& path) {
    auto source = std::make_unique<StaticImageSource>(path);
    UUID id = source->getId();

    if (!registerSource(std::move(source)).isNull()) {
        Logger::debug("SourceManager: Created image source for '{}'", utf8FromPath(path.filename()));
        return id;
    }

    Logger::error("SourceManager: Failed to create image source for '{}'", utf8FromPath(path));
    return UUID();
}

UUID SourceManager::findImageSourceByPath(const std::filesystem::path& path) const {
    for (const auto& [id, entry] : sources_) {
        const auto* img = dynamic_cast<const StaticImageSource*>(entry.source.get());
        if (img && img->getPath() == path) {
            return id;
        }
    }
    return UUID();
}

UUID SourceManager::getOrCreateImageSource(const std::filesystem::path& path) {
    UUID existing = findImageSourceByPath(path);
    if (!existing.isNull()) {
        return existing;  // dedupe: one decode/upload shared across quads
    }
    return createImageSource(path);
}

UUID SourceManager::findVideoSourceByPath(const std::filesystem::path& path) const {
    for (const auto& [id, entry] : sources_) {
        // Both video back-ends report SourceType::VIDEO_FILE and store their path;
        // match either so HAP and H.264 clips dedupe the same way (ADR-044).
        if (const auto* vid = dynamic_cast<const VideoFileSource*>(entry.source.get())) {
            if (vid->getPath() == path) return id;
        } else if (const auto* hap = dynamic_cast<const HapFileSource*>(entry.source.get())) {
            if (hap->getPath() == path) return id;
        }
    }
    return UUID();
}

UUID SourceManager::getOrCreateVideoSource(const std::filesystem::path& path) {
    UUID existing = findVideoSourceByPath(path);
    if (!existing.isNull()) {
        return existing;  // dedupe: one decode shared across quads
    }
    return createVideoSource(path);
}

UUID SourceManager::createVideoSource(const std::filesystem::path& path) {
    // Probe the container: HAP clips route to the demux-only HapFileSource so the
    // GPU-compressed frame is preserved; everything else uses the FFmpeg-decode
    // VideoFileSource (ADR-044). Both report SourceType::VIDEO_FILE.
    std::unique_ptr<TextureSource> source;
    if (isHapContainer(path)) {
        source = std::make_unique<HapFileSource>(path);
        Logger::debug("SourceManager: '{}' is HAP — using HapFileSource", utf8FromPath(path.filename()));
    } else {
        // Hardware frames stay on the GPU when this GL context can share them (ADR-056).
        DecodeOptions options = VideoFileSource::playbackDecodeOptions();
        options.zeroCopy = dxInterop_->available();
        source = std::make_unique<VideoFileSource>(path, options);
    }
    UUID id = source->getId();

    if (!registerSource(std::move(source)).isNull()) {
        Logger::debug("SourceManager: Created video source for '{}'", utf8FromPath(path.filename()));
        return id;
    }

    Logger::error("SourceManager: Failed to create video source for '{}'", utf8FromPath(path));
    return UUID();
}

UUID SourceManager::createCameraSource(const std::string& deviceName) {
    auto source = std::make_unique<CameraSource>(deviceName);
    UUID id = source->getId();

    if (!registerSource(std::move(source)).isNull()) {
        Logger::debug("SourceManager: Created camera source '{}'",
                      deviceName.empty() ? "(default)" : deviceName);
        return id;
    }

    Logger::error("SourceManager: Failed to create camera source '{}'",
                  deviceName.empty() ? "(default)" : deviceName);
    return UUID();
}

UUID SourceManager::createProceduralSource(ProceduralType type, int width, int height) {
    auto source = ProceduralFactory::create(type, width, height);
    if (!source) {
        Logger::error("SourceManager: Failed to create procedural source of type {}",
                      ProceduralFactory::getTypeName(type));
        return UUID();
    }

    UUID id = source->getId();
    std::string typeName = ProceduralFactory::getTypeName(type);

    if (!registerSource(std::move(source)).isNull()) {
        Logger::debug("SourceManager: Created {} procedural source ({}x{})",
                      typeName, width, height);
        return id;
    }

    Logger::error("SourceManager: Failed to register {} procedural source", typeName);
    return UUID();
}

UUID SourceManager::registerSource(std::unique_ptr<TextureSource> source) {
    if (!source) {
        return UUID();
    }

    UUID id = source->getId();

    SourceEntry entry;
    entry.source = std::move(source);
    entry.needsTextureUpdate = true;

    // Initialize the source
    if (!entry.source->initialize()) {
        return UUID();
    }

    // Create GPU texture
    createGPUTexture(entry);

    sources_[id] = std::move(entry);

    if (sourceAddedCallback_) {
        sourceAddedCallback_(id);
    }

    return id;
}

void SourceManager::removeSource(const UUID& id) {
    auto it = sources_.find(id);
    if (it != sources_.end()) {
        it->second.source->shutdown();
        sources_.erase(it);

        if (sourceRemovedCallback_) {
            sourceRemovedCallback_(id);
        }
    }
}

void SourceManager::clear() {
    for (auto& [id, entry] : sources_) {
        entry.source->shutdown();
    }
    sources_.clear();
}

size_t SourceManager::releaseUnreferenced(const std::unordered_set<UUID>& inUse) {
    std::vector<UUID> unused;
    for (const auto& [id, entry] : sources_) {
        const SourceType type = entry.source->getType();
        const bool fileBacked = type == SourceType::STATIC_IMAGE || type == SourceType::VIDEO_FILE;
        const bool procedural = type >= SourceType::PROCEDURAL_SOLID &&
                                type <= SourceType::PROCEDURAL_GRID;
        const bool camera = type == SourceType::WEBCAM;
        if ((fileBacked || procedural || camera) && inUse.count(id) == 0) {
            unused.push_back(id);
        }
    }
    for (const UUID& id : unused) {
        Logger::debug("SourceManager: releasing unused source '{}'", sources_[id].source->getName());
        removeSource(id);
    }
    return unused.size();
}

TextureSource* SourceManager::getSource(const UUID& id) {
    auto it = sources_.find(id);
    return it != sources_.end() ? it->second.source.get() : nullptr;
}

const TextureSource* SourceManager::getSource(const UUID& id) const {
    auto it = sources_.find(id);
    return it != sources_.end() ? it->second.source.get() : nullptr;
}

bool SourceManager::hasSource(const UUID& id) const {
    return sources_.find(id) != sources_.end();
}

std::filesystem::path SourceManager::getSourcePath(const UUID& id) const {
    const TextureSource* source = getSource(id);
    if (const auto* img = dynamic_cast<const StaticImageSource*>(source)) return img->getPath();
    if (const auto* vid = dynamic_cast<const VideoFileSource*>(source)) return vid->getPath();
    if (const auto* hap = dynamic_cast<const HapFileSource*>(source)) return hap->getPath();
    return {};
}

std::vector<UUID> SourceManager::getAllSourceIds() const {
    std::vector<UUID> ids;
    ids.reserve(sources_.size());
    for (const auto& [id, entry] : sources_) {
        ids.push_back(id);
    }
    return ids;
}

size_t SourceManager::getSourceCount() const {
    return sources_.size();
}

Texture* SourceManager::getTexture(const UUID& id) {
    auto it = sources_.find(id);
    if (it == sources_.end()) {
        return nullptr;
    }

    SourceEntry& entry = it->second;

    // For GPU-native sources, the texture wraps the source's native texture
    // For CPU sources, we manage the texture
    return entry.texture.get();
}

std::unordered_map<UUID, Texture*> SourceManager::getTextureMap(const std::vector<UUID>& sourceIds) {
    std::unordered_map<UUID, Texture*> map;
    for (const UUID& id : sourceIds) {
        Texture* tex = getTexture(id);
        if (tex) {
            map[id] = tex;
        }
    }
    return map;
}

std::unordered_map<UUID, Texture*> SourceManager::getTextureMap() {
    std::unordered_map<UUID, Texture*> map;
    for (auto& [id, entry] : sources_) {
        if (entry.texture) {
            map[id] = entry.texture.get();
        }
    }
    return map;
}

void SourceManager::updateAll(double deltaTime) {
    yuvConverter_->begin();  // GL state is saved once, at the first conversion, and restored below
    for (auto& [id, entry] : sources_) {
        if (!entry.source || !entry.source->isReady()) {
            continue;
        }
        auto* playable = dynamic_cast<PlayableSource*>(entry.source.get());
        if (playable && playable->isTimelineLocked()) {
            playable->followTimeline(masterTimeline_);
        }

        // Update the source
        bool hasNewFrame = entry.source->update(deltaTime);

        if (hasNewFrame) {
            if (entry.source->isGPUNative()) {
                // GPU-native source: update texture wrapper if needed
                uint32_t glId = entry.source->getGLTextureId();
                if (entry.texture && entry.texture->getId() != glId) {
                    entry.texture->setGLTextureId(
                        glId,
                        entry.source->getWidth(),
                        entry.source->getHeight(),
                        4  // RGBA for compute shader outputs
                    );
                }
            } else {
                // CPU source: upload frame data to GPU texture
                const auto start = std::chrono::steady_clock::now();
                uploadFrameToTexture(entry);
                uploadStats_.seconds +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                ++uploadStats_.uploads;
            }
        }
    }
    // The zero-copy frames staged above, all under one interop lock (ADR-056).
    const auto start = std::chrono::steady_clock::now();
    dxInterop_->finish(*yuvConverter_, [this](void* key, const YuvPicture&, const std::string& why) {
        fallBackFromZeroCopy(*static_cast<SourceEntry*>(key), why);
    });
    uploadStats_.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    yuvConverter_->end();

    // The shared planes of one source that fell back: releasing a target unregisters it
    // from GL and frees its D3D11 textures, a few milliseconds each, so when many sources
    // fall back together they go one a frame instead of all in the frame they failed.
    for (auto& [id, entry] : sources_) {
        if (entry.zeroCopyFailed && entry.zeroCopy) {
            entry.zeroCopy.reset();
            break;
        }
    }
}

void SourceManager::seekAllToStart() {
    for (auto& [id, entry] : sources_) {
        if (auto* playable = dynamic_cast<PlayableSource*>(entry.source.get())) {
            playable->seek(0.0);
        }
    }
    restartMasterTimeline();
}

void SourceManager::playMasterTimeline() {
    masterTimeline_.play(std::chrono::steady_clock::now());
}

void SourceManager::pauseMasterTimeline() {
    masterTimeline_.pause(std::chrono::steady_clock::now());
}

void SourceManager::restartMasterTimeline() {
    masterTimeline_.seek(0.0, std::chrono::steady_clock::now());
}

void SourceManager::setTimelineLocked(const UUID& id, bool locked) {
    if (auto* playable = getSourceAs<PlayableSource>(id)) {
        playable->setTimelineLocked(locked, masterTimeline_);
    }
}

void SourceManager::setSourceAddedCallback(SourceAddedCallback callback) {
    sourceAddedCallback_ = std::move(callback);
}

void SourceManager::setSourceRemovedCallback(SourceRemovedCallback callback) {
    sourceRemovedCallback_ = std::move(callback);
}

void SourceManager::createGPUTexture(SourceEntry& entry) {
    if (!entry.source) {
        return;
    }

    entry.texture = std::make_unique<Texture>();

    if (entry.source->isGPUNative()) {
        // GPU-native: wrap the source's texture (non-owning)
        uint32_t glId = entry.source->getGLTextureId();
        if (glId != 0) {
            entry.texture->setGLTextureId(
                glId,
                entry.source->getWidth(),
                entry.source->getHeight(),
                4
            );
        }
    } else {
        // CPU source: create empty texture for uploads
        int width = entry.source->getWidth();
        int height = entry.source->getHeight();
        if (width > 0 && height > 0) {
            entry.texture->createEmpty(width, height, 4);
        }
        // Video frames arrive as YUV planes (ADR-055): the upload state's GL objects
        // live as long as the source, so its GL object count never changes mid-play.
        if (dynamic_cast<VideoFileSource*>(entry.source.get())) {
            entry.yuvUpload = std::make_unique<YuvUploadState>();
        } else if (dynamic_cast<HapFileSource*>(entry.source.get())) {
            entry.compressedUpload = std::make_unique<CompressedUploadState>();  // likewise (ADR-044)
        }
    }
}

void SourceManager::uploadFrameToTexture(SourceEntry& entry) {
    if (!entry.source || !entry.texture) {
        return;
    }

    const FrameData* frame = entry.source->getCurrentFrame();
    if (!frame || !frame->isValid()) {
        return;
    }

    // GPU-compressed (HAP/BCn) path (ADR-044). The frame already holds BCn block
    // bytes; we upload them directly and let the shader sample the compressed
    // texture. Capability-gated: an unsupported format is skipped (texture stays
    // black) and logged once — never a crash (ADR-044 §4 + deviation note).
    if (frame->isCompressed()) {
        if (!GpuCapabilities::get().supports(frame->compressedFormat)) {
            if (!entry.compressedUnsupportedLogged) {
                Logger::warning("SourceManager: GPU lacks the texture-compression "
                                "extension for this HAP clip — skipping upload "
                                "(texture stays black). See ADR-044.");
                entry.compressedUnsupportedLogged = true;
            }
            entry.source->markFrameConsumed();
            return;
        }

        // (Re)create when the texture is missing, was an ordinary RGB(A) texture
        // (e.g. createGPUTexture pre-provisioned RGBA), or its dimensions/format
        // changed; storage only, the frame arrives through the pixel buffers.
        if (entry.texture->getId() == 0 ||
            !entry.texture->isCompressed() ||
            entry.texture->compressedFormat() != frame->compressedFormat ||
            entry.texture->getWidth() != frame->width ||
            entry.texture->getHeight() != frame->height) {
            entry.texture->createCompressed(frame->width, frame->height, frame->compressedFormat,
                                            nullptr, frame->compressed->size());
        }
        // Through the source's pixel buffers, the bytes copied off the main thread (TD-31).
        if (!entry.compressedUpload) entry.compressedUpload = std::make_unique<CompressedUploadState>();
        if (compressedUploader_->upload(frame->compressed, *entry.texture, *entry.compressedUpload)) {
            compressedUploader_->prefetch(frame->nextCompressed, *entry.compressedUpload);
        }

        entry.source->markFrameConsumed();
        return;
    }

    if (frame->isYuv()) {
        uploadYuvFrame(entry, *frame);
        return;
    }

    // Reallocate if the texture doesn't match the frame — including a CHANNEL
    // mismatch: createGPUTexture provisions RGBA, but video frames are RGB(3),
    // and uploading 3-channel data as RGBA over-reads the buffer (crash).
    if (entry.texture->getWidth() != frame->width ||
        entry.texture->getHeight() != frame->height ||
        entry.texture->getChannels() != frame->channels) {
        entry.texture->createEmpty(frame->width, frame->height, frame->channels);
    }

    // Upload pixel data
    entry.texture->update(frame->pixels.data());

    // Mark frame as consumed
    entry.source->markFrameConsumed();
}

// Video as YUV planes (ADR-055): the planes go up through the source's pixel buffers
// and a GPU pass writes them into its RGBA texture, the one texture every consumer samples.
void SourceManager::uploadYuvFrame(SourceEntry& entry, const FrameData& frame) {
    Texture& texture = *entry.texture;
    if (texture.getId() == 0 || texture.isCompressed() || texture.getChannels() != 4 ||
        texture.getWidth() != frame.width || texture.getHeight() != frame.height) {
        texture.createEmpty(frame.width, frame.height, 4);
    }
    if (frame.yuv.onGpu()) {
        // Left on the GPU (ADR-056): staged now, delivered by finish() at the end of updateAll().
        if (!entry.zeroCopyFailed) {
            std::string why;
            if (!entry.zeroCopy) entry.zeroCopy = std::make_unique<DxInteropTarget>();
            if (dxInterop_->stage(frame.yuv, *entry.zeroCopy, texture, &entry, why)) {
                entry.source->markFrameConsumed();
                return;
            }
            fallBackFromZeroCopy(entry, why);
        }
        // Fallen back: the texture keeps the last frame shown (see fallBackFromZeroCopy).
        entry.source->markFrameConsumed();
        return;
    }
    if (!entry.yuvUpload) entry.yuvUpload = std::make_unique<YuvUploadState>();  // a registered source
    if (yuvConverter_->convert(frame.yuv, texture, *entry.yuvUpload)) {
        yuvConverter_->prefetch(frame.nextYuv, *entry.yuvUpload);
    }
    entry.source->markFrameConsumed();
}

// This source cannot use zero-copy (ADR-056): its decoder copies frames through memory
// from now on, including the frames it had queued on the GPU, on its own thread. Frames
// on the GPU that reach this thread meanwhile are skipped and the texture keeps the last
// frame shown: reading them back here would wait on the GPU on the main thread (16
// streams falling back at once stalled a frame for 700 ms).
void SourceManager::fallBackFromZeroCopy(SourceEntry& entry, const std::string& why) {
    if (entry.zeroCopyFailed) return;
    entry.zeroCopyFailed = true;  // its target is never staged again; released by updateAll()
    if (auto* video = dynamic_cast<VideoFileSource*>(entry.source.get())) video->stopZeroCopy(why);
}

} // namespace quadmapper
