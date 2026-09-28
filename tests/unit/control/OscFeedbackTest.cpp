#include "control/OscFeedback.hpp"

#include <gtest/gtest.h>

#include <map>

using namespace quadmapper::control;

namespace {

OscArg f(float v) { OscArg a; a.type = OscArg::Type::Float; a.tag = 'f'; a.f = v; return a; }
OscArg i(int v) { OscArg a; a.type = OscArg::Type::Int; a.tag = 'i'; a.i = v; return a; }

OscFeedback::Sample sample(const std::string& key, float v) { return {key, key, f(v)}; }

// Collect at `now` and return "address=value" strings.
std::vector<std::string> collect(OscFeedback& fb, double now, size_t max = 1000) {
    std::vector<OscMessage> out;
    fb.collect(now, out, max);
    std::vector<std::string> text;
    for (const auto& m : out) {
        EXPECT_EQ(m.args.size(), 1u);
        const OscArg& a = m.args[0];
        text.push_back(m.address + "=" +
                       (a.type == OscArg::Type::Int ? std::to_string(a.i) : std::to_string(a.f).substr(0, 4)));
    }
    return text;
}

constexpr double kInterval = OscFeedback::kDefaultMinInterval;

}  // namespace

TEST(OscFeedbackTest, SendsEverythingOnceThenOnlyChanges) {
    OscFeedback fb;
    fb.setValues({sample("/a", 0.5f), sample("/b", 1.0f)});
    EXPECT_EQ(collect(fb, 0.0), (std::vector<std::string>{"/a=0.50", "/b=1.00"}));
    fb.setValues({sample("/a", 0.5f), sample("/b", 1.0f)});
    EXPECT_TRUE(collect(fb, 1.0).empty()) << "unchanged values are not resent";
    fb.setValues({sample("/a", 0.5f), sample("/b", 0.0f)});
    EXPECT_EQ(collect(fb, 2.0), (std::vector<std::string>{"/b=0.00"}));
    EXPECT_EQ(fb.pendingCount(), 0u);
}

TEST(OscFeedbackTest, RateLimitsEachAddressAndCoalescesToTheLatestValue) {
    OscFeedback fb;
    fb.setValues({sample("/fader", 0.1f), sample("/other", 0.0f)});
    ASSERT_EQ(collect(fb, 10.0).size(), 2u);

    // Three changes inside one interval: nothing until the interval is over, then
    // only the latest value.
    fb.setValues({sample("/fader", 0.2f), sample("/other", 0.0f)});
    EXPECT_TRUE(collect(fb, 10.0 + kInterval * 0.3).empty());
    fb.setValues({sample("/fader", 0.3f), sample("/other", 0.0f)});
    EXPECT_TRUE(collect(fb, 10.0 + kInterval * 0.6).empty());
    fb.setValues({sample("/fader", 0.4f), sample("/other", 0.0f)});
    EXPECT_EQ(fb.pendingCount(), 1u);
    EXPECT_TRUE(collect(fb, 10.0 + kInterval * 0.99).empty());
    EXPECT_EQ(collect(fb, 10.0 + kInterval * 1.01), (std::vector<std::string>{"/fader=0.40"}));

    // Per address: /other was last sent at 10.0, so it may go now while /fader waits.
    fb.setValues({sample("/fader", 0.5f), sample("/other", 1.0f)});
    EXPECT_EQ(collect(fb, 10.0 + kInterval * 1.5), (std::vector<std::string>{"/other=1.00"}));
    EXPECT_EQ(collect(fb, 10.0 + kInterval * 2.05), (std::vector<std::string>{"/fader=0.50"}));
}

TEST(OscFeedbackTest, AtMostOneMessagePerIntervalUnderAContinuousSweep) {
    OscFeedback fb;
    size_t sent = 0;
    // A fader moving every 1 ms for one second, collected every millisecond.
    for (int ms = 0; ms <= 1000; ++ms) {
        fb.setValues({sample("/sweep", static_cast<float>(ms) / 1000.0f)});
        sent += collect(fb, ms / 1000.0).size();
    }
    EXPECT_LE(sent, 31u);  // <= 30 Hz (+1 for the first at t=0)
    EXPECT_GE(sent, 29u);
    // The last value reaches the controller once the interval allows.
    const auto last = collect(fb, 2.0);
    ASSERT_EQ(last.size(), 1u);
    EXPECT_EQ(last[0], "/sweep=1.00");
}

TEST(OscFeedbackTest, AValueThatReturnsBeforeItIsDueSendsNothing) {
    OscFeedback fb;
    fb.setValues({sample("/x", 0.5f)});
    collect(fb, 0.0);
    fb.setValues({sample("/x", 0.9f)});
    EXPECT_TRUE(collect(fb, kInterval * 0.5).empty());
    fb.setValues({sample("/x", 0.5f)});  // back to what the controller has
    EXPECT_EQ(fb.pendingCount(), 0u);
    EXPECT_TRUE(collect(fb, 5.0).empty());
}

TEST(OscFeedbackTest, SendAllResendsEveryValueRespectingTheRateLimit) {
    OscFeedback fb;
    fb.setValues({sample("/a", 1.0f), sample("/b", 2.0f)});
    collect(fb, 0.0);
    fb.requestSendAll();
    EXPECT_EQ(fb.pendingCount(), 2u);
    EXPECT_TRUE(collect(fb, kInterval * 0.5).empty());
    EXPECT_EQ(collect(fb, kInterval), (std::vector<std::string>{"/a=1.00", "/b=2.00"}));
    EXPECT_TRUE(collect(fb, 10.0).empty());
}

TEST(OscFeedbackTest, ResetMakesEverythingNewAgain) {
    OscFeedback fb;
    fb.setValues({sample("/a", 1.0f)});
    collect(fb, 0.0);
    fb.reset();  // a new destination: it has nothing
    EXPECT_EQ(collect(fb, 0.001), (std::vector<std::string>{"/a=1.00"})) << "no rate limit after reset";
}

TEST(OscFeedbackTest, MarkKnownSuppressesTheEchoButNotACorrection) {
    OscFeedback fb;
    fb.setValues({sample("/q/opacity", 0.0f)});
    collect(fb, 0.0);
    // The controller sent 0.7 and the target took it: nothing to send back.
    fb.markKnown("/q/opacity", f(0.7f));
    fb.setValues({sample("/q/opacity", 0.7f)});
    EXPECT_TRUE(collect(fb, 1.0).empty());
    // The controller sent 5 but the target clamped to 1: the controller is corrected.
    fb.markKnown("/q/opacity", f(5.0f));
    fb.setValues({sample("/q/opacity", 1.0f)});
    EXPECT_EQ(collect(fb, 2.0), (std::vector<std::string>{"/q/opacity=1.00"}));
    fb.markKnown("/unknown", f(1.0f));  // ignored
    EXPECT_EQ(fb.size(), 1u);
}

TEST(OscFeedbackTest, ForgetsVanishedKeysAndResendsOnAnAddressChange) {
    OscFeedback fb;
    fb.setValues({{"/qm/quad/id1/opacity", "/qm/quad/Left/opacity", f(0.5f)},
                  {"/qm/quad/id2/opacity", "/qm/quad/Right/opacity", f(0.5f)}});
    collect(fb, 0.0);
    // id2 deleted; id1 renamed: its new address has never been sent.
    fb.setValues({{"/qm/quad/id1/opacity", "/qm/quad/Wall/opacity", f(0.5f)}});
    EXPECT_EQ(fb.size(), 1u);
    EXPECT_EQ(collect(fb, 5.0), (std::vector<std::string>{"/qm/quad/Wall/opacity=0.50"}));
}

TEST(OscFeedbackTest, TypeChangeCountsAsAChangeAndIntegersCompareExactly) {
    OscFeedback fb;
    fb.setValues({{"/qm/scene/active", "/qm/scene/active", i(1)}});
    EXPECT_EQ(collect(fb, 0.0), (std::vector<std::string>{"/qm/scene/active=1"}));
    fb.setValues({{"/qm/scene/active", "/qm/scene/active", i(1)}});
    EXPECT_TRUE(collect(fb, 1.0).empty());
    fb.setValues({{"/qm/scene/active", "/qm/scene/active", i(2)}});
    EXPECT_EQ(collect(fb, 2.0), (std::vector<std::string>{"/qm/scene/active=2"}));
    fb.markKnown("/qm/scene/active", f(2.0f));  // same number, other type
    EXPECT_EQ(fb.pendingCount(), 1u);
}

TEST(OscFeedbackTest, CapsMessagesPerCallAndServesEveryAddressInTurn) {
    OscFeedback fb;
    std::vector<OscFeedback::Sample> samples;
    for (int k = 0; k < 10; ++k) samples.push_back(sample("/k" + std::to_string(k), 0.0f));
    fb.setValues(samples);
    EXPECT_EQ(collect(fb, 0.0, 4), (std::vector<std::string>{"/k0=0.00", "/k1=0.00", "/k2=0.00", "/k3=0.00"}));
    EXPECT_EQ(collect(fb, 0.0, 4), (std::vector<std::string>{"/k4=0.00", "/k5=0.00", "/k6=0.00", "/k7=0.00"}));
    EXPECT_EQ(collect(fb, 0.0, 4), (std::vector<std::string>{"/k8=0.00", "/k9=0.00"}));

    // Every address changing every interval with a cap of 4: nobody starves.
    std::map<std::string, int> sends;
    for (int round = 1; round <= 30; ++round) {
        samples.clear();
        for (int k = 0; k < 10; ++k) samples.push_back(sample("/k" + std::to_string(k), static_cast<float>(round)));
        fb.setValues(samples);
        for (const auto& text : collect(fb, round * kInterval * 1.01, 4)) ++sends[text.substr(0, 3)];
    }
    ASSERT_EQ(sends.size(), 10u);
    for (const auto& [address, count] : sends) EXPECT_GE(count, 10) << address;
    OscFeedback empty;
    EXPECT_TRUE(collect(empty, 0.0).empty());
    std::vector<OscMessage> none;
    EXPECT_EQ(fb.collect(100.0, none, 0), 0u);
}
