#pragma once

#include "core/UUID.hpp"
#include "AudioBuffer.hpp"
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace quadmapper {

/**
 * @brief Audio source with volume and mute controls
 *
 * Represents an audio source that can be mixed. Contains volume,
 * mute, and pan controls applied during mixing.
 */
class AudioSource {
public:
    /**
     * @brief Construct an audio source with a unique ID
     */
    AudioSource();

    /**
     * @brief Construct an audio source with a specific ID
     * @param sourceId Unique identifier for this source
     */
    explicit AudioSource(const UUID& sourceId);

    ~AudioSource();

    // Non-copyable, movable
    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;
    AudioSource(AudioSource&&) noexcept;
    AudioSource& operator=(AudioSource&&) noexcept;

    /**
     * @brief Get the unique source ID
     */
    const UUID& getId() const { return sourceId_; }

    /**
     * @brief Get/set the source name for display
     */
    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    // --- Volume Controls (QM-030) ---

    /**
     * @brief Get the current volume (0.0 to 1.0)
     */
    float getVolume() const { return volume_.load(); }

    /**
     * @brief Set the volume (0.0 to 1.0, clamped)
     */
    void setVolume(float volume);

    /**
     * @brief Get the volume in decibels (-inf to 0 dB)
     */
    float getVolumeDb() const;

    /**
     * @brief Set the volume from decibels (-60 dB to 0 dB)
     */
    void setVolumeDb(float db);

    /**
     * @brief Check if source is muted
     */
    bool isMuted() const { return muted_.load(); }

    /**
     * @brief Set mute state
     */
    void setMuted(bool muted) { muted_ = muted; }

    /**
     * @brief Toggle mute state
     * @return New mute state
     */
    bool toggleMute();

    /**
     * @brief Get the stereo pan (-1.0 left to 1.0 right)
     */
    float getPan() const { return pan_.load(); }

    /**
     * @brief Set the stereo pan (-1.0 to 1.0)
     */
    void setPan(float pan);

    // --- Source State ---

    /**
     * @brief Check if source is enabled (can produce audio)
     */
    bool isEnabled() const { return enabled_.load(); }

    /**
     * @brief Set enabled state
     */
    void setEnabled(bool enabled) { enabled_ = enabled; }

    /**
     * @brief Get the effective volume (volume * !muted * enabled)
     */
    float getEffectiveVolume() const;

    /**
     * @brief Get left channel gain (considers volume, mute, pan)
     */
    float getLeftGain() const;

    /**
     * @brief Get right channel gain (considers volume, mute, pan)
     */
    float getRightGain() const;

    // --- Audio Buffer ---

    /**
     * @brief Set the audio buffer for this source
     * @param buffer Shared pointer to audio buffer (source does not own)
     */
    void setBuffer(std::shared_ptr<AudioBuffer> buffer) { buffer_ = buffer; }

    /**
     * @brief Get the associated audio buffer
     */
    std::shared_ptr<AudioBuffer> getBuffer() const { return buffer_; }

    /**
     * @brief Read samples from the source's buffer, applying volume/mute
     * @param output Output buffer for stereo interleaved samples
     * @param frameCount Number of stereo frames to read
     * @param channels Number of channels in source (for deinterleaving)
     * @return Number of frames actually read
     */
    size_t readSamples(float* output, size_t frameCount, int channels);

    // Constants
    static constexpr float MIN_VOLUME = 0.0f;
    static constexpr float MAX_VOLUME = 1.0f;
    static constexpr float MIN_VOLUME_DB = -60.0f;
    static constexpr float MAX_VOLUME_DB = 0.0f;
    static constexpr float MIN_PAN = -1.0f;
    static constexpr float MAX_PAN = 1.0f;

private:
    UUID sourceId_;
    std::string name_;

    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<float> pan_{0.0f};

    std::shared_ptr<AudioBuffer> buffer_;

    // Pre-allocated scratch buffer for readSamples(); avoids heap allocation
    // on the RT audio callback. Grown (at most once per block size change) in
    // readSamples() before any audio data is touched, never inside the hot
    // per-frame loop. The audio thread is the sole writer and reader, so no
    // synchronisation is needed.
    mutable std::vector<float> readBuffer_;
};

} // namespace quadmapper
