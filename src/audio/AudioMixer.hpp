#pragma once

#include "AudioSource.hpp"
#include "AudioDevice.hpp"
#include "core/UUID.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace quadmapper {

/**
 * @brief Audio mixer callback function type
 *
 * Called when audio is mixed and ready to be analyzed (e.g., for level meters).
 * @param samples Interleaved stereo samples
 * @param frameCount Number of stereo frames
 * @param channels Number of channels (always 2 for stereo)
 */
using AudioMixerCallback = std::function<void(const float* samples, size_t frameCount, int channels)>;

/**
 * @brief Audio mixer with miniaudio backend
 *
 * Mixes multiple AudioSources together with master volume control.
 * Uses miniaudio for low-latency audio output.
 */
class AudioMixer {
public:
    AudioMixer();
    ~AudioMixer();

    // Non-copyable, movable
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioMixer(AudioMixer&&) noexcept;
    AudioMixer& operator=(AudioMixer&&) noexcept;

    /**
     * @brief Initialize the audio mixer
     * @param device Optional audio device to use (nullptr for default)
     * @return true if successful
     */
    bool initialize(AudioDevice* device = nullptr);

    /**
     * @brief Shutdown the audio mixer
     */
    void shutdown();

    /**
     * @brief Check if mixer is initialized and running
     */
    bool isInitialized() const;

    /**
     * @brief Start audio playback
     * @return true if successful
     */
    bool start();

    /**
     * @brief Stop audio playback
     */
    void stop();

    /**
     * @brief Check if mixer is currently playing
     */
    bool isPlaying() const;

    // --- Master Volume Control (QM-029) ---

    /**
     * @brief Get the master volume (0.0 to 1.0)
     */
    float getMasterVolume() const { return masterVolume_.load(); }

    /**
     * @brief Set the master volume (0.0 to 1.0)
     */
    void setMasterVolume(float volume);

    /**
     * @brief Get the master volume in decibels
     */
    float getMasterVolumeDb() const;

    /**
     * @brief Set the master volume from decibels
     */
    void setMasterVolumeDb(float db);

    /**
     * @brief Check if master is muted
     */
    bool isMasterMuted() const { return masterMuted_.load(); }

    /**
     * @brief Set master mute state
     */
    void setMasterMuted(bool muted) { masterMuted_ = muted; }

    /**
     * @brief Toggle master mute
     * @return New mute state
     */
    bool toggleMasterMute();

    // --- Source Management (QM-029) ---

    /**
     * @brief Add a source to the mixer
     * @param source Shared pointer to audio source
     */
    void addSource(std::shared_ptr<AudioSource> source);

    /**
     * @brief Remove a source from the mixer
     * @param sourceId ID of source to remove
     * @return true if source was found and removed
     */
    bool removeSource(const UUID& sourceId);

    /**
     * @brief Get a source by ID
     * @param sourceId ID of source to find
     * @return Shared pointer to source, or nullptr if not found
     */
    std::shared_ptr<AudioSource> getSource(const UUID& sourceId);

    /**
     * @brief Get all sources
     * @return Vector of source pointers
     */
    std::vector<std::shared_ptr<AudioSource>> getSources() const;

    /**
     * @brief Get the number of active sources
     */
    size_t getSourceCount() const;

    /**
     * @brief Remove all sources from the mixer
     */
    void clearSources();

    // --- Audio Parameters ---

    /**
     * @brief Get the sample rate
     */
    int getSampleRate() const;

    /**
     * @brief Get the channel count (always 2 for stereo output)
     */
    int getChannels() const;

    /**
     * @brief Get the buffer size in frames
     */
    uint32_t getBufferSizeFrames() const;

    // --- Callbacks ---

    /**
     * @brief Set a callback for mixed audio output
     *
     * This callback is called from the audio thread, so it must be
     * fast and thread-safe. Useful for level meters and visualization.
     */
    void setMixCallback(AudioMixerCallback callback);

    // Constants
    static constexpr float MIN_VOLUME = 0.0f;
    static constexpr float MAX_VOLUME = 1.0f;
    static constexpr float MIN_VOLUME_DB = -60.0f;
    static constexpr float MAX_VOLUME_DB = 0.0f;

private:
    std::unique_ptr<class AudioMixerImpl> impl_;

    std::atomic<float> masterVolume_{1.0f};
    std::atomic<bool> masterMuted_{false};
};

} // namespace quadmapper
