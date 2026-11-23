#pragma once

#include "TextureSource.hpp"
#include "core/UUID.hpp"
#include "core/Types.hpp"
#include <chrono>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <functional>

namespace quadmapper {

// Forward declarations
class Texture;
class YuvConverter;
class YuvUploadState;
class CompressedUploader;
class CompressedUploadState;
class DxInterop;
class DxInteropTarget;
class VideoDecoder;
class BackgroundWorker;

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
     * @return UUID of created source, or null UUID when the file does not exist, is not
     *         a container FFmpeg can read or holds no video (a cheap header probe)
     *
     * The file is opened off the main thread (TD-35): the source is registered at once,
     * not ready, and its transport (play, seek, loop, ...) already works; updateAll()
     * makes it ready once its decoder (or, for a HAP clip, its container) is open, or,
     * when its video cannot be decoded after all, leaves it empty with getErrorMessage()
     * set, its transport kept, and lists it in takeFailedOpens(). The header probe that
     * refuses a file and routes HAP clips stays here: callers rely on the refusal at
     * once (Missing Media relocation), and it decides the source's type.
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
     *
     * Its GL objects are freed by later updateAll() calls, a piece a frame, once the GPU
     * has finished the frames that used them; a video's decode threads are joined and its
     * FFmpeg and D3D11 objects freed on a background thread (TD-35). A source still
     * opening is dropped when its open finishes.
     */
    void removeSource(const UUID& id);

    /**
     * @brief Remove all sources (freed as removeSource() frees them), and release the
     *        shared zero-copy planes kept for later sources (a new or replaced project)
     */
    void clear();

    /**
     * @brief Wait for the background work: every source still opening is opened and
     *        made ready (or failed), and every removed source is freed, the shared
     *        zero-copy planes kept for later sources too (DxInterop::recycle)
     *
     * For tests, the harnesses' leak checks and the like; it blocks, so never on the
     * live path. Main thread.
     */
    void finishBackgroundWork();

    /**
     * @brief Do the GL set-up of video at start-up, not with the first clip (TD-35)
     *
     * Builds the YUV conversion shader and checks whether the GL context can share D3D11
     * textures, which otherwise ran inside the first video frame and the first
     * createVideoSource() (up to 60 ms each under load). No D3D11 work: a show without
     * video never creates a decode or interop device. Main thread, GL context current.
     */
    void prepareGpu();

    /**
     * @brief Sources still opening off the main thread
     */
    size_t getOpeningCount() const { return opening_.size(); }

    /**
     * @brief Removed sources whose GL state updateAll() has not freed yet (TD-35)
     */
    size_t getRetiredGlCount() const { return retiredGl_.size(); }

    /**
     * @brief Video sources whose open off the main thread failed since the last call
     *
     * Each is still registered, empty and not ready, with getErrorMessage() set and the
     * transport it was given kept; the caller decides what becomes of it (the editor
     * turns it into missing media). Main thread.
     */
    std::vector<UUID> takeFailedOpens();

    /**
     * @brief Test hook: run on the opener thread before each video file is opened
     *        (sources created from now on); it may block or throw
     */
    void setOpenHookForTesting(std::function<void(const std::filesystem::path&)> hook);

    /**
     * @brief Test hook for the fence removed GL state waits behind (TD-35)
     * @param neverSignals Every fence reads as not signalled, as after a GPU reset
     * @param limit How long one may stay unsignalled before it is taken as signalled
     */
    void setFenceHooksForTesting(bool neverSignals, std::chrono::milliseconds limit = FENCE_LIMIT);
    // A fence still unsignalled after this (a TDR or a lost device never signals it) is
    // taken as signalled, so removed GL state is still freed.
    static constexpr std::chrono::milliseconds FENCE_LIMIT{10000};

    /**
     * @brief Show `standIn`'s texture for `id` until `id` has a frame of its own
     *
     * A clip replacing another opens off the main thread (TD-35); meanwhile its
     * surfaces keep showing the clip it replaces instead of going blank.
     */
    void setStandIn(const UUID& id, const UUID& standIn);

    /**
     * @brief Release file-backed sources nothing uses any more (QM-088)
     * @param inUse Ids still referenced (quad bindings, the Video Player's source, ...)
     * @return Number of sources released
     *
     * Image, video (incl. HAP) and procedural sources not in inUse are removed as
     * removeSource() removes them (a video's decode threads joined off the main
     * thread), as are camera sources
     * (the AI capture path opens its own CameraSource, not one managed here); callers
     * put every source the media bin still offers into inUse. Call at a frame
     * boundary, never while a render pass holds texture pointers.
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
     * @brief Whether the source's current frame (getCurrentFrame()) is on its texture
     *
     * False before its first frame, and while a frame waits for its zero-copy set-up:
     * a stream's first frame on the GPU reaches the texture a few updates after the
     * source holds it (DxInterop::setUp, TD-35).
     */
    bool showsCurrentFrame(const UUID& id) const;

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
     * Video frames arrive as YUV planes and are converted into the source's RGBA
     * texture by a GPU pass (ADR-055), which restores the GL state it changes. Hardware
     * frames left on the GPU are converted there and shared with OpenGL (ADR-056); a
     * source that cannot share them falls back to the planes. It keeps its last frame
     * until its decoder delivers frames in memory: nothing is read back from the GPU on
     * this thread.
     */
    void updateAll(double deltaTime);

    /**
     * @brief Main-thread time updateAll() has spent uploading frames, since construction
     *
     * Wall time around each source's GPU upload (and, for video, its conversion pass),
     * for the --bench video scenario (ADR-055).
     */
    struct UploadStats {
        double seconds = 0.0;
        uint64_t uploads = 0;
    };
    const UploadStats& getUploadStats() const { return uploadStats_; }

    /**
     * @brief Reset every playable source (videos, etc.) to time 0.
     * Drives the "restart all" / reset-playheads button (ADR-042). Video sources
     * carry their own audio, so this also restarts audio-from-video in sync.
     * Restarts the master timeline too, so timeline-locked sources start over.
     */
    void seekAllToStart();

    // --- Master timeline (ADR-050 §3) ---

    /**
     * @brief The project's master timeline, which timeline-locked sources follow
     *
     * Running from construction. updateAll() passes it to every locked source before
     * updating it, so a locked source's position is the master time mapped onto its clip.
     */
    const PlaybackClock& getMasterTimeline() const { return masterTimeline_; }

    // Offline rendering (video export): the master timeline and every playable source,
    // present and future, read `source` instead of the steady clock. An export runs
    // its own SourceManager with this set, so the live show's clocks are untouched.
    void setTimeSource(std::function<PlaybackTimePoint()> source);
    PlaybackTimePoint clockNow() const;
    // True when every ready playable source has taken the frame for its clock's
    // current time (PlayableSource::framesCoverClock).
    bool framesCoverClock() const;
    void playMasterTimeline();
    void pauseMasterTimeline();
    void restartMasterTimeline();  // back to 0, keeping play/pause

    /**
     * @brief Lock or unlock a playable source to the master timeline
     */
    void setTimelineLocked(const UUID& id, bool locked);

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
        std::unique_ptr<YuvUploadState> yuvUpload; // plane textures + PBOs of a YUV video (ADR-055)
        std::unique_ptr<CompressedUploadState> compressedUpload; // PBOs of a HAP video (ADR-044)
        std::unique_ptr<DxInteropTarget> zeroCopy; // D3D11 texture shared with GL (ADR-056)
        bool zeroCopyFailed = false;               // this source fell back; its zeroCopy
                                                   // target was retired (retireGl)
        bool hasFrame = false;                     // a frame reached the texture
        bool uploadDeferred = false;               // the current frame waits for zero-copy
                                                   // set-up (DxInterop::setUp): offered again
        UUID standIn;                              // shown until then (setStandIn)
    };
    struct PendingOpen;  // a video or HAP file being opened on the opener thread
    // GL state of a removed source (or the shared planes of one that fell back), freed
    // by releaseRetiredGl() without waiting on this thread.
    struct GlFenceDeleter {
        void operator()(void* fence) const;  // glDeleteSync
    };
    struct RetiredGl {
        std::unique_ptr<Texture> texture;
        std::unique_ptr<YuvUploadState> yuvUpload;
        std::unique_ptr<CompressedUploadState> compressedUpload;
        std::unique_ptr<DxInteropTarget> zeroCopy;
        std::unique_ptr<void, GlFenceDeleter> fence;  // signalled once the GPU is done with it all
        std::chrono::steady_clock::time_point retiredAt;
    };

    std::unordered_map<UUID, SourceEntry> sources_;
    PlaybackClock masterTimeline_;
    std::function<PlaybackTimePoint()> timeSource_;  // empty: the steady clock
    UploadStats uploadStats_;
    std::unique_ptr<YuvConverter> yuvConverter_;  // YUV -> RGBA pass for video frames
    std::unique_ptr<CompressedUploader> compressedUploader_;  // BCn uploads of HAP frames
    std::unique_ptr<DxInterop> dxInterop_;        // zero-copy hardware frames (ADR-056)
    // TD-35: video files open on opener_ and removed videos are freed on reaper_, so
    // neither blocks the main thread. Both started on first use, joined by the destructor.
    std::vector<std::shared_ptr<PendingOpen>> opening_;
    std::vector<UUID> failedOpens_;     // see takeFailedOpens()
    std::function<void(const std::filesystem::path&)> openHookForTesting_;
    std::vector<RetiredGl> retiredGl_;  // freed a piece a frame, oldest first
    std::chrono::milliseconds fenceLimit_ = FENCE_LIMIT;  // setFenceHooksForTesting
    bool fenceNeverSignalsForTesting_ = false;
    bool fenceTimeoutLogged_ = false;
    std::unique_ptr<BackgroundWorker> opener_;
    std::unique_ptr<BackgroundWorker> reaper_;

    SourceAddedCallback sourceAddedCallback_;
    SourceRemovedCallback sourceRemovedCallback_;

    // Internal helpers
    UUID addEntry(std::unique_ptr<TextureSource> source);  // an initialized (or opening) source
    void adoptOpenedSources();
    void retire(SourceEntry& entry);
    void retireGl(RetiredGl retired);
    void releaseRetiredGl();
    void retireOpened(PendingOpen& pending);
    BackgroundWorker& reaper();
    void createGPUTexture(SourceEntry& entry);
    void uploadFrameToTexture(SourceEntry& entry);
    void uploadYuvFrame(SourceEntry& entry, const FrameData& frame);
    void fallBackFromZeroCopy(SourceEntry& entry, const std::string& why);
};

} // namespace quadmapper
