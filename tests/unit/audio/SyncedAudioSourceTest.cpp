// The audio-thread half of the sync (SyncedAudioSource::pull) with synthetic samples
// and injected callback times: every frame pushed carries its own index, so each test
// can see exactly which sample was written where.

#include <gtest/gtest.h>
#include "audio/AudioBuffer.hpp"
#include "audio/SyncedAudioSource.hpp"

#include <cmath>
#include <memory>
#include <vector>

namespace quadmapper {
namespace test {

namespace {

constexpr int RATE = 48000;
constexpr size_t BLOCK = 480;       // 10 ms callbacks
constexpr int64_t BLOCK_NS = 10000000;
constexpr int64_t T0 = 5000000000;  // an arbitrary steady_clock instant

class Probe : public SyncedAudioSource {
public:
    using SyncedAudioSource::SyncedAudioSource;
    std::vector<float> pullBlock(int64_t wallNs, double latency = 0.0, size_t frames = BLOCK) {
        std::vector<float> out(frames * 2, -99.0f);
        AudioRenderContext context;
        context.wallNs = wallNs;
        context.outputLatency = latency;
        context.sampleRate = RATE;
        pull(out.data(), frames, context);
        return out;
    }
};

class SyncedAudioSourceTest : public ::testing::Test {
protected:
    void SetUp() override {
        buffer = std::make_shared<AudioBuffer>(RATE * 2 * 4);
        source = std::make_unique<Probe>(buffer, RATE);
    }

    // Frames with left = index, right = -index.
    void pushFrames(int64_t first, int64_t count) {
        std::vector<float> samples(static_cast<size_t>(count) * 2);
        for (int64_t i = 0; i < count; ++i) {
            samples[i * 2] = static_cast<float>(first + i);
            samples[i * 2 + 1] = -static_cast<float>(first + i);
        }
        ASSERT_TRUE(buffer->push(samples.data(), samples.size()));
    }

    void segment(uint32_t generation, double timeline) {
        AudioSegment s;
        s.generation = generation;
        s.startPosition = buffer->writePosition();
        s.timeline = timeline;
        source->publishSegment(s);
    }

    void transport(uint32_t generation, bool audible, double timeline, int64_t wallNs) {
        AudioTransportAnchor a;
        a.generation = generation;
        a.audible = audible;
        a.timeline = timeline;
        a.wallNs = wallNs;
        source->publishTransport(a);
    }

    // Start playing generation 1 (timeline 0 = frame 0) at T0 with no latency, and pull
    // one block so the fade-in is done.
    void startPlaying(int64_t frames = RATE * 2) {
        segment(1, 0.0);
        pushFrames(0, frames);
        transport(1, true, 0.0, T0);
        source->pullBlock(T0);
        ASSERT_TRUE(source->playhead().aligned);
    }

    std::shared_ptr<AudioBuffer> buffer;
    std::unique_ptr<Probe> source;
};

}  // namespace

TEST_F(SyncedAudioSourceTest, SilentWithoutASegmentOrAnAudibleTransport) {
    pushFrames(0, 4800);
    std::vector<float> out = source->pullBlock(T0);
    for (float v : out) ASSERT_EQ(v, 0.0f);
    EXPECT_EQ(buffer->readPosition(), 0u);  // nothing consumed

    segment(1, 0.0);
    transport(1, false, 0.0, T0);  // paused
    out = source->pullBlock(T0);
    for (float v : out) ASSERT_EQ(v, 0.0f);
    EXPECT_FALSE(source->playhead().aligned);
    EXPECT_EQ(source->playhead().generation, 1u);  // adopted, waiting
}

TEST_F(SyncedAudioSourceTest, LinesUpSampleExactlyWithTheLatencyAccountedFor) {
    segment(1, 10.0);           // the run starts at timeline 10.0
    pushFrames(0, RATE);
    transport(1, true, 10.1, T0);
    // Written now, heard 20 ms later: must be the sample at 10.1 + 0.02 -> frame 5760.
    std::vector<float> out = source->pullBlock(T0, 0.020);
    // First block fades in: gain (i+1)/BLOCK on frame i.
    for (size_t i = 0; i < BLOCK; ++i) {
        const float gain = static_cast<float>(i + 1) / BLOCK;
        ASSERT_NEAR(out[i * 2], (5760.0f + i) * gain, 0.01f * (5760.0f + i)) << i;
    }
    EXPECT_NEAR(out[(BLOCK - 1) * 2], 5760.0f + BLOCK - 1, 1e-3);
    out = source->pullBlock(T0 + BLOCK_NS, 0.020);
    EXPECT_FLOAT_EQ(out[0], 6240.0f);  // continues without a gap, at full gain
    EXPECT_FLOAT_EQ(out[1], -6240.0f);

    const AudioPlayhead head = source->playhead();
    EXPECT_TRUE(head.aligned);
    EXPECT_EQ(head.generation, 1u);
    EXPECT_NEAR(head.nextTimeline, 10.0 + 6720.0 / RATE, 1e-9);
    EXPECT_EQ(head.nextWallNs, T0 + 2 * BLOCK_NS);
    // The sample heard at nextWallNs (written then, heard latency later) is the
    // transport's time at nextWallNs: the control thread's drift is zero.
    const double heard = head.nextTimeline - 0.020;
    const double clock = 10.1 + static_cast<double>(head.nextWallNs - T0) * 1e-9;
    EXPECT_NEAR(heard - clock, 0.0, 0.5 / RATE);
}

TEST_F(SyncedAudioSourceTest, WritesSilenceUntilALateStreamStarts) {
    segment(1, 10.0);
    pushFrames(0, RATE);
    transport(1, true, 9.995, T0);  // 5 ms before the first sample
    const std::vector<float> out = source->pullBlock(T0);
    for (size_t i = 0; i < 240; ++i) ASSERT_EQ(out[i * 2], 0.0f) << i;
    EXPECT_NEAR(out[240 * 2], 0.0f, 1e-6);           // frame 0, faded in from 0
    EXPECT_NEAR(out[(BLOCK - 1) * 2], 239.0f, 1e-3);  // frame 239 at full gain
    EXPECT_TRUE(source->playhead().aligned);
}

TEST_F(SyncedAudioSourceTest, WaitsWhenTheTargetIsNotDecodedYet) {
    segment(1, 0.0);
    pushFrames(0, 1000);
    transport(1, true, 1.0, T0);  // needs frame 48000
    std::vector<float> out = source->pullBlock(T0);
    for (float v : out) ASSERT_EQ(v, 0.0f);
    EXPECT_FALSE(source->playhead().aligned);
    EXPECT_EQ(buffer->readPosition(), 2000u);  // skipped what it had
    pushFrames(1000, RATE * 2);
    out = source->pullBlock(T0 + BLOCK_NS);  // now needs 1.01 s -> frame 48480
    EXPECT_NEAR(out[(BLOCK - 1) * 2], 48480.0f + BLOCK - 1, 1e-2);
    EXPECT_TRUE(source->playhead().aligned);
}

TEST_F(SyncedAudioSourceTest, DropCorrectionSkipsOneFrameInEveryThousand) {
    startPlaying();
    const size_t before = buffer->readPosition() / 2;
    source->setCorrection(-1);
    std::vector<float> all;
    for (int i = 0; i < 10; ++i) {
        const auto out = source->pullBlock(T0 + (i + 1) * BLOCK_NS);
        all.insert(all.end(), out.begin(), out.end());
    }
    EXPECT_EQ(buffer->readPosition() / 2 - before, 4800u + 4u);  // drops at 1000..4000
    EXPECT_EQ(source->playhead().corrections, 4u);
    int jumps = 0;
    for (size_t i = 1; i < all.size() / 2; ++i) {
        const float step = all[i * 2] - all[(i - 1) * 2];
        if (step == 2.0f) ++jumps;
        else ASSERT_EQ(step, 1.0f) << i;
    }
    EXPECT_EQ(jumps, 4);
}

TEST_F(SyncedAudioSourceTest, RepeatCorrectionHoldsOneFrameInEveryThousand) {
    startPlaying();
    const size_t before = buffer->readPosition() / 2;
    source->setCorrection(+1);
    std::vector<float> all;
    for (int i = 0; i < 10; ++i) {
        const auto out = source->pullBlock(T0 + (i + 1) * BLOCK_NS);
        all.insert(all.end(), out.begin(), out.end());
    }
    EXPECT_EQ(buffer->readPosition() / 2 - before, 4800u - 4u);
    EXPECT_EQ(source->playhead().corrections, 4u);
    int holds = 0;
    for (size_t i = 1; i < all.size() / 2; ++i) {
        const float step = all[i * 2] - all[(i - 1) * 2];
        if (step == 0.0f) ++holds;
        else ASSERT_EQ(step, 1.0f) << i;
    }
    EXPECT_EQ(holds, 4);
}

TEST_F(SyncedAudioSourceTest, FadesOutOnPauseThenHoldsItsPosition) {
    startPlaying();
    transport(1, false, 0.01, T0 + BLOCK_NS);
    std::vector<float> out = source->pullBlock(T0 + BLOCK_NS);
    // One buffer of stream, ramped down to silence.
    EXPECT_GT(out[0], 0.0f);
    EXPECT_NEAR(out[(BLOCK - 1) * 2], 0.0f, 1e-3);
    for (size_t i = 1; i < BLOCK; ++i) {
        const float gainNow = out[i * 2] / (480.0f + i);
        const float gainBefore = out[(i - 1) * 2] / (480.0f + i - 1);
        ASSERT_LE(gainNow, gainBefore + 1e-6f) << i;
    }
    EXPECT_FALSE(source->playhead().aligned);
    const size_t held = buffer->readPosition();
    out = source->pullBlock(T0 + 2 * BLOCK_NS);
    for (float v : out) ASSERT_EQ(v, 0.0f);
    EXPECT_EQ(buffer->readPosition(), held);
}

TEST_F(SyncedAudioSourceTest, ResumeLinesUpAgainFromTheHeldBuffer) {
    startPlaying();
    transport(1, false, 0.01, T0 + BLOCK_NS);
    source->pullBlock(T0 + BLOCK_NS);  // fade-out consumed frames 480..959
    // Resume two seconds later at the paused position 0.01 s (frame 480).
    const int64_t resume = T0 + 2000000000;
    transport(1, true, 0.01, resume);
    const std::vector<float> out = source->pullBlock(resume);
    // The held stream is at frame 960, ahead of the target 480: 480 frames of silence.
    for (size_t i = 0; i < BLOCK; ++i) ASSERT_EQ(out[i * 2], 0.0f) << i;
    const auto next = source->pullBlock(resume + BLOCK_NS);
    EXPECT_NEAR(next[(BLOCK - 1) * 2], 960.0f + BLOCK - 1, 1e-3);
}

TEST_F(SyncedAudioSourceTest, ANewSegmentFadesTheOldRunOutAndPlaysTheNewOne) {
    startPlaying();
    // A seek: the control thread asks for generation 2...
    transport(2, true, 3.0, T0 + BLOCK_NS);
    std::vector<float> out = source->pullBlock(T0 + BLOCK_NS);
    EXPECT_GT(out[0], 0.0f);  // ...the old run fades out
    EXPECT_NEAR(out[(BLOCK - 1) * 2], 0.0f, 1e-3);
    // ...and the decode thread answers with a run starting at timeline 3.0.
    segment(2, 3.0);
    pushFrames(1000000, RATE);
    out = source->pullBlock(T0 + 2 * BLOCK_NS);
    // Target 3.0 + 10 ms -> 480 frames into the new run, fading in.
    EXPECT_NEAR(out[(BLOCK - 1) * 2], 1000480.0f + BLOCK - 1, 1.0f);
    out = source->pullBlock(T0 + 3 * BLOCK_NS);
    EXPECT_FLOAT_EQ(out[0], 1000960.0f);
    EXPECT_EQ(source->playhead().generation, 2u);
}

// Callbacks stopped for a while (the output device was switched or rerouted): the
// buffered sound is stale, so the source lines up on the transport again instead of
// carrying on from where it stopped.
TEST_F(SyncedAudioSourceTest, AGapInCallbacksLinesUpAgain) {
    startPlaying();
    source->pullBlock(T0 + BLOCK_NS);  // frames 480..959
    const int64_t later = T0 + 300000000;  // 300 ms without a callback
    const std::vector<float> out = source->pullBlock(later);
    // Target: 0.3 s -> frame 14400 (not frame 960, where it stopped), faded in.
    EXPECT_NEAR(out[(BLOCK - 1) * 2], 14400.0f + BLOCK - 1, 1e-2);
    EXPECT_TRUE(source->playhead().aligned);
}

// Running out where the decode thread said the run ends is the end of the pass: silence,
// still in step, not an underrun (which would re-line-up and count against the stream).
TEST_F(SyncedAudioSourceTest, RunningOutAtTheStreamEndIsNotAnUnderrun) {
    startPlaying(700);
    source->publishStreamEnd(buffer->writePosition());
    std::vector<float> out = source->pullBlock(T0 + BLOCK_NS);
    EXPECT_FLOAT_EQ(out[219 * 2], 699.0f);
    for (size_t i = 220; i < BLOCK; ++i) ASSERT_EQ(out[i * 2], 0.0f);
    AudioPlayhead head = source->playhead();
    EXPECT_TRUE(head.aligned);
    EXPECT_TRUE(head.atEnd);
    EXPECT_EQ(head.underruns, 0u);
    out = source->pullBlock(T0 + 2 * BLOCK_NS);
    for (float v : out) ASSERT_EQ(v, 0.0f);
    EXPECT_EQ(source->playhead().underruns, 0u);

    // A new run clears the end.
    transport(2, true, 5.0, T0 + 3 * BLOCK_NS);
    source->clearStreamEnd();
    segment(2, 5.0);
    pushFrames(500000, RATE);
    source->pullBlock(T0 + 3 * BLOCK_NS);  // at the old end: nothing left to fade
    out = source->pullBlock(T0 + 4 * BLOCK_NS);
    EXPECT_FALSE(source->playhead().atEnd);
    EXPECT_NEAR(out[(BLOCK - 1) * 2], 500480.0f + BLOCK - 1, 1.0f);
}

TEST_F(SyncedAudioSourceTest, AnUnderrunGoesSilentAndLinesUpAgain) {
    startPlaying(700);  // frames 0..699; the first block used 0..479
    std::vector<float> out = source->pullBlock(T0 + BLOCK_NS);
    EXPECT_FLOAT_EQ(out[0], 480.0f);
    EXPECT_FLOAT_EQ(out[219 * 2], 699.0f);
    for (size_t i = 220; i < BLOCK; ++i) ASSERT_EQ(out[i * 2], 0.0f);
    AudioPlayhead head = source->playhead();
    EXPECT_FALSE(head.aligned);
    EXPECT_EQ(head.underruns, 1u);

    pushFrames(700, RATE);
    out = source->pullBlock(T0 + 3 * BLOCK_NS);  // target: frame 1440
    EXPECT_NEAR(out[(BLOCK - 1) * 2], 1440.0f + BLOCK - 1, 1e-2);
    EXPECT_TRUE(source->playhead().aligned);
}

}  // namespace test
}  // namespace quadmapper
