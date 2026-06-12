#pragma once

#include <filesystem>
#include <memory>
#include <atomic>
#include <thread>

namespace quadmapper {

// Forward declarations
struct AudioFrame;
struct AudioInfo;
class AudioBuffer;

/**
 * @brief FFmpeg-based audio decoder with background decoding thread
 *
 * Decodes audio streams from video files using FFmpeg.
 * Uses the pimpl pattern to isolate FFmpeg headers from the public interface.
 */
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    // Non-copyable, movable
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    AudioDecoder(AudioDecoder&&) noexcept;
    AudioDecoder& operator=(AudioDecoder&&) noexcept;

    /**
     * @brief Open an audio stream from a video/audio file
     * @param path Path to media file
     * @return true if audio stream found and opened successfully
     */
    bool open(const std::filesystem::path& path);

    /**
     * @brief Close the decoder and release resources
     */
    void close();

    /**
     * @brief Check if a file is open
     */
    bool isOpen() const;

    /**
     * @brief Get audio stream metadata
     */
    const AudioInfo& getInfo() const;

    /**
     * @brief Seek to a specific time
     * @param timeSeconds Target time in seconds
     * @return true if successful
     */
    bool seek(double timeSeconds);

    /**
     * @brief Decode the next audio frame
     * @return Decoded frame, or invalid frame if EOF/error
     *
     * The returned frame contains interleaved float samples.
     */
    AudioFrame decodeNextFrame();

    /**
     * @brief Get current playback position in seconds
     */
    double getCurrentTime() const;

    /**
     * @brief Check if at end of stream
     */
    bool isAtEnd() const;

    // --- Background decoding ---

    /**
     * @brief Start background decoding thread
     * @param buffer Audio buffer to push decoded samples into
     *
     * The decoder will continuously decode frames and push samples
     * to the provided buffer until stopped or end of stream.
     */
    void startBackgroundDecoding(AudioBuffer* buffer);

    /**
     * @brief Stop background decoding thread
     */
    void stopBackgroundDecoding();

    /**
     * @brief Check if background decoding is running
     */
    bool isBackgroundDecodingActive() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quadmapper
