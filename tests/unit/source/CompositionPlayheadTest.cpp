// Composition cue logic (ADR-061): holds, crossfades, loop, Switch mode, the events
// that start and pause the cues' clips. GL-free.

#include "source/CompositionSource.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using namespace quadmapper;

namespace {

CompositionSettings threeCues(CompositionMode mode, double hold = 2.0, double fade = 0.5) {
    CompositionSettings s;
    s.mode = mode;
    s.fadeSeconds = fade;
    for (int i = 0; i < 3; ++i) s.cues.push_back({UUID::generate(), hold});
    return s;
}

bool hasFaded(const CompositionEvents& e, int cue) {
    return std::find(e.faded.begin(), e.faded.end(), cue) != e.faded.end();
}

}  // namespace

TEST(CompositionPlayheadTest, TheFirstCueComesOnAtTheStart) {
    const CompositionSettings s = threeCues(CompositionMode::Timeline);
    CompositionPlayhead p;
    EXPECT_EQ(p.current(), 0);
    EXPECT_EQ(p.takeEvents().started, 0);
    EXPECT_EQ(p.takeEvents().started, -1);  // once
    EXPECT_DOUBLE_EQ(p.fadeMix(s), 1.0);
}

TEST(CompositionPlayheadTest, TimelineHoldsThenCrossfadesToTheNextCue) {
    const CompositionSettings s = threeCues(CompositionMode::Timeline, 2.0, 0.5);
    CompositionPlayhead p;
    p.takeEvents();
    p.step(1.9, s);
    EXPECT_EQ(p.current(), 0);
    p.step(0.2, s);  // 2.1 s: cue 2 is on, 0.1 s into its fade
    EXPECT_EQ(p.current(), 1);
    EXPECT_EQ(p.previous(), 0);
    EXPECT_NEAR(p.timeInCue(), 0.1, 1e-9);
    EXPECT_NEAR(p.fadeMix(s), 0.2, 1e-9);
    CompositionEvents e = p.takeEvents();
    EXPECT_EQ(e.started, 1);
    EXPECT_TRUE(e.faded.empty());
    p.step(0.4, s);  // the fade is over
    EXPECT_EQ(p.previous(), -1);
    EXPECT_DOUBLE_EQ(p.fadeMix(s), 1.0);
    e = p.takeEvents();
    EXPECT_TRUE(hasFaded(e, 0));
}

TEST(CompositionPlayheadTest, TimelineLoopsOrHoldsOnTheLastCue) {
    CompositionSettings s = threeCues(CompositionMode::Timeline, 1.0, 0.0);
    CompositionPlayhead looping;
    for (int i = 0; i < 3; ++i) looping.step(1.0, s);
    EXPECT_EQ(looping.current(), 0);  // round to the first

    s.loop = false;
    CompositionPlayhead once;
    for (int i = 0; i < 5; ++i) once.step(1.0, s);
    EXPECT_EQ(once.current(), 2);
    EXPECT_FALSE(once.running());
}

TEST(CompositionPlayheadTest, AHoldOfZeroWaitsForACommand) {
    CompositionSettings s = threeCues(CompositionMode::Timeline, 0.0);
    CompositionPlayhead p;
    p.step(100.0, s);
    EXPECT_EQ(p.current(), 0);
    p.next(s);
    EXPECT_EQ(p.current(), 1);
}

TEST(CompositionPlayheadTest, SwitchModeChangesOnlyOnCommand) {
    const CompositionSettings s = threeCues(CompositionMode::Switch, 1.0, 0.0);
    CompositionPlayhead p;
    p.takeEvents();
    p.step(10.0, s);
    EXPECT_EQ(p.current(), 0);
    p.cue(2, s);
    EXPECT_EQ(p.current(), 2);
    CompositionEvents e = p.takeEvents();
    EXPECT_EQ(e.started, 2);
    EXPECT_TRUE(hasFaded(e, 0));  // a cut: out at once
    p.next(s);
    EXPECT_EQ(p.current(), 0);  // wraps
    p.prev(s);
    EXPECT_EQ(p.current(), 2);
    p.cue(99, s);
    EXPECT_EQ(p.current(), 2);  // clamped
}

TEST(CompositionPlayheadTest, ACueDuringAFadeCutsTheOldOneShort) {
    const CompositionSettings s = threeCues(CompositionMode::Switch, 1.0, 1.0);
    CompositionPlayhead p;
    p.cue(1, s);
    p.step(0.3, s);
    p.takeEvents();
    p.cue(2, s);  // cue 0 was still fading out
    EXPECT_EQ(p.previous(), 1);
    EXPECT_TRUE(hasFaded(p.takeEvents(), 0));
}

TEST(CompositionPlayheadTest, PauseStopsTheHoldButNotAFade) {
    const CompositionSettings s = threeCues(CompositionMode::Timeline, 1.0, 1.0);
    CompositionPlayhead p;
    p.cue(1, s);
    p.pause();
    p.step(5.0, s);
    EXPECT_EQ(p.current(), 1);     // held
    EXPECT_EQ(p.previous(), -1);   // the fade finished
    p.play();
    p.step(1.0, s);
    EXPECT_EQ(p.current(), 2);
}

TEST(CompositionPlayheadTest, RemovedCuesAreClampedAndRestartGoesToTheFirst) {
    CompositionSettings s = threeCues(CompositionMode::Switch);
    CompositionPlayhead p;
    p.cue(2, s);
    s.cues.resize(1);
    p.step(0.0, s);
    EXPECT_EQ(p.current(), 0);
    EXPECT_EQ(p.previous(), -1);
    p.restart(s);
    EXPECT_EQ(p.current(), 0);
    EXPECT_EQ(p.takeEvents().started, 0);
    s.cues.clear();
    p.next(s);  // nothing to do, no crash
    p.step(1.0, s);
    EXPECT_EQ(p.current(), 0);
}
