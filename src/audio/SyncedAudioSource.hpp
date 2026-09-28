#pragma once

#include "AudioSource.hpp"
#include "SeqLock.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace quadmapper {

// Where a run of decoded audio sits on the source's timeline. The decode thread
// publishes one per resync request, before pushing the run's first sample.
struct AudioSegment {
    uint32_t generation = 0;     // the resync request it answers (0 = none yet)
    uint64_t startPosition = 0;  // AudioBuffer write position of its first sample
    double timeline = 0.0;       // timeline seconds of that sample
};

// What the audio thread should play, from the control thread once per frame.
struct AudioTransportAnchor {
    uint32_t generation = 0;     // play only the segment answering this request
    bool audible = false;        // playing forward at 1x
    double timeline = 0.0;       // the clock's timeline at wallNs...
    int64_t wallNs = 0;          // ...so the timeline at any instant is extrapolated at 1x
};

// What the audio thread did, for the control thread's drift measurement.
struct AudioPlayhead {
    uint32_t generation = 0;     // segment being played from
    bool aligned = false;        // playing in step with the transport
    double nextTimeline = 0.0;   // timeline of the next sample to be consumed...
    int64_t nextWallNs = 0;      // ...which is written at this wall time (heard latency later)
    bool atEnd = false;          // played to the end of the run (the pass is over)
    uint64_t callbacks = 0;
    uint64_t underruns = 0;      // times the buffer ran dry while playing
    uint64_t corrections = 0;    // samples dropped or repeated
};

/**
 * @brief Mixer input that plays a decoded stream in step with a playback clock
 *
 * The audio half of an AudioAttachment (ADR-050 §2). Three single-writer channels,
 * none with a lock:
 *   decode thread -> audio thread   AudioSegment (seqlock) + the AudioBuffer samples
 *   control       -> audio thread   AudioTransportAnchor (seqlock), correction (atomic)
 *   audio thread  -> control        AudioPlayhead (seqlock)
 *
 * On the audio thread, pull():
 *   - adopts a newly published segment by skipping the buffer to its first sample;
 *   - plays only while the transport is audible and names the adopted segment;
 *   - lines up sample-exactly when it starts (after a resync, resume or underrun): the
 *     frame written now is heard outputLatency later, so it must be the sample at
 *     transport time (now + latency); earlier samples are skipped, or silence is
 *     written until the stream starts;
 *   - while playing, drops (correction -1) or repeats (+1) one frame in every
 *     CORRECTION_INTERVAL, the soft correction the control thread asks for;
 *   - fades in when it starts and fades out over one buffer when it stops, so a
 *     start, stop or seek never clicks.
 * The buffer carries interleaved stereo at the rate given to the constructor.
 */
class SyncedAudioSource : public AudioSource {
public:
    static constexpr size_t CORRECTION_INTERVAL = 1000;

    SyncedAudioSource(std::shared_ptr<AudioBuffer> stereoBuffer, int sampleRate);

    // Decode thread. Before a segment, clearStreamEnd(); once its last sample is pushed,
    // publishStreamEnd(write position): running out there is the end, not an underrun.
    void publishSegment(const AudioSegment& segment) { segment_.store(segment); }
    void clearStreamEnd() { streamEnd_.store(NO_END, std::memory_order_release); }
    void publishStreamEnd(uint64_t position) { streamEnd_.store(position, std::memory_order_release); }

    // Control thread.
    void publishTransport(const AudioTransportAnchor& anchor) { transport_.store(anchor); }
    // +1: repeat a frame per interval (audio is ahead); -1: drop one (behind); 0: none.
    void setCorrection(int direction) { correction_.store(direction > 0 ? 1 : direction < 0 ? -1 : 0); }
    AudioPlayhead playhead() const { return playhead_.load(); }

protected:
    void pull(float* output, size_t frameCount, const AudioRenderContext& context) override;

private:
    double nextTimeline() const;
    // Copy up to frameCount frames of the stream, applying the soft correction.
    // Returns the frames written; fewer than asked means the buffer ran dry.
    size_t fillFromStream(float* output, size_t frameCount);
    void report(size_t frameCount, const AudioRenderContext& context);

    std::shared_ptr<AudioBuffer> stream_;
    const int sampleRate_;

    static constexpr uint64_t NO_END = ~uint64_t{0};

    SeqLockValue<AudioSegment> segment_;
    std::atomic<uint64_t> streamEnd_{NO_END};
    SeqLockValue<AudioTransportAnchor> transport_;
    std::atomic<int> correction_{0};
    SeqLockValue<AudioPlayhead> playhead_;

    // Audio thread only.
    AudioTransportAnchor transportCache_;
    uint32_t adoptedGeneration_ = 0;
    uint64_t segmentStart_ = 0;
    double segmentTimeline_ = 0.0;
    bool aligned_ = false;
    bool atEnd_ = false;
    size_t untilCorrection_ = CORRECTION_INTERVAL;
    float lastFrame_[2] = {0.0f, 0.0f};
    int64_t lastCallbackNs_ = 0;
    uint64_t callbacks_ = 0;
    uint64_t underruns_ = 0;
    uint64_t corrections_ = 0;
};

}  // namespace quadmapper
