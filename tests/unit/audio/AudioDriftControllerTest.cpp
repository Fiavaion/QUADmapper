// The drift rule of ADR-050 §2, against synthetic clocks: no device, no decoder.

#include <gtest/gtest.h>
#include "audio/AudioDriftController.hpp"
#include "audio/SyncedAudioSource.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace quadmapper {
namespace test {

TEST(AudioDriftControllerTest, WithinTwoMillisecondsDoesNothing) {
    AudioDriftController controller;
    for (double drift : {0.0, 0.0015, -0.0019, 0.002, -0.002}) {
        const AudioDriftDecision d = controller.update(drift);
        EXPECT_FALSE(d.hardResync) << drift;
        EXPECT_EQ(d.correction, 0) << drift;
    }
}

TEST(AudioDriftControllerTest, AheadRepeatsAndBehindDrops) {
    AudioDriftController ahead;
    EXPECT_EQ(ahead.update(0.010).correction, +1);  // audio ahead: repeat (slow down)
    AudioDriftController behind;
    EXPECT_EQ(behind.update(-0.010).correction, -1);  // behind: drop (catch up)
}

TEST(AudioDriftControllerTest, CorrectionStopsOnceBackWithinTwoMilliseconds) {
    AudioDriftController controller;
    ASSERT_EQ(controller.update(0.020).correction, 1);
    int frames = 0;
    double drift = 0.020;
    while (controller.update(drift).correction != 0 && frames < 10000) {
        drift -= 0.001 / 60.0;  // 1 ms per second, as a 1-in-1000 repeat gives
        ++frames;
    }
    EXPECT_LT(frames, 10000);
    EXPECT_LE(std::abs(controller.smoothedDrift()), AudioDriftController::SOFT_THRESHOLD);
}

TEST(AudioDriftControllerTest, OverEightyMillisecondsResyncsOnTheSecondFrame) {
    AudioDriftController controller;
    EXPECT_FALSE(controller.update(0.120).hardResync);  // one late timestamp is not enough
    EXPECT_TRUE(controller.update(0.120).hardResync);

    AudioDriftController behind;
    EXPECT_FALSE(behind.update(-0.081).hardResync);
    EXPECT_TRUE(behind.update(-0.081).hardResync);

    AudioDriftController exactly;  // 80 ms is still soft
    exactly.update(0.080);
    EXPECT_FALSE(exactly.update(0.080).hardResync);
}

TEST(AudioDriftControllerTest, ASingleOutlierDoesNotResync) {
    AudioDriftController controller;
    controller.update(0.001);
    EXPECT_FALSE(controller.update(0.200).hardResync);
    EXPECT_FALSE(controller.update(0.001).hardResync);
    EXPECT_FALSE(controller.update(0.200).hardResync);
}

TEST(AudioDriftControllerTest, NonFiniteDriftResets) {
    AudioDriftController controller;
    controller.update(0.02);
    const AudioDriftDecision d = controller.update(std::nan(""));
    EXPECT_FALSE(d.hardResync);
    EXPECT_EQ(d.correction, 0);
    EXPECT_EQ(controller.correction(), 0);
}

// A device whose clock runs fast or slow against the wall clock, measured once per
// 60 Hz frame with callback-timestamp jitter, corrected at the 0.1 % the callback
// applies (one sample in CORRECTION_INTERVAL). Over a simulated minute the drift must
// stay near zero without a single hard resync.
struct SkewResult {
    double maxDrift = 0.0;
    double maxLateDrift = 0.0;  // after `settleSeconds`
    int hardResyncs = 0;
};

static SkewResult simulateSkew(double skewPpm, double jitterSeconds, double initialDrift, unsigned seed,
                               int settleSeconds = 10) {
    AudioDriftController controller;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jitter(-jitterSeconds, jitterSeconds);
    const double frame = 1.0 / 60.0;
    const double correctionRate = 1.0 / static_cast<double>(SyncedAudioSource::CORRECTION_INTERVAL);
    double drift = initialDrift;
    int correction = 0;
    SkewResult result;
    for (int i = 0; i < 60 * 60; ++i) {
        drift += frame * skewPpm * 1e-6;           // the device's own rate error
        drift -= frame * correction * correctionRate;  // +1 repeats: audio falls back
        const AudioDriftDecision d = controller.update(drift + jitter(rng));
        if (d.hardResync) {
            ++result.hardResyncs;
            drift = 0.0;
        }
        correction = d.correction;
        result.maxDrift = std::max(result.maxDrift, std::abs(drift));
        if (i > 60 * settleSeconds) result.maxLateDrift = std::max(result.maxLateDrift, std::abs(drift));
    }
    return result;
}

TEST(AudioDriftControllerTest, HoldsAFastDeviceWithinThreeMilliseconds) {
    const SkewResult r = simulateSkew(+300.0, 0.0015, 0.0, 1);
    EXPECT_EQ(r.hardResyncs, 0);
    EXPECT_LT(r.maxDrift, 0.003);
}

TEST(AudioDriftControllerTest, HoldsASlowDeviceWithinThreeMilliseconds) {
    const SkewResult r = simulateSkew(-300.0, 0.0015, 0.0, 2);
    EXPECT_EQ(r.hardResyncs, 0);
    EXPECT_LT(r.maxDrift, 0.003);
}

TEST(AudioDriftControllerTest, SlewsAFortyMillisecondOffsetBackWithoutAResync) {
    // 1 ms per second: 40 ms is gone in ~40 s; after that it stays within 3 ms.
    const SkewResult r = simulateSkew(0.0, 0.0015, 0.040, 3, 45);
    EXPECT_EQ(r.hardResyncs, 0);
    EXPECT_LT(r.maxDrift, 0.041);
    EXPECT_LT(r.maxLateDrift, 0.003);
}

TEST(AudioDriftControllerTest, JitterAloneNeverTriggersAnything) {
    AudioDriftController controller;
    std::mt19937 rng(4);
    std::uniform_real_distribution<double> jitter(-0.0015, 0.0015);
    int corrections = 0;
    for (int i = 0; i < 3600; ++i) {
        const AudioDriftDecision d = controller.update(jitter(rng));
        EXPECT_FALSE(d.hardResync);
        corrections += d.correction != 0;
    }
    EXPECT_EQ(corrections, 0);
}

}  // namespace test
}  // namespace quadmapper
