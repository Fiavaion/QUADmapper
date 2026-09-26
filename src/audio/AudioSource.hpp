#pragma once

#include "core/UUID.hpp"
#include "AudioBuffer.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace quadmapper {

// What the mixer tells each source about the callback it is rendering.
struct AudioRenderContext {
    int64_t wallNs = 0;          // std::chrono::steady_clock time of the callback, in ns
    double outputLatency = 0.0;  // seconds from this callback until its first frame is heard
    int sampleRate = 48000;
};

/**
 * @brief One input of the mixer, with volume, mute and pan
 *
 * Controls are atomics the control thread sets and the audio callback reads. The
 * mixer calls render() on the real-time thread: it pulls stereo frames (pull(); the
 * base class reads its AudioBuffer as interleaved stereo) and applies the gains,
 * ramping from the last callback's gains to the new ones across the buffer so a
 * volume, mute or pan change never clicks.
 *
 * Sources are shared (std::shared_ptr) and registered with the mixer by address, so
 * they are neither copyable nor movable.
 */
class AudioSource {
public:
    AudioSource();
    explicit AudioSource(const UUID& sourceId);
    virtual ~AudioSource();

    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;

    const UUID& getId() const { return sourceId_; }

    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    // --- Volume, mute, pan (control thread) ---

    float getVolume() const { return volume_.load(); }
    void setVolume(float volume);          // clamped to 0..1; NaN counts as 0
    float getVolumeDb() const;             // -60..0 dB
    void setVolumeDb(float db);            // clamped to -60..0 dB

    bool isMuted() const { return muted_.load(); }
    void setMuted(bool muted) { muted_ = muted; }
    bool toggleMute();

    float getPan() const { return pan_.load(); }
    void setPan(float pan);                // -1 left .. 1 right

    // A disabled source is skipped by the mixer altogether (it does not consume).
    bool isEnabled() const { return enabled_.load(); }
    void setEnabled(bool enabled) { enabled_ = enabled; }

    float getEffectiveVolume() const;      // volume, or 0 while muted or disabled
    float getLeftGain() const;             // effective volume with constant-power pan
    float getRightGain() const;

    // --- Samples ---

    void setBuffer(std::shared_ptr<AudioBuffer> buffer) { buffer_ = std::move(buffer); }
    std::shared_ptr<AudioBuffer> getBuffer() const { return buffer_; }

    /**
     * @brief Render `frameCount` stereo frames into `output` (real-time thread)
     *
     * Every frame of `output` is written (silence where the source has nothing).
     * No lock, no allocation.
     */
    void render(float* output, size_t frameCount, const AudioRenderContext& context);

    static constexpr float MIN_VOLUME = 0.0f;
    static constexpr float MAX_VOLUME = 1.0f;
    static constexpr float MIN_VOLUME_DB = -60.0f;
    static constexpr float MAX_VOLUME_DB = 0.0f;
    static constexpr float MIN_PAN = -1.0f;
    static constexpr float MAX_PAN = 1.0f;

protected:
    // Fill all `frameCount` stereo frames of `output` (real-time thread). The base
    // reads interleaved stereo from the buffer and pads with silence.
    virtual void pull(float* output, size_t frameCount, const AudioRenderContext& context);

private:
    UUID sourceId_;
    std::string name_;

    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<float> pan_{0.0f};

    std::shared_ptr<AudioBuffer> buffer_;

    // Gains the last render() ended on; audio thread only. Negative = not rendered yet.
    float lastLeftGain_ = -1.0f;
    float lastRightGain_ = -1.0f;
};

} // namespace quadmapper
