#include "SourceManager.hpp"
#include "StaticImageSource.hpp"
#include "VideoFileSource.hpp"
#include "HapFileSource.hpp"
#include "CameraSource.hpp"
#include "procedural/ProceduralFactory.hpp"
#include "renderer/Texture.hpp"
#include "renderer/GpuCapabilities.hpp"
#include "core/Logger.hpp"

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
    std::string pathStr = path.string();
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

SourceManager::SourceManager() {
    masterTimeline_.play(std::chrono::steady_clock::now());
}

SourceManager::~SourceManager() {
    clear();
}

UUID SourceManager::createImageSource(const std::filesystem::path& path) {
    auto source = std::make_unique<StaticImageSource>(path);
    UUID id = source->getId();

    if (!registerSource(std::move(source)).isNull()) {
        Logger::debug("SourceManager: Created image source for '{}'", path.filename().string());
        return id;
    }

    Logger::error("SourceManager: Failed to create image source for '{}'", path.string());
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
        Logger::debug("SourceManager: '{}' is HAP — using HapFileSource", path.filename().string());
    } else {
        source = std::make_unique<VideoFileSource>(path);
    }
    UUID id = source->getId();

    if (!registerSource(std::move(source)).isNull()) {
        Logger::debug("SourceManager: Created video source for '{}'", path.filename().string());
        return id;
    }

    Logger::error("SourceManager: Failed to create video source for '{}'", path.string());
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
                uploadFrameToTexture(entry);
            }
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
        // changed. Otherwise re-upload the level in place.
        if (entry.texture->getId() == 0 ||
            !entry.texture->isCompressed() ||
            entry.texture->compressedFormat() != frame->compressedFormat ||
            entry.texture->getWidth() != frame->width ||
            entry.texture->getHeight() != frame->height) {
            entry.texture->createCompressed(frame->width, frame->height,
                                            frame->compressedFormat,
                                            frame->pixels.data(), frame->pixels.size());
        } else {
            entry.texture->updateCompressed(frame->pixels.data(), frame->pixels.size());
        }

        entry.source->markFrameConsumed();
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

} // namespace quadmapper
