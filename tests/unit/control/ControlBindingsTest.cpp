// The application's control targets (editor/ControlBindings.hpp) against real Scenes,
// Quads and a SourceManager, driven through the dispatcher as OSC and MIDI would.
// No GL: the stand-in playable source has zero size, so no texture is created.

#include "editor/ControlBindings.hpp"
#include "FakePlayable.hpp"
#include "control/ControlDispatcher.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace quadmapper;
using namespace quadmapper::control;

namespace {

struct AppFixture {
    std::vector<std::unique_ptr<Scene>> scenes;
    SourceManager sources;
    ControlRegistry registry;
    ControlDispatcher dispatcher{registry};
    std::vector<MidiMapping> mappings;
    std::vector<int> takes;
    std::vector<std::optional<float>> takeFades;  // per take; nullopt = the Preferences default
    bool blackout = false;
    float masterVolume = 1.0f;
    bool masterMuted = false;
    FakePlayable* clip = nullptr;
    UUID clipId;
    Quad* left = nullptr;
    Quad* right = nullptr;

    AppFixture() {
        auto a = std::make_unique<Scene>();
        a->setName("Opening");
        left = a->getQuadManager().createQuad("Left");
        right = a->getQuadManager().createQuad("Right");
        auto b = std::make_unique<Scene>();
        b->setName("Finale");
        b->getQuadManager().createQuad("Back");
        scenes.push_back(std::move(a));
        scenes.push_back(std::move(b));

        auto fake = std::make_unique<FakePlayable>("clip.mp4");
        clip = fake.get();
        clipId = sources.registerSource(std::move(fake));
        left->getTexture().mediaId = clipId;

        ControlBindingHost host;
        host.scenes = &scenes;
        host.sources = &sources;
        host.takeScene = [this](int n, std::optional<float> fade) {
            takes.push_back(n);
            takeFades.push_back(fade);
        };
        host.activeSceneNumber = [] { return 1; };
        host.isBlackout = [this] { return blackout; };
        host.setBlackout = [this](bool on) { blackout = on; };
        host.refuse = [this](const std::string& address, const std::string& reason) {
            dispatcher.noteRefused(address, reason);
        };
        host.getMasterVolume = [this] { return masterVolume; };
        host.setMasterVolume = [this](float v) { masterVolume = v; };
        host.isMasterMuted = [this] { return masterMuted; };
        host.setMasterMuted = [this](bool on) { masterMuted = on; };
        registerControlTargets(registry, host);
        refresh();
    }

    void refresh() { refreshControlEntities(registry, scenes, &sources); }

    void oscArgs(const std::string& address, std::vector<OscArg> args) {
        ControlEvent e;
        e.source = ControlEvent::Source::Osc;
        e.osc.address = address;
        e.osc.args = std::move(args);
        dispatcher.dispatch(e, mappings, 0.0);
    }

    void osc(const std::string& address, std::optional<float> value = std::nullopt) {
        ControlEvent e;
        e.source = ControlEvent::Source::Osc;
        e.osc.address = address;
        if (value) {
            OscArg arg;
            arg.type = OscArg::Type::Float;
            arg.f = *value;
            e.osc.args.push_back(arg);
        }
        dispatcher.dispatch(e, mappings, 0.0);
    }
};

OscArg intArg(int32_t v) {
    OscArg a;
    a.type = OscArg::Type::Int;
    a.i = v;
    return a;
}

OscArg floatArg(float v) {
    OscArg a;
    a.type = OscArg::Type::Float;
    a.f = v;
    return a;
}

OscArg stringArg(const char* v) {
    OscArg a;
    a.type = OscArg::Type::String;
    a.s = v;
    return a;
}

std::string path(const char* collection, const UUID& id, const char* action) {
    return std::string("/qm/") + collection + "/" + id.toString() + "/" + action;
}

}  // namespace

TEST(ControlBindingsTest, QuadOpacityAndVisibilityByNameAndId) {
    AppFixture fx;
    fx.osc("/qm/quad/left/opacity", 0.25f);
    EXPECT_FLOAT_EQ(fx.left->getOpacity(), 0.25f);
    fx.osc(path("quad", fx.right->getId(), "visible"), 0.0f);
    EXPECT_FALSE(fx.right->isVisible());
    fx.osc("/qm/quad/Right/visible");  // no argument toggles
    EXPECT_TRUE(fx.right->isVisible());
    fx.osc("/qm/quad/Back/opacity", 2.0f);  // quads of every scene, not just the active one
    EXPECT_FLOAT_EQ(fx.scenes[1]->getQuadManager().getQuads()[0]->getOpacity(), 1.0f);
}

TEST(ControlBindingsTest, SceneTakesQueueTheSceneNumber) {
    AppFixture fx;
    fx.osc("/qm/scene/take", 2.0f);
    fx.osc("/qm/scene/finale/take");
    fx.osc(path("scene", fx.scenes[0]->getId(), "take"), 1.0f);
    fx.osc(path("scene", fx.scenes[0]->getId(), "take"), 0.0f);  // button release: nothing
    EXPECT_EQ(fx.takes, (std::vector<int>{2, 2, 1}));
    // Without a fade time every take uses the Preferences default.
    EXPECT_EQ(fx.takeFades, (std::vector<std::optional<float>>(3, std::nullopt)));
}

// ADR-047 §5: /qm/scene/take i n f seconds carries its own fade; the one-argument form
// still works and uses the default.
TEST(ControlBindingsTest, SceneTakeCarriesAnOptionalFadeTime) {
    AppFixture fx;
    fx.oscArgs("/qm/scene/take", {intArg(2), floatArg(1.5f)});
    fx.oscArgs("/qm/scene/take", {floatArg(1.0f), intArg(3)});         // either numeric type
    fx.oscArgs("/qm/scene/take", {intArg(2), floatArg(0.0f)});         // 0 = an explicit cut
    fx.oscArgs("/qm/scene/take", {intArg(1)});                         // the 1-argument form
    fx.oscArgs("/qm/scene/take", {floatArg(2.4f), floatArg(600.0f)});  // rounded; clamped
    EXPECT_EQ(fx.takes, (std::vector<int>{2, 1, 2, 1, 2}));
    EXPECT_EQ(fx.takeFades, (std::vector<std::optional<float>>{1.5f, 3.0f, 0.0f, std::nullopt,
                                                                MAX_SCENE_FADE_SECONDS}));
    EXPECT_EQ(fx.dispatcher.badArguments(), 0u);
}

TEST(ControlBindingsTest, BadSceneTakeArgumentsTakeNothing) {
    AppFixture fx;
    const std::vector<std::vector<OscArg>> bad = {
        {intArg(2), floatArg(-1.0f)},                                   // negative time
        {intArg(2), floatArg(std::nanf(""))},                           // not a number
        {intArg(2), floatArg(std::numeric_limits<float>::infinity())},
        {intArg(2), stringArg("1.0")},                                  // a string time
        {intArg(2), floatArg(1.0f), floatArg(1.0f)},                    // three arguments
        {intArg(0), floatArg(1.0f)},                                    // no scene 0
        {floatArg(0.2f)},                                               // rounds below 1
        {stringArg("2"), floatArg(1.0f)},                               // a string scene
        {floatArg(2.0e6f)},                                             // beyond the index range
    };
    for (const auto& args : bad) fx.oscArgs("/qm/scene/take", args);
    fx.osc("/qm/scene/take");  // no argument: a value target needs its value
    EXPECT_TRUE(fx.takes.empty());
    EXPECT_EQ(fx.dispatcher.badArguments(), bad.size() + 1);
}

TEST(ControlBindingsTest, ParseSceneTakeArgs) {
    using control_bindings::parseSceneTakeArgs;
    const auto one = parseSceneTakeArgs({intArg(4)});
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->number, 4);
    EXPECT_FALSE(one->fadeSeconds.has_value());
    const auto two = parseSceneTakeArgs({intArg(4), intArg(2)});
    ASSERT_TRUE(two.has_value());
    EXPECT_EQ(two->fadeSeconds, std::optional<float>(2.0f));
    OscArg yes;
    yes.type = OscArg::Type::True;
    EXPECT_EQ(parseSceneTakeArgs({yes})->number, 1);                 // as the Index target did
    EXPECT_FALSE(parseSceneTakeArgs({intArg(1), yes}).has_value());  // a time is a number
    EXPECT_FALSE(parseSceneTakeArgs({}).has_value());
}

TEST(ControlBindingsTest, BlackoutSetsAndToggles) {
    AppFixture fx;
    fx.osc("/qm/blackout", 1.0f);
    EXPECT_TRUE(fx.blackout);
    fx.osc("/qm/blackout");
    EXPECT_FALSE(fx.blackout);
    fx.osc("/qm/blackout");
    EXPECT_TRUE(fx.blackout);
}

TEST(ControlBindingsTest, SourceTransportThroughPlayableSource) {
    AppFixture fx;
    fx.osc("/qm/source/clip.mp4/play");
    EXPECT_EQ(fx.clip->plays, 1);
    fx.osc("/qm/source/CLIP.MP4/pause", 1.0f);
    EXPECT_EQ(fx.clip->pauses, 1);
    fx.osc(path("source", fx.clipId, "restart"));
    EXPECT_EQ(fx.clip->seeks, 1);
    EXPECT_DOUBLE_EQ(fx.clip->time, 0.0);
    EXPECT_EQ(fx.clip->state, PlaybackState::PLAYING);
    fx.osc("/qm/source/clip.mp4/speed", 9.0f);
    EXPECT_FLOAT_EQ(fx.clip->speed, 4.0f);
    fx.osc("/qm/source/clip.mp4/speed", 0.5f);
    EXPECT_FLOAT_EQ(fx.clip->speed, 0.5f);
}

// ADR-050 §2: a source's sound (the Video Player's volume and mute) and the master.
TEST(ControlBindingsTest, SourceVolumeAndMuteThroughPlayableSource) {
    AppFixture fx;
    fx.osc("/qm/source/clip.mp4/volume", 0.4f);
    EXPECT_FLOAT_EQ(fx.clip->getAudioVolume(), 0.4f);
    fx.osc(path("source", fx.clipId, "volume"), 3.0f);  // clamped to the target's range
    EXPECT_FLOAT_EQ(fx.clip->getAudioVolume(), 1.0f);
    fx.osc("/qm/source/clip.mp4/volume", -1.0f);
    EXPECT_FLOAT_EQ(fx.clip->getAudioVolume(), 0.0f);

    fx.osc("/qm/source/clip.mp4/mute", 1.0f);
    EXPECT_TRUE(fx.clip->isAudioMuted());
    fx.osc("/qm/source/clip.mp4/mute");  // no argument toggles
    EXPECT_FALSE(fx.clip->isAudioMuted());
    fx.osc("/qm/source/clip.mp4/mute", 0.0f);
    EXPECT_FALSE(fx.clip->isAudioMuted());

    // MIDI CC 0-127 sweeps the volume.
    MidiMapping m;
    m.type = MidiControlType::CC;
    m.number = 7;
    m.target = path("source", fx.clipId, "volume");
    m.mode = MidiMode::Value;
    fx.mappings = {m};
    ControlEvent e;
    e.source = ControlEvent::Source::Midi;
    e.midi.type = MidiType::ControlChange;
    e.midi.number = 7;
    e.midi.value = 127;
    fx.dispatcher.dispatch(e, fx.mappings, 0.0);
    EXPECT_FLOAT_EQ(fx.clip->getAudioVolume(), 1.0f);
    e.midi.value = 0;
    fx.dispatcher.dispatch(e, fx.mappings, 0.0);
    EXPECT_FLOAT_EQ(fx.clip->getAudioVolume(), 0.0f);
}

TEST(ControlBindingsTest, MasterVolumeAndMute) {
    AppFixture fx;
    fx.osc("/qm/master/volume", 0.25f);
    EXPECT_FLOAT_EQ(fx.masterVolume, 0.25f);
    fx.osc("/qm/master/volume", 5.0f);
    EXPECT_FLOAT_EQ(fx.masterVolume, 1.0f);
    fx.osc("/qm/master/mute", 1.0f);
    EXPECT_TRUE(fx.masterMuted);
    fx.osc("/qm/master/mute");
    EXPECT_FALSE(fx.masterMuted);
}

TEST(ControlBindingsTest, ExternalControlNeverTouchesGeometry) {
    AppFixture fx;
    fx.left->getGeometry().setCorner(2, Vec2(0.9f, 0.8f));
    const auto leftCorners = fx.left->getGeometry().getCorners();
    const auto rightCorners = fx.right->getGeometry().getCorners();
    const int leftZ = fx.left->getZOrder();
    const size_t quadCount = fx.scenes[0]->getQuadManager().getQuadCount();

    // Drive every target there is, with a spread of values, as OSC and as MIDI.
    const auto targets = fx.registry.listTargets();
    ASSERT_FALSE(targets.empty());
    for (const auto& target : targets) {
        for (float v : {0.0f, 0.3f, 1.0f, 7.0f, -2.0f}) fx.osc(target.path, v);
        fx.osc(target.path);
        MidiMapping m;
        m.type = MidiControlType::CC;
        m.number = 1;
        m.target = target.path;
        m.mode = MidiMode::Value;
        fx.mappings = {m};
        for (int cc : {0, 64, 127}) {
            ControlEvent e;
            e.source = ControlEvent::Source::Midi;
            e.midi.type = MidiType::ControlChange;
            e.midi.number = 1;
            e.midi.value = static_cast<uint8_t>(cc);
            fx.dispatcher.dispatch(e, fx.mappings, 0.0);
        }
    }
    fx.osc("/qm/quad/left/corner", 0.5f);  // no such target
    fx.osc("/qm/quad/left/position", 0.5f);

    EXPECT_EQ(fx.left->getGeometry().getCorners(), leftCorners);
    EXPECT_EQ(fx.right->getGeometry().getCorners(), rightCorners);
    EXPECT_EQ(fx.left->getZOrder(), leftZ);
    EXPECT_EQ(fx.scenes[0]->getQuadManager().getQuadCount(), quadCount);
    EXPECT_EQ(fx.scenes.size(), 2u);
}

TEST(ControlBindingsTest, OnlyTheExpectedTargetsExist) {
    AppFixture fx;
    std::vector<std::string> actions;
    for (const auto& target : fx.registry.listTargets()) {
        actions.push_back(target.path.substr(target.path.find_last_of('/') + 1));
    }
    for (const auto& action : actions) {
        EXPECT_TRUE(action == "blackout" || action == "take" || action == "opacity" ||
                    action == "visible" || action == "play" || action == "pause" ||
                    action == "restart" || action == "speed" || action == "volume" ||
                    action == "mute" || action == "running")
            << action;
    }
    // blackout + master timeline x 4 + master volume/mute + 2 scene takes + 3 quads x 2
    // + 1 source x 6 (/qm/scene/take and /qm/scene/active are OSC only)
    EXPECT_EQ(actions.size(), 1u + 4u + 2u + 2u + 6u + 6u);
}

// What OSC feedback reports: every value target with a reader, on the addresses a
// controller would use.
TEST(ControlBindingsTest, FeedbackCoversEveryValueTarget) {
    AppFixture fx;
    std::vector<std::string> addresses;
    for (const auto& t : fx.registry.feedbackTargets(true)) addresses.push_back(t.address);
    const std::vector<std::string> expected = {
        "/qm/blackout", "/qm/master/volume", "/qm/master/mute", "/qm/scene/active", "/qm/timeline/running",
        "/qm/quad/Left/opacity", "/qm/quad/Left/visible", "/qm/quad/Right/opacity", "/qm/quad/Right/visible",
        "/qm/quad/Back/opacity", "/qm/quad/Back/visible", "/qm/source/clip.mp4/speed",
        "/qm/source/clip.mp4/volume", "/qm/source/clip.mp4/mute"};
    EXPECT_EQ(addresses, expected);
    for (const auto& t : fx.registry.feedbackTargets(false)) {
        if (t.entityId.empty()) continue;
        EXPECT_NE(t.address.find(t.entityId), std::string::npos) << "by id: " << t.address;
        EXPECT_EQ(t.address, t.canonicalPath);
    }
    // Every reader answers on the live show.
    for (const auto& t : fx.registry.feedbackTargets(true)) {
        EXPECT_TRUE(t.target->read(t.entityId).has_value()) << t.address;
    }
}

TEST(ControlBindingsTest, ActiveSceneReportsAndTakes) {
    std::vector<std::unique_ptr<Scene>> scenes;
    ControlRegistry registry;
    int active = 2;
    std::vector<int> takes;
    ControlBindingHost host;
    host.scenes = &scenes;
    host.takeScene = [&](int n, std::optional<float>) { takes.push_back(n); };
    host.activeSceneNumber = [&] { return active; };
    registerControlTargets(registry, host);
    const auto scene = registry.resolve("/qm/scene/active");
    ASSERT_TRUE(scene.has_value());
    EXPECT_EQ(scene->target->kind, TargetKind::Index);
    EXPECT_EQ(scene->target->read(""), std::optional<float>(2.0f));
    active = 0;
    EXPECT_FALSE(scene->target->read("").has_value());
    host.activeSceneNumber = nullptr;  // the registered copy keeps its reader
    EXPECT_FALSE(scene->target->read("").has_value());
    ControlRegistry bare;
    registerControlTargets(bare, ControlBindingHost{});
    EXPECT_FALSE(bare.resolve("/qm/scene/active")->target->read("").has_value()) << "no host reader";
    ControlDispatcher dispatcher(registry);
    std::vector<MidiMapping> none;
    ControlEvent e;
    e.source = ControlEvent::Source::Osc;
    e.osc.address = "/qm/scene/active";
    OscArg n;
    n.type = OscArg::Type::Int;
    n.i = 3;
    e.osc.args = {n};
    dispatcher.dispatch(e, none, 0.0);
    EXPECT_EQ(takes, (std::vector<int>{3}));
}

TEST(ControlBindingsTest, APatternMatchingSeveralSceneTakesIsRefused) {
    AppFixture fx;
    fx.osc("/qm/scene/*/take");
    EXPECT_TRUE(fx.takes.empty()) << "taking two scenes at once is meaningless";
    EXPECT_EQ(fx.dispatcher.refusals(), 1u);
    EXPECT_NE(fx.dispatcher.lastRefused().find("/qm/scene/*/take"), std::string::npos);
    EXPECT_NE(fx.dispatcher.lastRefused().find("2 scene takes"), std::string::npos);
    EXPECT_EQ(fx.dispatcher.unknownAddresses(), 0u);

    // /qm/scene/take and /qm/scene/active are both takes too.
    fx.osc("/qm/scene/*", 2.0f);
    EXPECT_TRUE(fx.takes.empty());
    EXPECT_EQ(fx.dispatcher.refusals(), 2u);

    // The other targets a pattern matched still act.
    fx.osc("/qm/*/*/{take,opacity}", 0.75f);
    EXPECT_TRUE(fx.takes.empty());
    EXPECT_FLOAT_EQ(fx.left->getOpacity(), 0.75f);
    EXPECT_FLOAT_EQ(fx.right->getOpacity(), 0.75f);
    EXPECT_EQ(fx.dispatcher.refusals(), 3u);

    // A pattern that names one scene takes it.
    fx.osc("/qm/scene/F*/take");
    fx.osc("/qm/scene/{opening,nothing}/take");
    EXPECT_EQ(fx.takes, (std::vector<int>{2, 1}));
    EXPECT_EQ(fx.dispatcher.refusals(), 3u);

    // Wildcards may fire a trigger on several sources.
    auto second = std::make_unique<FakePlayable>("clip2.mp4");
    FakePlayable* clip2 = second.get();
    fx.sources.registerSource(std::move(second));
    fx.refresh();
    const int plays1 = fx.clip->plays;
    const int plays2 = clip2->plays;
    fx.osc("/qm/source/*/play");
    EXPECT_EQ(fx.clip->plays, plays1 + 1);
    EXPECT_EQ(clip2->plays, plays2 + 1);
    EXPECT_EQ(fx.dispatcher.refusals(), 3u);
}

TEST(ControlBindingsTest, TimelineRunningFollowsAndDrivesTheMasterClock) {
    AppFixture fx;
    const PlaybackClock& master = fx.sources.getMasterTimeline();
    const auto running = fx.registry.resolve("/qm/timeline/running");
    ASSERT_TRUE(running.has_value());
    EXPECT_EQ(running->target->read(""), std::optional<float>(1.0f));
    fx.osc("/qm/timeline/running", 0.0f);
    EXPECT_FALSE(master.running());
    EXPECT_EQ(running->target->read(""), std::optional<float>(0.0f));
    fx.osc("/qm/timeline/running");  // no argument toggles
    EXPECT_TRUE(master.running());
    fx.osc("/qm/timeline/pause");
    EXPECT_EQ(running->target->read(""), std::optional<float>(0.0f)) << "follows the other controls";
}

TEST(ControlBindingsTest, LockedSourceRefusesItsOwnTransport) {
    AppFixture fx;
    fx.clip->play();
    fx.sources.setTimelineLocked(fx.clipId, true);
    ASSERT_TRUE(fx.clip->isTimelineLocked());
    const int plays = fx.clip->plays;

    fx.osc("/qm/source/clip.mp4/pause");
    fx.osc(path("source", fx.clipId, "restart"));
    fx.osc("/qm/source/clip.mp4/speed", 2.0f);
    fx.osc("/qm/source/clip.mp4/play");
    EXPECT_EQ(fx.clip->state, PlaybackState::PLAYING) << "a refused pause must not freeze it";
    EXPECT_EQ(fx.clip->pauses, 0);
    EXPECT_EQ(fx.clip->seeks, 0);
    EXPECT_EQ(fx.clip->plays, plays);
    EXPECT_FLOAT_EQ(fx.clip->speed, 1.0f);
    EXPECT_NE(fx.dispatcher.lastRefused().find(path("source", fx.clipId, "play")), std::string::npos);
    EXPECT_NE(fx.dispatcher.lastRefused().find("locked to master"), std::string::npos);

    // The same refusal through a MIDI mapping.
    MidiMapping m;
    m.type = MidiControlType::Note;
    m.number = 9;
    m.target = path("source", fx.clipId, "pause");
    m.mode = MidiMode::Trigger;
    fx.mappings.push_back(m);
    ControlEvent e;
    e.source = ControlEvent::Source::Midi;
    e.midi.type = MidiType::NoteOn;
    e.midi.number = 9;
    e.midi.value = 100;
    fx.dispatcher.dispatch(e, fx.mappings, 0.0);
    EXPECT_EQ(fx.clip->pauses, 0);
    EXPECT_NE(fx.dispatcher.lastRefused().find(path("source", fx.clipId, "pause")), std::string::npos);

    // Unlocked again, the source's own transport works.
    fx.sources.setTimelineLocked(fx.clipId, false);
    fx.osc("/qm/source/clip.mp4/pause");
    EXPECT_EQ(fx.clip->state, PlaybackState::PAUSED);
}

TEST(ControlBindingsTest, MasterTimelineTargetsDriveTheMasterClock) {
    AppFixture fx;
    const PlaybackClock& master = fx.sources.getMasterTimeline();
    ASSERT_TRUE(master.running()) << "the master runs from construction";
    fx.osc("/qm/timeline/pause");
    EXPECT_FALSE(master.running());
    fx.osc("/qm/timeline/play");
    EXPECT_TRUE(master.running());
    fx.osc("/qm/timeline/pause");
    fx.osc("/qm/timeline/restart");
    EXPECT_DOUBLE_EQ(master.position(std::chrono::steady_clock::now()), 0.0);
    EXPECT_FALSE(master.running()) << "restart keeps play/pause";

    // Offered for MIDI learn, and a learned control drives it.
    const auto targets = fx.registry.listTargets();
    for (const char* p : {"/qm/timeline/play", "/qm/timeline/pause", "/qm/timeline/restart"}) {
        EXPECT_TRUE(std::any_of(targets.begin(), targets.end(),
                                [p](const ControlRegistry::ListedTarget& t) { return t.path == p; }))
            << p;
    }
    fx.dispatcher.beginLearn("/qm/timeline/play");
    ControlEvent e;
    e.source = ControlEvent::Source::Midi;
    e.midi.type = MidiType::NoteOn;
    e.midi.number = 12;
    e.midi.value = 100;
    fx.dispatcher.dispatch(e, fx.mappings, 0.0);  // learns, does not act
    ASSERT_EQ(fx.mappings.size(), 1u);
    EXPECT_FALSE(master.running());
    fx.dispatcher.dispatch(e, fx.mappings, 1.0);
    EXPECT_TRUE(master.running());
}

TEST(ControlBindingsTest, DeletedQuadMakesItsTargetsUnresolvedAndInert) {
    AppFixture fx;
    const std::string rightVisible = path("quad", fx.right->getId(), "visible");
    MidiMapping m;
    m.type = MidiControlType::Note;
    m.number = 5;
    m.target = rightVisible;
    m.mode = MidiMode::Toggle;
    fx.mappings.push_back(m);

    fx.scenes[0]->deleteQuad(fx.right->getId());
    fx.right = nullptr;
    fx.refresh();
    EXPECT_FALSE(fx.registry.describe(rightVisible));

    ControlEvent e;
    e.source = ControlEvent::Source::Midi;
    e.midi.type = MidiType::NoteOn;
    e.midi.number = 5;
    e.midi.value = 100;
    fx.dispatcher.dispatch(e, fx.mappings, 0.0);  // must not crash or act
    fx.osc(rightVisible, 1.0f);
    EXPECT_EQ(fx.mappings.size(), 1u) << "the mapping is kept";
    EXPECT_GE(fx.dispatcher.unknownAddresses(), 1u);
}

TEST(ControlBindingsTest, TargetsReResolveWhenTheQuadWasDeletedBeforeRefresh) {
    AppFixture fx;
    const std::string rightOpacity = path("quad", fx.right->getId(), "opacity");
    auto resolved = fx.registry.resolve(rightOpacity);
    ASSERT_TRUE(resolved);
    fx.scenes[0]->deleteQuad(fx.right->getId());  // entity list is now stale
    resolved->target->apply(resolved->entityId, 0.5f);  // finds no quad: does nothing
    SUCCEED();
}

TEST(ControlBindingsTest, SourcesAreOrderedByFirstUseThenId) {
    AppFixture fx;
    auto second = std::make_unique<FakePlayable>("clip.mp4");  // same name, unused
    const UUID secondId = fx.sources.registerSource(std::move(second));
    fx.refresh();
    const auto& entities = fx.registry.entities("source");
    ASSERT_EQ(entities.size(), 2u);
    EXPECT_EQ(entities[0].id, fx.clipId.toString()) << "the source a quad shows comes first";
    EXPECT_EQ(entities[1].id, secondId.toString());
    auto r = fx.registry.resolve("/qm/source/clip.mp4/play");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->entityId, fx.clipId.toString());
}

TEST(ControlBindingsTest, RemapTargetPathRewritesSavedIds) {
    const UUID saved = UUID::generate();
    const UUID live = UUID::generate();
    const std::unordered_map<UUID, UUID> map{{saved, live}};
    EXPECT_EQ(remapTargetPath(path("quad", saved, "opacity"), "quad", map), path("quad", live, "opacity"));
    EXPECT_EQ(remapTargetPath(path("source", saved, "play"), "quad", map), path("source", saved, "play"));
    const UUID gone = UUID::generate();
    EXPECT_EQ(remapTargetPath(path("quad", gone, "visible"), "quad", map), path("quad", gone, "visible"));
    EXPECT_EQ(remapTargetPath("/qm/blackout", "quad", map), "/qm/blackout");
    EXPECT_EQ(remapTargetPath("/qm/quad/Left Wall/opacity", "quad", map), "/qm/quad/Left Wall/opacity");
    EXPECT_EQ(remapTargetPath("/qm/quad/zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz/opacity", "quad", map),
              "/qm/quad/zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz/opacity");
    EXPECT_EQ(remapTargetPath("/qm/quad/", "quad", map), "/qm/quad/");
}

TEST(ControlBindingsTest, MappingConvertsToAndFromTheProjectDto) {
    MidiMapping m;
    m.device = "APC mini";
    m.type = MidiControlType::CC;
    m.channel = 3;
    m.number = 48;
    m.target = "/qm/blackout";
    m.mode = MidiMode::Momentary;
    const Project::MidiMappingData d = toProjectMapping(m);
    EXPECT_EQ(d.type, "cc");
    EXPECT_EQ(d.mode, "momentary");
    EXPECT_EQ(d.bits, 7);
    EXPECT_TRUE(sameBinding(fromProjectMapping(d), m));

    m.number = 7;
    m.bits = 14;
    const Project::MidiMappingData d14 = toProjectMapping(m);
    EXPECT_EQ(d14.bits, 14);
    const MidiMapping back = fromProjectMapping(d14);
    EXPECT_TRUE(back.is14Bit());
    EXPECT_TRUE(sameBinding(back, m));
}

// ---- Preferences (AppPreferences "control") ------------------------------------

TEST(ControlPreferencesTest, DefaultsAreOffPort8000AllInterfaces) {
    const ControlPreferences prefs;
    EXPECT_FALSE(prefs.oscEnabled);
    EXPECT_EQ(prefs.oscPort, 8000);
    EXPECT_EQ(prefs.oscBindAddress, "0.0.0.0");
    EXPECT_TRUE(prefs.midiDevices.empty());
}

TEST(ControlPreferencesTest, RoundTripsThroughThePreferencesFile) {
    const auto file = std::filesystem::temp_directory_path() /
                      ("qm_control_prefs_" + UUID::generate().toString() + ".json");
    {
        PreferencesDialog dialog;
        ControlPreferences& c = dialog.getPreferences().control;
        c.oscEnabled = true;
        c.oscPort = 9001;
        c.oscBindAddress = "127.0.0.1";
        c.midiDevices = {"APC mini", "nanoKONTROL2"};
        ASSERT_TRUE(dialog.saveToFile(file.string()));
    }
    PreferencesDialog loaded;
    ASSERT_TRUE(loaded.loadFromFile(file.string()));
    const ControlPreferences& c = loaded.getPreferences().control;
    EXPECT_TRUE(c.oscEnabled);
    EXPECT_EQ(c.oscPort, 9001);
    EXPECT_EQ(c.oscBindAddress, "127.0.0.1");
    EXPECT_EQ(c.midiDevices, (std::vector<std::string>{"APC mini", "nanoKONTROL2"}));
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// The audio output device is a machine preference beside the control ones (ADR-050).
TEST(AudioPreferencesTest, OutputDeviceRoundTripsAndDefaultsToTheSystemDevice) {
    EXPECT_TRUE(PerformancePreferences{}.audioDevice.empty());  // empty = system default
    const auto file = std::filesystem::temp_directory_path() /
                      ("qm_audio_prefs_" + UUID::generate().toString() + ".json");
    {
        PreferencesDialog dialog;
        dialog.getPreferences().performance.audioDevice = "Speakers (USB Audio)";
        ASSERT_TRUE(dialog.saveToFile(file.string()));
    }
    PreferencesDialog loaded;
    ASSERT_TRUE(loaded.loadFromFile(file.string()));
    EXPECT_EQ(loaded.getPreferences().performance.audioDevice, "Speakers (USB Audio)");
    {
        std::ofstream out(file, std::ios::binary);
        out << R"({"version": "1.0", "performance": {"audioDevice": 7}})";
    }
    PreferencesDialog odd;
    ASSERT_TRUE(odd.loadFromFile(file.string()));
    EXPECT_TRUE(odd.getPreferences().performance.audioDevice.empty());
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(ControlPreferencesTest, OldFilesAndBadValuesKeepDefaults) {
    const auto file = std::filesystem::temp_directory_path() /
                      ("qm_control_prefs_bad_" + UUID::generate().toString() + ".json");
    for (const char* text :
         {R"({"version": "1.0"})",
          R"({"version": "1.0", "control": {"oscEnabled": "yes", "oscPort": 70000,
               "oscBindAddress": 5, "midiDevices": [1, "Pad"]}})"}) {
        SCOPED_TRACE(text);
        {
            std::ofstream out(file, std::ios::binary);
            ASSERT_TRUE(out.good());
            out << text;
        }
        PreferencesDialog dialog;
        ASSERT_TRUE(dialog.loadFromFile(file.string()));
        const ControlPreferences& c = dialog.getPreferences().control;
        EXPECT_FALSE(c.oscEnabled);
        EXPECT_EQ(c.oscPort, 8000);
        EXPECT_EQ(c.oscBindAddress, "0.0.0.0");
        EXPECT_TRUE(c.midiDevices.empty() || c.midiDevices == std::vector<std::string>{"Pad"});
    }
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(ControlPreferencesTest, FeedbackSettingsDefaultRoundTripAndRejectBadValues) {
    const ControlPreferences defaults;
    EXPECT_FALSE(defaults.oscFeedbackEnabled);
    EXPECT_TRUE(defaults.oscFeedbackHost.empty()) << "empty = reply to the sender";
    EXPECT_EQ(defaults.oscFeedbackPort, 9000);
    EXPECT_FALSE(defaults.oscFeedbackUseIds);

    const auto file = std::filesystem::temp_directory_path() /
                      ("qm_control_feedback_" + UUID::generate().toString() + ".json");
    {
        PreferencesDialog dialog;
        ControlPreferences& c = dialog.getPreferences().control;
        c.oscFeedbackEnabled = true;
        c.oscFeedbackHost = "192.168.1.20";
        c.oscFeedbackPort = 0;
        c.oscFeedbackUseIds = true;
        ASSERT_TRUE(dialog.saveToFile(file.string()));
    }
    {
        PreferencesDialog loaded;
        ASSERT_TRUE(loaded.loadFromFile(file.string()));
        const ControlPreferences& c = loaded.getPreferences().control;
        EXPECT_TRUE(c.oscFeedbackEnabled);
        EXPECT_EQ(c.oscFeedbackHost, "192.168.1.20");
        EXPECT_EQ(c.oscFeedbackPort, 0);
        EXPECT_TRUE(c.oscFeedbackUseIds);
    }
    {
        std::ofstream out(file, std::ios::binary);
        out << R"({"version": "1.0", "control": {"oscFeedbackEnabled": 1, "oscFeedbackHost": [],
                   "oscFeedbackPort": 65536, "oscFeedbackUseIds": "no"}})";
    }
    PreferencesDialog odd;
    ASSERT_TRUE(odd.loadFromFile(file.string()));
    const ControlPreferences& c = odd.getPreferences().control;
    EXPECT_FALSE(c.oscFeedbackEnabled);
    EXPECT_TRUE(c.oscFeedbackHost.empty());
    EXPECT_EQ(c.oscFeedbackPort, 9000);
    EXPECT_FALSE(c.oscFeedbackUseIds);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// ---- Preferences file (TD-28): only settings the app honours ----------------------

namespace {

struct TempPrefsFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("qm_prefs_" + UUID::generate().toString() + ".json");
    ~TempPrefsFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    void write(const char* text) const {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }
    nlohmann::json read() const {
        std::ifstream in(path, std::ios::binary);
        return nlohmann::json::parse(in);
    }
};

}  // namespace

TEST(PreferencesFileTest, EverySettingRoundTrips) {
    TempPrefsFile file;
    {
        PreferencesDialog dialog;
        AppPreferences& p = dialog.getPreferences();
        p.general.defaultMediaFolder = "D:/show/media";
        p.general.autoLoadMediaOnStartup = false;
        p.general.recursiveFolderScan = true;
        p.general.autoSaveEnabled = false;
        p.general.autoSaveIntervalMinutes = 12;
        p.general.uiMode = 1;
        p.general.sceneFadeSeconds = 1.25f;
        p.performance.audioDevice = "Speakers (USB Audio)";
        p.control.oscEnabled = true;
        p.control.oscPort = 9100;
        p.control.oscBindAddress = "127.0.0.1";
        p.control.midiDevices = {"APC mini"};
        p.control.oscFeedbackEnabled = true;
        p.control.oscFeedbackHost = "10.0.0.5";
        p.control.oscFeedbackPort = 9001;
        p.control.oscFeedbackUseIds = true;
        ASSERT_TRUE(dialog.saveToFile(file.path.string()));
    }
    PreferencesDialog loaded;
    ASSERT_TRUE(loaded.loadFromFile(file.path.string()));
    const AppPreferences& p = loaded.getPreferences();
    EXPECT_EQ(p.general.defaultMediaFolder, "D:/show/media");
    EXPECT_FALSE(p.general.autoLoadMediaOnStartup);
    EXPECT_TRUE(p.general.recursiveFolderScan);
    EXPECT_FALSE(p.general.autoSaveEnabled);
    EXPECT_EQ(p.general.autoSaveIntervalMinutes, 12);
    EXPECT_EQ(p.general.uiMode, 1);
    EXPECT_FLOAT_EQ(p.general.sceneFadeSeconds, 1.25f);
    EXPECT_EQ(p.performance.audioDevice, "Speakers (USB Audio)");
    EXPECT_TRUE(p.control.oscEnabled);
    EXPECT_EQ(p.control.oscPort, 9100);
    EXPECT_EQ(p.control.oscBindAddress, "127.0.0.1");
    EXPECT_EQ(p.control.midiDevices, std::vector<std::string>{"APC mini"});
    EXPECT_TRUE(p.control.oscFeedbackEnabled);
    EXPECT_EQ(p.control.oscFeedbackHost, "10.0.0.5");
    EXPECT_EQ(p.control.oscFeedbackPort, 9001);
    EXPECT_TRUE(p.control.oscFeedbackUseIds);
}

TEST(PreferencesFileTest, DefaultsAreTheShippedBehaviour) {
    const AppPreferences p;
    EXPECT_TRUE(p.general.defaultMediaFolder.empty());
    EXPECT_TRUE(p.general.autoLoadMediaOnStartup);
    EXPECT_FALSE(p.general.recursiveFolderScan);
    EXPECT_TRUE(p.general.autoSaveEnabled);
    EXPECT_EQ(p.general.autoSaveIntervalMinutes, 1);  // ADR-046: at most a minute lost
    EXPECT_EQ(p.general.uiMode, 0);                   // ADR-042: Simple
    EXPECT_EQ(p.general.sceneFadeSeconds, 0.0f);      // ADR-047 §5: takes cut
    EXPECT_TRUE(p.performance.audioDevice.empty());
}

// A file written by an older version carries settings nothing honoured: they load
// without error, are ignored, and are gone from the next save.
TEST(PreferencesFileTest, SettingsThisVersionDroppedAreIgnoredAndNotWrittenBack) {
    TempPrefsFile file;
    file.write(R"({"version": "1.0",
        "general": {"defaultProjectPath": "C:/p", "defaultMeshResolution": 16,
                    "openLastProjectOnStartup": false, "recentProjectsMaxCount": 5,
                    "mediaFolderBookmarks": ["C:/a"], "createBackupOnSave": false,
                    "showWelcomeDialog": false, "confirmOnExit": false,
                    "rememberWindowLayout": false, "themeIndex": 1,
                    "autoSaveIntervalMinutes": 5, "uiMode": 1, "defaultMediaFolder": "E:/clips"},
        "performance": {"videoBufferFrames": 60, "videoDecodeThreads": 4,
                        "enableHardwareDecoding": false, "enableVSync": false,
                        "targetFrameRate": 30, "textureQuality": 0, "enableMipmaps": false,
                        "audioBufferSize": 256, "audioSampleRate": 44100,
                        "outputBufferCount": 3, "enableAdaptiveSync": true,
                        "audioDevice": "Line Out"},
        "control": {"oscEnabled": true}})");
    PreferencesDialog dialog;
    ASSERT_TRUE(dialog.loadFromFile(file.path.string()));
    const AppPreferences& p = dialog.getPreferences();
    EXPECT_EQ(p.general.autoSaveIntervalMinutes, 5);
    EXPECT_EQ(p.general.uiMode, 1);
    EXPECT_EQ(p.general.defaultMediaFolder, "E:/clips");
    EXPECT_EQ(p.performance.audioDevice, "Line Out");
    EXPECT_TRUE(p.control.oscEnabled);

    ASSERT_TRUE(dialog.saveToFile(file.path.string()));
    const nlohmann::json saved = file.read();
    std::vector<std::string> keys;
    for (const char* section : {"general", "performance", "control"}) {
        ASSERT_TRUE(saved.contains(section)) << section;
        for (const auto& item : saved[section].items()) keys.push_back(std::string(section) + "." + item.key());
    }
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<std::string>{
                        "control.midiDevices", "control.oscBindAddress", "control.oscEnabled",
                        "control.oscFeedbackEnabled", "control.oscFeedbackHost", "control.oscFeedbackPort",
                        "control.oscFeedbackUseIds", "control.oscPort",
                        "general.autoLoadMediaOnStartup", "general.autoSaveEnabled",
                        "general.autoSaveIntervalMinutes", "general.defaultMediaFolder",
                        "general.recursiveFolderScan", "general.sceneFadeSeconds", "general.uiMode",
                        "performance.audioDevice"}));
}

TEST(PreferencesFileTest, WrongTypesAndOutOfRangeValuesKeepDefaults) {
    TempPrefsFile file;
    for (const char* text :
         {R"({"version": "1.0", "general": {"autoSaveIntervalMinutes": 0, "uiMode": 2,
               "autoSaveEnabled": "no", "defaultMediaFolder": 3, "recursiveFolderScan": 1,
               "autoLoadMediaOnStartup": null}})",
          R"({"version": "1.0", "general": {"autoSaveIntervalMinutes": 31, "uiMode": -1}})",
          R"({"version": "1.0", "general": {"autoSaveIntervalMinutes": "5", "uiMode": 1.5}})",
          R"({"version": "1.0", "general": [1, 2], "performance": "x", "control": 7})"}) {
        SCOPED_TRACE(text);
        file.write(text);
        PreferencesDialog dialog;
        ASSERT_TRUE(dialog.loadFromFile(file.path.string()));
        const GeneralPreferences& g = dialog.getPreferences().general;
        EXPECT_EQ(g.autoSaveIntervalMinutes, 1);
        EXPECT_EQ(g.uiMode, 0);
        EXPECT_TRUE(g.autoSaveEnabled);
        EXPECT_TRUE(g.defaultMediaFolder.empty());
        EXPECT_FALSE(g.recursiveFolderScan);
        EXPECT_TRUE(g.autoLoadMediaOnStartup);
    }
}

// The scene take fade default (ADR-047 §5): seconds, 0 to MAX_SCENE_FADE_SECONDS, an
// integer or a fraction; anything else keeps the current value.
TEST(PreferencesFileTest, SceneFadeDefaultRoundTripsAndRejectsBadValues) {
    TempPrefsFile file;
    file.write(R"({"version": "1.0", "general": {"sceneFadeSeconds": 2}})");
    PreferencesDialog dialog;
    ASSERT_TRUE(dialog.loadFromFile(file.path.string()));
    EXPECT_FLOAT_EQ(dialog.getPreferences().general.sceneFadeSeconds, 2.0f);
    dialog.getPreferences().general.sceneFadeSeconds = 0.5f;
    ASSERT_TRUE(dialog.saveToFile(file.path.string()));
    PreferencesDialog loaded;
    ASSERT_TRUE(loaded.loadFromFile(file.path.string()));
    EXPECT_FLOAT_EQ(loaded.getPreferences().general.sceneFadeSeconds, 0.5f);

    for (const char* text : {R"({"version": "1.0", "general": {"sceneFadeSeconds": -0.5}})",
                             R"({"version": "1.0", "general": {"sceneFadeSeconds": 60.5}})",
                             R"({"version": "1.0", "general": {"sceneFadeSeconds": "1"}})",
                             R"({"version": "1.0", "general": {"sceneFadeSeconds": null}})",
                             R"({"version": "1.0", "general": {"sceneFadeSeconds": true}})"}) {
        SCOPED_TRACE(text);
        file.write(text);
        PreferencesDialog d;
        d.getPreferences().general.sceneFadeSeconds = 3.0f;
        ASSERT_TRUE(d.loadFromFile(file.path.string()));
        EXPECT_FLOAT_EQ(d.getPreferences().general.sceneFadeSeconds, 3.0f);
    }
}

TEST(PreferencesFileTest, AFileThatIsNotPreferencesIsRefusedAndChangesNothing) {
    TempPrefsFile file;
    for (const char* text : {"not json", "[1, 2]", R"({"general": {"uiMode": 1}})"}) {
        SCOPED_TRACE(text);
        file.write(text);
        PreferencesDialog dialog;
        EXPECT_FALSE(dialog.loadFromFile(file.path.string()));
        EXPECT_EQ(dialog.getPreferences().general.uiMode, 0);
    }
    PreferencesDialog missing;
    EXPECT_FALSE(missing.loadFromFile((file.path.string() + ".absent")));
}

// A user name outside the ANSI code page (TD follow-up): the path stays wide end to
// end, so the file is written where asked and reads back.
TEST(PreferencesFileTest, APathOutsideTheAnsiCodePageWorks) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      (L"qm_prefs_\u00fc\u65e5\u672c\u0416_" +
                                       std::filesystem::path(UUID::generate().toString()).wstring());
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / L"pr\u00e9f\u00e9rences.json";
    {
        PreferencesDialog dialog;
        dialog.getPreferences().general.defaultMediaFolder = u8"D:/Vid\u00e9os/\u65e5\u672c";
        ASSERT_TRUE(dialog.saveToFile(file));
    }
    EXPECT_TRUE(std::filesystem::exists(file));
    PreferencesDialog loaded;
    ASSERT_TRUE(loaded.loadFromFile(file));
    EXPECT_EQ(loaded.getPreferences().general.defaultMediaFolder, u8"D:/Vid\u00e9os/\u65e5\u672c");
    // First run: no file yet. Refused, never thrown (its name goes into the message).
    PreferencesDialog firstRun;
    EXPECT_FALSE(firstRun.loadFromFile(dir / L"\u65e5\u672c.json"));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#ifdef _WIN32
TEST(PreferencesFileTest, DefaultPathIsUnderRoamingAppData) {
    // %APPDATA% is the Roaming AppData folder, read wide (no windows.h here: its UUID
    // clashes with quadmapper::UUID).
    wchar_t* appData = nullptr;
    size_t length = 0;
    ASSERT_EQ(_wdupenv_s(&appData, &length, L"APPDATA"), 0);
    ASSERT_NE(appData, nullptr);
    const std::filesystem::path expected = std::filesystem::path(appData) / L"QUADmapper" / L"preferences.json";
    std::free(appData);
    EXPECT_TRUE(std::filesystem::equivalent(AppPreferences::getDefaultConfigPath().parent_path(),
                                            expected.parent_path()));
    EXPECT_EQ(AppPreferences::getDefaultConfigPath().filename(), expected.filename());
}
#endif

// The window against real (headless) ImGui: shown only while es.showPreferences is set,
// and its General tab edits the editor's UI mode directly (Application saves it).
TEST(PreferencesWindowTest, RendersOnlyWhenOpenAndShowsTheEditorsMode) {
    ImGuiContext* imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    EditorState es;
    EditorContext ctx{es, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    PreferencesDialog dialog;
    auto frame = [&] {
        ImGui::NewFrame();
        dialog.render(ctx, nullptr);
        const bool shown = ImGui::FindWindowByName("Preferences") != nullptr &&
                           ImGui::FindWindowByName("Preferences")->Active;
        ImGui::Render();
        return shown;
    };
    EXPECT_FALSE(frame()) << "closed by default";
    es.showPreferences = true;
    es.uiMode = EditorState::UiMode::Advanced;
    EXPECT_TRUE(frame());
    EXPECT_TRUE(frame());
    EXPECT_EQ(es.uiMode, EditorState::UiMode::Advanced) << "rendering alone changes nothing";
    es.showPreferences = false;
    EXPECT_FALSE(frame());
    ImGui::DestroyContext(imgui);
}

// Dragging "Every (minutes)" applies the value on every frame but writes the file once,
// on release: it used to rewrite preferences.json on every frame of the drag.
TEST(PreferencesWindowTest, DraggingTheAutosaveSliderSavesOnceOnRelease) {
    TempPrefsFile file;
    ImGuiContext* imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    EditorState es;
    es.showPreferences = true;
    EditorContext ctx{es, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    PreferencesDialog dialog;
    dialog.setConfigPath(file.path);
    const int& interval = dialog.getPreferences().general.autoSaveIntervalMinutes;
    auto frame = [&] {
        ImGui::NewFrame();
        dialog.render(ctx, nullptr);
        ImGui::Render();
    };
    frame();
    frame();

    // The slider's id: its label hashed in the General tab's id scope.
    ImGuiWindow* window = ImGui::FindWindowByName("Preferences");
    ASSERT_NE(window, nullptr);
    ImGuiTabBar* tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("PreferenceTabs"));
    ASSERT_NE(tabs, nullptr);
    const ImGuiID slider = ImHashStr("Every (minutes)", 0, tabs->VisibleTabId);

    // Find it by hovering down the left of the window (it is 200 px wide there).
    ImVec2 at;
    bool found = false;
    for (float y = window->Pos.y; y < window->Pos.y + window->Size.y && !found; y += 4.0f) {
        for (float x = window->Pos.x + 16.0f; x < window->Pos.x + 200.0f && !found; x += 24.0f) {
            io.AddMousePosEvent(x, y);
            frame();
            if (ImGui::GetCurrentContext()->HoveredId == slider) {
                at = ImVec2(x, y);
                found = true;
            }
        }
    }
    ASSERT_TRUE(found) << "the autosave slider was not found under the mouse";

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    const int pressed = interval;
    for (int i = 1; i <= 20; ++i) {
        io.AddMousePosEvent(at.x + 8.0f * static_cast<float>(i), at.y);
        frame();
    }
    EXPECT_GT(interval, pressed) << "the drag applies at once";
    EXPECT_FALSE(std::filesystem::exists(file.path)) << "written while still dragging";

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    ASSERT_TRUE(std::filesystem::exists(file.path)) << "not written on release";
    EXPECT_EQ(file.read()["general"]["autoSaveIntervalMinutes"].get<int>(), interval);

    // Once: later frames do not write it again.
    std::filesystem::remove(file.path);
    for (int i = 0; i < 5; ++i) frame();
    EXPECT_FALSE(std::filesystem::exists(file.path));

    ImGui::DestroyContext(imgui);
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(file.path) += ".bak", ec);
}
