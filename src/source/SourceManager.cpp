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
#include "core/BackgroundWorker.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "core/MainThreadTimings.hpp"
#include "video/VideoDecoder.hpp"

#include <glad/glad.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <utility>

#ifdef QUADMAPPER_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

namespace quadmapper {

namespace {

// What a cheap probe of a video file's container finds.
enum class ContainerKind {
    Unreadable,  // FFmpeg cannot open it as a container: it cannot be decoded
    NoVideo,     // it opens, but holds no video stream (sound only, for example)
    Hap,         // its video stream is HAP
    Video,       // any other video stream
};

// Cheap codec probe: open the container, look at its streams, close. FFmpeg stays
// behind this .cpp (ADR-011); routes HAP clips to the demux-only HapFileSource instead
// of the FFmpeg-decode VideoFileSource (ADR-044), and refuses a file that cannot be a
// video before its source exists. Runs on the main thread, so it reads the container
// header only: HAP lives in containers (QuickTime, AVI, Matroska) whose header names
// the codec. The streams are probed (avformat_find_stream_info decodes frames) only
// when the header does not.
ContainerKind probeContainer(const std::filesystem::path& path) {
#ifdef QUADMAPPER_HAS_FFMPEG
    AVFormatContext* ctx = nullptr;
    const std::string pathStr = utf8FromPath(path);  // FFmpeg file names are UTF-8
    if (avformat_open_input(&ctx, pathStr.c_str(), nullptr, nullptr) < 0) {
        return ContainerKind::Unreadable;
    }

    auto scan = [ctx](bool& unknown) {
        unknown = false;
        bool video = false;
        for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
            const AVCodecParameters* cp = ctx->streams[i]->codecpar;
            if (cp->codec_id == AV_CODEC_ID_HAP) return ContainerKind::Hap;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO) video = true;
            if (cp->codec_id == AV_CODEC_ID_NONE || cp->codec_type == AVMEDIA_TYPE_UNKNOWN) unknown = true;
        }
        return video ? ContainerKind::Video : ContainerKind::NoVideo;
    };
    bool unknown = false;
    ContainerKind kind = scan(unknown);
    if (kind != ContainerKind::Hap && (unknown || ctx->nb_streams == 0) &&
        avformat_find_stream_info(ctx, nullptr) >= 0) {
        kind = scan(unknown);
    }

    avformat_close_input(&ctx);
    return kind;
#else
    (void)path;
    return ContainerKind::Video;
#endif
}

} // namespace

// A video file opening on the opener thread. The worker writes `decoder` (or `hapFile`)
// and then sets `done`; the main thread reads them only after seeing `done`.
struct SourceManager::PendingOpen {
    UUID id;
    std::filesystem::path path;
    bool hap = false;                    // a HAP clip: its container, not a decoder
    DecodeOptions options;
    std::atomic<bool> cancelled{false};  // the source was removed: open nothing more
    std::atomic<bool> done{false};
    std::unique_ptr<VideoDecoder> decoder;  // null when the file could not be opened
    std::unique_ptr<HapFileSource::OpenedFile> hapFile;  // likewise
};

SourceManager::SourceManager()
    : yuvConverter_(std::make_unique<YuvConverter>())  // no GL work until the first video frame
    , compressedUploader_(std::make_unique<CompressedUploader>())  // nor until the first HAP frame
    , dxInterop_(std::make_unique<DxInterop>())        // nor any D3D11 work
{
    masterTimeline_.play(std::chrono::steady_clock::now());
}

// Every worker is joined before what its tasks use is destroyed: the opener uses
// dxInterop_, the reaper frees sources and decoders. GL objects are freed here, on the
// main thread, as before.
SourceManager::~SourceManager() {
    clear();
    opener_.reset();  // waits for an open in progress; queued ones were cancelled
    for (const auto& pending : opening_) retireOpened(*pending);
    opening_.clear();
    reaper_.reset();  // frees everything retired, then joins
    retiredGl_.clear();
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
    ScopedMainThreadTiming timing("createVideoSource");
    // Probe the container: HAP clips route to the demux-only HapFileSource so the
    // GPU-compressed frame is preserved; everything else uses the FFmpeg-decode
    // VideoFileSource (ADR-044). Both report SourceType::VIDEO_FILE.
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        Logger::error("SourceManager: Failed to create video source for '{}': no such file", utf8FromPath(path));
        return UUID();
    }
    ContainerKind kind = ContainerKind::Video;
    {
        ScopedMainThreadTiming probeTiming("videoProbe");
        kind = probeContainer(path);
    }
    if (kind == ContainerKind::Unreadable || kind == ContainerKind::NoVideo) {
        Logger::error("SourceManager: Failed to create video source for '{}': {}", utf8FromPath(path),
                      kind == ContainerKind::Unreadable ? "not a file FFmpeg can read" : "it holds no video");
        return UUID();
    }
    // The file opens on the opener thread (TD-35: up to 3.4 s on the main thread under
    // load, a HAP clip's container up to 18 ms); updateAll() hands what was opened to the
    // source. A HAP clip demuxes only (ADR-044); any other is decoded through FFmpeg, and
    // the first stream that leaves frames on the GPU also prepares the zero-copy device
    // there, so its first frame does not create it (TD-35: up to 3.1 s).
    auto pending = std::make_shared<PendingOpen>();
    pending->path = path;
    if (kind == ContainerKind::Hap) {
        Logger::debug("SourceManager: '{}' is HAP — using HapFileSource", utf8FromPath(path.filename()));
        auto source = std::make_unique<HapFileSource>(path);
        source->beginOpen();
        pending->hap = true;
        pending->id = addEntry(std::move(source));
    } else {
        // Hardware frames stay on the GPU when this GL context can share them (ADR-056).
        pending->options = VideoFileSource::playbackDecodeOptions();
        pending->options.zeroCopy = dxInterop_->available();
        auto source = std::make_unique<VideoFileSource>(path, pending->options);
        source->beginOpen();
        pending->id = addEntry(std::move(source));
    }
    opening_.push_back(pending);
    if (!opener_) opener_ = std::make_unique<BackgroundWorker>("Video opener");
    // Whatever happens in the open, `done` is set: an exception fails the open (the
    // source then goes the way of a file that cannot be decoded), never leaves it opening.
    opener_->post([pending, interop = dxInterop_.get(), hook = openHookForTesting_] {
        if (!pending->cancelled) {
            try {
                if (hook) hook(pending->path);
                if (pending->hap) {
                    pending->hapFile = HapFileSource::openFile(pending->path);
                } else {
                    std::unique_ptr<VideoDecoder> decoder =
                        VideoFileSource::openDecoder(pending->path, pending->options);
                    if (decoder && decoder->isZeroCopy() && !pending->cancelled) {
                        interop->prepare(VideoDecoder::sharedD3dDevice());
                    }
                    // Its decode thread too, idle until finishOpen() hands it the clock's
                    // position, so the main thread does not create it (TD-35).
                    if (decoder) decoder->startIdleThread();
                    pending->decoder = std::move(decoder);
                }
            } catch (const std::exception& e) {
                Logger::error("SourceManager: Opening '{}' failed: {}", utf8FromPath(pending->path), e.what());
            } catch (...) {
                Logger::error("SourceManager: Opening '{}' failed", utf8FromPath(pending->path));
            }
        }
        pending->done.store(true, std::memory_order_release);
    });
    Logger::debug("SourceManager: Opening video source for '{}'", utf8FromPath(path.filename()));
    return pending->id;
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

    if (!source->initialize()) {
        return UUID();
    }
    return addEntry(std::move(source));
}

UUID SourceManager::addEntry(std::unique_ptr<TextureSource> source) {
    const UUID id = source->getId();

    SourceEntry entry;
    entry.source = std::move(source);
    entry.needsTextureUpdate = true;

    // Create GPU texture
    createGPUTexture(entry);

    sources_[id] = std::move(entry);

    if (sourceAddedCallback_) {
        sourceAddedCallback_(id);
    }

    return id;
}

void SourceManager::removeSource(const UUID& id) {
    ScopedMainThreadTiming timing("removeSource");
    auto it = sources_.find(id);
    if (it != sources_.end()) {
        retire(it->second);
        sources_.erase(it);

        if (sourceRemovedCallback_) {
            sourceRemovedCallback_(id);
        }
    }
}

void SourceManager::clear() {
    ScopedMainThreadTiming timing("clearSources");
    for (auto& [id, entry] : sources_) {
        retire(entry);
    }
    sources_.clear();
}

// Free a source that is leaving: its GL objects here (they need this thread's context),
// a video file's decode threads, FFmpeg and D3D11 objects on the reaper thread. Joining
// those threads and freeing their decoders took up to 3.6 s on the main thread (TD-35).
void SourceManager::retire(SourceEntry& entry) {
    for (const auto& pending : opening_) {
        if (pending->id == entry.source->getId()) pending->cancelled = true;
    }
    RetiredGl gl;
    gl.texture = std::move(entry.texture);
    gl.yuvUpload = std::move(entry.yuvUpload);
    gl.compressedUpload = std::move(entry.compressedUpload);
    // Its shared planes, still registered, wait for the next source of their size (TD-35).
    gl.zeroCopy = dxInterop_->recycle(std::move(entry.zeroCopy));
    retireGl(std::move(gl));
    if (entry.source->getType() == SourceType::VIDEO_FILE) {
        // Video and HAP sources hold no GL objects; everything they own may be freed
        // on any thread once their own threads are joined, which shutdown() does.
        std::shared_ptr<TextureSource> doomed(std::move(entry.source));
        reaper().post([doomed = std::move(doomed)]() mutable {
            doomed->shutdown();
            doomed.reset();
        });
    } else {
        // Procedural sources may own GL textures: freed here. So are cameras: DirectShow
        // capture graphs are COM objects of the apartment that built them, which is this
        // thread (CameraSource), so their device is opened and closed here.
        entry.source->shutdown();
        entry.source.reset();
    }
}

// Removed GL state waits here until it can be freed without this thread waiting on the
// GPU, the driver or the copier thread (releaseRetiredGl). The fence marks the end of
// every GL command that may still read it: the frames already drawn with its texture,
// the planes uploaded or converted into it.
void SourceManager::retireGl(RetiredGl retired) {
    if (!retired.texture && !retired.yuvUpload && !retired.compressedUpload && !retired.zeroCopy) return;
    if (glFenceSync) retired.fence.reset(glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0));
    retiredGl_.push_back(std::move(retired));
}

void SourceManager::GlFenceDeleter::operator()(void* fence) const {
    glDeleteSync(static_cast<GLsync>(fence));
}

// Free removed GL state without waiting on this thread (TD-35: up to 318 ms for one
// source under load, in glDeleteTextures, wglDXUnregisterObjectNV or waiting for a frame
// copy). An entry is touched only once its fence has signalled (checked, never waited
// on); a pixel buffer still being filled by the copier waits for a later frame (a copy
// merely queued is dropped). Then its objects go one per step (a texture, an upload
// state, one shared plane): at least one step a frame, and more only while the frame's
// 1 ms budget lasts. A single driver call can still stall its step (unregistering a plane
// took up to 57 ms with the GPU done with it), which is why removed sources' shared
// planes are kept for the next source instead (DxInterop::recycle).
void SourceManager::releaseRetiredGl() {
    if (retiredGl_.empty()) return;
    ScopedMainThreadTiming timing("retiredGlRelease");
    constexpr double BUDGET_SECONDS = 0.001;
    const auto start = std::chrono::steady_clock::now();
    bool stepped = false;
    auto budgetLeft = [&] {
        return !stepped || std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() <
                               BUDGET_SECONDS;
    };
    for (size_t i = 0; i < retiredGl_.size() && budgetLeft();) {
        RetiredGl& r = retiredGl_[i];
        if (r.fence) {
            // Flushes, so the fence is sure to signal, but never waits (timeout 0).
            const GLenum status =
                glClientWaitSync(static_cast<GLsync>(r.fence.get()), GL_SYNC_FLUSH_COMMANDS_BIT, 0);
            if (status == GL_TIMEOUT_EXPIRED) break;  // fences signal in order: so are the later ones
            r.fence.reset();  // signalled (or failed: then nothing can be learned by waiting)
        }
        if (r.yuvUpload && r.yuvUpload->idle()) {
            r.yuvUpload.reset();
            stepped = true;
        } else if (r.compressedUpload && r.compressedUpload->idle()) {
            r.compressedUpload.reset();
            stepped = true;
        } else if (r.zeroCopy) {
            if (r.zeroCopy->releaseStep()) r.zeroCopy.reset();
            stepped = true;
        } else if (r.texture) {
            r.texture.reset();
            stepped = true;
        } else if (!r.yuvUpload && !r.compressedUpload) {
            retiredGl_.erase(retiredGl_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        } else {
            ++i;  // a copy still fills one of its buffers: try the next entry
        }
    }
}

// What an open made for a source that no longer wants it: freed on the reaper (a decoder
// joins its threads and frees FFmpeg and D3D11 objects; a HAP container closes its file).
void SourceManager::retireOpened(PendingOpen& pending) {
    std::shared_ptr<VideoDecoder> decoder(std::move(pending.decoder));
    std::shared_ptr<HapFileSource::OpenedFile> hapFile(std::move(pending.hapFile));
    if (!decoder && !hapFile) return;
    reaper().post([decoder = std::move(decoder), hapFile = std::move(hapFile)]() mutable {
        decoder.reset();
        hapFile.reset();
    });
}

BackgroundWorker& SourceManager::reaper() {
    if (!reaper_) reaper_ = std::make_unique<BackgroundWorker>("Source reaper");
    return *reaper_;
}

// Main thread: give each opened decoder to its source, which starts decoding at once.
void SourceManager::adoptOpenedSources() {
    for (auto it = opening_.begin(); it != opening_.end();) {
        PendingOpen& pending = **it;
        if (!pending.done.load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        auto entry = pending.cancelled ? sources_.end() : sources_.find(pending.id);
        TextureSource* source = entry != sources_.end() ? entry->second.source.get() : nullptr;
        if (auto* video = dynamic_cast<VideoFileSource*>(source); video && video->isOpening()) {
            ScopedMainThreadTiming timing("finishOpen");
            if (!video->finishOpen(std::move(pending.decoder))) failedOpens_.push_back(pending.id);
        } else if (auto* hap = dynamic_cast<HapFileSource*>(source); hap && hap->isOpening()) {
            ScopedMainThreadTiming timing("finishOpen");
            if (!hap->finishOpen(std::move(pending.hapFile))) failedOpens_.push_back(pending.id);
        }
        retireOpened(pending);  // whatever its source, removed meanwhile, did not take
        it = opening_.erase(it);
    }
}

void SourceManager::prepareGpu() {
    yuvConverter_->prepare();
    dxInterop_->available();
}

void SourceManager::finishBackgroundWork() {
    if (opener_) opener_->drain();
    adoptOpenedSources();
    if (reaper_) reaper_->drain();
    retiredGl_.clear();
    dxInterop_->releaseSpares();
}

std::vector<UUID> SourceManager::takeFailedOpens() {
    return std::exchange(failedOpens_, {});
}

void SourceManager::setOpenHookForTesting(std::function<void(const std::filesystem::path&)> hook) {
    openHookForTesting_ = std::move(hook);
}

void SourceManager::setStandIn(const UUID& id, const UUID& standIn) {
    auto it = sources_.find(id);
    if (it != sources_.end()) it->second.standIn = standIn;
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

bool SourceManager::showsCurrentFrame(const UUID& id) const {
    auto it = sources_.find(id);
    return it != sources_.end() && it->second.hasFrame && !it->second.uploadDeferred;
}

Texture* SourceManager::getTexture(const UUID& id) {
    auto it = sources_.find(id);
    if (it == sources_.end()) {
        return nullptr;
    }

    SourceEntry& entry = it->second;

    // A replacement still opening shows the source it replaces (setStandIn), when that
    // has a picture to show.
    if (!entry.hasFrame && !entry.standIn.isNull()) {
        auto standIn = sources_.find(entry.standIn);
        if (standIn != sources_.end() && standIn->second.texture && standIn->second.texture->isValid()) {
            return standIn->second.texture.get();
        }
    }

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
    for (const auto& [id, entry] : sources_) {
        if (Texture* texture = getTexture(id)) {
            map[id] = texture;
        }
    }
    return map;
}

void SourceManager::updateAll(double deltaTime) {
    ScopedMainThreadTiming timing("updateAll");
    adoptOpenedSources();
    dxInterop_->beginFrame();  // zero-copy set-up: one costly GL call this frame at most (TD-35)
    yuvConverter_->begin();  // GL state is saved once, at the first conversion, and restored below
    for (auto& [id, entry] : sources_) {
        if (!entry.source) {
            continue;
        }
        // A locked source still opening follows the master too: its clock already runs.
        auto* playable = dynamic_cast<PlayableSource*>(entry.source.get());
        if (playable && playable->isTimelineLocked()) {
            playable->followTimeline(masterTimeline_);
        }
        if (!entry.source->isReady()) {
            continue;
        }

        // Update the source; a frame whose upload had to wait (uploadDeferred) is offered again.
        const bool hasNewFrame = entry.source->update(deltaTime) || entry.uploadDeferred;

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
            if (!entry.uploadDeferred) entry.hasFrame = true;
        }
    }
    // The zero-copy frames staged above, all under one interop lock (ADR-056).
    const auto start = std::chrono::steady_clock::now();
    dxInterop_->finish(*yuvConverter_, [this](void* key, const YuvPicture&, const std::string& why) {
        fallBackFromZeroCopy(*static_cast<SourceEntry*>(key), why);
    });
    uploadStats_.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    yuvConverter_->end();

    releaseRetiredGl();
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
    entry.uploadDeferred = false;  // set again if the frame must wait (uploadYuvFrame)
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
    // Left on the GPU (ADR-056), a source's first frame sets its shared planes up over a
    // few updates (TD-35); meanwhile it keeps the frame, and its texture what it showed.
    std::string why;
    DxInterop::SetUp setUp = DxInterop::SetUp::Failed;
    if (frame.yuv.onGpu() && !entry.zeroCopyFailed) {
        if (!entry.zeroCopy) entry.zeroCopy = std::make_unique<DxInteropTarget>();
        setUp = dxInterop_->setUp(frame.yuv, *entry.zeroCopy, why);
        if (setUp == DxInterop::SetUp::Later) {
            entry.uploadDeferred = true;
            return;
        }
    }
    Texture& texture = *entry.texture;
    if (texture.getId() == 0 || texture.isCompressed() || texture.getChannels() != 4 ||
        texture.getWidth() != frame.width || texture.getHeight() != frame.height) {
        texture.createEmpty(frame.width, frame.height, 4);
    }
    if (frame.yuv.onGpu()) {
        // Staged now, delivered by finish() at the end of updateAll().
        if (!entry.zeroCopyFailed) {
            if (setUp == DxInterop::SetUp::Ready &&
                dxInterop_->stage(frame.yuv, *entry.zeroCopy, texture, &entry, why)) {
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
    entry.zeroCopyFailed = true;  // never staged again
    // Its shared planes go as a removed source's do (unregistering a target from GL costs
    // milliseconds; 16 sources fall back together when the lock fails for all).
    RetiredGl gl;
    gl.zeroCopy = std::move(entry.zeroCopy);
    retireGl(std::move(gl));
    if (auto* video = dynamic_cast<VideoFileSource*>(entry.source.get())) video->stopZeroCopy(why);
}

} // namespace quadmapper
