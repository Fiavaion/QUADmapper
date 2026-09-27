#pragma once

#include "core/Types.hpp"
#include "core/YuvPicture.hpp"

#include <filesystem>
#include <memory>
#include <vector>
#include <cstdint>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>

namespace quadmapper {

/**
 * @brief Decoded video frame data
 */
struct VideoFrame {
    std::vector<uint8_t> data;    // RGB24 pixel data, when yuv is not set
    // The decoded planes, shared with the decoder (DecodeOptions::yuvOutput). The GPU
    // turns them into RGBA (ADR-055); copying the frame copies a handle, not pixels.
    YuvPicture yuv;
    int width = 0;
    int height = 0;
    double pts = 0.0;             // Presentation timestamp in seconds
    // Playback-clock timeline time at which this frame becomes current (ADR-050).
    // Equals pts outside startPlayback(); the queue is always in due order.
    double due = 0.0;
    int64_t frameNumber = 0;

    bool isValid() const { return width > 0 && height > 0 && (yuv.isValid() || !data.empty()); }
};

/**
 * @brief How a file is decoded (ADR-055)
 *
 * The defaults (software, RGB24) suit one-off decodes such as thumbnails; playback
 * turns both options on.
 */
struct DecodeOptions {
    // Decode H.264, HEVC, VP9 and AV1 on the GPU (D3D11VA, one device shared by the
    // app). A stream the GPU refuses, or that fails later, carries on in software.
    bool hardware = false;
    // Hand frames on as YUV planes (VideoFrame::yuv) for the GPU conversion pass
    // instead of converting them to RGB24 on the CPU. Pixel formats the pass does not
    // handle still arrive as RGB24.
    bool yuvOutput = false;
    // Leave hardware frames on the GPU (VideoFrame::yuv.gpu) for the zero-copy path
    // (ADR-056) instead of copying them to memory. Needs hardware and yuvOutput; set
    // only when the renderer can share D3D11 textures. VideoDecoder::stopZeroCopy()
    // turns it off for the rest of the stream.
    bool zeroCopy = false;
    // Test hook: treat the GPU as failed once it has produced this many frames
    // (0: already at open). Negative: never.
    int failHardwareAfterFrames = -1;
    // Test hooks for zero-copy (ADR-056): the GPU frame pool cannot make more than this
    // many textures (as if video memory ran out; negative: no limit), and the device
    // cannot share NV12/P010 textures.
    int failGpuTextureAfter = -1;
    bool denySharedFormat = false;
};

// Memory the backward GOP decoder may hold for Ping-Pong's reverse leg (ADR-050 §4).
constexpr size_t DEFAULT_REVERSE_CACHE_BYTES = size_t(512) << 20;

/**
 * @brief What background decoding produces for a clip played through a PlaybackClock
 *
 * Frames come out in timeline order for the clock's mapping, each stamped with
 * VideoFrame::due: Loop decodes the clip again from the start after each pass, and
 * Ping-Pong's reverse legs decode groups of pictures backwards (seek to the keyframe
 * before, decode forward into a bounded cache, emit newest first).
 */
struct DecodePlan {
    double timeline = 0.0;               // timeline instant the first frame must cover
    double duration = 0.0;               // the clock's clip duration (unknown: no wrapping)
    LoopMode loopMode = LoopMode::ONCE;
    size_t reverseCacheBytes = DEFAULT_REVERSE_CACHE_BYTES;
};

/**
 * @brief Video metadata
 */
struct VideoInfo {
    int width = 0;
    int height = 0;
    double duration = 0.0;        // Total duration in seconds
    double frameRate = 0.0;       // Frames per second
    int64_t totalFrames = 0;
    std::string codecName;
    bool hasAudio = false;
    int audioChannels = 0;
    int audioSampleRate = 0;
};

/**
 * @brief FFmpeg-based video decoder with background decoding thread
 */
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // Non-copyable, movable
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) noexcept;
    VideoDecoder& operator=(VideoDecoder&&) noexcept;

    /**
     * @brief Open a video file
     * @param path Path to video file
     * @param options Hardware decoding and output format
     * @return true if successful
     */
    bool open(const std::filesystem::path& path, const DecodeOptions& options = {});

    /**
     * @brief Open a live webcam / capture device (Windows dshow)
     * @param deviceName dshow device name, e.g. "HD User Facing"
     * @param width  Desired capture width  (0 = device default)
     * @param height Desired capture height (0 = device default)
     * @param fps    Desired capture frame rate (0 = device default)
     * @return true if successful
     *
     * Live sources have no duration and cannot seek. Requires libavdevice
     * (QUADMAPPER_HAS_AVDEVICE); returns false if unavailable. See QM-055.
     */
    bool openCamera(const std::string& deviceName, int width = 0, int height = 0, double fps = 0.0);

    /**
     * @brief Whether the open source is a live capture device (no seek/duration)
     */
    bool isLive() const;

    /**
     * @brief Close the video and release resources
     */
    void close();

    /**
     * @brief Check if a video is open
     */
    bool isOpen() const;

    /**
     * @brief Get video metadata
     */
    const VideoInfo& getInfo() const;

    /**
     * @brief Seek to a specific time
     * @param timeSeconds Target time in seconds
     * @return true if successful
     */
    bool seek(double timeSeconds);

    /**
     * @brief Seek to a specific frame
     * @param frameNumber Target frame number
     * @return true if successful
     */
    bool seekFrame(int64_t frameNumber);

    /**
     * @brief Decode the next frame
     * @return Decoded frame, or invalid frame if EOF/error
     */
    VideoFrame decodeNextFrame();

    /**
     * @brief Get current playback position in seconds
     */
    double getCurrentTime() const;

    /**
     * @brief Get current frame number
     */
    int64_t getCurrentFrame() const;

    /**
     * @brief Check if at end of file
     */
    bool isAtEnd() const;

    // --- Background decoding ---

    /**
     * @brief Start background decoding thread
     * @param bufferSize Number of frames to buffer ahead
     */
    void startBackgroundDecoding(size_t bufferSize = 8);

    /**
     * @brief Restart background decoding to follow a playback clock (ADR-050)
     * @param plan Timeline position, clip duration and loop mode to produce frames for
     * @param bufferSize Number of frames to buffer ahead
     *
     * Stops the thread, discards buffered frames and positions the decoder at the
     * frame the plan's timeline instant shows. Frames then carry VideoFrame::due.
     */
    void startPlayback(const DecodePlan& plan, size_t bufferSize = 8);

    /**
     * @brief Why Ping-Pong could not run backwards within cacheBytes, from the index
     * @return Empty when the file's keyframe spacing fits (or is unknown until played)
     */
    std::string checkReverse(size_t cacheBytes) const;

    /**
     * @brief Set when a reverse leg could not be decoded within the plan's cache
     *
     * Decoding then idles; the player falls back to Loop and restarts the plan.
     */
    std::string getReverseError() const;

    /**
     * @brief Largest amount of decoded frame data the reverse cache has held, in bytes
     */
    size_t getReverseCachePeakBytes() const;

    /**
     * @brief Where the video really ends (last frame's pts + one frame), 0 until known
     *
     * Known once a startPlayback() forward pass reaches the end of the stream. Players
     * use it to correct a container duration that is missing or wrong (Matroska files
     * whose timestamps start late declare their end time as the duration).
     */
    double getObservedEnd() const;

    /**
     * @brief Stop background decoding thread
     */
    void stopBackgroundDecoding();

    /**
     * @brief Get a frame from the background buffer
     * @return Next buffered frame, or invalid frame if buffer empty
     */
    VideoFrame getBufferedFrame();

    /**
     * @brief Peek when the next buffered frame becomes current
     * @param due Receives the front frame's VideoFrame::due
     * @return false if the buffer is empty
     */
    bool peekBufferedDue(double& due) const;

    /**
     * @brief The planes of the next buffered frame (a shared handle), or an empty picture
     *
     * Lets the upload start copying the frame expected next (ADR-055).
     */
    YuvPicture peekBufferedPicture() const;

    /**
     * @brief Check if there are buffered frames available
     */
    bool hasBufferedFrames() const;

    /**
     * @brief Get number of currently buffered frames
     */
    size_t getBufferedFrameCount() const;

    /**
     * @brief Why background decoding is failing, or empty while it is healthy
     *
     * Set after DecodeBackoff::ERROR_THRESHOLD consecutive non-EOF failures (the
     * thread then backs off instead of spinning); cleared by the next good frame.
     */
    std::string getLastError() const;

    // --- Hardware decoding (ADR-055) ---

    /**
     * @brief Whether frames currently come from the GPU decoder
     */
    bool isHardwareDecoding() const;

    /**
     * @brief Why this stream decodes in software although hardware was asked for
     * @return Empty while hardware decoding runs, or when it was not requested
     */
    std::string getDecodeNote() const;

    /**
     * @brief How many D3D11 decode devices the app has created (0 or 1)
     *
     * One device, created with the first hardware decoder, serves every decoder until
     * the app exits (ADR-055).
     */
    static int hardwareDevicesCreated();

    // --- Zero-copy frames (ADR-056) ---

    /**
     * @brief Whether hardware frames are being left on the GPU
     *
     * False once zero-copy stopped: the renderer refused a frame, the device cannot share
     * the stream's texture format, or its GPU frames could not be allocated.
     */
    bool isZeroCopy() const;

    /**
     * @brief Copy frames to memory from now on (the renderer could not use them)
     *
     * Thread-safe. The background decode thread then releases the stream's GPU frame
     * pool and copies the frames it had queued on the GPU to memory, off the caller's
     * thread. A frame taken from the queue before that may still be on the GPU;
     * downloadPicture() brings it to memory.
     */
    void stopZeroCopy(const std::string& reason);

    /**
     * @brief Why a stream that asked for zero-copy copies through memory, or empty
     */
    std::string getZeroCopyNote() const;

    /**
     * @brief A picture left on the GPU, copied to memory (P010 or NV12 planes)
     *
     * Waits for the GPU. For the few frames already decoded when a stream falls back.
     * Returns `picture` unchanged if it is not on the GPU, an invalid picture on failure.
     */
    static YuvPicture downloadPicture(const YuvPicture& picture);

    /**
     * @brief D3D11 textures holding frames left on the GPU, across every decoder
     *
     * For leak checks: back to its earlier value once the frames and decoders are gone.
     */
    static long liveGpuFrames();

    /**
     * @brief Decoders' other D3D11 objects: readback staging textures and GPU copy
     *        event queries, across every decoder
     *
     * For leak checks: back to its earlier value once the decoders are gone. The shared
     * decode device (ADR-055) is not counted; it lives until the app exits.
     */
    static long liveD3dObjects();

    /**
     * @brief GPU-to-memory frame copies the calling thread has made (they wait on the GPU)
     *
     * For tests that the main thread never reads frames back (ADR-056 fallback).
     */
    static long gpuReadbacksOnThisThread();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quadmapper
