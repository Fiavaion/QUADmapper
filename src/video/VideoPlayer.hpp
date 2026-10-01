#pragma once

#include "VideoDecoder.hpp"
#include "VideoPlaybackState.hpp"
#include "PlaybackTransport.hpp"
#include <filesystem>
#include <memory>
#include <functional>
#include <string>

namespace quadmapper {

/**
 * @brief High-level video player with playback controls
 *
 * Combines VideoDecoder with a PlaybackTransport (ADR-050): the clock follows the
 * wall clock, Loop and Ping-Pong wrap analytically, and the decoder is repositioned
 * to follow the clock (never the clock restarted to follow the decoder).
 */
class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    // Non-copyable, movable
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;
    VideoPlayer(VideoPlayer&&) noexcept;
    VideoPlayer& operator=(VideoPlayer&&) noexcept;

    /**
     * @brief Load a video file
     * @param path Path to video file
     * @param options Hardware decoding and frame format (see DecodeOptions)
     * @return true if successful
     */
    bool load(const std::filesystem::path& path, const DecodeOptions& options = {});

    /**
     * @brief Load in two steps, the file opened elsewhere (TD-35)
     *
     * beginOpen() unloads and names the file; the transport (play, pause, seek, speed,
     * loop, lock) works from then on and its clock runs, while a worker opens a
     * VideoDecoder on the file. finishOpen() takes that decoder and starts decoding
     * wherever the clock is by then. False if no open was begun, or if the decoder is
     * not open: the player is then no longer opening and stays empty, but its transport
     * state (playing, loop, speed, lock) is kept, so a project saved now writes the clip
     * back as it was asked to play.
     */
    void beginOpen(const std::filesystem::path& path);
    bool finishOpen(std::unique_ptr<VideoDecoder> decoder);
    bool isOpening() const { return opening_; }

    /**
     * @brief Unload the current video
     */
    void unload();

    /**
     * @brief Check if a video is loaded
     */
    bool isLoaded() const;

    // --- Playback Controls ---

    /**
     * @brief Start or resume playback
     */
    void play();

    /**
     * @brief Pause playback
     */
    void pause();

    /**
     * @brief Stop playback and reset to beginning
     */
    void stop();

    /**
     * @brief Toggle between play and pause
     */
    void togglePlayPause();

    /**
     * @brief Seek to a specific time
     * @param timeSeconds Target time in seconds
     */
    void seek(double timeSeconds);

    /**
     * @brief Seek to a specific progress position
     * @param progress 0.0 = start, 1.0 = end
     */
    void seekProgress(float progress);

    /**
     * @brief Skip forward/backward by a number of seconds
     */
    void skip(double seconds);

    // --- Speed Control ---

    /**
     * @brief Set playback speed
     * @param speed Speed multiplier (0.1 - 4.0)
     */
    void setSpeed(float speed);

    /**
     * @brief Get current playback speed
     */
    float getSpeed() const;

    // --- Loop Control ---

    /**
     * @brief Set loop mode: Once, Loop or Ping-Pong
     *
     * Ping-Pong decodes backwards within the reverse cache; a file whose groups of
     * pictures do not fit plays as Loop, and getLoopModeNotice() says why.
     */
    void setLoopMode(LoopMode mode);

    /**
     * @brief Get the loop mode as requested
     */
    LoopMode getLoopMode() const;

    /**
     * @brief Why the requested loop mode is not what plays, or empty
     */
    const std::string& getLoopModeNotice() const;

    /**
     * @brief Memory the Ping-Pong reverse leg may use (default 512 MB)
     */
    void setReverseCacheBytes(size_t bytes);

    // --- Timeline lock (ADR-050 §3) ---

    /**
     * @brief Follow the master timeline (position = master time mapped onto this clip)
     */
    void setTimelineLocked(bool locked, const PlaybackClock& master);
    bool isTimelineLocked() const;

    /**
     * @brief Pick up master pause/resume/restart; call before update() while locked
     */
    void followTimeline(const PlaybackClock& master);

    // --- State ---

    /**
     * @brief Get current playback state
     */
    const VideoPlaybackState& getState() const;

    /**
     * @brief The playback clock; sample it at now()
     */
    const PlaybackClock& getClock() const;
    PlaybackTimePoint now() const;

    /**
     * @brief Replace the clock's time source (tests); default std::chrono::steady_clock
     */
    void setTimeSource(PlaybackTransport::TimeSource source);

    /**
     * @brief Get video info
     */
    const VideoInfo& getVideoInfo() const;

    /**
     * @brief Get file path
     */
    const std::filesystem::path& getPath() const;

    /**
     * @brief Decoder failure message, or empty while decoding is healthy
     */
    std::string getErrorMessage() const;

    /**
     * @brief Most decoded frame data the reverse cache has held, in bytes
     */
    size_t getReverseCachePeakBytes() const;

    /**
     * @brief Whether the clip is decoding on the GPU right now (ADR-055)
     */
    bool isHardwareDecoding() const;

    /**
     * @brief Why the clip decodes in software although hardware was asked for, or empty
     */
    std::string getDecodeNote() const;

    /**
     * @brief Whether decoded frames stay on the GPU for the zero-copy path (ADR-056)
     */
    bool isZeroCopy() const;

    /**
     * @brief Copy frames through memory from now on; see VideoDecoder::stopZeroCopy()
     */
    void stopZeroCopy(const std::string& reason);

    /**
     * @brief Why a clip that asked for zero-copy copies through memory, or empty
     */
    std::string getZeroCopyNote() const;

    // --- Frame Access ---

    /**
     * @brief Update the player (call each frame)
     * @param deltaTime Ignored: the playback clock reads the wall clock (ADR-050)
     * @return true if a new frame is ready
     */
    bool update(double deltaTime);

    /**
     * @brief Get the current frame for display
     * @return Current frame, or invalid frame if none ready
     */
    const VideoFrame& getCurrentFrame() const;

    /**
     * @brief The planes of the frame buffered after the current one, or an empty picture
     */
    YuvPicture peekNextPicture() const;

    /**
     * @brief Check if a new frame is ready
     */
    bool hasNewFrame() const;

    /**
     * @brief Mark the current frame as consumed
     */
    void consumeFrame();

    // --- Callbacks ---

    using EndCallback = std::function<void()>;

    /**
     * @brief Set callback for when video ends (in ONCE mode)
     */
    void setEndCallback(EndCallback callback);

private:
    // The decoder just opened (load or finishOpen): start playing it.
    void start();
    // Transport changes apply: a clip is loaded, or opening.
    bool takesTransport() const;
    // Restart decoding at the clock's position when a transport change asked for it.
    void applyTransport();
    void reposition();
    void handleEndOfClip(const ClipPosition& clip);
    // Once the decoder has seen the real end, correct a wrong container duration.
    void checkDuration();
    void refreshState();
    // Pop the buffered frames due at target (see FrameSelection.hpp); returns the count.
    size_t takeDueFrames(double target, bool haveFrame);

    std::unique_ptr<VideoDecoder> decoder_;
    PlaybackTransport transport_;
    VideoPlaybackState state_;
    VideoFrame currentFrame_;
    std::filesystem::path path_;
    size_t reverseCacheBytes_ = DEFAULT_REVERSE_CACHE_BYTES;

    bool opening_ = false;         // beginOpen() ran; finishOpen() has not
    bool hasNewFrame_ = false;
    bool refreshPending_ = false;  // a load/seek made the shown frame stale
    double lastTimeline_ = 0.0;    // clock timeline at the previous update
    bool durationChecked_ = false; // the decoder's observed end was compared once

    EndCallback endCallback_;
};

} // namespace quadmapper
