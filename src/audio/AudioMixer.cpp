#include "AudioMixer.hpp"
#include "core/Logger.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace quadmapper {

// Forward declaration
class AudioMixerImpl;

// miniaudio callback function - must be at file scope
static void audioDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

class AudioMixerImpl {
public:
    explicit AudioMixerImpl(AudioMixer* owner)
        : owner_(owner)
    {}

    ~AudioMixerImpl() {
        shutdown();
    }

    bool initialize(AudioDevice* device) {
        if (initialized_) {
            return true;
        }

        // Get device settings
        if (device && device->isInitialized()) {
            sampleRate_ = device->getSampleRate();
            channels_ = device->getChannels();
            bufferSizeFrames_ = device->getBufferSizeFrames();
        } else {
            sampleRate_ = AudioDevice::DEFAULT_SAMPLE_RATE;
            channels_ = AudioDevice::DEFAULT_CHANNELS;
            bufferSizeFrames_ = AudioDevice::DEFAULT_BUFFER_FRAMES;
        }

        // Configure device
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format = ma_format_f32;
        deviceConfig.playback.channels = static_cast<ma_uint32>(channels_);
        deviceConfig.sampleRate = static_cast<ma_uint32>(sampleRate_);
        deviceConfig.periodSizeInFrames = bufferSizeFrames_;
        deviceConfig.dataCallback = audioDataCallback;
        deviceConfig.pUserData = this;

        // Initialize device
        ma_result result = ma_device_init(nullptr, &deviceConfig, &maDevice_);
        if (result != MA_SUCCESS) {
            LOG_ERROR("Failed to initialize audio playback device: {}", static_cast<int>(result));
            return false;
        }

        // Allocate mix buffer
        mixBuffer_.resize(static_cast<size_t>(bufferSizeFrames_) * channels_);
        tempBuffer_.resize(static_cast<size_t>(bufferSizeFrames_) * channels_);

        initialized_ = true;
        LOG_INFO("Audio mixer initialized: {} Hz, {} channels, {} buffer frames",
                 sampleRate_, channels_, bufferSizeFrames_);
        return true;
    }

    void shutdown() {
        if (!initialized_) {
            return;
        }

        stop();
        ma_device_uninit(&maDevice_);

        mixBuffer_.clear();
        tempBuffer_.clear();
        initialized_ = false;

        LOG_INFO("Audio mixer shutdown");
    }

    bool isInitialized() const {
        return initialized_;
    }

    bool start() {
        if (!initialized_) {
            return false;
        }

        if (playing_) {
            return true;
        }

        ma_result result = ma_device_start(&maDevice_);
        if (result != MA_SUCCESS) {
            LOG_ERROR("Failed to start audio playback: {}", static_cast<int>(result));
            return false;
        }

        playing_ = true;
        LOG_INFO("Audio playback started");
        return true;
    }

    void stop() {
        if (!initialized_ || !playing_) {
            return;
        }

        ma_device_stop(&maDevice_);
        playing_ = false;
        LOG_INFO("Audio playback stopped");
    }

    bool isPlaying() const {
        return playing_;
    }

    // Source management
    void addSource(std::shared_ptr<AudioSource> source) {
        if (!source) return;

        std::lock_guard<std::mutex> lock(sourcesMutex_);
        sources_.push_back(source);
        LOG_DEBUG("Added audio source: {}", source->getId().toString());
    }

    bool removeSource(const UUID& sourceId) {
        std::lock_guard<std::mutex> lock(sourcesMutex_);

        auto it = std::find_if(sources_.begin(), sources_.end(),
            [&sourceId](const std::shared_ptr<AudioSource>& s) {
                return s->getId() == sourceId;
            });

        if (it != sources_.end()) {
            sources_.erase(it);
            LOG_DEBUG("Removed audio source: {}", sourceId.toString());
            return true;
        }
        return false;
    }

    std::shared_ptr<AudioSource> getSource(const UUID& sourceId) {
        std::lock_guard<std::mutex> lock(sourcesMutex_);

        auto it = std::find_if(sources_.begin(), sources_.end(),
            [&sourceId](const std::shared_ptr<AudioSource>& s) {
                return s->getId() == sourceId;
            });

        return (it != sources_.end()) ? *it : nullptr;
    }

    std::vector<std::shared_ptr<AudioSource>> getSources() const {
        std::lock_guard<std::mutex> lock(sourcesMutex_);
        return sources_;
    }

    size_t getSourceCount() const {
        std::lock_guard<std::mutex> lock(sourcesMutex_);
        return sources_.size();
    }

    void clearSources() {
        std::lock_guard<std::mutex> lock(sourcesMutex_);
        sources_.clear();
        LOG_DEBUG("Cleared all audio sources");
    }

    int getSampleRate() const { return sampleRate_; }
    int getChannels() const { return channels_; }
    uint32_t getBufferSizeFrames() const { return bufferSizeFrames_; }

    void setMixCallback(AudioMixerCallback callback) {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        mixCallback_ = callback;
    }

    // Audio callback - called from audio thread
    void processAudio(float* output, size_t frameCount) {
        // Clear output buffer
        std::memset(output, 0, frameCount * channels_ * sizeof(float));

        // Get master volume and mute state
        float masterVol = owner_->getMasterVolume();
        bool masterMuted = owner_->isMasterMuted();

        if (masterMuted || masterVol <= 0.0f) {
            return;  // Silence when muted
        }

        // Lock sources for reading
        std::unique_lock<std::mutex> sourceLock(sourcesMutex_);
        auto sourcesCopy = sources_;  // Copy to avoid holding lock during mixing
        sourceLock.unlock();

        // Mix all sources
        for (auto& source : sourcesCopy) {
            if (!source || !source->isEnabled()) {
                continue;
            }

            auto buffer = source->getBuffer();
            if (!buffer || buffer->isEmpty()) {
                continue;
            }

            // Read samples from source (applies source volume/pan)
            size_t framesRead = source->readSamples(tempBuffer_.data(), frameCount, channels_);
            if (framesRead == 0) {
                continue;
            }

            // Mix into output
            size_t sampleCount = framesRead * channels_;
            for (size_t i = 0; i < sampleCount; ++i) {
                output[i] += tempBuffer_[i];
            }
        }

        // Apply master volume
        if (masterVol < 1.0f) {
            size_t sampleCount = frameCount * channels_;
            for (size_t i = 0; i < sampleCount; ++i) {
                output[i] *= masterVol;
            }
        }

        // Clamp output to [-1.0, 1.0] to prevent clipping
        size_t sampleCount = frameCount * channels_;
        for (size_t i = 0; i < sampleCount; ++i) {
            output[i] = std::clamp(output[i], -1.0f, 1.0f);
        }

        // Call mix callback if set
        std::lock_guard<std::mutex> callbackLock(callbackMutex_);
        if (mixCallback_) {
            mixCallback_(output, frameCount, channels_);
        }
    }

private:
    AudioMixer* owner_;
    ma_device maDevice_{};
    bool initialized_ = false;
    bool playing_ = false;

    int sampleRate_ = AudioDevice::DEFAULT_SAMPLE_RATE;
    int channels_ = AudioDevice::DEFAULT_CHANNELS;
    uint32_t bufferSizeFrames_ = AudioDevice::DEFAULT_BUFFER_FRAMES;

    std::vector<std::shared_ptr<AudioSource>> sources_;
    mutable std::mutex sourcesMutex_;

    std::vector<float> mixBuffer_;
    std::vector<float> tempBuffer_;

    AudioMixerCallback mixCallback_;
    std::mutex callbackMutex_;
};

// miniaudio callback function
static void audioDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;  // Unused for playback

    auto* impl = static_cast<AudioMixerImpl*>(pDevice->pUserData);
    if (impl) {
        impl->processAudio(static_cast<float*>(pOutput), static_cast<size_t>(frameCount));
    }
}

// Public interface implementation
AudioMixer::AudioMixer()
    : impl_(std::make_unique<AudioMixerImpl>(this))
{}

AudioMixer::~AudioMixer() = default;

AudioMixer::AudioMixer(AudioMixer&& other) noexcept
    : impl_(std::move(other.impl_))
    , masterVolume_(other.masterVolume_.load())
    , masterMuted_(other.masterMuted_.load())
{
}

AudioMixer& AudioMixer::operator=(AudioMixer&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        masterVolume_ = other.masterVolume_.load();
        masterMuted_ = other.masterMuted_.load();
    }
    return *this;
}

bool AudioMixer::initialize(AudioDevice* device) {
    return impl_->initialize(device);
}

void AudioMixer::shutdown() {
    impl_->shutdown();
}

bool AudioMixer::isInitialized() const {
    return impl_->isInitialized();
}

bool AudioMixer::start() {
    return impl_->start();
}

void AudioMixer::stop() {
    impl_->stop();
}

bool AudioMixer::isPlaying() const {
    return impl_->isPlaying();
}

void AudioMixer::setMasterVolume(float volume) {
    masterVolume_ = std::clamp(volume, MIN_VOLUME, MAX_VOLUME);
}

float AudioMixer::getMasterVolumeDb() const {
    float vol = masterVolume_.load();
    if (vol <= 0.0f) {
        return MIN_VOLUME_DB;
    }
    float db = 20.0f * std::log10(vol);
    return std::max(db, MIN_VOLUME_DB);
}

void AudioMixer::setMasterVolumeDb(float db) {
    db = std::clamp(db, MIN_VOLUME_DB, MAX_VOLUME_DB);
    float linear = std::pow(10.0f, db / 20.0f);
    masterVolume_ = linear;
}

bool AudioMixer::toggleMasterMute() {
    bool newState = !masterMuted_.load();
    masterMuted_ = newState;
    return newState;
}

void AudioMixer::addSource(std::shared_ptr<AudioSource> source) {
    impl_->addSource(source);
}

bool AudioMixer::removeSource(const UUID& sourceId) {
    return impl_->removeSource(sourceId);
}

std::shared_ptr<AudioSource> AudioMixer::getSource(const UUID& sourceId) {
    return impl_->getSource(sourceId);
}

std::vector<std::shared_ptr<AudioSource>> AudioMixer::getSources() const {
    return impl_->getSources();
}

size_t AudioMixer::getSourceCount() const {
    return impl_->getSourceCount();
}

void AudioMixer::clearSources() {
    impl_->clearSources();
}

int AudioMixer::getSampleRate() const {
    return impl_->getSampleRate();
}

int AudioMixer::getChannels() const {
    return impl_->getChannels();
}

uint32_t AudioMixer::getBufferSizeFrames() const {
    return impl_->getBufferSizeFrames();
}

void AudioMixer::setMixCallback(AudioMixerCallback callback) {
    impl_->setMixCallback(callback);
}

} // namespace quadmapper
