#pragma once

#include "core/UUID.hpp"
#include "core/Types.hpp"
#include "FrameData.hpp"
#include "video/PlaybackClock.hpp"
#include <functional>
#include <string>
#include <cstdint>

namespace quadmapper {

/**
 * @brief Capabilities flags for a texture source
 *
 * Used by UI and SourceManager to determine available features.
 */
struct SourceCapabilities {
    bool isStatic = false;          // Never changes after load (images)
    bool hasPlayback = false;       // Supports play/pause/seek (video)
    bool hasParameters = false;     // Adjustable parameters (procedural)
    bool supportsAsync = true;      // Can decode on background thread
    bool isGPUNative = false;       // Renders directly to GPU texture
};

/**
 * @brief Abstract base class for all texture sources
 *
 * TextureSource provides a unified interface for all types of content
 * that can be applied as textures to quads:
 * - Static images (PNG, JPG, etc.)
 * - Video files (via FFmpeg)
 * - Live sources (webcam, NDI, capture cards)
 * - Procedural textures (GPU-generated patterns)
 * - Shared textures (Spout, Syphon)
 *
 * The lifecycle is:
 * 1. Create source via factory/constructor
 * 2. Call initialize() to load/prepare
 * 3. Call update() each frame from main thread
 * 4. Get frame data via getCurrentFrame() or getGLTextureId()
 * 5. Call shutdown() when done
 *
 * Threading model:
 * - update() is always called from the main thread
 * - Background threads may produce frames internally
 * - All OpenGL operations happen on main thread
 */
class TextureSource {
public:
    virtual ~TextureSource() = default;

    // --- Identity ---

    /**
     * @brief Get unique identifier for this source
     */
    virtual const UUID& getId() const = 0;

    /**
     * @brief Get display name for UI
     */
    virtual std::string getName() const = 0;

    /**
     * @brief Get the source type
     */
    virtual SourceType getType() const = 0;

    /**
     * @brief Get human-readable type name (e.g., "Video", "Noise")
     */
    virtual std::string getTypeName() const = 0;

    // --- Lifecycle ---

    /**
     * @brief Initialize the source (load file, start capture, etc.)
     * @return true if initialization succeeded
     *
     * Must be called before update(). For GPU-native sources,
     * this creates the output texture.
     */
    virtual bool initialize() = 0;

    /**
     * @brief Shutdown and release all resources
     *
     * After shutdown, the source cannot be used again.
     */
    virtual void shutdown() = 0;

    /**
     * @brief Check if the source is ready to provide frames
     */
    virtual bool isReady() const = 0;

    // --- Per-Frame Update ---

    /**
     * @brief Update the source (called each frame from main thread)
     * @param deltaTime Time since last update in seconds
     * @return true if a new frame is available
     *
     * For video: advances playback, checks for new decoded frames
     * For procedural: regenerates if parameters changed or animated
     * For static: returns false after first frame
     */
    virtual bool update(double deltaTime) = 0;

    // --- Frame Access (CPU-based sources) ---

    /**
     * @brief Check if a new frame is available for upload
     */
    virtual bool hasNewFrame() const = 0;

    /**
     * @brief Get current frame data for GPU upload
     * @return Pointer to frame data, or nullptr if none available
     *
     * Only valid for CPU-based sources (isGPUNative() == false).
     * The returned pointer is valid until the next update() call.
     */
    virtual const FrameData* getCurrentFrame() const = 0;

    /**
     * @brief Mark the current frame as consumed after GPU upload
     */
    virtual void markFrameConsumed() = 0;

    // --- GPU Texture Access (GPU-native sources) ---

    /**
     * @brief Get OpenGL texture ID for GPU-native sources
     * @return OpenGL texture ID, or 0 if not GPU-native
     *
     * For compute shader procedurals that render directly to texture.
     */
    virtual uint32_t getGLTextureId() const { return 0; }

    /**
     * @brief Check if this source renders directly to GPU texture
     *
     * GPU-native sources don't use getCurrentFrame(); they provide
     * a texture ID directly via getGLTextureId().
     */
    virtual bool isGPUNative() const { return false; }

    // --- Capabilities ---

    /**
     * @brief Get source capabilities for UI/optimization
     */
    virtual SourceCapabilities getCapabilities() const = 0;

    // --- Dimensions ---

    /**
     * @brief Get texture width in pixels
     */
    virtual int getWidth() const = 0;

    /**
     * @brief Get texture height in pixels
     */
    virtual int getHeight() const = 0;

    // Non-copyable
    TextureSource(const TextureSource&) = delete;
    TextureSource& operator=(const TextureSource&) = delete;

protected:
    TextureSource() = default;
};

/**
 * @brief Interface for playable sources (video, streams)
 *
 * Sources that support playback controls implement this interface
 * in addition to TextureSource.
 */
class PlayableSource : public TextureSource {
public:
    // --- Playback Controls ---

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void togglePlayPause() = 0;

    // --- Seeking ---

    virtual bool canSeek() const = 0;
    virtual void seek(double timeSeconds) = 0;
    virtual void seekProgress(float progress) = 0;  // 0.0 to 1.0

    // --- State ---

    virtual PlaybackState getPlaybackState() const = 0;
    virtual double getCurrentTime() const = 0;
    virtual double getDuration() const = 0;
    virtual float getProgress() const = 0;

    // --- Speed ---

    virtual void setSpeed(float speed) = 0;
    virtual float getSpeed() const = 0;

    // --- Loop ---

    // ONCE, LOOP or PING_PONG (ADR-050). getLoopMode() returns what was asked for; a
    // file that cannot play backwards within the reverse cache plays PING_PONG as LOOP
    // and getLoopModeNotice() says why (empty otherwise).
    virtual void setLoopMode(LoopMode mode) = 0;
    virtual LoopMode getLoopMode() const = 0;
    virtual std::string getLoopModeNotice() const = 0;

    // --- Timeline lock (ADR-050 §3) ---

    // A locked source's position is the master timeline mapped onto its clip (modulo
    // its duration, reflected for Ping-Pong); its own seek, stop and speed are ignored.
    virtual void setTimelineLocked(bool locked, const PlaybackClock& master) = 0;
    virtual bool isTimelineLocked() const = 0;
    // Pick up the master's pause, resume or restart; SourceManager calls it every
    // update for locked sources.
    virtual void followTimeline(const PlaybackClock& master) = 0;

    // --- Clock ---

    // The source's playback clock, sampled at getClockNow() (std::chrono::steady_clock
    // unless a test injected time). Position, direction, running state, speed and the
    // discontinuity count (seeks, wraps, reflections) for anything slaved to playback.
    virtual const PlaybackClock& getClock() const = 0;
    virtual PlaybackTimePoint getClockNow() const = 0;

    // --- Offline rendering (video export, owner 2026-11-23) ---

    // Drive the source's clock with `source` instead of the steady clock.
    virtual void setTimeSource(std::function<PlaybackTimePoint()> source) { (void)source; }
    // True once the frame for the clock's current time has been taken by update(), so
    // the texture shows exactly that frame: the next decoded frame is due later, the
    // clip has no more frames, or the source holds a frame (paused). False while it
    // opens or while the frame is still being decoded. Export calls update() until it
    // is true before each frame it renders.
    virtual bool framesCoverClock() const { return true; }

    // --- Health ---

    // Why playback is failing (decode errors), or empty while it is healthy.
    virtual std::string getErrorMessage() const { return {}; }

    // --- Audio (ADR-050 §2) ---

    // Volume (0-1) and mute of the source's sound. Plain values the app's audio engine
    // applies every frame; saved with the transport. A source without audio keeps them.
    void setAudioVolume(float volume) {
        audioVolume_ = (volume >= 0.0f) ? (volume <= 1.0f ? volume : 1.0f) : 0.0f;  // NaN -> 0
    }
    float getAudioVolume() const { return audioVolume_; }
    void setAudioMuted(bool muted) { audioMuted_ = muted; }
    bool isAudioMuted() const { return audioMuted_; }

private:
    float audioVolume_ = 1.0f;
    bool audioMuted_ = false;
};

} // namespace quadmapper
