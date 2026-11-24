#include "AudioDevice.hpp"
#include "core/Logger.hpp"

// miniaudio implementation - define before including
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <cstring>

namespace quadmapper {

class AudioDevice::Impl {
public:
    Impl() = default;

    ~Impl() {
        shutdown();
    }

    bool initialize(bool nullBackend) {
        if (initialized_) {
            return true;
        }

        ma_context_config contextConfig = ma_context_config_init();
        const ma_backend nullOnly[] = {ma_backend_null};
        ma_result result = nullBackend
            ? ma_context_init(nullOnly, 1, &contextConfig, &context_)
            : ma_context_init(nullptr, 0, &contextConfig, &context_);

        if (result != MA_SUCCESS) {
            LOG_ERROR("Failed to initialize audio context: {}", static_cast<int>(result));
            return false;
        }

        initialized_ = true;
        refreshDevices();

        LOG_INFO("Audio device system initialized");
        return true;
    }

    void shutdown() {
        if (!initialized_) {
            return;
        }

        ma_context_uninit(&context_);
        devices_.clear();
        selectedDeviceId_.clear();
        initialized_ = false;

        LOG_INFO("Audio device system shutdown");
    }

    bool isInitialized() const {
        return initialized_;
    }

    std::vector<AudioDeviceInfo> enumerateDevices() {
        if (!initialized_) {
            return {};
        }

        return devices_;
    }

    void refreshDevices() {
        if (!initialized_) {
            return;
        }

        devices_.clear();
        deviceIds_.clear();

        ma_device_info* playbackDevices;
        ma_uint32 playbackCount;
        ma_device_info* captureDevices;
        ma_uint32 captureCount;

        ma_result result = ma_context_get_devices(
            &context_,
            &playbackDevices, &playbackCount,
            &captureDevices, &captureCount
        );

        if (result != MA_SUCCESS) {
            LOG_ERROR("Failed to enumerate audio devices: {}", static_cast<int>(result));
            return;
        }

        // Process playback devices
        for (ma_uint32 i = 0; i < playbackCount; ++i) {
            AudioDeviceInfo info;
            info.id = std::to_string(i);  // Use index as ID

            // Copy device name
            info.name = playbackDevices[i].name;
            info.isDefault = playbackDevices[i].isDefault != 0;
            info.isPlayback = true;

            deviceIds_.push_back(playbackDevices[i].id);

            // Default capabilities
            info.maxChannels = 2;  // Assume stereo minimum
            info.minSampleRate = 44100;
            info.maxSampleRate = 192000;

            devices_.push_back(info);

            if (info.isDefault && selectedDeviceId_.empty()) {
                selectedDeviceId_ = info.id;
            }

            LOG_DEBUG("Found audio device: {} ({})", info.name, info.isDefault ? "default" : "");
        }

        LOG_INFO("Enumerated {} audio output devices", devices_.size());
    }

    AudioDeviceInfo getDefaultDevice() const {
        for (const auto& device : devices_) {
            if (device.isDefault) {
                return device;
            }
        }

        // Return first device if no default
        if (!devices_.empty()) {
            return devices_[0];
        }

        return AudioDeviceInfo{};
    }

    AudioDeviceInfo getSelectedDevice() const {
        for (const auto& device : devices_) {
            if (device.id == selectedDeviceId_) {
                return device;
            }
        }
        return getDefaultDevice();
    }

    bool selectDevice(const std::string& deviceId) {
        if (deviceId.empty()) {
            return selectDefaultDevice();
        }

        for (const auto& device : devices_) {
            if (device.id == deviceId) {
                selectedDeviceId_ = deviceId;
                LOG_INFO("Selected audio device: {}", device.name);
                return true;
            }
        }

        LOG_WARNING("Audio device not found: {}", deviceId);
        return false;
    }

    bool selectDefaultDevice() {
        for (const auto& device : devices_) {
            if (device.isDefault) {
                selectedDeviceId_ = device.id;
                LOG_INFO("Selected default audio device: {}", device.name);
                return true;
            }
        }

        if (!devices_.empty()) {
            selectedDeviceId_ = devices_[0].id;
            LOG_INFO("Selected first audio device: {}", devices_[0].name);
            return true;
        }

        return false;
    }

    int getSampleRate() const {
        return sampleRate_;
    }

    bool setSampleRate(int sampleRate) {
        if (sampleRate < 8000 || sampleRate > 192000) {
            return false;
        }
        sampleRate_ = sampleRate;
        return true;
    }

    int getChannels() const {
        return channels_;
    }

    bool setChannels(int channels) {
        if (channels < 1 || channels > 8) {
            return false;
        }
        channels_ = channels;
        return true;
    }

    uint32_t getBufferSizeFrames() const {
        return bufferSizeFrames_;
    }

    bool setBufferSizeFrames(uint32_t frames) {
        if (frames < 64 || frames > 8192) {
            return false;
        }
        bufferSizeFrames_ = frames;
        return true;
    }

    std::string findDeviceIdByName(const std::string& name) const {
        for (const auto& device : devices_) {
            if (device.name == name) return device.id;
        }
        return {};
    }

    const ma_device_id* getNativeDeviceId(const std::string& deviceId) const {
        if (deviceId.empty()) {
            return nullptr;  // Use default
        }

        try {
            size_t index = std::stoul(deviceId);
            if (index < deviceIds_.size()) {
                return &deviceIds_[index];
            }
        } catch (...) {
            Logger::warning("AudioDevice: '{}' is not a device id; using the default device", deviceId);
        }

        return nullptr;
    }

    ma_context* getContext() {
        return initialized_ ? &context_ : nullptr;
    }

private:
    ma_context context_{};
    bool initialized_ = false;

    std::vector<AudioDeviceInfo> devices_;
    std::vector<ma_device_id> deviceIds_;  // Native device IDs
    std::string selectedDeviceId_;

    int sampleRate_ = AudioDevice::DEFAULT_SAMPLE_RATE;
    int channels_ = AudioDevice::DEFAULT_CHANNELS;
    uint32_t bufferSizeFrames_ = AudioDevice::DEFAULT_BUFFER_FRAMES;
};

// Public interface implementation
AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}
AudioDevice::~AudioDevice() = default;
AudioDevice::AudioDevice(AudioDevice&&) noexcept = default;
AudioDevice& AudioDevice::operator=(AudioDevice&&) noexcept = default;

bool AudioDevice::initialize(bool nullBackend) { return impl_->initialize(nullBackend); }
void AudioDevice::shutdown() { impl_->shutdown(); }
bool AudioDevice::isInitialized() const { return impl_->isInitialized(); }

std::vector<AudioDeviceInfo> AudioDevice::enumerateDevices() { return impl_->enumerateDevices(); }
void AudioDevice::refreshDevices() { impl_->refreshDevices(); }
AudioDeviceInfo AudioDevice::getDefaultDevice() const { return impl_->getDefaultDevice(); }
AudioDeviceInfo AudioDevice::getSelectedDevice() const { return impl_->getSelectedDevice(); }
bool AudioDevice::selectDevice(const std::string& deviceId) { return impl_->selectDevice(deviceId); }
bool AudioDevice::selectDefaultDevice() { return impl_->selectDefaultDevice(); }
std::string AudioDevice::findDeviceIdByName(const std::string& name) const { return impl_->findDeviceIdByName(name); }
void* AudioDevice::nativeContext() { return impl_->getContext(); }
const void* AudioDevice::nativeDeviceId(const std::string& deviceId) const { return impl_->getNativeDeviceId(deviceId); }

int AudioDevice::getSampleRate() const { return impl_->getSampleRate(); }
bool AudioDevice::setSampleRate(int sampleRate) { return impl_->setSampleRate(sampleRate); }
int AudioDevice::getChannels() const { return impl_->getChannels(); }
bool AudioDevice::setChannels(int channels) { return impl_->setChannels(channels); }
uint32_t AudioDevice::getBufferSizeFrames() const { return impl_->getBufferSizeFrames(); }
bool AudioDevice::setBufferSizeFrames(uint32_t frames) { return impl_->setBufferSizeFrames(frames); }

} // namespace quadmapper
