#pragma once

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

namespace quadmapper {

/**
 * @brief Decoded video frame data
 */
struct VideoFrame {
    std::vector<uint8_t> data;    // RGB24 pixel data
    int width = 0;
    int height = 0;
    double pts = 0.0;             // Presentation timestamp in seconds
    int64_t frameNumber = 0;

    bool isValid() const { return !data.empty() && width > 0 && height > 0; }
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
     * @return true if successful
     */
    bool open(const std::filesystem::path& path);

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
     * @brief Stop background decoding thread
     */
    void stopBackgroundDecoding();

    /**
     * @brief Get a frame from the background buffer
     * @return Next buffered frame, or invalid frame if buffer empty
     */
    VideoFrame getBufferedFrame();

    /**
     * @brief Check if there are buffered frames available
     */
    bool hasBufferedFrames() const;

    /**
     * @brief Get number of currently buffered frames
     */
    size_t getBufferedFrameCount() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quadmapper
