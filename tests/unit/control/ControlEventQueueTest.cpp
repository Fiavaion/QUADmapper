#include "control/ControlEventQueue.hpp"

#include <gtest/gtest.h>

#include <thread>

using namespace quadmapper::control;

namespace {
ControlEvent oscEvent(const std::string& address) {
    ControlEvent e;
    e.source = ControlEvent::Source::Osc;
    e.osc.address = address;
    return e;
}
ControlEvent midiEvent(uint8_t number) {
    ControlEvent e;
    e.source = ControlEvent::Source::Midi;
    e.midi.type = MidiType::ControlChange;
    e.midi.number = number;
    return e;
}
}  // namespace

TEST(ControlEventQueueTest, DrainsInOrderUpToTheLimit) {
    ControlEventQueue q(8);
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.push(oscEvent("/" + std::to_string(i))));
    std::vector<ControlEvent> out;
    EXPECT_EQ(q.drain(out, 3), 3u);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].osc.address, "/0");
    EXPECT_EQ(out[2].osc.address, "/2");
    EXPECT_EQ(q.size(), 2u);
    EXPECT_EQ(q.drain(out, 100), 2u);
    EXPECT_EQ(out[4].osc.address, "/4");
    EXPECT_EQ(q.drain(out, 100), 0u);
}

TEST(ControlEventQueueTest, FullQueueDropsAndCounts) {
    ControlEventQueue q(4);
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.push(midiEvent(static_cast<uint8_t>(i))));
    EXPECT_FALSE(q.push(midiEvent(99)));
    EXPECT_FALSE(q.push(midiEvent(98)));
    EXPECT_EQ(q.droppedCount(), 2u);
    std::vector<ControlEvent> out;
    q.drain(out, 10);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[3].midi.number, 3) << "the oldest events are kept";
}

TEST(ControlEventQueueTest, WrapsAroundTheRing) {
    ControlEventQueue q(3);
    std::vector<ControlEvent> out;
    for (int round = 0; round < 10; ++round) {
        ASSERT_TRUE(q.push(midiEvent(static_cast<uint8_t>(round))));
        ASSERT_TRUE(q.push(midiEvent(static_cast<uint8_t>(round + 100))));
        out.clear();
        ASSERT_EQ(q.drain(out, 10), 2u);
        EXPECT_EQ(out[0].midi.number, round);
        EXPECT_EQ(out[1].midi.number, round + 100);
    }
}

TEST(ControlEventQueueTest, ConcurrentProducersLoseNothingBelowCapacity) {
    ControlEventQueue q(20000);
    static constexpr int kPerThread = 4000;
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&q, t] {
            for (int i = 0; i < kPerThread; ++i) {
                if (t % 2 == 0) q.push(oscEvent("/t"));
                else            q.push(midiEvent(static_cast<uint8_t>(i % 128)));
            }
        });
    }
    std::vector<ControlEvent> out;
    size_t drained = 0;
    while (drained < 4u * kPerThread) {
        drained += q.drain(out, 256);
        std::this_thread::yield();
    }
    for (auto& p : producers) p.join();
    drained += q.drain(out, 100000);
    EXPECT_EQ(drained, 4u * kPerThread);
    EXPECT_EQ(q.droppedCount(), 0u);
}
