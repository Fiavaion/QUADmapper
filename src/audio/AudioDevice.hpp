#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace quadmapper {

/**
 * @brief Audio device information structure
 */
struct AudioDeviceInfo {
    std::string id;           // Unique device identifier
    std::string name;         // Human-readable device name
    bool isDefault = false;   // Is this the system default device?
    bool isPlayback = true;   // Is this a playback device?
    int maxChannels = 2;      // Maximum supported channels
    int minSampleRate = 44100;
    int maxSampleRate = 48000;

    bool isValid() const { return !id.empty() && !name.empty(); }
};

/**
 * @brief Audio output device management using miniaudio
 *
 * Enumerates available audio output devices and manages device selection.
 * Uses the pimpl pattern to isolate miniaudio headers.
 */
class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();

    // Non-copyable, movable
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;
    AudioDevice(AudioDevice&&) noexcept;
    AudioDevice& operator=(AudioDevice&&) noexcept;

    /**
     * @brief Initialize the audio device system
     * @param nullBackend Use miniaudio's null backend (a silent device that still runs
     *                    callbacks in real time), for tests and headless runs
     * @return true if successful
     */
    bool initialize(bool nullBackend = false);

    /**
     * @brief Shutdown the audio device system
     */
    void shutdown();

    /**
     * @brief Check if initialized
     */
    bool isInitialized() const;

    // --- Device Enumeration (QM-031) ---

    /**
     * @brief Enumerate available playback devices
     * @return Vector of device info structures
     */
    std::vector<AudioDeviceInfo> enumerateDevices();

    /**
     * @brief Refresh the device list
     * Call this to update the cached device list after system changes.
     */
    void refreshDevices();

    /**
     * @brief Get the default playback device
     * @return Device info for default device, or invalid info if none
     */
    AudioDeviceInfo getDefaultDevice() const;

    /**
     * @brief Get the currently selected device
     * @return Device info for selected device
     */
    AudioDeviceInfo getSelectedDevice() const;

    /**
     * @brief Select an output device by ID
     * @param deviceId Device identifier (empty for system default)
     * @return true if device was found and selected
     */
    bool selectDevice(const std::string& deviceId);

    /**
     * @brief Select the system default device
     * @return true if successful
     */
    bool selectDefaultDevice();

    /**
     * @brief ID of the playback device named `name`, or empty if none is present
     */
    std::string findDeviceIdByName(const std::string& name) const;

    // --- For AudioMixer: the miniaudio objects behind this enumeration ---

    /** The ma_context devices were enumerated with (nullptr before initialize()). */
    void* nativeContext();

    /** The ma_device_id of device `deviceId`, or nullptr for an empty or unknown id. */
    const void* nativeDeviceId(const std::string& deviceId) const;

    // --- Device Parameters ---

    /**
     * @brief Get the current sample rate
     */
    int getSampleRate() const;

    /**
     * @brief Set the desired sample rate
     * @param sampleRate Sample rate in Hz (e.g., 44100, 48000)
     * @return true if the rate is valid for the current device
     */
    bool setSampleRate(int sampleRate);

    /**
     * @brief Get the current channel count
     */
    int getChannels() const;

    /**
     * @brief Set the desired channel count
     * @param channels Number of channels (1=mono, 2=stereo)
     * @return true if the channel count is valid
     */
    bool setChannels(int channels);

    /**
     * @brief Get the buffer size in frames
     */
    uint32_t getBufferSizeFrames() const;

    /**
     * @brief Set the buffer size in frames
     * @param frames Buffer size (affects latency)
     * @return true if successful
     */
    bool setBufferSizeFrames(uint32_t frames);

    // Constants
    static constexpr int DEFAULT_SAMPLE_RATE = 48000;
    static constexpr int DEFAULT_CHANNELS = 2;
    static constexpr uint32_t DEFAULT_BUFFER_FRAMES = 512;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quadmapper
