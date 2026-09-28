#pragma once

#include "AudioSource.hpp"
#include "AudioDevice.hpp"
#include "core/UUID.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace quadmapper {

/**
 * @brief Tap on the mixed output (level meters, tests)
 *
 * Runs on the real-time audio thread after the mix: it must not lock, allocate or block.
 * @param samples    Interleaved stereo samples
 * @param frameCount Number of stereo frames
 * @param wallNs     std::chrono::steady_clock time of the callback, in ns
 */
using AudioMixerCallback = std::function<void(const float* samples, size_t frameCount, int64_t wallNs)>;

/**
 * @brief Mixes AudioSources to one playback device (miniaudio), with master volume
 *
 * Output is always 48 kHz float stereo; miniaudio converts to the device's own format.
 *
 * Threading: the control thread adds and removes sources and sets volumes; the device
 * callback mixes. The callback reads a published array of raw source pointers, so it
 * takes no lock, touches no reference count and frees nothing. removeSource() and
 * clearSources() publish the new array and then wait until the callback is not inside
 * a mix that could still hold the old one (one callback at most), so once they return
 * the callback can no longer reach the removed source and its owner may destroy it.
 */
class AudioMixer {
public:
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int CHANNELS = 2;

    AudioMixer();
    ~AudioMixer();

    // The device callback holds a pointer to this mixer: neither copyable nor movable.
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;

    /**
     * @brief Open the playback device (not yet started)
     * @param device   Enumeration to open from (nullptr: miniaudio's default context)
     * @param deviceId One of device->enumerateDevices() ids; empty = the system default,
     *                 followed when Windows changes it
     * @return false if the device could not be opened (the mixer then stays closed)
     */
    bool initialize(AudioDevice* device = nullptr, const std::string& deviceId = {});

    /** Stop and close the device. Sources stay registered. */
    void shutdown();

    bool isInitialized() const;
    bool start();
    void stop();
    bool isPlaying() const;

    /** True once the device stopped without stop() being called (unplugged, driver reset). */
    bool isDeviceLost() const;

    /** Name of the open device, or empty. */
    std::string getDeviceName() const;

    /** Estimated seconds from a callback until its first frame is heard (0 when closed). */
    double getOutputLatency() const;

    // --- Master volume (any thread) ---

    float getMasterVolume() const { return masterVolume_.load(); }
    void setMasterVolume(float volume);
    float getMasterVolumeDb() const;
    void setMasterVolumeDb(float db);
    bool isMasterMuted() const { return masterMuted_.load(); }
    void setMasterMuted(bool muted) { masterMuted_ = muted; }
    bool toggleMasterMute();

    // --- Sources (control thread) ---

    void addSource(std::shared_ptr<AudioSource> source);
    /** Returns once the callback can no longer reach the source; true if it was registered. */
    bool removeSource(const UUID& sourceId);
    std::shared_ptr<AudioSource> getSource(const UUID& sourceId);
    std::vector<std::shared_ptr<AudioSource>> getSources() const;
    size_t getSourceCount() const;
    void clearSources();

    int getSampleRate() const { return SAMPLE_RATE; }
    int getChannels() const { return CHANNELS; }
    uint32_t getBufferSizeFrames() const;

    /** Set (or clear, with an empty function) the output tap. */
    void setMixCallback(AudioMixerCallback callback);

    /**
     * @brief Mix one buffer on the calling thread, without a device
     *
     * For offline tests and benchmarks; never while a device is playing.
     */
    void renderOffline(float* output, size_t frameCount, int64_t wallNs);

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
