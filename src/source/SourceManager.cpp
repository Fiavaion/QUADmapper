#include "SourceManager.hpp"
#include "StaticImageSource.hpp"
#include "VideoFileSource.hpp"
#include "CameraSource.hpp"
#include "procedural/ProceduralFactory.hpp"
#include "renderer/Texture.hpp"
#include "core/Logger.hpp"

namespace quadmapper {

SourceManager::SourceManager() = default;

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

UUID SourceManager::createVideoSource(const std::filesystem::path& path) {
    auto source = std::make_unique<VideoFileSource>(path);
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

    // Check if texture needs resize
    if (entry.texture->getWidth() != frame->width ||
        entry.texture->getHeight() != frame->height) {
        entry.texture->createEmpty(frame->width, frame->height, frame->channels);
    }

    // Upload pixel data
    entry.texture->update(frame->pixels.data());

    // Mark frame as consumed
    entry.source->markFrameConsumed();
}

} // namespace quadmapper
