#pragma once

#include <vector>
#include <cstdint>

namespace quadmapper {

/**
 * @brief Decoded audio frame data
 *
 * Contains interleaved float samples from a decoded audio frame.
 * All audio is converted to float format for uniform processing.
 */
struct AudioFrame {
    std::vector<float> samples;  // Interleaved float samples [-1.0, 1.0]
    int channels = 0;            // Number of audio channels
    int sampleRate = 0;          // Sample rate in Hz
    int sampleCount = 0;         // Number of samples per channel
    double pts = 0.0;            // Presentation timestamp in seconds

    /**
     * @brief Check if this frame contains valid audio data
     */
    bool isValid() const {
        return !samples.empty() && channels > 0 && sampleRate > 0 && sampleCount > 0;
    }

    /**
     * @brief Get the duration of this frame in seconds
     */
    double duration() const {
        return sampleRate > 0 ? static_cast<double>(sampleCount) / sampleRate : 0.0;
    }

    /**
     * @brief Get the total number of interleaved samples
     */
    size_t totalSamples() const {
        return static_cast<size_t>(sampleCount) * channels;
    }

    /**
     * @brief Clear the frame data
     */
    void clear() {
        samples.clear();
        channels = 0;
        sampleRate = 0;
        sampleCount = 0;
        pts = 0.0;
    }
};

/**
 * @brief Audio stream metadata
 */
struct AudioInfo {
    int channels = 0;            // Number of audio channels
    int sampleRate = 0;          // Sample rate in Hz
    double duration = 0.0;       // Total duration in seconds
    std::string codecName;       // Codec name (e.g., "aac", "mp3")
    int64_t totalSamples = 0;    // Total samples per channel
    int bitRate = 0;             // Bit rate in bits per second

    bool isValid() const {
        return channels > 0 && sampleRate > 0;
    }
};

} // namespace quadmapper
