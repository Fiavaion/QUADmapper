#include "AudioAttachment.hpp"

#include "AudioBuffer.hpp"
#include "AudioDecoder.hpp"
#include "AudioFrame.hpp"
#include "AudioMixer.hpp"
#include "LoopCrossfade.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace quadmapper {

namespace {

constexpr int CHANNELS = 2;
// A decoded frame this much later than the running position is a gap: filled with
// silence. Earlier by more than a frame is an overlap (a seek pre-roll): trimmed.
constexpr double GAP_TOLERANCE_SECONDS = 0.002;
constexpr int MAX_DECODE_FAILURES = 64;
constexpr double MIN_CLIP_DURATION = 1e-3;  // shorter counts as unknown
constexpr double LEG_MATCH_TOLERANCE = 1e-3;
// After a clock discontinuity, drift beyond this is a seek (a loop wrap measures ~0),
// so it is jumped rather than slewed. Well above callback-timestamp jitter.
constexpr double JUMP_RESYNC_THRESHOLD = 0.010;
// The callback cannot wake the decode thread (no system calls on the audio thread),
// so the thread polls for room: every 10 ms normally (the buffer holds 1.5 s), every
// 1 ms for up to ADOPT_POLLS wakes while a new run waits for the callback to skip the
// old one out of its way (not for ever: with no device there are no callbacks).
constexpr auto IDLE_POLL = std::chrono::milliseconds(10);
constexpr auto ADOPT_POLL = std::chrono::milliseconds(1);
constexpr int ADOPT_POLLS = 100;
constexpr size_t SILENCE_CHUNK_FRAMES = 4096;  // silence is pushed from a fixed scratch
constexpr size_t END_FADE_FRAMES = 240;  // 5 ms at 48 kHz
constexpr double LOOP_CROSSFADE_SECONDS = 0.005;  // equal-power crossfade at a Loop wrap

}  // namespace

struct AudioAttachment::DecodeState {
    explicit DecodeState(int sampleRate)
        : crossfade(static_cast<size_t>(std::llround(LOOP_CROSSFADE_SECONDS * sampleRate)))
        , seamTail(crossfade.frames() * 2, 0.0f) {}

    AudioDecoder decoder;
    std::vector<float> queue;  // decoded samples waiting for buffer space
    size_t queueOffset = 0;
    int64_t silence = 0;       // silent frames to push before the queue (gaps, padding)
    std::vector<float> zeros = std::vector<float>(SILENCE_CHUNK_FRAMES * 2, 0.0f);
    int adoptPolls = 0;
    bool endPublished = false;
    int64_t clipFrame = 0;     // clip position (in frames) of the next queued frame
    int64_t legEndFrame = std::numeric_limits<int64_t>::max();
    uint64_t segmentStart = 0; // buffer position of the current run's first sample
    bool loop = false;
    bool haveSegment = false;
    bool passEnded = false;
    int failures = 0;
    // Loop wrap: the outgoing sound past the clip's end (from the frame the pass was
    // cut in) and the last frame queued, for the crossfade into the next pass.
    LoopCrossfade crossfade;
    std::vector<float> seamTail;
    size_t seamTailFrames = 0;
    float lastQueued[2] = {0.0f, 0.0f};
};

AudioAttachment::AudioAttachment(std::filesystem::path path, AudioMixer& mixer)
    : path_(std::move(path))
    , mixer_(mixer)
    , sampleRate_(mixer.getSampleRate())
    , buffer_(std::make_shared<AudioBuffer>(
          static_cast<size_t>(BUFFER_SECONDS * mixer.getSampleRate()) * CHANNELS))
    , source_(std::make_shared<SyncedAudioSource>(buffer_, mixer.getSampleRate()))
    , decode_(std::make_unique<DecodeState>(mixer.getSampleRate())) {
    source_->setName(path_.filename().u8string());
    thread_ = std::thread(&AudioAttachment::decodeLoop, this);
}

AudioAttachment::~AudioAttachment() {
    // 1. Out of the mix: once removeSource() returns no callback can reach source_.
    if (registered_) mixer_.removeSource(source_->getId());
    // 2. Stop the decode thread, the only other user of buffer_ and the decoder.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
    // 3. The members (source, buffer, decoder) are freed here, on this thread.
}

bool AudioAttachment::isDecoderIdle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    // Waiting, with nothing to decode: the pass is over, or the buffer is full now
    // (checked live: the callback may have made room since the thread went to sleep).
    return !pending_ && waiting_.load() && (streamDone_.load() || buffer_->freeSpace() < CHANNELS);
}

double AudioAttachment::bufferedSeconds() const {
    return static_cast<double>(buffer_->available() / CHANNELS) / sampleRate_;
}

// ---- Control thread -----------------------------------------------------------

bool AudioAttachment::legMatches(double legBase) const {
    const double offset = legBase - (requested_.timeline - requested_.clip);
    if (requested_.loop && requested_.duration > MIN_CLIP_DURATION) {
        // The decode thread runs from pass to pass, so any whole number of passes on.
        const double rem = std::fmod(std::abs(offset), requested_.duration);
        return std::min(rem, requested_.duration - rem) < LEG_MATCH_TOLERANCE;
    }
    return std::abs(offset) < LEG_MATCH_TOLERANCE;
}

void AudioAttachment::requestResync(double timeline, double clip, const AudioTransport& transport,
                                    const char* reason) {
    LOG_DEBUG("Audio resync ({}): {} -> timeline {} clip {}", reason, path_.filename().u8string(), timeline, clip);
    Request request;
    request.generation = ++generation_;
    request.timeline = timeline;
    request.clip = clip;
    request.duration = transport.duration;
    request.loop = transport.loop;
    requested_ = request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_ = request;
        pending_ = true;
    }
    wake_.notify_one();
    drift_.reset();
    source_->setCorrection(0);
    unalignedSinceNs_.reset();
    ++resyncCount_;
}

void AudioAttachment::update(const AudioTransport& transport) {
    source_->setVolume(transport.volume);
    source_->setMuted(transport.muted || transport.gated);
    lastDrift_ = std::numeric_limits<double>::quiet_NaN();
    if (state_.load() != AudioAttachmentState::Ready) return;
    if (!registered_) {
        mixer_.addSource(source_);
        registered_ = true;
    }

    const double latency = mixer_.getOutputLatency();
    const bool jumped = haveDiscontinuity_ && transport.discontinuity != lastDiscontinuity_;
    lastDiscontinuity_ = transport.discontinuity;
    haveDiscontinuity_ = true;
    const bool mappingChanged = generation_ == 0 || transport.duration != requested_.duration ||
                                transport.loop != requested_.loop;
    const double legBase = transport.timeline - transport.clip;
    const AudioPlayhead head = source_->playhead();
    const bool current = head.generation == generation_;

    if (transport.audible) {
        pausedPrimed_ = false;
        const char* resync = mappingChanged ? "new mapping" : !legMatches(legBase) ? "moved pass" : nullptr;
        if (!resync && current && head.aligned && head.atEnd) {
            // Played to the end of the audio while the picture runs on (a track shorter
            // than its clip, length unknown): nothing to keep in step until a seek.
            unalignedSinceNs_.reset();
            drift_.reset();
            source_->setCorrection(0);
            if (jumped) resync = "seek";
        } else if (!resync && current && head.aligned) {
            unalignedSinceNs_.reset();
            // The sample heard now: the callback's next sample, advanced by the time
            // since, less the device latency.
            const double heard = head.nextTimeline +
                static_cast<double>(transport.wallNs - head.nextWallNs) * 1e-9 - latency;
            const double drift = heard - transport.timeline;
            lastDrift_ = drift;
            if (jumped && std::abs(drift) > JUMP_RESYNC_THRESHOLD) {
                resync = "seek";  // jump there rather than slew at 0.1 %
            } else {
                const AudioDriftDecision decision = drift_.update(drift);
                if (decision.hardResync) resync = "drift over 80 ms";
                else source_->setCorrection(decision.correction);
            }
        } else if (!resync) {
            // Lining up (after a resync, a resume or an underrun). The callback does it
            // from the buffer; resync when the buffer cannot get it there.
            drift_.reset();
            source_->setCorrection(0);
            if (!current) {
                // The decoder has not answered the last request yet (a slow seek):
                // waiting, not failing to line up.
                unalignedSinceNs_.reset();
            } else {
                const double gap = head.nextTimeline - (transport.timeline + latency);
                const double reach = static_cast<double>(buffer_->capacity() / CHANNELS) / sampleRate_;
                if (gap > AudioDriftController::HARD_THRESHOLD || gap < -reach) {
                    resync = "buffer out of reach";
                } else if (!unalignedSinceNs_ || head.callbacks == unalignedCallbacks_) {
                    // Start (or, with no callbacks running, keep restarting) the timeout.
                    unalignedSinceNs_ = transport.wallNs;
                    unalignedCallbacks_ = head.callbacks;
                } else if (static_cast<double>(transport.wallNs - *unalignedSinceNs_) * 1e-9 > ALIGN_TIMEOUT) {
                    resync = "lining up timed out";
                }
            }
        }
        if (resync) requestResync(transport.timeline, transport.clip, transport, resync);
    } else {
        drift_.reset();
        source_->setCorrection(0);
        unalignedSinceNs_.reset();
        if (!transport.running) {
            // Paused: keep the buffer primed at the paused position, so resuming plays
            // at once and in step. A pause alone leaves the buffer close enough.
            const bool farFromResume = current &&
                std::abs(head.nextTimeline - (transport.timeline + latency)) >
                    AudioDriftController::HARD_THRESHOLD;
            if (mappingChanged || jumped || (farFromResume && !pausedPrimed_)) {
                requestResync(transport.timeline, transport.clip, transport, "prime paused position");
                pausedPrimed_ = true;
            }
        } else {
            pausedPrimed_ = false;
            // Silent while running (non-1x, reversing): nothing until it is audible,
            // except that a known upcoming forward leg is pre-rolled.
            if (transport.nextAudibleStart &&
                *transport.nextAudibleStart - transport.timeline < PREROLL_LEAD &&
                std::abs(requested_.timeline - *transport.nextAudibleStart) > LEG_MATCH_TOLERANCE) {
                requestResync(*transport.nextAudibleStart, 0.0, transport, "pre-roll forward leg");
            }
        }
    }

    AudioTransportAnchor anchor;
    anchor.generation = generation_;
    anchor.audible = transport.audible;
    anchor.timeline = transport.timeline;
    anchor.wallNs = transport.wallNs;
    source_->publishTransport(anchor);
}

// ---- Decode thread ------------------------------------------------------------

void AudioAttachment::decodeLoop() {
    DecodeState& d = *decode_;
    if (!d.decoder.open(path_, sampleRate_, CHANNELS)) {
        streamDone_ = true;
        waiting_ = true;
        state_ = AudioAttachmentState::NoAudio;
        return;
    }
    state_ = AudioAttachmentState::Ready;

    for (;;) {
        Request request;
        bool haveRequest = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!stop_ && !pending_) {
                const bool queueEmpty = d.queueOffset == d.queue.size() && d.silence == 0;
                streamDone_ = !d.haveSegment || (queueEmpty && d.passEnded);
                waiting_ = true;
                const bool adopting = d.haveSegment && buffer_->readPosition() < d.segmentStart &&
                                      d.adoptPolls < ADOPT_POLLS;
                if (adopting) ++d.adoptPolls;
                wake_.wait_for(lock, adopting ? ADOPT_POLL : IDLE_POLL, [&] { return stop_ || pending_; });
                waiting_ = false;
            }
            if (stop_) break;
            if (pending_) {
                request = request_;
                pending_ = false;
                haveRequest = true;
                waiting_ = false;
            }
        }
        if (haveRequest) startSegment(request);
        if (d.haveSegment) produce();
    }
}

void AudioAttachment::startSegment(const Request& request) {
    DecodeState& d = *decode_;
    const double clip = std::max(0.0, request.clip);
    d.decoder.seek(clip);
    d.queue.clear();
    d.queueOffset = 0;
    d.silence = 0;
    d.adoptPolls = 0;
    d.endPublished = false;
    d.legEndFrame = request.duration > MIN_CLIP_DURATION
        ? std::llround(request.duration * sampleRate_)
        : std::numeric_limits<int64_t>::max();
    d.clipFrame = std::min<int64_t>(std::llround(clip * sampleRate_), d.legEndFrame);
    d.loop = request.loop && request.duration > MIN_CLIP_DURATION;
    d.haveSegment = true;
    d.passEnded = false;
    d.failures = 0;
    d.crossfade.disarm();
    d.seamTailFrames = 0;
    d.lastQueued[0] = d.lastQueued[1] = 0.0f;

    AudioSegment segment;
    segment.generation = request.generation;
    segment.startPosition = buffer_->writePosition();
    d.segmentStart = segment.startPosition;
    segment.timeline = (request.timeline - request.clip) +
                       static_cast<double>(d.clipFrame) / sampleRate_;
    source_->clearStreamEnd();  // before the segment: the callback must not see the old end
    source_->publishSegment(segment);
}

void AudioAttachment::produce() {
    DecodeState& d = *decode_;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_ || stop_) return;
        }
        if (d.silence > 0) {  // silence goes before the queued samples
            const int64_t room = static_cast<int64_t>(buffer_->freeSpace() / CHANNELS);
            const int64_t count = std::min({d.silence, room, static_cast<int64_t>(SILENCE_CHUNK_FRAMES)});
            if (count == 0) return;  // full
            push(d.zeros.data(), static_cast<size_t>(count));
            d.silence -= count;
            continue;
        }
        if (d.queueOffset < d.queue.size()) {
            const size_t space = buffer_->freeSpace() / CHANNELS * CHANNELS;
            if (space == 0) return;  // full: wait for the callback to consume
            const size_t count = std::min(space, d.queue.size() - d.queueOffset);
            push(d.queue.data() + d.queueOffset, count / CHANNELS);
            d.queueOffset += count;
            if (d.queueOffset == d.queue.size()) {
                d.queue.clear();
                d.queueOffset = 0;
            }
            continue;
        }
        if (d.passEnded) {
            if (!d.endPublished) {  // everything is pushed: this is where the run ends
                source_->publishStreamEnd(buffer_->writePosition());
                d.endPublished = true;
            }
            return;
        }
        if (d.clipFrame >= d.legEndFrame) {
            endOfPass();
            continue;
        }

        AudioFrame frame = d.decoder.decodeNextFrame();
        if (!frame.isValid() || frame.channels != CHANNELS) {
            if (d.decoder.isAtEnd() || ++d.failures > MAX_DECODE_FAILURES) endOfPass();
            continue;
        }
        d.failures = 0;

        // Place the frame at its own timestamp: pad a gap, trim an overlap.
        const int64_t framePos = std::llround(frame.pts * sampleRate_);
        const int64_t tolerance = std::llround(GAP_TOLERANCE_SECONDS * sampleRate_);
        const float* samples = frame.samples.data();
        int64_t frames = frame.sampleCount;
        if (framePos > d.clipFrame + tolerance) {
            queueSilence(framePos - d.clipFrame);
        } else if (framePos < d.clipFrame - 1) {
            const int64_t skip = d.clipFrame - framePos;
            if (skip >= frames) continue;
            samples += skip * CHANNELS;
            frames -= skip;
        }
        queueFrames(samples, frames);
    }
}

// Push `frames` frames to the buffer (the caller checked there is room), mixing the
// Loop-wrap crossfade into the first frames of a new pass.
void AudioAttachment::push(const float* samples, size_t frames) {
    LoopCrossfade& crossfade = decode_->crossfade;
    while (frames > 0) {
        size_t count = frames;
        const float* out = samples;
        if (crossfade.delay() > 0) {  // the outgoing pass's padding: unchanged
            count = std::min(count, crossfade.delay());
            crossfade.skip(count);
        } else if (crossfade.remaining() > 0) {
            count = std::min(count, crossfade.remaining());
            out = crossfade.mix(samples, count);
        }
        buffer_->push(out, count * CHANNELS);
        samples += count * CHANNELS;
        frames -= count;
    }
}

void AudioAttachment::queueFrames(const float* samples, int64_t frames) {
    DecodeState& d = *decode_;
    const int64_t room = d.legEndFrame - d.clipFrame;  // a pass ends at the clip's end
    const bool cut = frames > room;
    if (cut && d.loop && room >= 0) {
        // The sound past the clip's end: the outgoing side of the wrap's crossfade.
        d.seamTailFrames = static_cast<size_t>(
            std::min<int64_t>(frames - room, static_cast<int64_t>(d.crossfade.frames())));
        std::copy(samples + room * CHANNELS,
                  samples + (room + static_cast<int64_t>(d.seamTailFrames)) * CHANNELS, d.seamTail.begin());
    }
    frames = std::min(frames, room);
    if (frames <= 0) return;
    d.queue.insert(d.queue.end(), samples, samples + frames * CHANNELS);
    d.clipFrame += frames;
    d.lastQueued[0] = samples[(frames - 1) * CHANNELS];
    d.lastQueued[1] = samples[(frames - 1) * CHANNELS + 1];
    if (cut && !d.loop) {
        // The sound runs on past the clip's end and this pass stops here (Once, a
        // Ping-Pong forward leg): fade its last few milliseconds so the cut does not
        // click. (A Loop pass runs straight into the next, as the picture does.)
        const size_t fade = std::min<size_t>(END_FADE_FRAMES, (d.queue.size() - d.queueOffset) / CHANNELS);
        float* tail = d.queue.data() + d.queue.size() - fade * CHANNELS;
        for (size_t i = 0; i < fade; ++i) {
            const float gain = static_cast<float>(fade - 1 - i) / static_cast<float>(fade);
            tail[i * CHANNELS] *= gain;
            tail[i * CHANNELS + 1] *= gain;
        }
    }
}

// Only called with the queue empty (a frame is decoded only then), so counting the
// silence and pushing it before the queue keeps the order.
void AudioAttachment::queueSilence(int64_t frames) {
    DecodeState& d = *decode_;
    frames = std::min(frames, d.legEndFrame - d.clipFrame);
    if (frames <= 0) return;
    d.silence += frames;
    d.clipFrame += frames;
    d.lastQueued[0] = d.lastQueued[1] = 0.0f;
}

void AudioAttachment::endOfPass() {
    DecodeState& d = *decode_;
    // Audio shorter than the clip: fill to the clip's end, so the callback plays on in
    // step (silently) until the pass ends instead of running dry.
    if (d.legEndFrame != std::numeric_limits<int64_t>::max()) queueSilence(d.legEndFrame - d.clipFrame);
    if (!d.loop || d.legEndFrame <= 0) {
        d.passEnded = true;
        return;
    }
    // Loop: run on into the next pass. The timeline stays contiguous, so the wrap needs
    // no resync. The next pass fades in over the outgoing sound (equal power, same
    // sample count), starting once this pass's padding and queue are pushed.
    const size_t pending = static_cast<size_t>(d.silence) + (d.queue.size() - d.queueOffset) / CHANNELS;
    d.crossfade.arm(d.seamTail.data(), d.seamTailFrames, d.lastQueued, pending,
                    static_cast<size_t>(d.legEndFrame));
    d.seamTailFrames = 0;
    d.decoder.seek(0.0);
    d.clipFrame = 0;
    d.failures = 0;
}

}  // namespace quadmapper
