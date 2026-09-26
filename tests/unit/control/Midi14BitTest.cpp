#include "control/ControlDispatcher.hpp"
#include "control/ControlRegistry.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <set>

using namespace quadmapper::control;

namespace {

constexpr const char* kQuad = "11111111-aaaa-bbbb-cccc-000000000001";

struct Rig {
    ControlRegistry registry;
    ControlDispatcher dispatcher{registry};
    std::vector<MidiMapping> mappings;
    std::vector<float> opacityApplies;   // every value applied, in order
    float opacity = 0.0f;
    bool visible = false;
    int fires = 0;

    Rig() {
        registry.addEntityTarget("quad",
                                 {"opacity", TargetKind::Float, 0, 1, true,
                                  [this](const std::string&, float v) {
                                      opacity = v;
                                      opacityApplies.push_back(v);
                                  },
                                  [this](const std::string&) { return std::optional<float>(opacity); }});
        registry.addEntityTarget("quad",
                                 {"visible", TargetKind::Bool, 0, 1, true,
                                  [this](const std::string&, float v) { visible = v >= 0.5f; },
                                  [this](const std::string&) { return std::optional<float>(visible ? 1.f : 0.f); }});
        registry.addGlobal("/qm/go", {"", TargetKind::Trigger, 0, 1, true,
                                      [this](const std::string&, float) { ++fires; }, nullptr});
        registry.setEntities("quad", {{kQuad, "Wall"}});
    }

    static std::string path(const char* action) { return std::string("/qm/quad/") + kQuad + "/" + action; }

    void cc(int number, int value, double now, int channel = 1, const std::string& device = "Faders") {
        ControlEvent e;
        e.source = ControlEvent::Source::Midi;
        e.midi.type = MidiType::ControlChange;
        e.midi.number = static_cast<uint8_t>(number);
        e.midi.value = static_cast<uint8_t>(value);
        e.midi.channel = static_cast<uint8_t>(channel);
        std::copy(device.begin(), device.end(), e.device.begin());
        dispatcher.dispatch(e, mappings, now);
    }
    // A 14-bit move, MSB then LSB (the MIDI 1.0 order).
    void move(int number, int value14, double now) {
        cc(number, value14 >> 7, now);
        cc(number + 32, value14 & 0x7F, now);
    }
    MidiMapping& map14(int number, const char* action = "opacity", MidiMode mode = MidiMode::Value) {
        MidiMapping m;
        m.type = MidiControlType::CC;
        m.number = number;
        m.bits = 14;
        m.target = path(action);
        m.mode = mode;
        mappings.push_back(m);
        return mappings.back();
    }
};

float expected(int value14) { return static_cast<float>(value14) / 16383.0f; }

}  // namespace

// ---- Resolution ---------------------------------------------------------------------

TEST(Midi14BitTest, ValueModeUses16383Steps) {
    Rig rig;
    rig.map14(7);
    for (int v : {0, 1, 127, 128, 8191, 8192, 12345, 16382, 16383}) {
        rig.move(7, v, 1.0);
        EXPECT_FLOAT_EQ(rig.opacity, expected(v)) << v;
        EXPECT_EQ(rig.mappings[0].lastValue, v);
    }
    // Neighbouring 14-bit values land on different opacities: finer than 7-bit.
    std::set<float> distinct;
    for (int v = 8000; v < 8128; ++v) {
        rig.move(7, v, 2.0);
        distinct.insert(rig.opacity);
    }
    EXPECT_EQ(distinct.size(), 128u) << "128 consecutive 14-bit steps are all distinct";
}

TEST(Midi14BitTest, SevenBitMappingsAreUnchanged) {
    Rig rig;
    MidiMapping m;
    m.type = MidiControlType::CC;
    m.number = 7;
    m.target = Rig::path("opacity");
    m.mode = MidiMode::Value;
    rig.mappings.push_back(m);
    rig.cc(7, 64, 1.0);
    EXPECT_FLOAT_EQ(rig.opacity, 64.0f / 127.0f);
    rig.cc(39, 100, 1.0);  // the would-be LSB is another controller for a 7-bit mapping
    EXPECT_FLOAT_EQ(rig.opacity, 64.0f / 127.0f);
    EXPECT_EQ(rig.mappings[0].lastValue, 64);
}

// ---- Ordering -----------------------------------------------------------------------

TEST(Midi14BitTest, MsbThenLsbEndsOnTheExactValue) {
    Rig rig;
    rig.map14(1);
    rig.move(1, 0x2000 | 0x55, 1.0);
    EXPECT_FLOAT_EQ(rig.opacity, expected(0x2055));
    // MSB alone (coarse controller or a later LSB): LSB resets to 0 per MIDI 1.0.
    rig.cc(1, 0x50, 2.0);
    EXPECT_FLOAT_EQ(rig.opacity, expected(0x50 << 7));
    // LSB alone refines the current MSB.
    rig.cc(33, 0x7F, 3.0);
    EXPECT_FLOAT_EQ(rig.opacity, expected((0x50 << 7) | 0x7F));
}

TEST(Midi14BitTest, LsbThenMsbEndsOnTheExactValue) {
    Rig rig;
    rig.map14(1);
    // First ever message is an LSB: nothing to combine with yet, nothing applied.
    rig.cc(33, 0x11, 1.0);
    EXPECT_TRUE(rig.opacityApplies.empty());
    rig.cc(1, 0x22, 1.0);  // its MSB follows: the LSB just received is kept
    EXPECT_FLOAT_EQ(rig.opacity, expected((0x22 << 7) | 0x11));
    // Crossing an MSB boundary LSB-first: the final value is exact.
    rig.cc(33, 0x00, 2.0);
    rig.cc(1, 0x23, 2.0);
    EXPECT_FLOAT_EQ(rig.opacity, expected(0x23 << 7));
    rig.cc(33, 0x7F, 3.0);
    rig.cc(1, 0x40, 3.0);
    EXPECT_FLOAT_EQ(rig.opacity, expected((0x40 << 7) | 0x7F));
}

TEST(Midi14BitTest, AStaleLsbIsNotReusedByALaterMsb) {
    Rig rig;
    rig.map14(1);
    rig.cc(1, 0x10, 1.0);
    rig.cc(33, 0x33, 1.0);
    // Long after, an MSB alone: the old LSB is not kept (it belonged to the last pair).
    rig.cc(1, 0x11, 1.0 + k14BitPairSeconds + 0.5);
    EXPECT_FLOAT_EQ(rig.opacity, expected(0x11 << 7));
}

TEST(Midi14BitTest, OnlyTheMsbDrivesPressAndRelease) {
    Rig rig;
    rig.map14(2, "visible", MidiMode::Toggle);
    rig.move(2, 0x3FFF, 1.0);  // MSB 127 = press
    EXPECT_TRUE(rig.visible);
    rig.cc(34, 0x00, 1.0);     // LSB changes never press
    rig.cc(34, 0x7F, 1.0);
    EXPECT_TRUE(rig.visible);
    rig.move(2, 0, 2.0);       // release
    rig.move(2, 0x2000, 3.0);  // MSB 64 = press again
    EXPECT_FALSE(rig.visible);

    Rig value;
    value.map14(2, "visible", MidiMode::Value);
    value.move(2, 8191, 1.0);
    EXPECT_FALSE(value.visible);
    value.move(2, 8192, 1.0);
    EXPECT_TRUE(value.visible);
}

TEST(Midi14BitTest, ListensOnlyToItsPairChannelAndDevice) {
    Rig rig;
    MidiMapping& m = rig.map14(5);
    m.channel = 2;
    m.device = "Faders";
    rig.cc(5, 100, 1.0, 1);             // wrong channel
    rig.cc(5, 100, 1.0, 2, "Other");    // wrong device
    rig.cc(6, 100, 1.0, 2);             // another controller
    rig.cc(38, 100, 1.0, 2);            // LSB of CC 6
    EXPECT_TRUE(rig.opacityApplies.empty());
    rig.cc(5, 64, 1.0, 2);
    rig.cc(37, 1, 1.0, 2);
    EXPECT_FLOAT_EQ(rig.opacity, expected((64 << 7) | 1));
}

// ---- Learn --------------------------------------------------------------------------

TEST(Midi14BitTest, LearnDetectsAPairWhenTheLsbFollows) {
    Rig rig;
    rig.dispatcher.beginLearn(Rig::path("opacity"));
    rig.cc(7, 0x40, 5.0);
    ASSERT_EQ(rig.mappings.size(), 1u);
    EXPECT_EQ(rig.mappings[0].bits, 7) << "a single CC is 7-bit until its LSB shows up";
    rig.cc(39, 0x01, 5.0 + k14BitPairSeconds * 0.5);
    ASSERT_EQ(rig.mappings.size(), 1u);
    EXPECT_EQ(rig.mappings[0].bits, 14);
    EXPECT_EQ(rig.mappings[0].number, 7);
    EXPECT_EQ(rig.mappings[0].mode, MidiMode::Value);
    EXPECT_EQ(rig.mappings[0].lastValue, (0x40 << 7) | 1);
    EXPECT_TRUE(rig.opacityApplies.empty()) << "the learning gesture does not drive the target";
    // Next moves drive it at 14 bits.
    rig.move(7, 12000, 6.0);
    EXPECT_FLOAT_EQ(rig.opacity, expected(12000));
}

TEST(Midi14BitTest, LearnDetectsAnLsbFirstPairAndRenumbersToTheMsb) {
    Rig rig;
    rig.dispatcher.beginLearn(Rig::path("opacity"));
    rig.cc(39, 0x01, 5.0);
    rig.cc(7, 0x40, 5.0);
    ASSERT_EQ(rig.mappings.size(), 1u);
    EXPECT_EQ(rig.mappings[0].bits, 14);
    EXPECT_EQ(rig.mappings[0].number, 7);
    EXPECT_EQ(rig.mappings[0].lastValue, (0x40 << 7) | 1);
    EXPECT_EQ(rig.dispatcher.lastLearnedIndex(), 0);
    rig.cc(39, 0x02, 6.0);
    rig.cc(7, 0x41, 6.0);
    EXPECT_FLOAT_EQ(rig.opacity, expected((0x41 << 7) | 2));
}

TEST(Midi14BitTest, LearnStaysSevenBitWithoutAPairInTime) {
    Rig rig;
    rig.dispatcher.beginLearn(Rig::path("opacity"));
    rig.cc(7, 0x40, 5.0);
    rig.cc(39, 0x01, 5.0 + k14BitPairSeconds + 0.01);  // too late
    ASSERT_EQ(rig.mappings.size(), 1u);
    EXPECT_EQ(rig.mappings[0].bits, 7);

    Rig other;  // another controller, channel or device is not a pair
    other.dispatcher.beginLearn(Rig::path("opacity"));
    other.cc(7, 0x40, 5.0);
    other.cc(40, 0x01, 5.0);
    other.cc(39, 0x01, 5.0, 2);
    other.cc(39, 0x01, 5.0, 1, "Other");
    EXPECT_EQ(other.mappings[0].bits, 7);

    Rig high;  // a CC above 63 never pairs
    high.dispatcher.beginLearn(Rig::path("opacity"));
    high.cc(70, 1, 5.0);
    high.cc(102, 1, 5.0);
    EXPECT_EQ(high.mappings[0].bits, 7);
}

TEST(Midi14BitTest, RelearningAnLsbFirstPairOverAnExistingMappingKeepsOne) {
    Rig rig;
    MidiMapping existing;
    existing.device = "Faders";
    existing.type = MidiControlType::CC;
    existing.channel = 1;
    existing.number = 7;
    existing.target = Rig::path("opacity");
    existing.mode = MidiMode::Value;
    rig.mappings.push_back(existing);  // 7-bit CC 7

    rig.dispatcher.beginLearn(Rig::path("opacity"));
    rig.cc(39, 0x05, 5.0);  // creates a CC 39 mapping ...
    ASSERT_EQ(rig.mappings.size(), 2u);
    rig.cc(7, 0x10, 5.0);   // ... which becomes CC 7 14-bit, the same as the first
    ASSERT_EQ(rig.mappings.size(), 1u);
    EXPECT_EQ(rig.mappings[0].bits, 14);
    EXPECT_EQ(rig.mappings[0].number, 7);
    EXPECT_EQ(rig.dispatcher.lastLearnedIndex(), 0);
}

TEST(Midi14BitTest, APendingPairIsDroppedWhenTheMappingChanged) {
    Rig rig;
    rig.dispatcher.beginLearn(Rig::path("opacity"));
    rig.cc(7, 0x40, 5.0);
    rig.mappings.clear();  // deleted in the panel before the LSB arrived
    rig.cc(39, 0x01, 5.0);
    EXPECT_TRUE(rig.mappings.empty());
    EXPECT_TRUE(rig.opacityApplies.empty());
}

TEST(Midi14BitTest, SameBindingComparesBits) {
    MidiMapping a;
    a.type = MidiControlType::CC;
    a.number = 3;
    MidiMapping b = a;
    EXPECT_TRUE(sameBinding(a, b));
    b.bits = 14;
    EXPECT_FALSE(sameBinding(a, b));
    EXPECT_TRUE(b.is14Bit());
    b.type = MidiControlType::Note;
    EXPECT_FALSE(b.is14Bit()) << "only a CC can be 14-bit";
}
