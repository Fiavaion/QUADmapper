#include "AudioSource.hpp"

#include <algorithm>
#include <cmath>

namespace quadmapper {

AudioSource::AudioSource()
    : sourceId_(UUID::generate()) {}

AudioSource::AudioSource(const UUID& sourceId)
    : sourceId_(sourceId) {}

AudioSource::~AudioSource() = default;

void AudioSource::setVolume(float volume) {
    volume_ = std::isfinite(volume) ? std::clamp(volume, MIN_VOLUME, MAX_VOLUME) : MIN_VOLUME;
}

float AudioSource::getVolumeDb() const {
    const float vol = volume_.load();
    if (vol <= 0.0f) return MIN_VOLUME_DB;
    return std::max(20.0f * std::log10(vol), MIN_VOLUME_DB);
}

void AudioSource::setVolumeDb(float db) {
    db = std::clamp(db, MIN_VOLUME_DB, MAX_VOLUME_DB);
    volume_ = std::pow(10.0f, db / 20.0f);
}

bool AudioSource::toggleMute() {
    const bool newState = !muted_.load();
    muted_ = newState;
    return newState;
}

void AudioSource::setPan(float pan) {
    pan_ = std::isfinite(pan) ? std::clamp(pan, MIN_PAN, MAX_PAN) : 0.0f;
}

float AudioSource::getEffectiveVolume() const {
    if (muted_.load() || !enabled_.load()) return 0.0f;
    return volume_.load();
}

// Constant-power pan: pan 0 gives both channels cos(pi/4) of the volume.
float AudioSource::getLeftGain() const {
    const float effective = getEffectiveVolume();
    if (effective <= 0.0f) return 0.0f;
    return effective * std::cos((pan_.load() + 1.0f) * 0.25f * 3.14159265f);
}

float AudioSource::getRightGain() const {
    const float effective = getEffectiveVolume();
    if (effective <= 0.0f) return 0.0f;
    return effective * std::sin((pan_.load() + 1.0f) * 0.25f * 3.14159265f);
}

void AudioSource::pull(float* output, size_t frameCount, const AudioRenderContext&) {
    const size_t samples = frameCount * 2;
    const size_t got = buffer_ ? buffer_->read(output, samples) : 0;
    std::fill(output + got, output + samples, 0.0f);
}

void AudioSource::render(float* output, size_t frameCount, const AudioRenderContext& context) {
    if (!output || frameCount == 0) return;
    pull(output, frameCount, context);

    const float targetLeft = getLeftGain();
    const float targetRight = getRightGain();
    if (lastLeftGain_ < 0.0f) {  // first callback: start at the set gain
        lastLeftGain_ = targetLeft;
        lastRightGain_ = targetRight;
    }

    // Ramp linearly from the previous callback's gains to the new ones, so a change
    // lands over one buffer instead of as a step (no zipper noise).
    const float spanLeft = targetLeft - lastLeftGain_;
    const float spanRight = targetRight - lastRightGain_;
    const float count = static_cast<float>(frameCount);
    for (size_t i = 0; i < frameCount; ++i) {
        const float t = static_cast<float>(i + 1) / count;  // reaches the target exactly
        output[i * 2] *= lastLeftGain_ + spanLeft * t;
        output[i * 2 + 1] *= lastRightGain_ + spanRight * t;
    }
    lastLeftGain_ = targetLeft;
    lastRightGain_ = targetRight;
}

} // namespace quadmapper
