// The application's control targets (editor/ControlBindings.hpp) against real Scenes,
// Quads and a SourceManager, driven through the dispatcher as OSC and MIDI would.
// No GL: the stand-in playable source has zero size, so no texture is created.

#include "editor/ControlBindings.hpp"
#include "FakePlayable.hpp"
#include "control/ControlDispatcher.hpp"

#include <gtest/gtest.h>

#include <chrono>
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

// The show transport (ADR-050 Amendment 1): the timeline controls reach every video,
// not only those locked to the master.
TEST(ControlBindingsTest, TimelineTargetsDriveEveryVideo) {
    AppFixture fx;
    fx.clip->play();  // unlocked and playing
    auto lockedFake = std::make_unique<FakePlayable>("locked.mp4");
    FakePlayable* locked = lockedFake.get();
    fx.sources.setTimelineLocked(fx.sources.registerSource(std::move(lockedFake)), true);
    auto stoppedFake = std::make_unique<FakePlayable>("stopped.mp4");
    FakePlayable* stopped = stoppedFake.get();
    stopped->state = PlaybackState::STOPPED;
    fx.sources.registerSource(std::move(stoppedFake));

    fx.osc("/qm/timeline/pause");
    EXPECT_EQ(fx.clip->state, PlaybackState::PAUSED) << "an unlocked video pauses with the show";
    EXPECT_EQ(locked->pauses, 0) << "a locked video follows the master clock instead";
    EXPECT_FALSE(fx.sources.getMasterTimeline().running());

    fx.osc("/qm/timeline/play");
    EXPECT_EQ(fx.clip->state, PlaybackState::PLAYING);
    EXPECT_EQ(locked->plays, 0);
    EXPECT_EQ(stopped->state, PlaybackState::STOPPED) << "only a paused video is resumed";
    EXPECT_TRUE(fx.sources.getMasterTimeline().running());

    fx.clip->time = 4.0;
    fx.osc("/qm/timeline/restart");
    EXPECT_DOUBLE_EQ(fx.clip->time, 0.0) << "restart sends every video to its start";
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


// Compositions (ADR-061): /qm/composition/<n>/next, prev, cue i k, play, pause,
// restart and fade, by name or id; the cue's clip is restarted.
TEST(ControlBindingsTest, CompositionCuesByOscAndTheirClips) {
    AppFixture app;
    auto other = std::make_unique<FakePlayable>("other.mp4");
    FakePlayable* otherClip = other.get();
    const UUID otherId = app.sources.registerSource(std::move(other));
    CompositionSettings settings;
    settings.name = "Show";
    settings.mode = CompositionMode::Switch;
    settings.fadeSeconds = 0.0;
    settings.cues = {{app.clipId, 2.0}, {otherId, 2.0}, {UUID(), 2.0}};
    const UUID id = app.sources.createCompositionSource(settings);
    auto* comp = app.sources.getSourceAs<CompositionSource>(id);
    ASSERT_NE(comp, nullptr);
    app.refresh();
    auto sourceOf = [&](const UUID& s) { return app.sources.getSource(s); };

    comp->advance(0.0, sourceOf);  // cue 1 on: its clip from the start; the others paused
    EXPECT_EQ(app.clip->state, PlaybackState::PLAYING);
    EXPECT_GE(app.clip->seeks, 1);
    EXPECT_EQ(otherClip->state, PlaybackState::PAUSED);

    app.osc("/qm/composition/show/next");  // names ignore case
    EXPECT_EQ(comp->playhead().current(), 1);
    comp->advance(0.0, sourceOf);
    EXPECT_EQ(otherClip->state, PlaybackState::PLAYING);
    EXPECT_EQ(app.clip->state, PlaybackState::PAUSED);  // cut: faded out at once

    app.oscArgs("/qm/composition/Show/cue", {intArg(3)});
    EXPECT_EQ(comp->playhead().current(), 2);
    app.osc("/qm/composition/" + id.toString() + "/prev");
    EXPECT_EQ(comp->playhead().current(), 1);
    app.osc("/qm/composition/Show/pause");
    EXPECT_FALSE(comp->playhead().running());
    app.osc("/qm/composition/Show/play");
    EXPECT_TRUE(comp->playhead().running());
    app.osc("/qm/composition/Show/fade", 2.5f);
    EXPECT_NEAR(comp->settings().fadeSeconds, 2.5, 1e-6);
    app.osc("/qm/composition/Show/restart");
    EXPECT_EQ(comp->playhead().current(), 0);
    // No such composition: nothing happens, nothing crashes.
    app.osc("/qm/composition/Nothing/next");
}

TEST(ControlBindingsTest, CompositionsNeverNestAndKeepTheirOrder) {
    AppFixture app;
    CompositionSettings first;
    first.name = "First";
    const UUID a = app.sources.createCompositionSource(first);
    CompositionSettings second;
    second.name = "Second";
    second.cues = {{a, 1.0}, {app.clipId, 1.0}};  // a composition as a cue is dropped
    const UUID b = app.sources.createCompositionSource(second);
    const auto* comp = app.sources.getSourceAs<CompositionSource>(b);
    ASSERT_NE(comp, nullptr);
    ASSERT_EQ(comp->settings().cues.size(), 1u);
    EXPECT_EQ(comp->settings().cues[0].sourceId, app.clipId);
    EXPECT_EQ(app.sources.getCompositionIds(), (std::vector<UUID>{a, b}));
    app.sources.removeSource(a);
    EXPECT_EQ(app.sources.getCompositionIds(), (std::vector<UUID>{b}));
}


// Review fixes (ADR-061): a cue removed under the playhead starts the one now there;
// Restart clips on cue turned off lets the paused clips run; a clip's length fills a
// hold that waited for it.
TEST(ControlBindingsTest, CompositionFollowsItsCueListAndTransportSetting) {
    AppFixture app;
    auto other = std::make_unique<FakePlayable>("other.mp4");
    FakePlayable* otherClip = other.get();
    const UUID otherId = app.sources.registerSource(std::move(other));
    CompositionSettings settings;
    settings.mode = CompositionMode::Timeline;
    settings.fadeSeconds = 0.0;
    settings.cues = {{app.clipId, 4.0}, {otherId, 0.0}};
    settings.cues[1].holdFromClip = true;
    const UUID id = app.sources.createCompositionSource(settings);
    auto* comp = app.sources.getSourceAs<CompositionSource>(id);
    ASSERT_NE(comp, nullptr);
    auto sourceOf = [&](const UUID& s) { return app.sources.getSource(s); };

    comp->advance(1.0, sourceOf);
    EXPECT_DOUBLE_EQ(comp->settings().cues[1].holdSeconds, 10.0) << "the clip's length";
    EXPECT_FALSE(comp->settings().cues[1].holdFromClip);
    EXPECT_EQ(otherClip->state, PlaybackState::PAUSED);

    comp->editSettings().cues.erase(comp->editSettings().cues.begin());  // the cue showing
    const int seeks = otherClip->seeks;
    comp->advance(0.0, sourceOf);
    EXPECT_EQ(comp->playhead().current(), 0);
    EXPECT_EQ(otherClip->state, PlaybackState::PLAYING) << "the cue now on starts";
    EXPECT_GT(otherClip->seeks, seeks);
    EXPECT_NEAR(comp->playhead().timeInCue(), 0.0, 1e-9);

    comp->editSettings().cues.push_back({app.clipId, 4.0});
    comp->advance(0.0, sourceOf);
    app.clip->pause();
    EXPECT_EQ(app.clip->state, PlaybackState::PAUSED);
    comp->editSettings().restartOnCue = false;
    comp->advance(0.0, sourceOf);
    EXPECT_EQ(app.clip->state, PlaybackState::PLAYING) << "every clip keeps running";
}
