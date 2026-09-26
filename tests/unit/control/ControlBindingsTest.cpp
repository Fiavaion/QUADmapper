// The application's control targets (app/ControlBindings.hpp) against real Scenes,
// Quads and a SourceManager, driven through the dispatcher as OSC and MIDI would.
// No GL: the stand-in playable source has zero size, so no texture is created.

#include "app/ControlBindings.hpp"
#include "control/ControlDispatcher.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

using namespace quadmapper;
using namespace quadmapper::control;

namespace {

class FakePlayable : public PlayableSource {
public:
    explicit FakePlayable(std::string name) : name_(std::move(name)) {}
    const UUID& getId() const override { return id_; }
    std::string getName() const override { return name_; }
    SourceType getType() const override { return SourceType::VIDEO_FILE; }
    std::string getTypeName() const override { return "Fake video"; }
    bool initialize() override { return true; }
    void shutdown() override {}
    bool isReady() const override { return true; }
    bool update(double) override { return false; }
    bool hasNewFrame() const override { return false; }
    const FrameData* getCurrentFrame() const override { return nullptr; }
    void markFrameConsumed() override {}
    SourceCapabilities getCapabilities() const override { return {}; }
    int getWidth() const override { return 0; }
    int getHeight() const override { return 0; }

    void play() override { state = PlaybackState::PLAYING; ++plays; }
    void pause() override { state = PlaybackState::PAUSED; ++pauses; }
    void stop() override { state = PlaybackState::STOPPED; }
    void togglePlayPause() override {}
    bool canSeek() const override { return true; }
    void seek(double t) override { time = t; ++seeks; }
    void seekProgress(float) override {}
    PlaybackState getPlaybackState() const override { return state; }
    double getCurrentTime() const override { return time; }
    double getDuration() const override { return 10.0; }
    float getProgress() const override { return 0.0f; }
    void setSpeed(float s) override { speed = s; }
    float getSpeed() const override { return speed; }
    void setLoopMode(LoopMode) override {}
    LoopMode getLoopMode() const override { return LoopMode::LOOP; }
    std::string getLoopModeNotice() const override { return {}; }
    void setTimelineLocked(bool, const PlaybackClock&) override {}
    bool isTimelineLocked() const override { return false; }
    void followTimeline(const PlaybackClock&) override {}
    const PlaybackClock& getClock() const override { return clock_; }
    PlaybackTimePoint getClockNow() const override { return {}; }

    PlaybackState state = PlaybackState::PAUSED;
    int plays = 0, pauses = 0, seeks = 0;
    double time = 5.0;
    float speed = 1.0f;

private:
    PlaybackClock clock_;
    UUID id_ = UUID::generate();
    std::string name_;
};

struct AppFixture {
    std::vector<std::unique_ptr<Scene>> scenes;
    SourceManager sources;
    ControlRegistry registry;
    ControlDispatcher dispatcher{registry};
    std::vector<MidiMapping> mappings;
    std::vector<int> takes;
    bool blackout = false;
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
        host.takeScene = [this](int n) { takes.push_back(n); };
        host.isBlackout = [this] { return blackout; };
        host.setBlackout = [this](bool on) { blackout = on; };
        registerControlTargets(registry, host);
        refresh();
    }

    void refresh() { refreshControlEntities(registry, scenes, &sources); }

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
                    action == "restart" || action == "speed")
            << action;
    }
    // blackout + 2 scene takes + 3 quads x 2 + 1 source x 4
    EXPECT_EQ(actions.size(), 1u + 2u + 6u + 4u);
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
    EXPECT_TRUE(sameBinding(fromProjectMapping(d), m));
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
