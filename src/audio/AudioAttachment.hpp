#pragma once

#include "AudioDriftController.hpp"
#include "SyncedAudioSource.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace quadmapper {

class AudioBuffer;
class AudioMixer;

// A source's playback clock as the audio needs it, sampled once per frame by the
// control thread (the app builds it from the source's PlaybackClock).
struct AudioTransport {
    int64_t wallNs = 0;           // steady_clock instant the fields describe, in ns
    double timeline = 0.0;        // unwrapped media seconds (continuous across loop wraps)
    double clip = 0.0;            // position in the clip
    uint64_t discontinuity = 0;   // the clock's break counter (seeks, wraps, reflections)
    bool running = false;         // false while paused
    bool audible = false;         // running forward at 1x and not ended
    double duration = 0.0;        // clip length; <= 0 when unknown
    bool loop = false;            // Loop mode: the audio runs on across the wrap
    // Timeline at which playback next becomes audible when that is known ahead (the
    // next forward leg while a Ping-Pong reverse leg plays at 1x).
    std::optional<double> nextAudibleStart;
    float volume = 1.0f;
    bool muted = false;
    // Not in a scene anyone sees or hears: silent (the gain ramps to 0), but kept in
    // step so it comes back in sync when its scene is taken.
    bool gated = false;
    // Level of the scenes it plays in, 0-1: below 1 while they cross-fade (ADR-047
    // §5). Multiplies the volume, so the mixer's per-buffer gain ramp smooths it.
    float sceneGain = 1.0f;
};

enum class AudioAttachmentState {
    Probing,  // opening the file on the decode thread
    NoAudio,  // the file has no decodable audio stream
    Ready,
};

/**
 * @brief The audio of one video source: its own decoder, slaved to the source's clock
 *
 * ADR-050 §2. Owns an AudioDecoder (its own demux of the file) on a decode thread,
 * which feeds a SyncedAudioSource registered with the app's mixer.
 *
 * update() runs once per frame on the control thread. It publishes the clock to the
 * audio thread and measures drift = audio time - clock time, where audio time is the
 * timeline of the sample being heard (the callback's position plus the time since,
 * minus the device latency):
 *   - |drift| > 80 ms, a seek (a discontinuity with |drift| > 10 ms) or a mapping
 *     change (duration, loop mode): hard resync —
 *     the decode thread flushes, seeks to the clock and pre-rolls; the audio thread
 *     skips to the new run and lines up sample-exactly;
 *   - 2-80 ms: soft correction, one sample in 1000 dropped or repeated by the callback;
 *   - otherwise nothing.
 * While paused the buffer is kept primed at the paused position so playback resumes
 * in step at once; a Ping-Pong reverse leg pre-rolls the next forward leg.
 *
 * In Loop mode the decode thread cuts or pads each pass to the clip duration and runs
 * straight on into the next, so the wrap needs no resync; the first 5 ms of each new
 * pass is an equal-power crossfade from the outgoing sound (LoopCrossfade), so the
 * seam does not click.
 *
 * Destruction order (the teardown the audio thread relies on): the source is removed
 * from the mixer first — removeSource() returns only once no callback can reach it —
 * then the decode thread is joined, then the source, buffer and decoder are freed, all
 * on the control thread.
 */
class AudioAttachment {
public:
    AudioAttachment(std::filesystem::path path, AudioMixer& mixer);
    ~AudioAttachment();

    AudioAttachment(const AudioAttachment&) = delete;
    AudioAttachment& operator=(const AudioAttachment&) = delete;

    void update(const AudioTransport& transport);

    AudioAttachmentState state() const { return state_.load(); }
    const std::filesystem::path& path() const { return path_; }

    // --- Diagnostics (and tests) ---

    AudioPlayhead playhead() const { return source_->playhead(); }
    // The volume and mute the last update() gave the mixer input (the gain the
    // callbacks ramp to: 0 while muted, else the volume).
    float volume() const { return source_->getVolume(); }
    bool muted() const { return source_->isMuted(); }
    // Drift measured by the last update(), in seconds; NaN when nothing was measurable.
    double lastDrift() const { return lastDrift_; }
    uint64_t resyncCount() const { return resyncCount_; }
    // Decode thread idle: no request pending and the buffer full or the pass ended.
    bool isDecoderIdle() const;
    // Seconds of decoded sound waiting in the buffer.
    double bufferedSeconds() const;

    static constexpr double BUFFER_SECONDS = 1.5;
    static constexpr double PREROLL_LEAD = 0.5;   // pre-roll a forward leg this early
    static constexpr double ALIGN_TIMEOUT = 0.3;  // resync if lining up takes longer

private:
    struct Request {
        uint32_t generation = 0;
        double timeline = 0.0;
        double clip = 0.0;
        double duration = 0.0;
        bool loop = false;
    };

    // Control thread.
    void requestResync(double timeline, double clip, const AudioTransport& transport, const char* reason);
    // Does the clock's current pass start where the requested run puts it?
    bool legMatches(double legBase) const;

    // Decode thread.
    void decodeLoop();
    void startSegment(const Request& request);
    // Push queued samples, decoding more as the queue empties; returns when the buffer
    // is full, the pass ended or a request arrived.
    void produce();
    void push(const float* samples, size_t frames);
    void queueFrames(const float* samples, int64_t frames);
    void queueSilence(int64_t frames);
    void endOfPass();

    const std::filesystem::path path_;
    AudioMixer& mixer_;
    const int sampleRate_;
    std::shared_ptr<AudioBuffer> buffer_;
    std::shared_ptr<SyncedAudioSource> source_;
    std::atomic<AudioAttachmentState> state_{AudioAttachmentState::Probing};

    // Control thread state.
    bool registered_ = false;
    uint32_t generation_ = 0;
    Request requested_;           // the last request made
    uint64_t lastDiscontinuity_ = 0;
    bool haveDiscontinuity_ = false;
    bool pausedPrimed_ = false;
    std::optional<int64_t> unalignedSinceNs_;
    uint64_t unalignedCallbacks_ = 0;
    AudioDriftController drift_;
    double lastDrift_ = 0.0;
    uint64_t resyncCount_ = 0;

    // Control <-> decode thread.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    bool pending_ = false;
    Request request_;
    std::atomic<bool> waiting_{false};     // decode thread asleep between polls
    std::atomic<bool> streamDone_{false};  // no segment yet, or the pass ended and was pushed
    std::thread thread_;

    // Decode thread only (the decoder lives in the .cpp).
    struct DecodeState;
    std::unique_ptr<DecodeState> decode_;
};

}  // namespace quadmapper
