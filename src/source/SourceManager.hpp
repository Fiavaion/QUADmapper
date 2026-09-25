#pragma once

#include "TextureSource.hpp"
#include "core/UUID.hpp"
#include "core/Types.hpp"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <functional>

namespace quadmapper {

// Forward declarations
class Texture;

/**
 * @brief Central manager for all texture sources
 *
 * SourceManager owns all TextureSource instances and their associated
 * GPU textures. It provides:
 * - Source lifecycle management (create, destroy)
 * - Per-frame updates for all sources
 * - CPU→GPU texture upload for non-GPU-native sources
 * - Texture lookup for rendering
 *
 * Usage:
 * @code
 * SourceManager sources;
 *
 * // Create sources
 * UUID imageId = sources.createImageSource("photo.jpg");
 * UUID videoId = sources.createVideoSource("movie.mp4");
 * UUID noiseId = sources.createProceduralSource(ProceduralType::NOISE);
 *
 * // Each frame
 * sources.updateAll(deltaTime);
 *
 * // Render
 * Texture* tex = sources.getTexture(imageId);
 * renderer.renderQuad(quad, tex);
 * @endcode
 */
class SourceManager {
public:
    SourceManager();
    ~SourceManager();

    // Non-copyable
    SourceManager(const SourceManager&) = delete;
    SourceManager& operator=(const SourceManager&) = delete;

    // --- Source Creation ---

    /**
     * @brief Create a static image source from file
     * @param path Path to image file
     * @return UUID of created source, or null UUID on failure
     */
    UUID createImageSource(const std::filesystem::path& path);

    /**
     * @brief Find an existing image source already loaded from `path`
     * @return Its UUID, or null UUID if none
     */
    UUID findImageSourceByPath(const std::filesystem::path& path) const;

    /**
     * @brief Reuse the existing image source for `path`, else create one (ADR-040).
     *
     * Dedupe by path so the same file shared across many quads decodes/uploads
     * once — the basis for one image (and, in P3, one video) feeding many quads.
     */
    UUID getOrCreateImageSource(const std::filesystem::path& path);

    /**
     * @brief Find an existing video source already loaded from `path`
     * @return Its UUID, or null UUID if none
     */
    UUID findVideoSourceByPath(const std::filesystem::path& path) const;

    /**
     * @brief Reuse the existing video source for `path`, else create one (ADR-040).
     *
     * Dedupe by path so one video shared across many quads decodes once — the
     * basis for wrapping a single video around a box's faces.
     */
    UUID getOrCreateVideoSource(const std::filesystem::path& path);

    /**
     * @brief Create a video file source
     * @param path Path to video file
     * @return UUID of created source, or null UUID on failure
     */
    UUID createVideoSource(const std::filesystem::path& path);

    /**
     * @brief Create a live camera / capture-device source
     * @param deviceName dshow device name; empty = first available device
     * @return UUID of created source, or null UUID on failure
     */
    UUID createCameraSource(const std::string& deviceName = "");

    /**
     * @brief Create a procedural texture source
     * @param type Type of procedural texture
     * @param width Texture width (default 1920)
     * @param height Texture height (default 1080)
     * @return UUID of created source
     */
    UUID createProceduralSource(ProceduralType type, int width = 1920, int height = 1080);

    /**
     * @brief Register an externally-created source
     * @param source The source to register (takes ownership)
     * @return UUID of the registered source
     */
    UUID registerSource(std::unique_ptr<TextureSource> source);

    // --- Source Removal ---

    /**
     * @brief Remove a source and free its resources
     * @param id Source UUID
     */
    void removeSource(const UUID& id);

    /**
     * @brief Remove all sources
     */
    void clear();

    /**
     * @brief Release file-backed sources nothing uses any more (QM-088)
     * @param inUse Ids still referenced (quad bindings, the Video Player's source, ...)
     * @return Number of sources released
     *
     * Image, video (incl. HAP) and procedural sources not in inUse are shut down
     * (decode thread joined) and their GPU textures freed; callers put every source the
     * media bin still offers into inUse. Camera sources are never released here (the AI
     * capture path owns them). Call at a frame boundary, never while a render pass holds
     * texture pointers.
     */
    size_t releaseUnreferenced(const std::unordered_set<UUID>& inUse);

    // --- Source Access ---

    /**
     * @brief Get a source by ID
     * @param id Source UUID
     * @return Pointer to source, or nullptr if not found
     */
    TextureSource* getSource(const UUID& id);
    const TextureSource* getSource(const UUID& id) const;

    /**
     * @brief Get a source cast to a specific type
     * @tparam T The source type (must derive from TextureSource)
     * @param id Source UUID
     * @return Pointer to typed source, or nullptr if not found or wrong type
     */
    template<typename T>
    T* getSourceAs(const UUID& id) {
        return dynamic_cast<T*>(getSource(id));
    }

    /**
     * @brief Check if a source exists
     * @param id Source UUID
     */
    bool hasSource(const UUID& id) const;

    /**
     * @brief File a source was loaded from (image, video or HAP), else empty
     */
    std::filesystem::path getSourcePath(const UUID& id) const;

    /**
     * @brief Get all source IDs
     */
    std::vector<UUID> getAllSourceIds() const;

    /**
     * @brief Get count of active sources
     */
    size_t getSourceCount() const;

    // --- Texture Access ---

    /**
     * @brief Get the GPU texture for a source
     * @param id Source UUID
     * @return Pointer to texture, or nullptr if source not found
     *
     * Returns the GPU texture that can be used for rendering.
     * For GPU-native sources, this wraps the source's native texture.
     * For CPU sources, this is the uploaded texture managed by SourceManager.
     */
    Texture* getTexture(const UUID& id);

    /**
     * @brief Build a texture map for rendering
     * @param sourceIds List of source IDs to include
     * @return Map from source ID to texture pointer
     *
     * Convenience method for building the texture map required by Renderer.
     */
    std::unordered_map<UUID, Texture*> getTextureMap(const std::vector<UUID>& sourceIds);

    /**
     * @brief Get texture map for all sources
     */
    std::unordered_map<UUID, Texture*> getTextureMap();

    // --- Per-Frame Update ---

    /**
     * @brief Update all sources and upload new frames to GPU
     * @param deltaTime Time since last update in seconds
     *
     * Must be called each frame from the main thread.
     * This updates all sources and uploads any new frame data to GPU textures.
     */
    void updateAll(double deltaTime);

    /**
     * @brief Reset every playable source (videos, etc.) to time 0.
     * Drives the "restart all" / reset-playheads button (ADR-042). Video sources
     * carry their own audio, so this also restarts audio-from-video in sync.
     */
    void seekAllToStart();

    // --- Callbacks ---

    using SourceAddedCallback = std::function<void(const UUID& id)>;
    using SourceRemovedCallback = std::function<void(const UUID& id)>;

    void setSourceAddedCallback(SourceAddedCallback callback);
    void setSourceRemovedCallback(SourceRemovedCallback callback);

private:
    struct SourceEntry {
        std::unique_ptr<TextureSource> source;
        std::unique_ptr<Texture> texture;       // GPU texture (owned for CPU sources)
        bool needsTextureUpdate = true;         // True if frame data changed
        uint64_t lastFrameNumber = 0;           // For detecting new frames
        bool compressedUnsupportedLogged = false; // Rate-limit the BCn-unsupported skip log (ADR-044)
    };

    std::unordered_map<UUID, SourceEntry> sources_;

    SourceAddedCallback sourceAddedCallback_;
    SourceRemovedCallback sourceRemovedCallback_;

    // Internal helpers
    void createGPUTexture(SourceEntry& entry);
    void uploadFrameToTexture(SourceEntry& entry);
};

} // namespace quadmapper
