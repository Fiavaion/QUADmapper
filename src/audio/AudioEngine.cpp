#include "AudioEngine.hpp"

#include "AudioDevice.hpp"
#include "AudioMixer.hpp"
#include "core/Logger.hpp"

namespace quadmapper {

AudioEngine::AudioEngine()
    : AudioEngine(Options{}) {}

AudioEngine::AudioEngine(Options options)
    : options_(options)
    , device_(std::make_unique<AudioDevice>())
    , mixer_(std::make_unique<AudioMixer>()) {}

AudioEngine::~AudioEngine() {
    attachments_.clear();  // each leaves the mix before its decoder stops
    mixer_->shutdown();
    mixer_.reset();
    device_.reset();       // the mixer's device was opened on this context
}

bool AudioEngine::open(const std::string& deviceName) {
    preferredDevice_ = deviceName;
    mixer_->shutdown();  // no-op when closed; sources stay registered across a reopen
    if (!device_->isInitialized()) {
        device_->initialize(options_.nullBackend);
    } else {
        device_->refreshDevices();
    }

    std::string deviceId;
    if (!deviceName.empty()) {
        deviceId = device_->findDeviceIdByName(deviceName);
        if (deviceId.empty()) {
            LOG_WARNING("Audio device '{}' is not connected; using the system default", deviceName);
        }
    }
    if (!mixer_->initialize(device_.get(), deviceId) || !mixer_->start()) {
        mixer_->shutdown();
        outputState_ = AudioOutputState::Failed;
        LOG_ERROR("No audio output: running silent");
        return false;
    }
    outputState_ = AudioOutputState::Open;
    return true;
}

std::vector<std::string> AudioEngine::listDevices() {
    std::vector<std::string> names;
    if (!device_->isInitialized() && !device_->initialize(options_.nullBackend)) return names;
    device_->refreshDevices();
    for (const auto& info : device_->enumerateDevices()) names.push_back(info.name);
    return names;
}

std::string AudioEngine::activeDeviceName() const {
    return outputState_ == AudioOutputState::Open ? mixer_->getDeviceName() : std::string();
}

void AudioEngine::poll() {
    if (outputState_ == AudioOutputState::Open && mixer_->isDeviceLost()) {
        outputState_ = AudioOutputState::Lost;
        LOG_WARNING("Audio device '{}' stopped (unplugged?); select an output to resume sound",
                    mixer_->getDeviceName());
    } else if (outputState_ == AudioOutputState::Lost && !mixer_->isDeviceLost()) {
        outputState_ = AudioOutputState::Open;  // miniaudio brought it back (rerouted)
        LOG_INFO("Audio device '{}' is playing again", mixer_->getDeviceName());
    }
}

float AudioEngine::masterVolume() const { return mixer_->getMasterVolume(); }
void AudioEngine::setMasterVolume(float volume) { mixer_->setMasterVolume(volume); }
bool AudioEngine::masterMuted() const { return mixer_->isMasterMuted(); }
void AudioEngine::setMasterMuted(bool muted) { mixer_->setMasterMuted(muted); }

void AudioEngine::sync(const UUID& id, const std::filesystem::path& path, const AudioTransport& transport) {
    auto it = attachments_.find(id);
    if (it == attachments_.end() || it->second->path() != path) {
        // New source, or the same id now showing another file (relocated media).
        auto attachment = std::make_unique<AudioAttachment>(path, *mixer_);
        it = attachments_.insert_or_assign(id, std::move(attachment)).first;
    }
    it->second->update(transport);
}

void AudioEngine::retainOnly(const std::unordered_set<UUID>& live) {
    for (auto it = attachments_.begin(); it != attachments_.end();) {
        it = live.count(it->first) ? std::next(it) : attachments_.erase(it);
    }
}

SourceAudioStatus AudioEngine::status(const UUID& id) const {
    const AudioAttachment* a = attachment(id);
    if (!a) return SourceAudioStatus::None;
    switch (a->state()) {
        case AudioAttachmentState::Probing: return SourceAudioStatus::Probing;
        case AudioAttachmentState::NoAudio: return SourceAudioStatus::NoAudio;
        default:                            return SourceAudioStatus::Ready;
    }
}

const AudioAttachment* AudioEngine::attachment(const UUID& id) const {
    auto it = attachments_.find(id);
    return it != attachments_.end() ? it->second.get() : nullptr;
}

}  // namespace quadmapper
