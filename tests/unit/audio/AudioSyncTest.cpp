// Audio from video files kept on the playback clock (ADR-050 §2), end to end: a real
// decode of a real file, the real mixer path, a real PlaybackClock.
//
// Sync is measured from the sound itself, not from the attachment's own bookkeeping:
// the file is decoded once more as a reference, and each window of mixed output is
// cross-correlated against the reference around the clip position the clock says is
// being heard (callback time + output latency). The lag of the best match is the sync
// error.
//
// Offline tests drive AudioMixer::renderOffline() with simulated time (a minute of
// playback in a second or two); one test runs the real device thread (null backend)
// in real time. Media from D:\testing Videos; the tests skip when it is absent.

#include <gtest/gtest.h>

#include "app/SourceAudio.hpp"
#include "audio/AudioAttachment.hpp"
#include "audio/AudioDecoder.hpp"
#include "audio/AudioDevice.hpp"
#include "audio/AudioEngine.hpp"
#include "audio/AudioFrame.hpp"
#include "audio/AudioMixer.hpp"
#include "audio/LoopCrossfade.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Scene.hpp"
#include "video/PlaybackClock.hpp"
#include "AudioTestMedia.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <optional>
#include <unordered_set>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <process.h>  // _getpid
#endif

namespace quadmapper {
namespace test {

namespace {

constexpr int RATE = AudioMixer::SAMPLE_RATE;
// Real sound all the way through (several of the test clips carry a silent track).
const char* const SYNC_CLIP = "keying woman.mp4";
constexpr double CLIP_DURATION = 18.151467;  // its video stream, which the clock uses
// A centred source's constant-power pan gain: the output level relative to the file.
constexpr float PAN_GAIN = 0.70710678f;
constexpr size_t BLOCK = 480;  // 10 ms, the device period
using Clock = std::chrono::steady_clock;

Clock::time_point at(Clock::time_point base, double seconds) {
    return base + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

int64_t toNs(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// The whole stream at the mixer's format, sample i at clip time i / RATE.
struct Reference {
    std::vector<float> stereo;
    int64_t frames() const { return static_cast<int64_t>(stereo.size() / 2); }

    static Reference decode(const std::filesystem::path& path) {
        Reference ref;
        AudioDecoder decoder;
        if (!decoder.open(path, RATE, 2)) return ref;
        for (AudioFrame f = decoder.decodeNextFrame(); f.isValid(); f = decoder.decodeNextFrame()) {
            const int64_t pos = std::llround(f.pts * RATE);
            if (pos < 0) continue;
            if (static_cast<size_t>(pos) * 2 > ref.stereo.size()) ref.stereo.resize(static_cast<size_t>(pos) * 2, 0.0f);
            ref.stereo.resize(static_cast<size_t>(pos) * 2);
            ref.stereo.insert(ref.stereo.end(), f.samples.begin(), f.samples.end());
        }
        return ref;
    }
};

// One rendered callback.
struct Block {
    int64_t wallNs = 0;
    std::vector<float> samples;
};

struct SyncStats {
    int windows = 0;          // windows measured
    int unmatched = 0;        // audible windows with no confident match
    int quiet = 0;            // audible windows too quiet to measure
    int dropouts = 0;         // quiet output where the reference is clearly not quiet
    double maxError = 0.0;    // largest |lag|, seconds
    double sumError = 0.0;
    int silentChecked = 0;    // windows that had to be silent
    int silentViolations = 0; // ...and were not
    double meanError() const { return windows ? sumError / windows : 0.0; }
};

// The clock as it was over time: a copy at the start and after every transport event.
// PlaybackClock is a value type, so a copy extrapolates exactly as the original did.
struct ClockHistory {
    std::vector<std::pair<Clock::time_point, PlaybackClock>> snapshots;

    void record(Clock::time_point when, const PlaybackClock& clock) { snapshots.emplace_back(when, clock); }

    std::vector<Clock::time_point> times() const {
        std::vector<Clock::time_point> t;
        for (const auto& s : snapshots) t.push_back(s.first);
        return t;
    }

    // The clip position heard at `heard`, when audible then.
    std::optional<double> expect(Clock::time_point heard) const {
        const PlaybackClock* clock = nullptr;
        for (const auto& s : snapshots) {
            if (s.first <= heard) clock = &s.second;
        }
        if (!clock) return std::nullopt;
        const AudioTransport t = audioTransportOf(*clock, heard);
        if (!t.audible) return std::nullopt;
        return t.clip;
    }
};

// Correlate `window` frames of output starting at output frame `start` with the
// reference around clip position `clip`; returns the lag (seconds) of the best match,
// or nullopt when the window is quiet or nothing matches convincingly.
std::optional<double> lagOf(const std::vector<float>& out, size_t start, size_t window,
                            const Reference& ref, double clip, double period, double* bestScore = nullptr,
                            double energyScale = 1.0) {
    double outEnergy = 0.0;
    for (size_t k = 0; k < window; ++k) {
        outEnergy += out[(start + k) * 2] * out[(start + k) * 2] + out[(start + k) * 2 + 1] * out[(start + k) * 2 + 1];
    }
    const int64_t periodFrames = period > 0.0 ? std::llround(period * RATE) : 0;
    auto refAt = [&](int64_t frame, int ch) -> float {
        if (periodFrames > 0) frame = ((frame % periodFrames) + periodFrames) % periodFrames;
        if (frame < 0 || frame >= ref.frames()) return 0.0f;
        return ref.stereo[frame * 2 + ch];
    };
    const int64_t centre = std::llround(clip * RATE);
    if (outEnergy < 1e-4 * energyScale) {
        // Quiet output. Report how loud the reference is there (a dropout if it is not
        // quiet too): -(reference energy) through bestScore.
        double refHere = 0.0;
        for (size_t k = 0; k < window; ++k) {
            const float l = refAt(centre + static_cast<int64_t>(k), 0), r = refAt(centre + static_cast<int64_t>(k), 1);
            refHere += l * l + r * r;
        }
        if (bestScore) *bestScore = -refHere;
        return std::nullopt;
    }
    double best = -1.0;
    int64_t bestLag = 0;
    auto scan = [&](int64_t from, int64_t to) {
    for (int64_t lag = from; lag <= to; ++lag) {
        double dot = 0.0, refEnergy = 0.0;
        for (size_t k = 0; k < window; ++k) {
            const int64_t f = centre + static_cast<int64_t>(k) + lag;
            const float l = refAt(f, 0), r = refAt(f, 1);
            dot += out[(start + k) * 2] * l + out[(start + k) * 2 + 1] * r;
            refEnergy += l * l + r * r;
        }
        if (refEnergy <= 1e-9) continue;
        const double score = dot / std::sqrt(outEnergy * refEnergy);
        if (score > best) {
            best = score;
            bestLag = lag;
        }
    }
    };
    // Look within +-3 ms first (where an in-step window matches), then +-50 ms.
    scan(-RATE * 3 / 1000, RATE * 3 / 1000);
    if (best < 0.95) scan(-RATE / 20, RATE / 20);
    if (best < 0.9) {
        if (bestScore) *bestScore = best;
        return std::nullopt;
    }
    if (bestScore) *bestScore = best;
    return static_cast<double>(bestLag) / RATE;
}

// Measure every `stride`-th block. A block is measured only when the clock says the
// whole window is audible and continuous, and it is not within `settle` seconds of a
// transport event; blocks whose whole span is inaudible (and past the one-buffer
// fade-out) must be silent.
SyncStats measure(const std::vector<Block>& blocks, double latency, const Reference& ref, double period,
                  const ClockHistory& history, const std::vector<Clock::time_point>& events,
                  int stride = 5, double settle = 0.15, bool verbose = false, double energyScale = 1.0) {
    SyncStats stats;
    const size_t window = 256;
    for (size_t b = 0; b < blocks.size(); b += stride) {
        const Block& block = blocks[b];
        const Clock::time_point wall = Clock::time_point(std::chrono::nanoseconds(block.wallNs));
        const Clock::time_point heardFirst = at(wall, latency);
        const Clock::time_point heardLast = at(wall, latency + static_cast<double>(BLOCK) / RATE);
        bool nearEvent = false;
        for (const auto& e : events) {
            const double fromEvent = std::chrono::duration<double>(heardFirst - e).count();
            // Before the event's effect reaches the speaker, and settle time after it.
            if (fromEvent > -(latency + 0.03) && fromEvent < settle + latency) nearEvent = true;
        }
        if (nearEvent) continue;
        const auto first = history.expect(heardFirst);
        const auto last = history.expect(heardLast);
        if (!first && !last) {
            ++stats.silentChecked;
            float peak = 0.0f;
            for (float v : block.samples) peak = std::max(peak, std::abs(v));
            if (peak > 0.0f) ++stats.silentViolations;
            continue;
        }
        if (!first || !last) continue;
        if (*last < *first || *last - *first > 0.02) continue;  // a wrap inside the window
        double score = 0.0;
        const auto lag = lagOf(block.samples, 0, window, ref, *first, period, &score, energyScale);
        const double sinceStart = std::chrono::duration<double>(wall - history.snapshots.front().first).count();
        if (!lag && score <= 0.0) {
            ++stats.quiet;
            // The source's gain (constant-power pan) halves the energy; 1e-2 is ~-40 dB RMS.
            if (-score > 1e-2) {
                ++stats.dropouts;
                if (verbose) {
                    std::printf("  dropout at %.3f s (clip %.3f): reference energy %.4f\n", sinceStart, *first,
                                -score);
                }
            }
            continue;
        }
        if (!lag) {
            ++stats.unmatched;
            if (verbose) std::printf("  unmatched at %.3f s (clip %.3f): best score %.3f\n", sinceStart, *first, score);
            continue;
        }
        if (verbose && std::abs(*lag) > 0.002) {
            std::printf("  window at %.3f s (clip %.3f): lag %.2f ms, score %.3f\n", sinceStart, *first,
                        *lag * 1000.0, score);
        }
        ++stats.windows;
        stats.maxError = std::max(stats.maxError, std::abs(*lag));
        stats.sumError += std::abs(*lag);
    }
    return stats;
}

// Real time the offline rig lets the decode thread have before giving up on it.
const auto DECODER_WAIT_LIMIT = std::chrono::seconds(30);

// Simulated time: a 60 Hz control frame (attachment update) and a 10 ms device
// callback that runs `skewPpm` fast or slow against the wall clock.
class OfflineRig {
public:
    explicit OfflineRig(const std::filesystem::path& path, double skewPpm = 0.0)
        : skew_(skewPpm * 1e-6) {
        device_.initialize(/*nullBackend=*/true);
        mixer_.initialize(&device_);  // opened for its latency figure, never started
        attachment_ = std::make_unique<AudioAttachment>(path, mixer_);
        base_ = Clock::now();
        // Probing happens on the decode thread; wait for it like the app's frames would.
        // The deadline only guards a hang: simulated time does not move while waiting,
        // so a loaded machine (ctest -j) just waits longer (TD-23).
        const auto deadline = std::chrono::steady_clock::now() + DECODER_WAIT_LIMIT;
        while (attachment_->state() == AudioAttachmentState::Probing &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    ~OfflineRig() {
        attachment_.reset();
        mixer_.shutdown();
    }

    PlaybackClock clock;
    std::vector<Block> blocks;
    ClockHistory history;
    double maxDrift = 0.0;  // largest |drift| the attachment measured, away from events
    std::function<bool(double)> gated;  // when set: gated silent at these seconds

    Clock::time_point base() const { return base_; }
    Clock::time_point now() const { return at(base_, now_); }
    double latency() const { return mixer_.getOutputLatency(); }
    AudioAttachment& attachment() { return *attachment_; }

    // Advance to `until` seconds, running `onFrame` before each control frame.
    void runUntil(double until, const std::function<void(double)>& onFrame = {}) {
        while (now_ < until) {
            if (nextFrame_ <= nextCallback_) {
                now_ = nextFrame_;
                if (onFrame) onFrame(now_);
                const uint64_t resyncs = attachment_->resyncCount();
                AudioTransport transport = audioTransportOf(clock, now());
                transport.gated = gated && gated(now_);
                attachment_->update(transport);
                const double drift = attachment_->lastDrift();
                if (std::isfinite(drift) && !nearEvent(0.4)) maxDrift = std::max(maxDrift, std::abs(drift));
                if (attachment_->resyncCount() != resyncs) waitForDecoder();
                nextFrame_ += 1.0 / 60.0;
            } else {
                now_ = nextCallback_;
                Block block;
                block.wallNs = toNs(now());
                block.samples.resize(BLOCK * 2);
                mixer_.renderOffline(block.samples.data(), BLOCK, block.wallNs);
                blocks.push_back(std::move(block));
                const uint64_t underruns = attachment_->playhead().underruns;
                if (underruns != underruns_) {
                    std::printf("  underrun at %.3f s (buffered %.3f s)\n", now_, attachment_->bufferedSeconds());
                    underruns_ = underruns;
                }
                nextCallback_ += static_cast<double>(BLOCK) / RATE / (1.0 + skew_);
                // Simulated time runs far faster than real time; give the decoder the
                // real time it would have had (it refills much faster than it plays).
                if (++callbacksSinceWait_ >= 50 || !attachment_->playhead().aligned ||
                    attachment_->bufferedSeconds() < 0.5) {
                    waitForDecoder();
                }
            }
        }
    }

    // Call after every change to the clock.
    void event() { history.record(now(), clock); }

private:
    bool nearEvent(double seconds) const {
        for (const auto& e : history.snapshots) {
            const double d = std::chrono::duration<double>(now() - e.first).count();
            if (d >= 0.0 && d < seconds) return true;
        }
        return false;
    }

    void waitForDecoder() {
        callbacksSinceWait_ = 0;
        const auto deadline = std::chrono::steady_clock::now() + DECODER_WAIT_LIMIT;
        while (!attachment_->isDecoderIdle() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    AudioDevice device_;
    AudioMixer mixer_;
    std::unique_ptr<AudioAttachment> attachment_;
    Clock::time_point base_;
    double skew_ = 0.0;
    double now_ = 0.0;
    double nextFrame_ = 0.0;
    double nextCallback_ = 0.0;
    int callbacksSinceWait_ = 0;
    uint64_t underruns_ = 0;
};

// A second, independent measure: sound onsets. Where the sound starts after at least
// 50 ms of quiet is a sharp, unambiguous landmark (correlation can be fooled by
// periodic sound, a steady tone for one). Every onset in the output is mapped to the
// clip position the clock gave for the instant it was heard, and compared with the
// same onset in the reference. `gain` is the output level relative to the reference
// (the source's pan gain times the master volume), so both cross the same threshold
// on the same sample when they are aligned.
struct OnsetStats {
    int onsets = 0;
    int unpaired = 0;       // output onsets with no reference onset within 60 ms
    double maxError = 0.0;  // seconds; positive errors mean the sound came late
    double sumError = 0.0;
    double meanError() const { return onsets ? sumError / onsets : 0.0; }
};

constexpr float ONSET_LEVEL = 0.02f;   // reference amplitude that counts as sound
constexpr float QUIET_LEVEL = 0.004f;  // ...and as quiet
constexpr int QUIET_FRAMES = RATE / 20;

// Sound starting after at least 50 ms of quiet. The onset is the first sample above
// the sound level within 20 ms of the first one above the quiet level (coded sound can
// creep in for a few milliseconds before it is loud).
struct OnsetDetector {
    float soundLevel;
    float quietLevel;
    int quiet = 0;
    int rising = -1;  // samples since the sound crept in; -1 = not rising

    bool feed(float level) {
        bool onset = false;
        if (rising >= 0) {
            if (level > soundLevel) {
                onset = true;
                rising = -1;
            } else if (++rising > RATE / 50) {
                rising = -1;
            }
        } else if (quiet >= QUIET_FRAMES && level >= quietLevel) {
            if (level > soundLevel) onset = true;
            else rising = 0;
        }
        quiet = level < quietLevel ? quiet + 1 : 0;
        return onset;
    }
};

// `looped`: the clip plays on from its end into its start, so an onset at the very
// start counts when the end is quiet.
std::vector<double> referenceOnsets(const Reference& ref, bool looped) {
    std::vector<double> onsets;
    OnsetDetector detector{ONSET_LEVEL, QUIET_LEVEL};
    for (int64_t n = looped ? std::max<int64_t>(0, ref.frames() - QUIET_FRAMES) : ref.frames(); n < ref.frames(); ++n) {
        detector.feed(std::max(std::abs(ref.stereo[n * 2]), std::abs(ref.stereo[n * 2 + 1])));
    }
    for (int64_t n = 0; n < ref.frames(); ++n) {
        const float level = std::max(std::abs(ref.stereo[n * 2]), std::abs(ref.stereo[n * 2 + 1]));
        if (detector.feed(level)) onsets.push_back(static_cast<double>(n) / RATE);
    }
    return onsets;
}

OnsetStats measureOnsets(const std::vector<Block>& blocks, double latency, const Reference& ref, double period,
                         const ClockHistory& history, const std::vector<Clock::time_point>& events,
                         float gain, double settle = 0.3) {
    OnsetStats stats;
    const std::vector<double> refOnsets = referenceOnsets(ref, period > 0.0);
    OnsetDetector detector{ONSET_LEVEL * gain, QUIET_LEVEL * gain};
    for (const Block& block : blocks) {
        const Clock::time_point wall = Clock::time_point(std::chrono::nanoseconds(block.wallNs));
        for (size_t k = 0; k < BLOCK; ++k) {
            const float level = std::max(std::abs(block.samples[k * 2]), std::abs(block.samples[k * 2 + 1]));
            if (!detector.feed(level)) continue;
            const Clock::time_point heard = at(wall, latency + static_cast<double>(k) / RATE);
            bool nearEvent = false;
            for (const auto& e : events) {
                const double fromEvent = std::chrono::duration<double>(heard - e).count();
                if (fromEvent > -(latency + 0.03) && fromEvent < settle + latency) nearEvent = true;
            }
            if (nearEvent) continue;
            const auto clip = history.expect(heard);
            if (!clip) continue;
            double best = 1.0;
            for (double r : refOnsets) {
                double d = *clip - r;
                if (period > 0.0) d -= period * std::round(d / period);
                if (std::abs(d) < std::abs(best)) best = d;
            }
            if (std::abs(best) > 0.060) {
                ++stats.unpaired;
                std::printf("  unpaired onset heard at clip %.4f (%.3f s after start)\n", *clip,
                            std::chrono::duration<double>(heard - history.snapshots.front().first).count());
                continue;
            }
            ++stats.onsets;
            stats.maxError = std::max(stats.maxError, std::abs(best));
            stats.sumError += std::abs(best);
        }
    }
    return stats;
}

void reportOnsets(const char* name, const OnsetStats& s) {
    std::printf("[ ONSETS   ] %s: %d onsets, max |error| %.2f ms, mean %.3f ms, %d unpaired\n", name, s.onsets,
                s.maxError * 1000.0, s.meanError() * 1000.0, s.unpaired);
}

void report(const char* name, const SyncStats& s, double maxDrift, uint64_t resyncs, const AudioPlayhead& head) {
    std::printf("[ SYNC     ] %s: %d windows, max |error| %.2f ms, mean %.3f ms, %d unmatched, %d quiet (%d dropouts), "
                "%d silent windows (%d not silent), drift max %.2f ms, %llu resyncs, %llu corrections, "
                "%llu underruns\n",
                name, s.windows, s.maxError * 1000.0, s.meanError() * 1000.0, s.unmatched, s.quiet, s.dropouts,
                s.silentChecked,
                s.silentViolations, maxDrift * 1000.0, static_cast<unsigned long long>(resyncs),
                static_cast<unsigned long long>(head.corrections), static_cast<unsigned long long>(head.underruns));
}

}  // namespace

// A simulated minute of Loop playback on a device running 200 ppm fast: pauses,
// seeks while playing and while paused, 2x (silent) and back, twelve loop wraps.
TEST(AudioSyncTest, TracksTheClockWithinTwentyMillisecondsOverASimulatedMinute) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    const Reference ref = Reference::decode(clip);
    ASSERT_GT(ref.frames(), RATE * 4);

    OfflineRig rig(clip, /*skewPpm=*/200.0);
    ASSERT_EQ(rig.attachment().state(), AudioAttachmentState::Ready);
    PlaybackClock& clock = rig.clock;
    clock.setDuration(CLIP_DURATION, rig.now());
    clock.setLoopMode(LoopMode::LOOP, rig.now());
    clock.play(rig.now());
    rig.event();

    struct Step { double time; std::function<void()> act; };
    const std::vector<Step> steps = {
        {7.3, [&] { clock.pause(rig.now()); }},
        {8.8, [&] { clock.play(rig.now()); }},
        {15.2, [&] { clock.seek(2.3, rig.now()); }},
        {21.0, [&] { clock.setSpeed(2.0, rig.now()); }},
        {24.0, [&] { clock.setSpeed(1.0, rig.now()); }},
        {30.0, [&] { clock.pause(rig.now()); }},
        {30.5, [&] { clock.seek(CLIP_DURATION - 0.1, rig.now()); }},
        {31.5, [&] { clock.play(rig.now()); }},  // plays straight across a wrap
        {40.0, [&] { clock.seek(0.1, rig.now()); }},
        {47.0, [&] { clock.seek(3.0, rig.now()); }},
    };
    size_t next = 0;
    rig.runUntil(60.0, [&](double t) {
        while (next < steps.size() && t >= steps[next].time) {
            steps[next].act();
            rig.event();
            ++next;
        }
    });

    const SyncStats stats = measure(rig.blocks, rig.latency(), ref, CLIP_DURATION, rig.history,
                                    rig.history.times(), 5, 0.15, /*verbose=*/true);
    report("minute", stats, rig.maxDrift, rig.attachment().resyncCount(), rig.attachment().playhead());

    EXPECT_GT(stats.windows, 700);
    EXPECT_LT(stats.maxError, 0.020);
    EXPECT_LT(stats.meanError(), 0.002);
    EXPECT_LE(stats.unmatched, stats.windows / 50);
    EXPECT_GT(stats.silentChecked, 20);  // the paused and 2x stretches
    EXPECT_EQ(stats.silentViolations, 0);
    EXPECT_LT(rig.maxDrift, 0.020);
    // One per seek / resume-after-2x / paused seek, none for the twelve wraps.
    EXPECT_LE(rig.attachment().resyncCount(), 10u);
    EXPECT_GT(rig.attachment().playhead().corrections, 0u);  // the 200 ppm was slewed out
}

// Ping-Pong: sound on the forward legs only, in step from the start of each forward
// leg (it is pre-rolled during the reverse leg), silence on the reverse legs.
TEST(AudioSyncTest, PingPongPlaysForwardLegsInStepAndReverseLegsSilent) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    const Reference ref = Reference::decode(clip);
    OfflineRig rig(clip);
    ASSERT_EQ(rig.attachment().state(), AudioAttachmentState::Ready);
    PlaybackClock& clock = rig.clock;
    clock.setDuration(CLIP_DURATION, rig.now());
    clock.setLoopMode(LoopMode::PING_PONG, rig.now());
    clock.play(rig.now());
    rig.event();
    rig.runUntil(2.0 * CLIP_DURATION + 6.0);

    // Forward, reverse, forward: the reflections are the only events, and the settle
    // window after them is short (the next forward leg is pre-rolled).
    std::vector<Clock::time_point> reflections;
    for (int leg = 0; leg <= 2; ++leg) reflections.push_back(at(rig.base(), leg * CLIP_DURATION));
    const SyncStats stats = measure(rig.blocks, rig.latency(), ref, 0.0, rig.history, reflections, 3, 0.05);
    report("ping-pong", stats, rig.maxDrift, rig.attachment().resyncCount(), rig.attachment().playhead());
    EXPECT_GT(stats.windows, 150);
    EXPECT_LT(stats.maxError, 0.020);
    EXPECT_GT(stats.silentChecked, 100);  // the reverse legs
    EXPECT_EQ(stats.silentViolations, 0);
}

// A timeline-locked source follows the master: its audio follows the master's pause,
// resume and restart.
TEST(AudioSyncTest, TimelineLockedAudioFollowsTheMaster) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    const Reference ref = Reference::decode(clip);
    OfflineRig rig(clip);
    ASSERT_EQ(rig.attachment().state(), AudioAttachmentState::Ready);
    PlaybackClock master;
    master.play(rig.now());
    PlaybackClock& clock = rig.clock;
    clock.setDuration(CLIP_DURATION, rig.now());
    clock.setLoopMode(LoopMode::LOOP, rig.now());
    clock.follow(master, rig.now());
    rig.event();

    struct Step { double time; std::function<void()> act; };
    const std::vector<Step> steps = {
        {4.0, [&] { master.pause(rig.now()); }},
        {5.0, [&] { master.play(rig.now()); }},
        {12.3, [&] { master.seek(0.0, rig.now()); }},  // restart
    };
    size_t next = 0;
    rig.runUntil(20.0, [&](double t) {
        bool changed = false;
        while (next < steps.size() && t >= steps[next].time) {
            steps[next].act();
            changed = true;
            ++next;
        }
        clock.follow(master, rig.now());  // what SourceManager::updateAll does
        if (changed) rig.event();
    });
    const SyncStats stats = measure(rig.blocks, rig.latency(), ref, CLIP_DURATION, rig.history, rig.history.times());
    report("timeline lock", stats, rig.maxDrift, rig.attachment().resyncCount(), rig.attachment().playhead());

    EXPECT_GT(stats.windows, 200);
    EXPECT_LT(stats.maxError, 0.020);
    EXPECT_GT(stats.silentChecked, 5);  // the master's pause
    EXPECT_EQ(stats.silentViolations, 0);
}

// Once: the sound stops at the end of the clip and stays stopped.
// A pass that stops while the sound runs on (here a 3 s "clip" of a longer track, as
// when an audio track outlasts its video) fades its last 5 ms instead of cutting.
TEST(AudioSyncTest, APassCutMidSoundFadesOut) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    // End the "clip" where the track is loud.
    const Reference ref = Reference::decode(clip);
    double cut = 0.0;
    for (int64_t n = RATE * 3; n < ref.frames() && cut == 0.0; n += RATE / 100) {
        float peak = 0.0f;
        for (int64_t i = n - RATE / 50; i < n; ++i) peak = std::max(peak, std::abs(ref.stereo[i * 2]));
        if (peak > 0.2f) cut = static_cast<double>(n) / RATE;
    }
    ASSERT_GT(cut, 0.0);
    OfflineRig rig(clip);
    ASSERT_EQ(rig.attachment().state(), AudioAttachmentState::Ready);
    rig.clock.setDuration(cut, rig.now());
    rig.clock.setLoopMode(LoopMode::ONCE, rig.now());
    rig.clock.seek(cut - 1.5, rig.now());
    rig.clock.play(rig.now());
    rig.runUntil(2.0);
    std::vector<float> left;
    for (const Block& b : rig.blocks) {
        for (size_t k = 0; k < BLOCK; ++k) left.push_back(b.samples[k * 2]);
    }
    size_t last = left.size();
    while (last > 0 && left[last - 1] == 0.0f) --last;
    ASSERT_GT(last, 2000u);
    float before = 0.0f, end = 0.0f;
    for (size_t i = last - 1200; i < last - 240; ++i) before = std::max(before, std::abs(left[i]));
    for (size_t i = last - 24; i < last; ++i) end = std::max(end, std::abs(left[i]));
    ASSERT_GT(before, 0.01f);  // there was sound up to the cut
    EXPECT_LT(end, 0.15f * before);
}

// An audio track that ends before its clip (here a 20 s clip over 18.15 s of sound):
// the pass is padded with silence to the clip's end, so the callback plays on in step
// instead of running dry and resyncing over and over; a Loop wraps on time after it.
TEST(AudioSyncTest, AShortAudioTrackIsPaddedToTheClipsEnd) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    for (LoopMode mode : {LoopMode::ONCE, LoopMode::LOOP}) {
        SCOPED_TRACE(static_cast<int>(mode));
        const Reference ref = Reference::decode(clip);
        OfflineRig rig(clip);
        ASSERT_EQ(rig.attachment().state(), AudioAttachmentState::Ready);
        rig.clock.setDuration(20.0, rig.now());
        rig.clock.setLoopMode(mode, rig.now());
        rig.clock.seek(17.0, rig.now());
        rig.clock.play(rig.now());
        rig.event();
        rig.runUntil(mode == LoopMode::LOOP ? 7.0 : 3.5);
        EXPECT_EQ(rig.attachment().resyncCount(), 1u);  // the first line-up only
        EXPECT_EQ(rig.attachment().playhead().underruns, 0u);
        if (mode == LoopMode::LOOP) {
            // After the wrap at 3 s the sound is in step again, without a resync.
            ClockHistory history = rig.history;
            const SyncStats stats = measure(rig.blocks, rig.latency(), ref, 20.0, history, {}, 3, 0.15);
            EXPECT_GT(stats.windows, 50);
            EXPECT_LT(stats.maxError, 0.005);
        }
    }
}

TEST(AudioSyncTest, OnceStopsAtTheEnd) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    OfflineRig rig(clip);
    ASSERT_EQ(rig.attachment().state(), AudioAttachmentState::Ready);
    rig.clock.setDuration(CLIP_DURATION, rig.now());
    rig.clock.setLoopMode(LoopMode::ONCE, rig.now());
    rig.clock.seek(CLIP_DURATION - 3.0, rig.now());
    rig.clock.play(rig.now());
    rig.runUntil(5.0);
    float peakBefore = 0.0f, peakAfter = 0.0f;
    const double latency = rig.latency();
    for (const Block& b : rig.blocks) {
        const double t = std::chrono::duration<double>(Clock::time_point(std::chrono::nanoseconds(b.wallNs)) -
                                                       rig.base()).count() + latency;
        for (float v : b.samples) {
            if (t > 0.2 && t < 2.8) peakBefore = std::max(peakBefore, std::abs(v));
            if (t > 3.05) peakAfter = std::max(peakAfter, std::abs(v));
        }
    }
    EXPECT_GT(peakBefore, 0.01f);
    EXPECT_EQ(peakAfter, 0.0f);
}

// The A/V sync clip (a white flash and a 1 kHz beep together on every whole second,
// 44.1 kHz mono, so resampled and upmixed on the way): each beep must start when the
// clock - the video - is at a whole second. Loop across wraps, a seek, a pause and 2x.
TEST(AudioSyncTest, BeepsStartOnTheSecondInTheFlashAndBeepClip) {
    const auto clip = audio_test::mediaPath("av_sync_test_44k_mono.mp4");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    const Reference ref = Reference::decode(clip);
    ASSERT_GT(ref.frames(), RATE * 9);
    OfflineRig rig(clip, /*skewPpm=*/-150.0);
    ASSERT_EQ(rig.attachment().state(), AudioAttachmentState::Ready);
    PlaybackClock& clock = rig.clock;
    clock.setDuration(10.0, rig.now());
    clock.setLoopMode(LoopMode::LOOP, rig.now());
    clock.play(rig.now());
    rig.event();
    struct Step { double time; std::function<void()> act; };
    const std::vector<Step> steps = {
        {5.4, [&] { clock.seek(3.25, rig.now()); }},
        {12.2, [&] { clock.pause(rig.now()); }},
        {13.1, [&] { clock.play(rig.now()); }},
        {20.3, [&] { clock.setSpeed(2.0, rig.now()); }},
        {22.6, [&] { clock.setSpeed(1.0, rig.now()); }},
    };
    size_t next = 0;
    rig.runUntil(40.0, [&](double t) {
        while (next < steps.size() && t >= steps[next].time) {
            steps[next].act();
            rig.event();
            ++next;
        }
    });

    // The reference beeps start on the second (the file is built that way).
    const std::vector<double> refOnsets = referenceOnsets(ref, false);
    ASSERT_GE(refOnsets.size(), 9u);
    for (double t : refOnsets) EXPECT_NEAR(t, std::round(t), 0.002) << t;

    const OnsetStats onsets = measureOnsets(rig.blocks, rig.latency(), ref, 10.0, rig.history,
                                            rig.history.times(), PAN_GAIN);
    reportOnsets("flash and beep", onsets);
    EXPECT_GE(onsets.onsets, 25);
    EXPECT_LT(onsets.maxError, 0.005);
    EXPECT_EQ(onsets.unpaired, 0);
}

// Real time, the real device thread, driven the way the app drives it:
// AudioEngine::sync() from a ~60 Hz loop, the clock on the real steady clock. Output is
// captured from the mixer's tap in the callback and measured twice: against each
// callback's own timestamp (what the sync itself uses), and against a straight line
// fitted through (frames played, callback time) - a constant-rate device - which
// takes out the scheduling jitter of the callbacks themselves.
struct RealTimeResult {
    SyncStats raw;
    SyncStats fitted;

    double callbackJitterMs = 0.0;  // largest distance of a callback from the fitted line
    uint64_t resyncs = 0;
    AudioPlayhead head;
};

static RealTimeResult runRealTime(const std::filesystem::path& clip, const Reference& ref, bool nullBackend,
                                  float masterVolume) {
    RealTimeResult result;
    AudioEngine engine(AudioEngine::Options{nullBackend});
    if (!engine.open("")) return result;
    engine.setMasterVolume(masterVolume);
    const size_t capacity = static_cast<size_t>(RATE) * 10;
    std::vector<float> captured(capacity * 2);
    std::vector<int64_t> blockWall(capacity / 64);
    std::vector<uint32_t> blockFrames(capacity / 64);
    std::atomic<size_t> framesCaptured{0};
    std::atomic<size_t> blocksCaptured{0};
    engine.mixer().setMixCallback([&](const float* samples, size_t frames, int64_t wallNs) {
        const size_t f = framesCaptured.load(std::memory_order_relaxed);
        const size_t b = blocksCaptured.load(std::memory_order_relaxed);
        if (f + frames > capacity || b >= blockWall.size()) return;
        std::copy(samples, samples + frames * 2, captured.begin() + f * 2);
        blockWall[b] = wallNs;
        blockFrames[b] = static_cast<uint32_t>(frames);
        framesCaptured.store(f + frames, std::memory_order_release);
        blocksCaptured.store(b + 1, std::memory_order_release);
    });

    const UUID id = UUID::generate();
    PlaybackClock clock;
    const Clock::time_point base = Clock::now();
    clock.setDuration(CLIP_DURATION, base);
    clock.setLoopMode(LoopMode::LOOP, base);
    clock.play(base);
    ClockHistory history;
    history.record(base, clock);
    bool seeked = false, paused = false, resumed = false;
    while (true) {
        const Clock::time_point now = Clock::now();
        const double t = std::chrono::duration<double>(now - base).count();
        if (t > 7.0) break;
        if (!seeked && t > 2.0) { clock.seek(3.7, now); history.record(now, clock); seeked = true; }
        if (!paused && t > 4.0) { clock.pause(now); history.record(now, clock); paused = true; }
        if (!resumed && t > 4.6) { clock.play(now); history.record(now, clock); resumed = true; }
        engine.sync(id, clip, audioTransportOf(clock, now));
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    engine.mixer().setMixCallback({});  // returns once the callback no longer runs the tap

    // Blocks of BLOCK frames with their callback times, and the fitted line.
    std::vector<Block> blocks;
    std::vector<size_t> blockOffset;
    std::vector<double> framesAt, wallAt;
    size_t offset = 0;
    for (size_t b = 0; b < blocksCaptured.load(); ++b) {
        framesAt.push_back(static_cast<double>(offset));
        wallAt.push_back(static_cast<double>(blockWall[b]));
        if (blockFrames[b] == BLOCK) {
            Block block;
            block.wallNs = blockWall[b];
            block.samples.assign(captured.begin() + offset * 2, captured.begin() + (offset + BLOCK) * 2);
            blocks.push_back(std::move(block));
            blockOffset.push_back(offset);
        }
        offset += blockFrames[b];
    }
    if (framesAt.size() < 10) return result;
    const double n = static_cast<double>(framesAt.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < framesAt.size(); ++i) {
        // Relative to the first callback, so the sums stay well inside double precision.
        const double x = framesAt[i];
        const double y = wallAt[i] - wallAt[0];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);  // ns per frame
    const double intercept = (sy - slope * sx) / n + wallAt[0];
    for (size_t i = 0; i < framesAt.size(); ++i) {
        const double fit = intercept + slope * framesAt[i];
        result.callbackJitterMs = std::max(result.callbackJitterMs, std::abs(wallAt[i] - fit) * 1e-6);
    }
    std::vector<Block> fittedBlocks = blocks;
    for (size_t i = 0; i < fittedBlocks.size(); ++i) {
        fittedBlocks[i].wallNs = static_cast<int64_t>(intercept + slope * static_cast<double>(blockOffset[i]));
    }

    const double latency = engine.mixer().getOutputLatency();
    const double scale = static_cast<double>(masterVolume) * masterVolume;
    result.raw = measure(blocks, latency, ref, CLIP_DURATION, history, history.times(), 2, 0.35, false, scale);
    result.fitted = measure(fittedBlocks, latency, ref, CLIP_DURATION, history, history.times(), 2, 0.35, false,
                            scale);
    if (const AudioAttachment* attachment = engine.attachment(id)) {
        result.resyncs = attachment->resyncCount();
        result.head = attachment->playhead();
    }
    std::printf("[ SYNC     ] callback timing: %.2f ppm off nominal, callbacks up to %.1f ms off a straight line, "
                "latency estimate %.1f ms\n",
                (slope / (1e9 / RATE) - 1.0) * 1e6, result.callbackJitterMs, latency * 1000.0);
    return result;
}

TEST(AudioSyncTest, RealTimeNullDeviceStaysInStepThroughASeekAndAPause) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    const Reference ref = Reference::decode(clip);
    const RealTimeResult r = runRealTime(clip, ref, /*nullBackend=*/true, 1.0f);
    report("real-time null device, callback times", r.raw, 0.0, r.resyncs, r.head);
    report("real-time null device, constant-rate device", r.fitted, 0.0, r.resyncs, r.head);

    EXPECT_GT(r.fitted.windows, 100);
    EXPECT_LT(r.fitted.maxError, 0.020);
    EXPECT_EQ(r.fitted.silentViolations, 0);
    EXPECT_EQ(r.fitted.dropouts, 0);
    // The null backend's timer paces callbacks coarsely; even measured against those
    // raw times the mean stays well inside the budget.
    EXPECT_LT(r.raw.meanError(), 0.010);
}

// The same on the machine's real output device, at 5 % volume. It makes sound, so it
// is run by hand (--gtest_also_run_disabled_tests).
TEST(AudioSyncTest, DISABLED_RealTimeDefaultDevice) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    const Reference ref = Reference::decode(clip);
    const RealTimeResult r = runRealTime(clip, ref, /*nullBackend=*/false, 0.05f);
    report("real-time default device, callback times", r.raw, 0.0, r.resyncs, r.head);
    report("real-time default device, constant-rate device", r.fitted, 0.0, r.resyncs, r.head);

    EXPECT_LT(r.raw.maxError, 0.020);
    EXPECT_LT(r.fitted.maxError, 0.020);
    EXPECT_EQ(r.fitted.dropouts, 0);
}

// Creating, syncing and releasing attachments in quick succession while the device
// thread mixes (the source GC, undo and project loads do this) never crashes, leaks a
// mixer input or leaves an attachment behind.
TEST(AudioSyncTest, AttachmentsComeAndGoWhileTheDeviceRuns) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioEngine engine(AudioEngine::Options{true});
    ASSERT_TRUE(engine.open(""));
    PlaybackClock clock;
    const Clock::time_point base = Clock::now();
    clock.setDuration(CLIP_DURATION, base);
    clock.setLoopMode(LoopMode::LOOP, base);
    clock.play(base);

    std::vector<UUID> ids;
    for (int i = 0; i < 6; ++i) ids.push_back(UUID::generate());
    for (int round = 0; round < 40; ++round) {
        std::unordered_set<UUID> live;
        for (size_t i = 0; i < ids.size(); ++i) {
            if ((round + i) % 3 == 0) continue;  // a different subset each round
            live.insert(ids[i]);
            engine.sync(ids[i], clip, audioTransportOf(clock, Clock::now()));
        }
        engine.retainOnly(live);
        EXPECT_EQ(engine.attachmentCount(), live.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(round % 4 == 0 ? 30 : 2));
    }
    engine.retainOnly({});
    EXPECT_EQ(engine.attachmentCount(), 0u);
    EXPECT_EQ(engine.mixer().getSourceCount(), 0u);
}

TEST(AudioSyncTest, AVideoWithoutAudioReportsNoAudio) {
    const auto clip = audio_test::mediaPath("4k_10Sec_Timecode.mp4");
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioEngine engine(AudioEngine::Options{true});
    ASSERT_TRUE(engine.open(""));
    const UUID id = UUID::generate();
    PlaybackClock clock;
    for (int i = 0; i < 200 && engine.status(id) != SourceAudioStatus::NoAudio; ++i) {
        engine.sync(id, clip, audioTransportOf(clock, Clock::now()));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(engine.status(id), SourceAudioStatus::NoAudio);
    EXPECT_EQ(engine.mixer().getSourceCount(), 0u);  // never joins the mix
    EXPECT_EQ(engine.status(UUID::generate()), SourceAudioStatus::None);
}

TEST(AudioSyncTest, EngineFallsBackToTheDefaultDeviceWhenThePreferredOneIsMissing) {
    AudioEngine engine(AudioEngine::Options{true});
    EXPECT_TRUE(engine.open("A device that is not plugged in"));
    EXPECT_EQ(engine.outputState(), AudioOutputState::Open);
    EXPECT_EQ(engine.preferredDevice(), "A device that is not plugged in");
    EXPECT_FALSE(engine.activeDeviceName().empty());
    const auto devices = engine.listDevices();
    ASSERT_FALSE(devices.empty());
    EXPECT_TRUE(engine.selectDevice(devices.front()));
    EXPECT_EQ(engine.activeDeviceName(), devices.front());
    engine.setMasterVolume(0.3f);
    engine.setMasterMuted(true);
    EXPECT_FLOAT_EQ(engine.masterVolume(), 0.3f);
    EXPECT_TRUE(engine.masterMuted());
}

// ---- The transport mapping the app feeds the engine ---------------------------

TEST(AudioTransportTest, AudibleOnlyRunningForwardAtOneX) {
    const Clock::time_point t0 = Clock::now();
    PlaybackClock clock;
    clock.setDuration(10.0, t0);
    clock.setLoopMode(LoopMode::LOOP, t0);
    EXPECT_FALSE(audioTransportOf(clock, t0).audible);  // paused
    clock.play(t0);
    AudioTransport t = audioTransportOf(clock, at(t0, 1.0));
    EXPECT_TRUE(t.audible);
    EXPECT_TRUE(t.running);
    EXPECT_TRUE(t.loop);
    EXPECT_NEAR(t.timeline, 1.0, 1e-9);
    EXPECT_NEAR(t.clip, 1.0, 1e-9);
    EXPECT_EQ(t.wallNs, toNs(at(t0, 1.0)));
    clock.setSpeed(2.0, at(t0, 1.0));
    EXPECT_FALSE(audioTransportOf(clock, at(t0, 1.5)).audible);
    clock.setSpeed(1.0, at(t0, 1.5));
    EXPECT_TRUE(audioTransportOf(clock, at(t0, 1.6)).audible);
    clock.setDirection(-1, at(t0, 1.6));
    EXPECT_FALSE(audioTransportOf(clock, at(t0, 1.7)).audible);
}

TEST(AudioTransportTest, LoopTimelineRunsOnAcrossTheWrap) {
    const Clock::time_point t0 = Clock::now();
    PlaybackClock clock;
    clock.setDuration(5.0, t0);
    clock.setLoopMode(LoopMode::LOOP, t0);
    clock.play(t0);
    const AudioTransport t = audioTransportOf(clock, at(t0, 12.0));
    EXPECT_NEAR(t.timeline, 12.0, 1e-9);
    EXPECT_NEAR(t.clip, 2.0, 1e-9);
    EXPECT_TRUE(t.audible);
    EXPECT_FALSE(t.nextAudibleStart.has_value());
}

TEST(AudioTransportTest, PingPongReverseLegIsSilentAndNamesTheNextForwardLeg) {
    const Clock::time_point t0 = Clock::now();
    PlaybackClock clock;
    clock.setDuration(5.0, t0);
    clock.setLoopMode(LoopMode::PING_PONG, t0);
    clock.play(t0);
    const AudioTransport reverse = audioTransportOf(clock, at(t0, 7.0));
    EXPECT_FALSE(reverse.audible);
    EXPECT_TRUE(reverse.running);
    ASSERT_TRUE(reverse.nextAudibleStart.has_value());
    EXPECT_NEAR(*reverse.nextAudibleStart, 10.0, 1e-9);
    const AudioTransport forward = audioTransportOf(clock, at(t0, 11.0));
    EXPECT_TRUE(forward.audible);
    EXPECT_NEAR(forward.clip, 1.0, 1e-9);
    EXPECT_NEAR(forward.timeline - forward.clip, 10.0, 1e-9);
}

// A timeline-locked source that is paused (by the user, or because it is off screen)
// keeps following the master, so its clock runs; it must still be silent.
TEST(AudioTransportTest, ANonPlayingSourceIsSilentEvenWhenItsClockRuns) {
    const Clock::time_point t0 = Clock::now();
    PlaybackClock master;
    master.play(t0);
    PlaybackClock locked;
    locked.setDuration(10.0, t0);
    locked.setLoopMode(LoopMode::LOOP, t0);
    locked.follow(master, t0);
    const AudioTransport running = audioTransportOf(locked, at(t0, 1.0));
    ASSERT_TRUE(running.audible);
    for (PlaybackState state : {PlaybackState::PAUSED, PlaybackState::STOPPED, PlaybackState::FINISHED}) {
        const AudioTransport t = withPlaybackState(running, state);
        EXPECT_FALSE(t.audible);
        EXPECT_FALSE(t.running);
        EXPECT_FALSE(t.nextAudibleStart.has_value());
        EXPECT_DOUBLE_EQ(t.timeline, running.timeline);  // the position is kept for priming
    }
    EXPECT_TRUE(withPlaybackState(running, PlaybackState::PLAYING).audible);
}

TEST(AudioTransportTest, OnceIsSilentOnceEnded) {
    const Clock::time_point t0 = Clock::now();
    PlaybackClock clock;
    clock.setDuration(5.0, t0);
    clock.setLoopMode(LoopMode::ONCE, t0);
    clock.play(t0);
    EXPECT_TRUE(audioTransportOf(clock, at(t0, 4.0)).audible);
    EXPECT_FALSE(audioTransportOf(clock, at(t0, 6.0)).audible);
}

// ---- Which sources are heard (lead decision on ADR-050 §2) ---------------------

TEST(AudibleSourcesTest, ActiveSceneAndScenesOnEnabledOutputsAreHeard) {
    const UUID clipA = UUID::generate(), clipB = UUID::generate(), clipC = UUID::generate();
    const UUID clipD = UUID::generate(), shared = UUID::generate();
    SceneList scenes;
    auto makeScene = [&](std::vector<UUID> media) {
        auto scene = std::make_unique<Scene>();
        for (const UUID& m : media) scene->getQuadManager().createQuad("q")->getTexture().mediaId = m;
        scenes.push_back(std::move(scene));
        return scenes.back()->getId();
    };
    const UUID editing = makeScene({clipA, shared});  // active, on no output
    const UUID onAir = makeScene({clipB});            // shown by an enabled output
    const UUID offAir = makeScene({clipC, shared});   // shown by a disabled output only
    makeScene({clipD});                               // edit-only, not active
    scenes.back()->getQuadManager().createQuad("empty");  // a quad with no media
    // liveSceneIds of the outputs: only the enabled output's scene.
    const std::unordered_set<UUID> live{onAir};

    const auto heard = audibleSourceIds(scenes, editing, live);
    EXPECT_TRUE(heard.count(clipA));
    EXPECT_TRUE(heard.count(clipB));
    EXPECT_TRUE(heard.count(shared));  // one heard scene is enough
    EXPECT_FALSE(heard.count(clipC));
    EXPECT_FALSE(heard.count(clipD));
    EXPECT_FALSE(heard.count(UUID()));
    EXPECT_EQ(heard.size(), 3u);

    // A scene take moves the active scene: the soundtrack follows the picture.
    const auto afterTake = audibleSourceIds(scenes, scenes[3]->getId(), live);
    EXPECT_TRUE(afterTake.count(clipD));
    EXPECT_FALSE(afterTake.count(clipA));
    EXPECT_TRUE(afterTake.count(clipB));
    // Enabling the other output makes its scene live too.
    EXPECT_TRUE(audibleSourceIds(scenes, editing, {onAir, offAir}).count(clipC));
    // No active scene and no enabled output: nothing is heard.
    EXPECT_TRUE(audibleSourceIds(scenes, UUID(), {}).empty());
}

// The live scenes follow the routing (ADR-047): the scenes enabled outputs show, each
// once however many outputs show it.
TEST(AudibleSourcesTest, LiveScenesAreThoseEnabledOutputsShow) {
    struct FakeOutput {
        UUID sceneId;
        bool enabled = true;
        bool isEnabled() const { return enabled; }
        const UUID& getSceneId() const { return sceneId; }
    };
    const UUID shared = UUID::generate(), offAir = UUID::generate();
    std::vector<std::unique_ptr<FakeOutput>> outputs;
    outputs.push_back(std::make_unique<FakeOutput>(FakeOutput{shared, true}));
    outputs.push_back(std::make_unique<FakeOutput>(FakeOutput{shared, true}));
    outputs.push_back(std::make_unique<FakeOutput>(FakeOutput{offAir, false}));
    outputs.push_back(std::make_unique<FakeOutput>(FakeOutput{UUID(), true}));
    EXPECT_EQ(liveSceneIds(outputs), (std::unordered_set<UUID>{shared}));
}

// A gated source stays in step and comes back in sync: gated for a stretch, then heard
// again, and the sound after the gate is exactly where the clock is.
TEST(AudioSyncTest, AGatedSourceIsSilentAndComesBackInStep) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    const Reference ref = Reference::decode(clip);
    OfflineRig rig(clip);
    ASSERT_EQ(rig.attachment().state(), AudioAttachmentState::Ready);
    rig.clock.setDuration(CLIP_DURATION, rig.now());
    rig.clock.setLoopMode(LoopMode::LOOP, rig.now());
    rig.clock.play(rig.now());
    rig.event();
    rig.gated = [](double t) { return t >= 4.0 && t < 8.0; };
    rig.runUntil(12.0);
    EXPECT_EQ(rig.attachment().resyncCount(), 1u);  // gating never resyncs

    float peakGated = 0.0f;
    std::vector<Block> heard;
    for (const Block& b : rig.blocks) {
        const double t = std::chrono::duration<double>(Clock::time_point(std::chrono::nanoseconds(b.wallNs)) -
                                                       rig.base()).count();
        if (t > 4.05 && t < 7.95) {
            for (float v : b.samples) peakGated = std::max(peakGated, std::abs(v));
        } else if (t > 8.1) {
            heard.push_back(b);
        }
    }
    EXPECT_EQ(peakGated, 0.0f);
    const SyncStats stats = measure(heard, rig.latency(), ref, CLIP_DURATION, rig.history, {}, 2, 0.0);
    EXPECT_GT(stats.windows, 30);
    EXPECT_LT(stats.maxError, 0.005);
}

// ---- Loop-wrap crossfade (TD-26) -----------------------------------------------

TEST(LoopCrossfadeTest, GainsAreEqualPowerAndTheFadeRunsItsLength) {
    LoopCrossfade fade(240);
    ASSERT_EQ(fade.frames(), 240u);
    EXPECT_EQ(fade.remaining(), 0u) << "nothing armed";

    // Incoming 1 over a continuation of 0 gives the fade-in gain; 0 over 1 the fade-out.
    std::vector<float> ones(240 * 2, 1.0f), zeros(240 * 2, 0.0f);
    const float last[2] = {0.0f, 0.0f};
    fade.arm(zeros.data(), 240, last, 0, 1000);
    const float* mixed = fade.mix(ones.data(), 240);
    const std::vector<float> in(mixed, mixed + 240 * 2);
    EXPECT_EQ(fade.remaining(), 0u);
    fade.arm(ones.data(), 240, last, 0, 1000);
    mixed = fade.mix(zeros.data(), 240);
    const std::vector<float> out(mixed, mixed + 240 * 2);
    for (size_t i = 0; i < 240; ++i) {
        EXPECT_NEAR(in[i * 2] * in[i * 2] + out[i * 2] * out[i * 2], 1.0f, 1e-5f) << i;
        EXPECT_EQ(in[i * 2], in[i * 2 + 1]);
        if (i > 0) EXPECT_GT(in[i * 2], in[(i - 1) * 2]);
    }
    EXPECT_LT(in.front(), 0.01f);
    EXPECT_GT(in.back(), 0.9999f);
}

TEST(LoopCrossfadeTest, PaddingPassesUnchangedAndAMissingTailHoldsTheLastFrame) {
    LoopCrossfade fade(8);
    const float last[2] = {0.5f, -0.25f};
    fade.arm(nullptr, 0, last, /*delay=*/5, /*limit=*/1000);
    EXPECT_EQ(fade.delay(), 5u);
    fade.skip(3);
    fade.skip(9);  // never below zero
    EXPECT_EQ(fade.delay(), 0u);
    std::vector<float> zeros(8 * 2, 0.0f);
    const float* mixed = fade.mix(zeros.data(), 8);
    // Silence incoming: what is heard is the held last frame fading out.
    EXPECT_NEAR(mixed[0], 0.5f * std::cos(0.5f / 8 * 1.5707963f), 1e-6f);
    EXPECT_NEAR(mixed[1], -0.25f * std::cos(0.5f / 8 * 1.5707963f), 1e-6f);
    for (size_t i = 1; i < 8; ++i) EXPECT_LT(std::abs(mixed[i * 2]), std::abs(mixed[(i - 1) * 2]));
}

TEST(LoopCrossfadeTest, AFadeLongerThanThePassEndsWithinIt) {
    LoopCrossfade fade(240);
    std::vector<float> ones(240 * 2, 1.0f), zeros(240 * 2, 0.0f);
    const float last[2] = {0.0f, 0.0f};
    fade.arm(zeros.data(), 240, last, 0, /*limit=*/60);
    EXPECT_EQ(fade.remaining(), 60u);
    const float* mixed = fade.mix(ones.data(), 60);
    EXPECT_GT(mixed[59 * 2], 0.9999f) << "fully faded in at the pass's end";
    EXPECT_EQ(fade.remaining(), 0u);
}

namespace {

// A stereo 16-bit PCM WAV of A * sin(2 pi f t + phase), `seconds` long.
struct Tone {
    double amplitude = 0.5;
    double frequency = 441.0;
    double phase = 0.0;
    double seconds = 1.0;
    double at(double t) const {
        if (t < 0.0 || t >= seconds) return 0.0;
        return amplitude * std::sin(2.0 * 3.14159265358979323846 * frequency * t + phase);
    }
    // Largest step between neighbouring samples.
    double naturalStep() const { return amplitude * 2.0 * 3.14159265358979323846 * frequency / RATE; }
};

std::filesystem::path writeToneWav(const Tone& tone, const char* name) {
    const auto path = std::filesystem::temp_directory_path() /
                      (std::string("qm_loopseam_") + std::to_string(_getpid()) + "_" + name + ".wav");
    const uint32_t frames = static_cast<uint32_t>(std::llround(tone.seconds * RATE));
    const uint32_t dataBytes = frames * 4;
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(2); u32(RATE); u32(RATE * 4); u16(4); u16(16);
    f.write("data", 4); u32(dataBytes);
    for (uint32_t i = 0; i < frames; ++i) {
        const auto v = static_cast<int16_t>(std::lround(tone.at(static_cast<double>(i) / RATE) * 32767.0));
        u16(static_cast<uint16_t>(v));
        u16(static_cast<uint16_t>(v));
    }
    return path;
}

struct SeamResult {
    double maxStep = 0.0;        // largest step anywhere in the output
    double maxOffSeamError = 0.0;  // largest |output - expected| away from the wraps
    int wraps = 0;
    int checked = 0;             // samples compared with the expected sound
    double lateError = 0.0;      // largest |output - expected| off the seams, last quarter
    uint64_t corrections = 0;    // soft drift corrections the attachment made
    uint64_t resyncs = 0;
};

// Play `tone` looped over a clip of `duration` seconds for `seconds`, offline.
SeamResult playLooped(const Tone& tone, double duration, double seconds, const char* name) {
    SeamResult r;
    const auto path = writeToneWav(tone, name);
    {
        OfflineRig rig(path);
        EXPECT_EQ(rig.attachment().state(), AudioAttachmentState::Ready);
        PlaybackClock& clock = rig.clock;
        clock.setDuration(duration, rig.now());
        clock.setLoopMode(LoopMode::LOOP, rig.now());
        clock.play(rig.now());
        rig.event();
        rig.runUntil(seconds);

        const double seamWindow = 0.006;  // the 5 ms fade and a sample either side
        float previous = 0.0f;
        bool first = true;
        for (const Block& block : rig.blocks) {
            const Clock::time_point wall = Clock::time_point(std::chrono::nanoseconds(block.wallNs));
            for (size_t k = 0; k < BLOCK; ++k) {
                const float left = block.samples[k * 2];
                if (!first) r.maxStep = std::max(r.maxStep, static_cast<double>(std::abs(left - previous)));
                previous = left;
                first = false;
                const auto clip = rig.history.expect(at(wall, rig.latency() + static_cast<double>(k) / RATE));
                // Settled (the start fades in over one buffer) and away from a wrap.
                const double sinceStart = std::chrono::duration<double>(wall - rig.base()).count();
                if (!clip || sinceStart < 0.1 || *clip < seamWindow || duration - *clip < 1.0 / RATE) continue;
                const double expected = PAN_GAIN * tone.at(*clip);
                r.maxOffSeamError = std::max(r.maxOffSeamError, std::abs(left - expected));
                if (sinceStart > 0.75 * seconds) r.lateError = std::max(r.lateError, std::abs(left - expected));
                ++r.checked;
            }
        }
        r.wraps = static_cast<int>(seconds / duration);
        r.corrections = rig.attachment().playhead().corrections;
        r.resyncs = rig.attachment().resyncCount();
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return r;
}

// Step bound across an equal-power fade of F frames between two signals of amplitude
// A whose own steps are at most s: |d(out)| <= 2 s + A * pi / F (both gains change by
// at most pi / (2F) per frame), at the pan gain, plus 16-bit rounding.
double seamStepBound(const Tone& tone) {
    const double F = 0.005 * RATE;
    return PAN_GAIN * (2.0 * tone.naturalStep() + tone.amplitude * 3.14159265358979323846 / F) + 1e-3;
}

}  // namespace

// The file runs on past the clip's end: the pass is cut mid-cycle (a quarter period
// off), a 0.35 step at the seam before the crossfade.
TEST(AudioSyncTest, ALoopWrapCrossfadesInsteadOfStepping) {
    Tone tone;
    tone.seconds = 0.8;
    // A whole number of samples: a pass is cut to the nearest sample, so a fractional
    // length would slip the audio a fraction of a sample per wrap (the drift control
    // slews that out, but this test checks sync to the sample).
    const double duration = std::round((0.5 + 0.25 / tone.frequency) * RATE) / RATE;
    const double rawStep = PAN_GAIN * std::abs(tone.at(duration - 1.0 / RATE) - tone.at(0.0));
    ASSERT_GT(rawStep, 4.0 * seamStepBound(tone)) << "the seam must be a real step for this test to mean anything";

    const SeamResult r = playLooped(tone, duration, 2.4, "cut");
    std::printf("[ SEAM     ] cut: %d wraps, max step %.4f (bound %.4f, raw seam %.4f), "
                "max error off the seams %.5f over %d samples\n",
                r.wraps, r.maxStep, seamStepBound(tone), rawStep, r.maxOffSeamError, r.checked);
    EXPECT_GE(r.wraps, 4);
    EXPECT_LE(r.maxStep, seamStepBound(tone));
    // In step with the clock, to the sample, everywhere but the 5 ms after each wrap.
    EXPECT_GT(r.checked, RATE);
    EXPECT_LT(r.maxOffSeamError, PAN_GAIN * tone.naturalStep() * 0.6 + 1e-3);
}

// The file is shorter than the clip: each pass ends in padding silence and the next
// starts at full amplitude (phase pi/2), a step up from 0 before the crossfade.
TEST(AudioSyncTest, ALoopWrapAfterPaddingFadesIn) {
    Tone tone;
    tone.seconds = 132.25 / tone.frequency;  // the file itself ends on a zero crossing
    tone.phase = 3.14159265358979323846 / 2.0;
    const double duration = 0.4;
    const double rawStep = PAN_GAIN * std::abs(tone.at(0.0));
    ASSERT_GT(rawStep, 4.0 * seamStepBound(tone));

    const SeamResult r = playLooped(tone, duration, 2.0, "padded");
    std::printf("[ SEAM     ] padded: %d wraps, max step %.4f (bound %.4f, raw seam %.4f), "
                "max error off the seams %.5f over %d samples\n",
                r.wraps, r.maxStep, seamStepBound(tone), rawStep, r.maxOffSeamError, r.checked);
    EXPECT_GE(r.wraps, 4);
    EXPECT_LE(r.maxStep, seamStepBound(tone));
    EXPECT_GT(r.checked, RATE);
    EXPECT_LT(r.maxOffSeamError, PAN_GAIN * tone.naturalStep() * 0.6 + 1e-3);
}

// A clip that is not a whole number of samples long: each pass is a sample longer or
// shorter as the fraction adds up, so after 60 wraps the sound is still on the clock to
// within half a sample (whole-sample passes slipped 0.37 samples a wrap: 22 samples,
// ~0.46 ms, by the end, which the check below sees).
TEST(AudioSyncTest, LoopPassesOfAFractionalLengthDoNotSlip) {
    Tone tone;
    tone.seconds = 0.3;
    const double duration = (std::round(0.15 * RATE) + 0.37) / RATE;
    const SeamResult r = playLooped(tone, duration, 60.5 * duration, "fraction");
    std::printf("[ SEAM     ] fraction: %d wraps, max error off the seams %.5f (last quarter %.5f) "
                "over %d samples, %llu corrections, %llu resyncs\n",
                r.wraps, r.maxOffSeamError, r.lateError, r.checked,
                static_cast<unsigned long long>(r.corrections), static_cast<unsigned long long>(r.resyncs));
    EXPECT_GE(r.wraps, 60);
    EXPECT_GT(r.checked, RATE * 5);
    // Within half a sample of the clock (the rounding of one pass), start and end alike.
    const double halfSample = PAN_GAIN * tone.naturalStep() * 0.5 + 1e-3;
    EXPECT_LT(r.maxOffSeamError, halfSample);
    EXPECT_LT(r.lateError, halfSample);
    EXPECT_EQ(r.corrections, 0u) << "no drift to slew out";
    EXPECT_EQ(r.resyncs, 1u) << "only the first line-up";
}

// ---- CPU cost (run by hand: --gtest_also_run_disabled_tests) ---------------------

#ifdef _WIN32
static double processCpuSeconds() {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    auto seconds = [](const FILETIME& f) {
        ULARGE_INTEGER v;
        v.LowPart = f.dwLowDateTime;
        v.HighPart = f.dwHighDateTime;
        return static_cast<double>(v.QuadPart) * 1e-7;
    };
    return seconds(kernel) + seconds(user);
}

static void measureCpu(int sources) {
    const auto clip = audio_test::mediaPath(SYNC_CLIP);
    if (clip.empty()) GTEST_SKIP() << "test media not present";
    AudioEngine engine(AudioEngine::Options{true});
    ASSERT_TRUE(engine.open(""));
    std::vector<UUID> ids;
    std::vector<PlaybackClock> clocks(static_cast<size_t>(sources));
    const Clock::time_point base = Clock::now();
    for (int i = 0; i < sources; ++i) {
        ids.push_back(UUID::generate());
        clocks[i].setDuration(CLIP_DURATION, base);
        clocks[i].setLoopMode(LoopMode::LOOP, base);
        clocks[i].seek(i * 1.7, base);
        clocks[i].play(base);
    }
    auto frame = [&] {
        const Clock::time_point now = Clock::now();
        for (int i = 0; i < sources; ++i) engine.sync(ids[i], clip, audioTransportOf(clocks[i], now));
    };
    // Warm up (probe, first decode), then measure 10 s of steady playback.
    for (int i = 0; i < 60; ++i) { frame(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
    const double cpu0 = processCpuSeconds();
    const Clock::time_point wall0 = Clock::now();
    while (Clock::now() - wall0 < std::chrono::seconds(10)) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const double cpu = processCpuSeconds() - cpu0;
    const double wall = std::chrono::duration<double>(Clock::now() - wall0).count();
    std::printf("[ CPU      ] %d audio source(s): %.2f%% of one core (%.3f s CPU over %.1f s)\n", sources,
                100.0 * cpu / wall, cpu, wall);
}

TEST(AudioCpuBench, DISABLED_OneSource) { measureCpu(1); }
TEST(AudioCpuBench, DISABLED_EightSources) { measureCpu(8); }
#endif

}  // namespace test
}  // namespace quadmapper
