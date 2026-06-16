#include "AudioSource.hpp"
#include <algorithm>
#include <cmath>

namespace quadmapper {

AudioSource::AudioSource()
    : sourceId_(UUID::generate())
{
}

AudioSource::AudioSource(const UUID& sourceId)
    : sourceId_(sourceId)
{
}

AudioSource::~AudioSource() = default;

AudioSource::AudioSource(AudioSource&& other) noexcept
    : sourceId_(other.sourceId_)
    , name_(std::move(other.name_))
    , volume_(other.volume_.load())
    , muted_(other.muted_.load())
    , enabled_(other.enabled_.load())
    , pan_(other.pan_.load())
    , buffer_(std::move(other.buffer_))
{
}

AudioSource& AudioSource::operator=(AudioSource&& other) noexcept {
    if (this != &other) {
        sourceId_ = other.sourceId_;
        name_ = std::move(other.name_);
        volume_ = other.volume_.load();
        muted_ = other.muted_.load();
        enabled_ = other.enabled_.load();
        pan_ = other.pan_.load();
        buffer_ = std::move(other.buffer_);
    }
    return *this;
}

void AudioSource::setVolume(float volume) {
    volume_ = std::clamp(volume, MIN_VOLUME, MAX_VOLUME);
}

float AudioSource::getVolumeDb() const {
    float vol = volume_.load();
    if (vol <= 0.0f) {
        return MIN_VOLUME_DB;
    }
    // Convert linear to dB: dB = 20 * log10(linear)
    float db = 20.0f * std::log10(vol);
    return std::max(db, MIN_VOLUME_DB);
}

void AudioSource::setVolumeDb(float db) {
    // Clamp dB value
    db = std::clamp(db, MIN_VOLUME_DB, MAX_VOLUME_DB);
    // Convert dB to linear: linear = 10^(dB/20)
    float linear = std::pow(10.0f, db / 20.0f);
    volume_ = linear;
}

bool AudioSource::toggleMute() {
    bool newState = !muted_.load();
    muted_ = newState;
    return newState;
}

void AudioSource::setPan(float pan) {
    pan_ = std::clamp(pan, MIN_PAN, MAX_PAN);
}

float AudioSource::getEffectiveVolume() const {
    if (muted_.load() || !enabled_.load()) {
        return 0.0f;
    }
    return volume_.load();
}

float AudioSource::getLeftGain() const {
    float effective = getEffectiveVolume();
    if (effective <= 0.0f) return 0.0f;

    float p = pan_.load();
    // Constant power panning
    // When pan = 0: both channels equal
    // When pan = -1: left at full, right at 0
    // When pan = 1: left at 0, right at full
    float leftGain = effective * std::cos((p + 1.0f) * 0.25f * 3.14159265f);
    return leftGain;
}

float AudioSource::getRightGain() const {
    float effective = getEffectiveVolume();
    if (effective <= 0.0f) return 0.0f;

    float p = pan_.load();
    // Constant power panning
    float rightGain = effective * std::sin((p + 1.0f) * 0.25f * 3.14159265f);
    return rightGain;
}

size_t AudioSource::readSamples(float* output, size_t frameCount, int channels) {
    if (!output || frameCount == 0 || !buffer_) {
        return 0;
    }

    // Calculate samples needed for stereo output.
    size_t samplesNeeded = frameCount * static_cast<size_t>(channels);

    // Grow the pre-allocated scratch buffer if this block is larger than any
    // seen before. In steady-state (fixed callback size) this branch never
    // fires, so there is no allocation on the RT path.
    if (readBuffer_.size() < samplesNeeded) {
        readBuffer_.resize(samplesNeeded);
    }

    // Read from the audio buffer (lock-free after AudioBuffer fix).
    size_t samplesRead = buffer_->read(readBuffer_.data(), samplesNeeded, channels);
    if (samplesRead == 0) {
        return 0;
    }

    size_t framesRead = samplesRead / static_cast<size_t>(channels);

    // Get gains for left and right channels
    float leftGain = getLeftGain();
    float rightGain = getRightGain();

    // Apply gains and convert to stereo interleaved output
    if (channels == 1) {
        // Mono source: apply same sample to both channels
        for (size_t i = 0; i < framesRead; ++i) {
            float sample = readBuffer_[i];
            output[i * 2] = sample * leftGain;
            output[i * 2 + 1] = sample * rightGain;
        }
    } else if (channels == 2) {
        // Stereo source: apply left gain to left, right gain to right
        for (size_t i = 0; i < framesRead; ++i) {
            output[i * 2] = readBuffer_[i * 2] * leftGain;
            output[i * 2 + 1] = readBuffer_[i * 2 + 1] * rightGain;
        }
    } else {
        // Multi-channel: mix down to stereo
        for (size_t i = 0; i < framesRead; ++i) {
            float left = 0.0f;
            float right = 0.0f;

            // Simple mixdown: odd channels to left, even to right
            for (int ch = 0; ch < channels; ++ch) {
                float sample = readBuffer_[i * channels + ch];
                if (ch % 2 == 0) {
                    left += sample;
                } else {
                    right += sample;
                }
            }

            // Normalize by number of channels contributing
            int leftChannels = (channels + 1) / 2;
            int rightChannels = channels / 2;
            if (leftChannels > 0) left /= leftChannels;
            if (rightChannels > 0) right /= rightChannels;

            output[i * 2] = left * leftGain;
            output[i * 2 + 1] = right * rightGain;
        }
    }

    return framesRead;
}

} // namespace quadmapper
