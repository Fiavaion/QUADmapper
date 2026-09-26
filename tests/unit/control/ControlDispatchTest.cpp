#include "control/ControlDispatcher.hpp"
#include "control/ControlRegistry.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <map>

using namespace quadmapper::control;

namespace {

// A registry over fake entities that records every apply, for exact assertions.
struct Fixture {
    ControlRegistry registry;
    ControlDispatcher dispatcher{registry};
    std::vector<MidiMapping> mappings;

    std::map<std::string, float> opacity;   // entity id -> value
    std::map<std::string, bool> visible;
    int blackoutValue = 0;
    int blackoutApplies = 0;
    std::vector<int> takes;
    std::vector<std::string> plays;

    Fixture() {
        registry.addGlobal("/qm/blackout",
                           {"", TargetKind::Bool, 0, 1, true,
                            [this](const std::string&, float v) {
                                blackoutValue = static_cast<int>(v);
                                ++blackoutApplies;
                            },
                            [this](const std::string&) { return std::optional<float>(float(blackoutValue)); }});
        registry.addGlobal("/qm/scene/take",
                           {"", TargetKind::Index, 1, 1e6f, false,
                            [this](const std::string&, float v) { takes.push_back(static_cast<int>(v)); },
                            nullptr});
        registry.addEntityTarget("quad",
                                 {"opacity", TargetKind::Float, 0, 1, true,
                                  [this](const std::string& id, float v) { opacity[id] = v; },
                                  [this](const std::string& id) { return std::optional<float>(opacity[id]); }});
        registry.addEntityTarget("quad",
                                 {"visible", TargetKind::Bool, 0, 1, true,
                                  [this](const std::string& id, float v) { visible[id] = v >= 0.5f; },
                                  [this](const std::string& id) { return std::optional<float>(visible[id] ? 1.f : 0.f); }});
        registry.addEntityTarget("source",
                                 {"play", TargetKind::Trigger, 0, 1, true,
                                  [this](const std::string& id, float) { plays.push_back(id); }, nullptr});
        registry.addEntityTarget("source",
                                 {"speed", TargetKind::Float, 0.1f, 4.0f, true,
                                  [this](const std::string& id, float v) { opacity["speed:" + id] = v; }, nullptr});
        registry.setEntities("quad", {{"11111111-aaaa-bbbb-cccc-000000000001", "Left Wall"},
                                      {"11111111-aaaa-bbbb-cccc-000000000002", "Right"},
                                      {"11111111-aaaa-bbbb-cccc-000000000003", "right"}});
        registry.setEntities("source", {{"22222222-aaaa-bbbb-cccc-000000000001", "clip.mp4"}});
    }

    void osc(const std::string& address, std::vector<OscArg> args = {}) {
        ControlEvent e;
        e.source = ControlEvent::Source::Osc;
        e.osc.address = address;
        e.osc.args = std::move(args);
        dispatcher.dispatch(e, mappings, 1.0);
    }
    void midi(MidiType type, uint8_t number, uint8_t value, uint8_t channel = 1,
              const std::string& device = "Pad") {
        ControlEvent e;
        e.source = ControlEvent::Source::Midi;
        e.midi.type = type;
        e.midi.number = number;
        e.midi.value = value;
        e.midi.channel = channel;
        std::copy(device.begin(), device.end(), e.device.begin());
        dispatcher.dispatch(e, mappings, 2.0);
    }
    MidiMapping& map(MidiControlType type, int number, const std::string& target, MidiMode mode) {
        MidiMapping m;
        m.type = type;
        m.number = number;
        m.target = target;
        m.mode = mode;
        mappings.push_back(m);
        return mappings.back();
    }
};

OscArg f(float v) { OscArg a; a.type = OscArg::Type::Float; a.f = v; return a; }
OscArg i(int v) { OscArg a; a.type = OscArg::Type::Int; a.i = v; return a; }
OscArg s(const std::string& v) { OscArg a; a.type = OscArg::Type::String; a.s = v; return a; }
OscArg t() { OscArg a; a.type = OscArg::Type::True; return a; }
OscArg fl() { OscArg a; a.type = OscArg::Type::False; return a; }

const std::string kLeft = "11111111-aaaa-bbbb-cccc-000000000001";
const std::string kRight = "11111111-aaaa-bbbb-cccc-000000000002";
const std::string kSource = "22222222-aaaa-bbbb-cccc-000000000001";

}  // namespace

// ---- Registry resolution -------------------------------------------------------

TEST(ControlRegistryTest, ResolvesByUuidThenCaseInsensitiveName) {
    Fixture fx;
    auto byId = fx.registry.resolve("/qm/quad/" + kLeft + "/opacity");
    ASSERT_TRUE(byId);
    EXPECT_EQ(byId->entityId, kLeft);
    EXPECT_EQ(byId->canonicalPath, "/qm/quad/" + kLeft + "/opacity");

    auto byUpperId = fx.registry.resolve("/qm/quad/11111111-AAAA-BBBB-CCCC-000000000001/opacity");
    ASSERT_TRUE(byUpperId);
    EXPECT_EQ(byUpperId->entityId, kLeft);

    auto byName = fx.registry.resolve("/qm/quad/left wall/OPACITY");
    ASSERT_TRUE(byName);
    EXPECT_EQ(byName->entityId, kLeft);
    EXPECT_EQ(byName->target->action, "opacity");
}

TEST(ControlRegistryTest, UuidWinsOverAnEntityNamedLikeAnotherUuid) {
    Fixture fx;
    fx.registry.setEntities("quad", {{"id-a", "id-b"}, {"id-b", "Other"}});
    auto r = fx.registry.resolve("/qm/quad/id-b/opacity");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->entityId, "id-b");
}

TEST(ControlRegistryTest, AmbiguousNameUsesFirstInListOrder) {
    Fixture fx;
    auto r = fx.registry.resolve("/qm/quad/RIGHT/visible");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->entityId, kRight);
    auto again = fx.registry.resolve("/qm/quad/RIGHT/visible");  // warns once only
    ASSERT_TRUE(again);
    EXPECT_EQ(again->entityId, kRight);
}

TEST(ControlRegistryTest, UnknownAddressesDoNotResolve) {
    Fixture fx;
    EXPECT_FALSE(fx.registry.resolve(""));
    EXPECT_FALSE(fx.registry.resolve("/"));
    EXPECT_FALSE(fx.registry.resolve("/qm"));
    EXPECT_FALSE(fx.registry.resolve("/qm/quad/Left Wall"));
    EXPECT_FALSE(fx.registry.resolve("/qm/quad/Nobody/opacity"));
    EXPECT_FALSE(fx.registry.resolve("/qm/quad/Left Wall/rotation"));
    EXPECT_FALSE(fx.registry.resolve("/qm/robot/Left Wall/opacity"));
    EXPECT_FALSE(fx.registry.resolve("/xx/quad/Left Wall/opacity"));
    EXPECT_FALSE(fx.registry.resolve("/qm/quad/Left Wall/opacity/extra"));
    EXPECT_FALSE(fx.registry.resolve("/qm/quad//opacity"));
    EXPECT_FALSE(fx.registry.resolve("qm/blackout"));
}

TEST(ControlRegistryTest, GlobalsMatchCaseInsensitively) {
    Fixture fx;
    auto r = fx.registry.resolve("/QM/Blackout");
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->entityId.empty());
    EXPECT_EQ(r->canonicalPath, "/qm/blackout");
}

TEST(ControlRegistryTest, ListsOnlyLearnableTargetsWithCanonicalPaths) {
    Fixture fx;
    const auto list = fx.registry.listTargets();
    // blackout + 3 quads x 2 + 1 source x 2; /qm/scene/take is not learnable.
    ASSERT_EQ(list.size(), 1u + 6u + 2u);
    EXPECT_EQ(list[0].path, "/qm/blackout");
    EXPECT_EQ(list[1].path, "/qm/quad/" + kLeft + "/opacity");
    EXPECT_EQ(list[1].label, "Quad \"Left Wall\" opacity");
    for (const auto& target : list) EXPECT_NE(target.path, "/qm/scene/take");
}

TEST(ControlRegistryTest, DescribeReportsUnresolvedTargets) {
    Fixture fx;
    EXPECT_EQ(fx.registry.describe("/qm/quad/" + kLeft + "/visible").value_or(""),
              "Quad \"Left Wall\" visible");
    EXPECT_FALSE(fx.registry.describe("/qm/quad/deleted-quad-id/visible"));
    fx.registry.setEntities("quad", {});
    EXPECT_FALSE(fx.registry.describe("/qm/quad/" + kLeft + "/visible"));
}

// ---- OSC semantics ---------------------------------------------------------------

TEST(ControlDispatchOscTest, FloatTargetsClampAndNeedAValue) {
    Fixture fx;
    fx.osc("/qm/quad/Left Wall/opacity", {f(0.25f)});
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 0.25f);
    fx.osc("/qm/quad/Left Wall/opacity", {f(7.0f)});
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 1.0f);
    fx.osc("/qm/quad/Left Wall/opacity", {i(-3)});
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 0.0f);
    fx.osc("/qm/source/clip.mp4/speed", {f(10.0f)});
    EXPECT_FLOAT_EQ(fx.opacity["speed:" + kSource], 4.0f);
    fx.osc("/qm/source/clip.mp4/speed", {f(0.0f)});
    EXPECT_FLOAT_EQ(fx.opacity["speed:" + kSource], 0.1f);

    fx.opacity[kLeft] = 0.5f;
    fx.osc("/qm/quad/Left Wall/opacity");  // no value
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 0.5f);
    EXPECT_EQ(fx.dispatcher.badArguments(), 1u);
}

TEST(ControlDispatchOscTest, NonFiniteAndNonNumericValuesAreRejected) {
    Fixture fx;
    fx.opacity[kLeft] = 0.5f;
    fx.osc("/qm/quad/Left Wall/opacity", {f(std::numeric_limits<float>::quiet_NaN())});
    fx.osc("/qm/quad/Left Wall/opacity", {f(std::numeric_limits<float>::infinity())});
    fx.osc("/qm/quad/Left Wall/opacity", {s("half")});
    OscArg blob;
    blob.type = OscArg::Type::Unsupported;
    blob.tag = 'b';
    fx.osc("/qm/quad/Left Wall/opacity", {blob});
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 0.5f);
    EXPECT_EQ(fx.dispatcher.badArguments(), 4u);
}

TEST(ControlDispatchOscTest, BoolTargetsSetOrToggle) {
    Fixture fx;
    fx.osc("/qm/blackout", {i(1)});
    EXPECT_EQ(fx.blackoutValue, 1);
    fx.osc("/qm/blackout", {f(0.0f)});
    EXPECT_EQ(fx.blackoutValue, 0);
    fx.osc("/qm/blackout", {t()});
    EXPECT_EQ(fx.blackoutValue, 1);
    fx.osc("/qm/blackout", {fl()});
    EXPECT_EQ(fx.blackoutValue, 0);
    fx.osc("/qm/blackout");  // a trigger toggles
    EXPECT_EQ(fx.blackoutValue, 1);
    fx.osc("/qm/blackout");
    EXPECT_EQ(fx.blackoutValue, 0);
    fx.osc("/qm/blackout", {f(0.49f)});
    EXPECT_EQ(fx.blackoutValue, 0);
    fx.osc("/qm/blackout", {f(0.5f)});
    EXPECT_EQ(fx.blackoutValue, 1);
}

TEST(ControlDispatchOscTest, TriggersFireOnPressNotRelease) {
    Fixture fx;
    fx.osc("/qm/source/clip.mp4/play");            // no argument
    fx.osc("/qm/source/clip.mp4/play", {f(1.0f)}); // button down
    fx.osc("/qm/source/clip.mp4/play", {f(0.0f)}); // button up: ignored
    EXPECT_EQ(fx.plays.size(), 2u);
}

TEST(ControlDispatchOscTest, SceneTakeNeedsANumber) {
    Fixture fx;
    fx.osc("/qm/scene/take", {i(2)});
    fx.osc("/qm/scene/take", {f(3.4f)});
    fx.osc("/qm/scene/take");           // no number
    fx.osc("/qm/scene/take", {i(0)});   // below 1
    fx.osc("/qm/scene/take", {i(-5)});
    EXPECT_EQ(fx.takes, (std::vector<int>{2, 3}));
    EXPECT_EQ(fx.dispatcher.badArguments(), 3u);
}

TEST(ControlDispatchOscTest, UnknownAddressesAreCountedAndListed) {
    Fixture fx;
    fx.osc("/nothing/here");
    fx.osc("/qm/quad/Nobody/opacity", {f(1.0f)});
    fx.osc("/nothing/here");
    EXPECT_EQ(fx.dispatcher.unknownAddresses(), 3u);
    ASSERT_EQ(fx.dispatcher.recentUnknown().size(), 2u);  // distinct, newest first
    EXPECT_EQ(fx.dispatcher.recentUnknown().front(), "/nothing/here");
    EXPECT_EQ(fx.dispatcher.oscMessages(), 3u);
    EXPECT_EQ(fx.dispatcher.recentOsc().front(), "/nothing/here");
}

TEST(ControlDispatchOscTest, RecentListsAreBounded) {
    Fixture fx;
    for (int n = 0; n < 100; ++n) fx.osc("/unknown/" + std::to_string(n), {i(n)});
    EXPECT_EQ(fx.dispatcher.recentOsc().size(), ControlDispatcher::kRecentMessages);
    EXPECT_EQ(fx.dispatcher.recentUnknown().size(), ControlDispatcher::kRecentUnknown);
    EXPECT_EQ(fx.dispatcher.recentOsc().front(), "/unknown/99 99");
}

TEST(ControlDispatchOscTest, ActivityIsRecordedByCanonicalPath) {
    Fixture fx;
    EXPECT_LT(fx.dispatcher.lastActivity("/qm/quad/" + kLeft + "/opacity"), 0.0);
    fx.osc("/qm/quad/left wall/opacity", {f(0.3f)});
    EXPECT_DOUBLE_EQ(fx.dispatcher.lastActivity("/qm/quad/" + kLeft + "/opacity"), 1.0);
}

TEST(ControlDispatchOscTest, DescribeFormatsArguments) {
    OscMessage m;
    m.address = "/a";
    m.args = {i(3), f(0.5f), s("x"), t(), fl()};
    EXPECT_EQ(ControlDispatcher::describe(m), "/a 3 0.5 \"x\" T F");
}

// ---- MIDI modes and mapping math -------------------------------------------------

TEST(ControlDispatchMidiTest, ValueModeMapsCcLinearlyIntoTheRange) {
    Fixture fx;
    fx.map(MidiControlType::CC, 7, "/qm/quad/" + kLeft + "/opacity", MidiMode::Value);
    fx.map(MidiControlType::CC, 8, "/qm/source/" + kSource + "/speed", MidiMode::Value);
    fx.midi(MidiType::ControlChange, 7, 0);
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 0.0f);
    fx.midi(MidiType::ControlChange, 7, 127);
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 1.0f);
    fx.midi(MidiType::ControlChange, 7, 64);
    EXPECT_NEAR(fx.opacity[kLeft], 64.0f / 127.0f, 1e-6f);
    fx.midi(MidiType::ControlChange, 8, 0);
    EXPECT_FLOAT_EQ(fx.opacity["speed:" + kSource], 0.1f);
    fx.midi(MidiType::ControlChange, 8, 127);
    EXPECT_FLOAT_EQ(fx.opacity["speed:" + kSource], 4.0f);
}

TEST(ControlDispatchMidiTest, TriggerModeFiresOnNoteOnAndCcCrossingUp) {
    Fixture fx;
    fx.map(MidiControlType::Note, 36, "/qm/source/" + kSource + "/play", MidiMode::Trigger);
    fx.map(MidiControlType::CC, 20, "/qm/source/" + kSource + "/play", MidiMode::Trigger);
    fx.midi(MidiType::NoteOn, 36, 100);
    fx.midi(MidiType::NoteOff, 36, 0);
    EXPECT_EQ(fx.plays.size(), 1u);
    fx.midi(MidiType::ControlChange, 20, 10);
    fx.midi(MidiType::ControlChange, 20, 63);
    EXPECT_EQ(fx.plays.size(), 1u);
    fx.midi(MidiType::ControlChange, 20, 64);   // crosses 64 upward
    fx.midi(MidiType::ControlChange, 20, 127);  // still high: no repeat
    EXPECT_EQ(fx.plays.size(), 2u);
    fx.midi(MidiType::ControlChange, 20, 0);
    fx.midi(MidiType::ControlChange, 20, 127);
    EXPECT_EQ(fx.plays.size(), 3u);
}

TEST(ControlDispatchMidiTest, TriggerModeOnBoolToggles) {
    Fixture fx;
    fx.map(MidiControlType::Note, 1, "/qm/blackout", MidiMode::Trigger);
    fx.midi(MidiType::NoteOn, 1, 90);
    EXPECT_EQ(fx.blackoutValue, 1);
    fx.midi(MidiType::NoteOff, 1, 0);
    EXPECT_EQ(fx.blackoutValue, 1);
    fx.midi(MidiType::NoteOn, 1, 90);
    EXPECT_EQ(fx.blackoutValue, 0);
}

TEST(ControlDispatchMidiTest, ToggleModeFlipsBoolAndFloat) {
    Fixture fx;
    fx.map(MidiControlType::Note, 2, "/qm/quad/" + kLeft + "/visible", MidiMode::Toggle);
    fx.map(MidiControlType::CC, 3, "/qm/quad/" + kLeft + "/opacity", MidiMode::Toggle);
    fx.visible[kLeft] = true;
    fx.midi(MidiType::NoteOn, 2, 1);
    EXPECT_FALSE(fx.visible[kLeft]);
    fx.midi(MidiType::NoteOn, 2, 1);
    EXPECT_TRUE(fx.visible[kLeft]);

    fx.opacity[kLeft] = 0.2f;
    fx.midi(MidiType::ControlChange, 3, 127);
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 1.0f);
    fx.midi(MidiType::ControlChange, 3, 0);   // release: no change
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 1.0f);
    fx.midi(MidiType::ControlChange, 3, 127);
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 0.0f);
}

TEST(ControlDispatchMidiTest, MomentaryModeFollowsTheButton) {
    Fixture fx;
    fx.map(MidiControlType::Note, 4, "/qm/blackout", MidiMode::Momentary);
    fx.midi(MidiType::NoteOn, 4, 127);
    EXPECT_EQ(fx.blackoutValue, 1);
    fx.midi(MidiType::NoteOff, 4, 0);
    EXPECT_EQ(fx.blackoutValue, 0);
    fx.map(MidiControlType::CC, 5, "/qm/quad/" + kLeft + "/opacity", MidiMode::Momentary);
    fx.opacity[kLeft] = 0.5f;
    fx.midi(MidiType::ControlChange, 5, 100);
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 1.0f);
    fx.midi(MidiType::ControlChange, 5, 3);
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 0.0f);
}

TEST(ControlDispatchMidiTest, ValueModeOnBoolAndNotes) {
    Fixture fx;
    fx.map(MidiControlType::CC, 9, "/qm/blackout", MidiMode::Value);
    fx.midi(MidiType::ControlChange, 9, 100);
    EXPECT_EQ(fx.blackoutValue, 1);
    fx.midi(MidiType::ControlChange, 9, 20);
    EXPECT_EQ(fx.blackoutValue, 0);
    fx.map(MidiControlType::Note, 10, "/qm/quad/" + kLeft + "/opacity", MidiMode::Value);
    fx.midi(MidiType::NoteOn, 10, 127);
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 1.0f);
    fx.midi(MidiType::NoteOff, 10, 64);
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 0.0f) << "note-off is value 0";
}

TEST(ControlDispatchMidiTest, ProgramChangeMappings) {
    Fixture fx;
    fx.map(MidiControlType::Program, 5, "/qm/source/" + kSource + "/play", MidiMode::Trigger);
    fx.midi(MidiType::ProgramChange, 4, 0);
    fx.midi(MidiType::ProgramChange, 5, 0);
    fx.midi(MidiType::ProgramChange, 5, 0);
    EXPECT_EQ(fx.plays.size(), 2u);
}

TEST(ControlDispatchMidiTest, DeviceChannelAndTypeFilters) {
    Fixture fx;
    MidiMapping& m = fx.map(MidiControlType::Note, 40, "/qm/source/" + kSource + "/play",
                            MidiMode::Trigger);
    m.device = "Pad";
    m.channel = 2;
    fx.midi(MidiType::NoteOn, 40, 100, 1, "Pad");         // wrong channel
    fx.midi(MidiType::NoteOn, 40, 100, 2, "Keys");        // wrong device
    fx.midi(MidiType::ControlChange, 40, 100, 2, "Pad");  // wrong type
    fx.midi(MidiType::NoteOn, 41, 100, 2, "Pad");         // wrong number
    EXPECT_TRUE(fx.plays.empty());
    fx.midi(MidiType::NoteOn, 40, 100, 2, "Pad");
    EXPECT_EQ(fx.plays.size(), 1u);
    fx.mappings[0].device = "ANY";
    fx.mappings[0].channel = 0;
    fx.midi(MidiType::NoteOn, 40, 100, 9, "Keys");
    EXPECT_EQ(fx.plays.size(), 2u);
}

TEST(ControlDispatchMidiTest, UnresolvedMappingsAreKeptAndDoNothing) {
    Fixture fx;
    fx.map(MidiControlType::Note, 1, "/qm/quad/deleted-quad/visible", MidiMode::Toggle);
    fx.midi(MidiType::NoteOn, 1, 100);
    ASSERT_EQ(fx.mappings.size(), 1u);
    EXPECT_TRUE(fx.visible.empty());
    EXPECT_DOUBLE_EQ(fx.mappings[0].lastActivity, 2.0) << "the input is still shown as active";
}

TEST(ControlDispatchMidiTest, ActivityAndLastValueAreTracked) {
    Fixture fx;
    fx.map(MidiControlType::CC, 7, "/qm/quad/" + kLeft + "/opacity", MidiMode::Value);
    fx.midi(MidiType::ControlChange, 7, 33);
    EXPECT_DOUBLE_EQ(fx.mappings[0].lastActivity, 2.0);
    EXPECT_EQ(fx.mappings[0].lastValue, 33);
    EXPECT_EQ(fx.dispatcher.midiMessages(), 1u);
    EXPECT_EQ(fx.dispatcher.lastMidi(), "CC ch1 7 = 33 (Pad)");
}

// ---- MIDI learn --------------------------------------------------------------------

TEST(ControlLearnTest, NextNoteBindsAndDoesNotFire) {
    Fixture fx;
    fx.dispatcher.beginLearn("/qm/blackout");
    ASSERT_TRUE(fx.dispatcher.isLearning());
    fx.midi(MidiType::NoteOff, 50, 0);  // a stray release does not bind
    EXPECT_TRUE(fx.dispatcher.isLearning());
    fx.midi(MidiType::NoteOn, 50, 90, 3, "Pad");
    EXPECT_FALSE(fx.dispatcher.isLearning());
    ASSERT_EQ(fx.mappings.size(), 1u);
    const MidiMapping& m = fx.mappings[0];
    EXPECT_EQ(m.device, "Pad");
    EXPECT_EQ(m.type, MidiControlType::Note);
    EXPECT_EQ(m.channel, 3);
    EXPECT_EQ(m.number, 50);
    EXPECT_EQ(m.target, "/qm/blackout");
    EXPECT_EQ(m.mode, MidiMode::Toggle);
    EXPECT_EQ(fx.blackoutApplies, 0) << "binding press is not applied";
    EXPECT_EQ(fx.dispatcher.lastLearnedIndex(), 0);

    fx.midi(MidiType::NoteOn, 50, 90, 3, "Pad");
    EXPECT_EQ(fx.blackoutValue, 1);
}

TEST(ControlLearnTest, CcBindsToCanonicalPathWithValueMode) {
    Fixture fx;
    fx.dispatcher.beginLearn("/qm/quad/Left Wall/opacity");  // by name: stored by UUID
    fx.midi(MidiType::ControlChange, 21, 100);
    ASSERT_EQ(fx.mappings.size(), 1u);
    EXPECT_EQ(fx.mappings[0].target, "/qm/quad/" + kLeft + "/opacity");
    EXPECT_EQ(fx.mappings[0].mode, MidiMode::Value);
    EXPECT_TRUE(fx.mappings[0].ccHigh) << "edge state primed from the binding value";
    fx.midi(MidiType::ControlChange, 21, 127);
    EXPECT_FLOAT_EQ(fx.opacity[kLeft], 1.0f);
}

TEST(ControlLearnTest, DefaultModesByTargetKind) {
    EXPECT_EQ(ControlDispatcher::defaultMode(MidiControlType::Note, TargetKind::Trigger), MidiMode::Trigger);
    EXPECT_EQ(ControlDispatcher::defaultMode(MidiControlType::CC, TargetKind::Trigger), MidiMode::Trigger);
    EXPECT_EQ(ControlDispatcher::defaultMode(MidiControlType::Note, TargetKind::Bool), MidiMode::Toggle);
    EXPECT_EQ(ControlDispatcher::defaultMode(MidiControlType::CC, TargetKind::Bool), MidiMode::Toggle);
    EXPECT_EQ(ControlDispatcher::defaultMode(MidiControlType::CC, TargetKind::Float), MidiMode::Value);
    EXPECT_EQ(ControlDispatcher::defaultMode(MidiControlType::Note, TargetKind::Float), MidiMode::Momentary);
    EXPECT_EQ(ControlDispatcher::defaultMode(MidiControlType::Program, TargetKind::Float), MidiMode::Trigger);
}

TEST(ControlLearnTest, RelearningTheSameControlDoesNotDuplicate) {
    Fixture fx;
    for (int n = 0; n < 3; ++n) {
        fx.dispatcher.beginLearn("/qm/blackout");
        fx.midi(MidiType::NoteOn, 60, 100);
    }
    EXPECT_EQ(fx.mappings.size(), 1u);
    fx.dispatcher.beginLearn("/qm/quad/" + kLeft + "/visible");
    fx.midi(MidiType::NoteOn, 60, 100);  // one control may drive several targets
    EXPECT_EQ(fx.mappings.size(), 2u);
    EXPECT_EQ(fx.dispatcher.lastLearnedIndex(), 1);
}

TEST(ControlLearnTest, CancelAndVanishedTargets) {
    Fixture fx;
    fx.dispatcher.beginLearn("/qm/blackout");
    fx.dispatcher.cancelLearn();
    fx.midi(MidiType::NoteOn, 60, 100);
    EXPECT_TRUE(fx.mappings.empty());

    fx.dispatcher.beginLearn("/qm/quad/" + kLeft + "/visible");
    fx.registry.setEntities("quad", {});  // the quad was deleted mid-learn
    fx.midi(MidiType::NoteOn, 60, 100);
    EXPECT_TRUE(fx.mappings.empty());
    EXPECT_FALSE(fx.dispatcher.isLearning());
}

TEST(ControlLearnTest, OscDoesNotEndLearn) {
    Fixture fx;
    fx.dispatcher.beginLearn("/qm/blackout");
    fx.osc("/qm/blackout", {i(1)});
    EXPECT_TRUE(fx.dispatcher.isLearning());
    EXPECT_EQ(fx.dispatcher.learnTarget(), "/qm/blackout");
}
