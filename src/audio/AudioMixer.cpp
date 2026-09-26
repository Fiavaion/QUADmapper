#include "AudioMixer.hpp"
#include "core/Logger.hpp"

#include <miniaudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

namespace quadmapper {

namespace {

using SourceList = std::vector<AudioSource*>;

// Frames mixed per inner pass; a device callback of any size is mixed in passes.
constexpr size_t MIX_CHUNK_FRAMES = 1024;
// 10 ms periods at 48 kHz, miniaudio's low-latency default.
constexpr uint32_t PERIOD_FRAMES = 480;
// How long removeSource() waits for a callback to finish before it gives up on
// freeing the old array (it is then kept until shutdown instead).
constexpr auto CALLBACK_WAIT_LIMIT = std::chrono::seconds(1);

int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frameCount);
void notificationCallback(const ma_device_notification* notification);

}  // namespace

class AudioMixerImpl {
public:
    explicit AudioMixerImpl(AudioMixer* owner)
        : owner_(owner)
        , scratch_(MIX_CHUNK_FRAMES * AudioMixer::CHANNELS)
        , rtSources_(new SourceList()) {}

    ~AudioMixerImpl() {
        shutdown();
        delete rtSources_.load();
        for (SourceList* list : quarantinedLists_) delete list;
        delete rtCallback_.load();
    }

    bool initialize(AudioDevice* device, const std::string& deviceId) {
        if (initialized_) return true;

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = AudioMixer::CHANNELS;
        config.sampleRate = AudioMixer::SAMPLE_RATE;
        config.periodSizeInFrames = PERIOD_FRAMES;
        config.dataCallback = dataCallback;
        config.notificationCallback = notificationCallback;
        config.pUserData = this;

        ma_context* context = nullptr;
        if (device && device->isInitialized()) {
            context = static_cast<ma_context*>(device->nativeContext());
            if (!deviceId.empty()) {
                const void* nativeId = device->nativeDeviceId(deviceId);
                if (!nativeId) {
                    LOG_ERROR("Audio device '{}' is not available", deviceId);
                    return false;
                }
                config.playback.pDeviceID = static_cast<const ma_device_id*>(nativeId);
            }
        }

        const ma_result result = ma_device_init(context, &config, &device_);
        if (result != MA_SUCCESS) {
            LOG_ERROR("Failed to open the audio playback device: {} ({})",
                      ma_result_description(result), static_cast<int>(result));
            return false;
        }

        char name[MA_MAX_DEVICE_NAME_LENGTH + 1] = {};
        ma_device_get_name(&device_, ma_device_type_playback, name, sizeof(name), nullptr);
        deviceName_ = name;
        const double internalRate = device_.playback.internalSampleRate > 0
            ? static_cast<double>(device_.playback.internalSampleRate)
            : static_cast<double>(AudioMixer::SAMPLE_RATE);
        periodFrames_ = device_.playback.internalPeriodSizeInFrames;
        // Estimate: the device's whole buffer (period x periods) is queued ahead of
        // the frames a callback writes. The exact figure depends on the driver.
        latency_.store(static_cast<double>(device_.playback.internalPeriodSizeInFrames) *
                       static_cast<double>(device_.playback.internalPeriods) / internalRate);
        lost_ = false;
        initialized_ = true;
        LOG_INFO("Audio output '{}' opened: {} Hz ({} Hz native), period {} x {} frames, "
                 "~{} ms output latency",
                 deviceName_, AudioMixer::SAMPLE_RATE, device_.playback.internalSampleRate,
                 device_.playback.internalPeriodSizeInFrames, device_.playback.internalPeriods,
                 static_cast<int>(latency_.load() * 1000.0 + 0.5));
        return true;
    }

    void shutdown() {
        if (!initialized_) return;
        stop();
        ma_device_uninit(&device_);  // no callback runs after this
        initialized_ = false;
        deviceName_.clear();
        latency_.store(0.0);
        // Nothing can reach quarantined arrays or sources any more.
        for (SourceList* list : quarantinedLists_) delete list;
        quarantinedLists_.clear();
        quarantinedSources_.clear();
        LOG_INFO("Audio output closed");
    }

    bool isInitialized() const { return initialized_; }

    bool start() {
        if (!initialized_) return false;
        if (playing_) return true;
        stopRequested_ = false;
        lost_ = false;
        const ma_result result = ma_device_start(&device_);
        if (result != MA_SUCCESS) {
            LOG_ERROR("Failed to start audio playback: {}", ma_result_description(result));
            return false;
        }
        playing_ = true;
        return true;
    }

    void stop() {
        if (!initialized_ || !playing_) return;
        stopRequested_ = true;
        ma_device_stop(&device_);  // returns after the last callback finished
        playing_ = false;
    }

    bool isPlaying() const { return playing_; }
    bool isDeviceLost() const { return lost_.load(); }
    std::string getDeviceName() const { return deviceName_; }
    double getOutputLatency() const { return latency_.load(); }
    uint32_t getBufferSizeFrames() const { return periodFrames_; }

    // --- Sources (control thread) ---

    void addSource(std::shared_ptr<AudioSource> source) {
        if (!source) return;
        std::lock_guard<std::mutex> lock(sourcesMutex_);
        sources_.push_back(std::move(source));
        publishLocked({});
    }

    bool removeSource(const UUID& sourceId) {
        std::lock_guard<std::mutex> lock(sourcesMutex_);
        auto it = std::find_if(sources_.begin(), sources_.end(),
                               [&](const std::shared_ptr<AudioSource>& s) { return s->getId() == sourceId; });
        if (it == sources_.end()) return false;
        std::vector<std::shared_ptr<AudioSource>> removed{std::move(*it)};
        sources_.erase(it);
        publishLocked(std::move(removed));
        return true;
    }

    std::shared_ptr<AudioSource> getSource(const UUID& sourceId) {
        std::lock_guard<std::mutex> lock(sourcesMutex_);
        auto it = std::find_if(sources_.begin(), sources_.end(),
                               [&](const std::shared_ptr<AudioSource>& s) { return s->getId() == sourceId; });
        return it != sources_.end() ? *it : nullptr;
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
        std::vector<std::shared_ptr<AudioSource>> removed = std::move(sources_);
        sources_.clear();
        publishLocked(std::move(removed));
    }

    void setMixCallback(AudioMixerCallback callback) {
        std::lock_guard<std::mutex> lock(sourcesMutex_);
        AudioMixerCallback* fresh = callback ? new AudioMixerCallback(std::move(callback)) : nullptr;
        AudioMixerCallback* old = rtCallback_.exchange(fresh, std::memory_order_seq_cst);
        if (waitForCallbackToFinish()) {
            delete old;
        } else {
            quarantinedCallbacks_.emplace_back(old);
        }
    }

    // --- Mixing (real-time thread, or the caller of renderOffline) ---

    void processAudio(float* output, size_t frameCount, int64_t wallNs) {
        if (frameCount == 0) return;
        // Odd while mixing. seq_cst pairs with the control thread's exchange + load in
        // publishLocked(): either this load sees the new array, or the control thread
        // sees the odd count and waits for the even one.
        callbackSequence_.fetch_add(1, std::memory_order_seq_cst);
        const SourceList* sources = rtSources_.load(std::memory_order_seq_cst);

        std::memset(output, 0, frameCount * AudioMixer::CHANNELS * sizeof(float));
        AudioRenderContext context;
        context.sampleRate = AudioMixer::SAMPLE_RATE;
        context.outputLatency = latency_.load(std::memory_order_relaxed);

        for (size_t done = 0; done < frameCount;) {
            const size_t chunk = std::min(MIX_CHUNK_FRAMES, frameCount - done);
            context.wallNs = wallNs + static_cast<int64_t>(
                static_cast<double>(done) * 1e9 / AudioMixer::SAMPLE_RATE);
            float* out = output + done * AudioMixer::CHANNELS;
            for (AudioSource* source : *sources) {
                if (!source->isEnabled()) continue;
                // Every enabled source renders, muted or not, so it keeps consuming
                // and stays in step with its clock.
                source->render(scratch_.data(), chunk, context);
                const size_t samples = chunk * AudioMixer::CHANNELS;
                for (size_t i = 0; i < samples; ++i) out[i] += scratch_[i];
            }
            done += chunk;
        }

        // Master gain, ramped across the buffer like the per-source gains.
        const float target = owner_->isMasterMuted() ? 0.0f : owner_->getMasterVolume();
        if (lastMasterGain_ < 0.0f) lastMasterGain_ = target;
        const float span = target - lastMasterGain_;
        const float count = static_cast<float>(frameCount);
        for (size_t i = 0; i < frameCount; ++i) {
            const float gain = lastMasterGain_ + span * (static_cast<float>(i + 1) / count);
            float* frame = output + i * AudioMixer::CHANNELS;
            frame[0] = std::clamp(frame[0] * gain, -1.0f, 1.0f);
            frame[1] = std::clamp(frame[1] * gain, -1.0f, 1.0f);
        }
        lastMasterGain_ = target;

        if (const AudioMixerCallback* tap = rtCallback_.load(std::memory_order_acquire)) {
            (*tap)(output, frameCount, wallNs);
        }
        callbackSequence_.fetch_add(1, std::memory_order_release);
    }

    // miniaudio stops and restarts a device it reroutes (the system default changed,
    // or the default device was unplugged): only a stop that is not followed by a
    // start, and was not asked for, is a lost device.
    void onNotification(ma_device_notification_type type) {
        switch (type) {
            case ma_device_notification_type_stopped:
                if (!stopRequested_.load()) lost_ = true;
                break;
            case ma_device_notification_type_started:
            case ma_device_notification_type_rerouted:
                lost_ = false;
                break;
            default:
                break;
        }
    }

private:
    // Swap in an array built from sources_ and free the old one once the callback
    // cannot hold it. `removed` keeps just-removed sources alive until then, so their
    // last reference is always dropped here, on the control thread.
    void publishLocked(std::vector<std::shared_ptr<AudioSource>> removed) {
        auto* fresh = new SourceList();
        fresh->reserve(sources_.size());
        for (const auto& source : sources_) fresh->push_back(source.get());
        SourceList* old = rtSources_.exchange(fresh, std::memory_order_seq_cst);
        if (waitForCallbackToFinish()) {
            delete old;
        } else {
            LOG_WARNING("Audio callback did not return within 1 s; keeping removed sources until shutdown");
            quarantinedLists_.push_back(old);
            for (auto& source : removed) quarantinedSources_.push_back(std::move(source));
        }
    }

    // If a mix is running now, wait for it to end. A mix that starts later loads the
    // array published before this call.
    bool waitForCallbackToFinish() const {
        const uint64_t sequence = callbackSequence_.load(std::memory_order_seq_cst);
        if ((sequence & 1u) == 0) return true;
        const auto deadline = std::chrono::steady_clock::now() + CALLBACK_WAIT_LIMIT;
        while (callbackSequence_.load(std::memory_order_acquire) == sequence) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }

    AudioMixer* owner_;
    ma_device device_{};
    bool initialized_ = false;
    bool playing_ = false;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> lost_{false};
    std::string deviceName_;
    std::atomic<double> latency_{0.0};
    uint32_t periodFrames_ = PERIOD_FRAMES;

    // Control side: owners of the registered sources.
    std::vector<std::shared_ptr<AudioSource>> sources_;
    mutable std::mutex sourcesMutex_;

    // Audio side: what the callback reads.
    std::vector<float> scratch_;
    float lastMasterGain_ = -1.0f;
    std::atomic<SourceList*> rtSources_;
    std::atomic<AudioMixerCallback*> rtCallback_{nullptr};
    std::atomic<uint64_t> callbackSequence_{0};

    // Kept alive when a callback overran the wait (never expected).
    std::vector<SourceList*> quarantinedLists_;
    std::vector<std::shared_ptr<AudioSource>> quarantinedSources_;
    std::vector<std::unique_ptr<AudioMixerCallback>> quarantinedCallbacks_;
};

namespace {

void dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frameCount) {
    if (auto* impl = static_cast<AudioMixerImpl*>(device->pUserData)) {
        impl->processAudio(static_cast<float*>(output), frameCount, steadyNowNs());
    }
}

void notificationCallback(const ma_device_notification* notification) {
    if (!notification || !notification->pDevice) return;
    auto* impl = static_cast<AudioMixerImpl*>(notification->pDevice->pUserData);
    if (impl) impl->onNotification(notification->type);
}

}  // namespace

// --- Public interface ---

AudioMixer::AudioMixer()
    : impl_(std::make_unique<AudioMixerImpl>(this)) {}

AudioMixer::~AudioMixer() {
    impl_->shutdown();  // the callback reads the master volume members below
}

bool AudioMixer::initialize(AudioDevice* device, const std::string& deviceId) {
    return impl_->initialize(device, deviceId);
}
void AudioMixer::shutdown() { impl_->shutdown(); }
bool AudioMixer::isInitialized() const { return impl_->isInitialized(); }
bool AudioMixer::start() { return impl_->start(); }
void AudioMixer::stop() { impl_->stop(); }
bool AudioMixer::isPlaying() const { return impl_->isPlaying(); }
bool AudioMixer::isDeviceLost() const { return impl_->isDeviceLost(); }
std::string AudioMixer::getDeviceName() const { return impl_->getDeviceName(); }
double AudioMixer::getOutputLatency() const { return impl_->getOutputLatency(); }
uint32_t AudioMixer::getBufferSizeFrames() const { return impl_->getBufferSizeFrames(); }

void AudioMixer::setMasterVolume(float volume) {
    masterVolume_ = std::isfinite(volume) ? std::clamp(volume, MIN_VOLUME, MAX_VOLUME) : MIN_VOLUME;
}

float AudioMixer::getMasterVolumeDb() const {
    const float vol = masterVolume_.load();
    if (vol <= 0.0f) return MIN_VOLUME_DB;
    return std::max(20.0f * std::log10(vol), MIN_VOLUME_DB);
}

void AudioMixer::setMasterVolumeDb(float db) {
    db = std::clamp(db, MIN_VOLUME_DB, MAX_VOLUME_DB);
    masterVolume_ = std::pow(10.0f, db / 20.0f);
}

bool AudioMixer::toggleMasterMute() {
    const bool newState = !masterMuted_.load();
    masterMuted_ = newState;
    return newState;
}

void AudioMixer::addSource(std::shared_ptr<AudioSource> source) { impl_->addSource(std::move(source)); }
bool AudioMixer::removeSource(const UUID& sourceId) { return impl_->removeSource(sourceId); }
std::shared_ptr<AudioSource> AudioMixer::getSource(const UUID& sourceId) { return impl_->getSource(sourceId); }
std::vector<std::shared_ptr<AudioSource>> AudioMixer::getSources() const { return impl_->getSources(); }
size_t AudioMixer::getSourceCount() const { return impl_->getSourceCount(); }
void AudioMixer::clearSources() { impl_->clearSources(); }
void AudioMixer::setMixCallback(AudioMixerCallback callback) { impl_->setMixCallback(std::move(callback)); }

void AudioMixer::renderOffline(float* output, size_t frameCount, int64_t wallNs) {
    if (!output || frameCount == 0 || impl_->isPlaying()) return;
    impl_->processAudio(output, frameCount, wallNs);
}

} // namespace quadmapper
