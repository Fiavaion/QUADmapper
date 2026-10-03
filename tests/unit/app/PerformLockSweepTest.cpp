// The perform lock sweep (TD-34, ADR-051 §4): lock a representative show, drive every
// editing entry point that runs headlessly, and check after each step that the show is
// what it was when the lock was taken. The lock is enforced where each edit starts (the
// panels, the canvas, the registry, the session, the pickers), so a path that forgets
// to ask is found here rather than on show night.
//
// What "the show" is: showFingerprint() below, the same live -> .qmap mapping the
// unsaved-changes rule uses (syncEditorToProject), serialized, plus the live state the
// projectors show that the file does not hold (the blend test pattern, the UV test
// grids). The machine preferences are compared too (preferencesFingerprint), since the
// ADR blocks Preferences edits under the lock.
//
// What may change under the lock is listed once, in ALLOWED UNDER THE LOCK below, each
// item with the ADR line that keeps it. Everything else must be byte-identical.
//
// Entry points: every registered action from every source; every panel window and
// Resources tab and the session modals under headless ImGui (each item found by
// hovering the window is clicked, double-clicked, dragged, typed into and
// right-clicked, popups opened that way are swept too, and keys are pressed into the
// window); the menu bar with its menus and submenus; every key with the common modifiers through the editor frame (the shortcut
// router and the scene take keys); the canvas with the mouse (ViewportEditor::Pointer)
// and its keys; drags dropped on the canvas and on every panel item; the pickers'
// answers (ProjectSession's *Chosen, the Video Player's load, the Preferences folder)
// and an OBJ import finishing under the lock; every OSC target and MIDI mapping, queued
// and applied as the application applies them, and a MIDI learn begun before locking;
// every key on an output window.
//
// The application's own frame code runs between the steps as Application runs it
// (TD-36): boundary() is Application::runFrameBoundaryTasks and the steps render() takes
// before the editor frame, and the scene takes (keys, output windows, OSC/MIDI) are
// queued and drained in the editor frame through editor/SceneTakes.hpp.
//
// The real EditorUi runs in a hidden GL 3.3 window (skips without GL), with the
// project session, a source manager and two unrealised outputs (no window is ever
// shown: OutputManager::initialize is not called, so perform mode cannot create one).

#include "app/ProjectSession.hpp"
#include "control/ControlDispatcher.hpp"
#include "control/ControlRegistry.hpp"
#include "control/ExternalControl.hpp"
#include "core/FileSystem.hpp"
#include "core/UUID.hpp"
#include "editor/ActionRegistry.hpp"
#include "editor/ControlBindings.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorServices.hpp"
#include "editor/EditorState.hpp"
#include "editor/MediaAssign.hpp"
#include "editor/MeshLoads.hpp"
#include "editor/MissingMedia.hpp"
#include "editor/NewQuads.hpp"
#include "editor/PerformLock.hpp"
#include "editor/ProjectorFit.hpp"
#include "editor/Preferences.hpp"
#include "editor/ProjectMapping.hpp"
#include "editor/SceneTakes.hpp"
#include "editor/SliceEditing.hpp"
#include "input/VertexKeyboard.hpp"
#include "media/MediaItem.hpp"
#include "media/MediaLibrary.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "project/Project.hpp"
#include "project/ProjectSerializer.hpp"
#include "scene/MeshObject.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Scene.hpp"
#include "scene/Slice.hpp"
#include "source/SourceManager.hpp"
#include "ui/EditorUi.hpp"
#include "ui/PanelHost.hpp"
#include "ui/ShortcutRouter.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"
#include "ui/panels/ResourcesPanel.hpp"
#include "ui/panels/ViewportEditor.hpp"
#include "ui/panels/VideoPlayerPanel.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace quadmapper;
namespace fs = std::filesystem;

namespace {

// ---- ALLOWED UNDER THE LOCK ----------------------------------------------------------
// ADR-051 §4 "Keeps", and the 2026-09-30 amendment. Each is either outside the
// fingerprint or masked by it, and a step may change it only where the sweep says so.
//
// Always (masked, or not part of the show at all):
//   - canvas zoom, pan and viewing: each scene's view (pan, zoom, grid, snap) is
//     cleared ("canvas zoom, pan and viewing");
//   - the Video Player transport and the master timeline: a source's playing flag is
//     cleared, and position is not in the file ("the Video Player transport");
//   - master volume, mute and the audio device: not in the fingerprint (amendment: "the
//     audio device and master volume ... stay available");
//   - blackout and perform mode (F11 / Esc): runtime state, not in the fingerprint
//     ("blackout, F11/Esc and perform mode");
//   - Save and Save As: they write files and mark the project clean, the show is the
//     same ("Save and autosave"; amendment: Save As stays available);
//   - the Simple / Advanced toggle (amendment), the selection and which panels are open:
//     editor view state, not the show.
// Only in the steps that take scenes (keys 1-9, OSC/MIDI takes, an output window's
// 1-9, the scene take actions):
//   - ALLOW_TAKES: which scene each output shows ("scene takes by key, OSC and MIDI,
//     with their fades").
// Only in the external control sweep:
//   - ALLOW_TARGETS: each OSC/MIDI target's own value, that is quad opacity and
//     visibility, and a source's speed, volume and mute ("every OSC/MIDI target").
enum Allow : unsigned {
    ALLOW_NONE = 0,
    ALLOW_TAKES = 1u << 0,
    ALLOW_TARGETS = 1u << 1,
};

// The show as the projectors show it, minus what `allow` lets change.
std::string showFingerprint(EditorState& es, OutputManager& om, SourceManager* sources,
                            unsigned allow) {
    Project project;
    syncEditorToProject(es, &om, sources, project);
    for (Project::SceneData& scene : project.getScenes()) {
        scene.view = Project::SceneViewData{};
        if (allow & ALLOW_TARGETS) {
            for (Quad& quad : scene.quads) {
                quad.setOpacity(1.0f);
                quad.setVisible(true);
            }
        }
    }
    for (Project::SourceData& source : project.getSources()) {
        if (!source.transport) continue;
        source.transport->playing = false;
        if (allow & ALLOW_TARGETS) {
            source.transport->speed = 1.0f;
            source.transport->volume = 1.0f;
            source.transport->muted = false;
        }
    }
    if (allow & ALLOW_TAKES) {
        // A take also frames an uncalibrated projector on the scene it puts on air (TD-36).
        for (Project::OutputData& output : project.getOutputs()) {
            output.sceneId = UUID();
            if (!output.projector.calibrated) output.projector = ProjectorCamera{};
        }
    }
    std::string text = ProjectSerializer().serialize(project);
    // On the projectors, not in the file.
    text += "\nblendTestPattern=" + std::to_string(om.isBlendTestPattern());
    std::set<std::string> grids;
    for (const auto& entry : es.quadTextures) grids.insert(entry.first.toString());
    text += "\nuvTestGrids=";
    for (const std::string& id : grids) text += id + ",";
    return text;
}

// The values ALLOW_TAKES and ALLOW_TARGETS mask, read without serializing (cheap): when
// a step changed one of them, the baselines are taken again.
std::string allowedValues(EditorState& es, OutputManager& om, SourceManager* sources) {
    Project project;
    syncEditorToProject(es, &om, sources, project);
    std::string text;
    for (const Project::OutputData& output : project.getOutputs()) text += output.sceneId.toString() + ";";
    for (const Project::SceneData& scene : project.getScenes()) {
        for (const Quad& quad : scene.quads) {
            text += std::to_string(quad.getOpacity()) + (quad.isVisible() ? "v;" : "h;");
        }
    }
    for (const Project::SourceData& source : project.getSources()) {
        if (!source.transport) continue;
        text += std::to_string(source.transport->speed) + "," + std::to_string(source.transport->volume) +
                (source.transport->muted ? "m;" : "s;");
    }
    return text;
}

// The machine preferences the lock keeps as they are: everything but the UI mode and
// the audio device (allowed, see above), with the registry's rebound keys.
std::string preferencesFingerprint(const AppPreferences& prefs, const ActionRegistry& registry) {
    std::string text;
    const GeneralPreferences& g = prefs.general;
    text += "folder=" + g.defaultMediaFolder + ";autoLoad=" + std::to_string(g.autoLoadMediaOnStartup) +
            ";recursive=" + std::to_string(g.recursiveFolderScan) +
            ";autoSave=" + std::to_string(g.autoSaveEnabled) +
            ";interval=" + std::to_string(g.autoSaveIntervalMinutes) +
            ";fade=" + std::to_string(g.sceneFadeSeconds);
    const ControlPreferences& c = prefs.control;
    text += ";osc=" + std::to_string(c.oscEnabled) + ":" + std::to_string(c.oscPort) + ":" +
            c.oscBindAddress + ";feedback=" + std::to_string(c.oscFeedbackEnabled) + ":" +
            c.oscFeedbackHost + ":" + std::to_string(c.oscFeedbackPort) + ":" +
            std::to_string(c.oscFeedbackUseIds) + ";midi=";
    for (const std::string& device : c.midiDevices) text += device + ",";
    const auto bindings = [&text](const KeyBindings& keys) {
        for (const auto& [id, chords] : keys) {
            text += id + "=";
            for (const std::string& chord : chords) text += chord + ",";
            text += ";";
        }
    };
    text += ";saved keys:";
    bindings(prefs.keyBindings);
    text += ";registry keys:";
    bindings(keyBindingsOf(registry));
    return text;
}

// Where two fingerprints first differ, with some context, for a failure message.
std::string firstDifference(const std::string& before, const std::string& after) {
    size_t i = 0;
    while (i < before.size() && i < after.size() && before[i] == after[i]) ++i;
    const size_t from = i > 160 ? i - 160 : 0;
    return "before: ..." + before.substr(from, 320) + "...\n after: ..." + after.substr(from, 320) + "...";
}

// What the panels ask of the application. The session's picker requests are counted,
// not opened: under the lock the window that asks is disabled, and a native dialog in a
// test would wait for a person.
class SweepServices : public EditorServices {
public:
    ProjectSession* session = nullptr;
    bool active = true;  // the AI menu as with a sidecar connected (One-Flash items)
    std::vector<AiAction> aiRuns;
    int locateRequests = 0;
    int searchRequests = 0;
    int preferenceSaves = 0;

    bool unsavedLabelShown() override { return session && session->unsavedLabelShown(glfwGetTime()); }
    bool isSearchingMedia() const override { return session && session->isSearchingMedia(); }
    bool isFileDialogBusy() const override { return session && session->isFileDialogBusy(); }
    std::string mediaSearchFolder() const override {
        return session ? session->mediaSearchFolder() : std::string();
    }
    void requestLocateMissingMedia(const MissingMediaItem&) override { ++locateRequests; }
    void requestMissingMediaSearch() override { ++searchRequests; }
    void savePreferences() override { ++preferenceSaves; }
    bool aiActive() const override { return active; }
    bool aiBusy() const override { return false; }
    void runAi(AiAction action) override { aiRuns.push_back(action); }
};

// An item under the mouse somewhere in a window: its id, a point on it, and the
// window's scroll at the time.
struct Hit {
    ImGuiID id = 0;
    ImVec2 pos;
    float scroll = 0.0f;
    bool disabled = false;
};

constexpr const char* MAIN_MENU_BAR = "##MainMenuBar";
constexpr const char* DRAG_SOURCE_WINDOW = "Sweep Drag Source";

// tests/fixtures: the clips the show plays and the pickers answer with.
fs::path fixturesDir() {
    return fs::path(__FILE__).parent_path().parent_path().parent_path() / "fixtures";
}

class PerformLockSweepTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!glfwInit()) GTEST_SKIP() << "no GLFW";
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        window_ = glfwCreateWindow(800, 600, "perform-lock-sweep", nullptr, nullptr);
        glfwDefaultWindowHints();
        if (!window_) {
            glfwTerminate();
            GTEST_SKIP() << "no GL 3.3 window";
        }
        glfwMakeContextCurrent(window_);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
            glfwTerminate();
            GTEST_SKIP() << "no GL";
        }

        imgui_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1600.0f, 1000.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        // As the application sets them.
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

        dir_ = fs::temp_directory_path() / ("qm_lock_sweep_" + UUID::generate().toString());
        fs::create_directories(dir_);
        prefsDialog_.setConfigPath(dir_ / "preferences.json");

        buildShow();
        registerActions();

        ProjectSession::Services services;
        services.sources = &sources_;
        services.outputs = &om_;
        services.library = &library_;
        session_ = std::make_unique<ProjectSession>(project_, es_, prefsDialog_.getPreferences(),
                                                    std::move(services));
        services_.session = session_.get();
        projectFile_ = utf8FromPath(dir_ / "show.qmap");
        project_.setFilePath(projectFile_);
        snprintf(es_.projectPath, sizeof(es_.projectPath), "%s", projectFile_.c_str());
        es_.initialized = true;
        ASSERT_TRUE(session_->ensureCleanBaseline());
        es_.recoveryFilePath.clear();  // never this machine's untitled recovery file
        es_.recoveryProjectPath.clear();

        ctx_.window = window_;
        ctx_.actions = &registry_;
        ctx_.control = &control_;
        ctx_.prefs = &prefsDialog_.getPreferences();
        ctx_.services = &services_;
        ui_ = std::make_unique<EditorUi>(prefsDialog_);
        ui_->attach(ctx_);
        player_ = dynamic_cast<VideoPlayerPanel*>(ui_->panels().find(VideoPlayerPanel::ID));
        ASSERT_NE(player_, nullptr);

        // Before the lock, as an operator would have it: the clip loaded onto the mesh
        // quad and playing (the Video Player then shows its transport), Advanced mode,
        // and the blend test pattern on (locking turns it off).
        es_.quadManager().selectQuad(stage_->getId());
        snprintf(es_.videoPath, sizeof(es_.videoPath), "%s", utf8FromPath(clip()).c_str());
        player_->loadVideo(ctx_, /*simpleMode=*/true);
        es_.quadManager().selectQuad(wall_->getId());
        es_.uiMode = EditorState::UiMode::Advanced;
        om_.setBlendTestPattern(true);

        lockShow();
    }

    void TearDown() override {
        ui_.reset();
        session_.reset();
        if (imgui_) ImGui::DestroyContext(imgui_);
        if (window_) {
            glfwDestroyWindow(window_);
            glfwTerminate();
        }
        std::error_code ec;
        if (!dir_.empty()) fs::remove_all(dir_, ec);
    }

    static fs::path clip() { return fixturesDir() / "videos" / "h264_closed_gop.mp4"; }

    // ---- The representative show ---------------------------------------------------
    // Two scenes; in the first a corner-pin quad with Bezier edges and a procedural
    // source, a mesh-edit quad (its grid moved) that the clip plays on, a quad whose
    // file is missing, a group, and a 3D object; two outputs on it, each sliced in two
    // with blend zones; two MIDI mappings.
    void buildShow() {
        Scene& main = es_.activeScene();
        main.setName("Main");
        QuadManager& quads = main.getQuadManager();
        wall_ = quads.createQuad("Wall");
        wall_->getGeometry().setCorners(
            {Vec2(0.05f, 0.1f), Vec2(0.4f, 0.08f), Vec2(0.42f, 0.5f), Vec2(0.06f, 0.48f)});
        wall_->getGeometry().setBezierEnabled(true);
        wall_->getGeometry().setBezierControl(0, 0, Vec2(0.02f, -0.03f));
        stage_ = quads.createQuad("Stage");
        stage_->getGeometry().setCorners(
            {Vec2(0.5f, 0.1f), Vec2(0.92f, 0.12f), Vec2(0.9f, 0.55f), Vec2(0.52f, 0.5f)});
        stage_->getGeometry().setGridCoords({0.0f, 0.5f, 1.0f}, {0.0f, 0.5f, 1.0f});
        stage_->getGeometry().setMeshMode(MeshMode::MESH_EDIT);
        stage_->getGeometry().setMeshVertex(1, 1, Vec2(0.01f, -0.02f));
        back_ = quads.createQuad("Back");
        back_->getGeometry().setCorners(
            {Vec2(0.2f, 0.6f), Vec2(0.7f, 0.6f), Vec2(0.7f, 0.9f), Vec2(0.2f, 0.9f)});

        auto pattern = std::make_unique<MediaItem>();
        pattern->setType(MediaType::PROCEDURAL);
        pattern->setName("Plasma");
        const UUID plasma = sources_.createProceduralSource(ProceduralType::PLASMA, 256, 256);
        pattern->setSourceId(plasma);
        MediaItem* item = library_.addItem(std::move(pattern));
        mediaItemId_ = item->getId();
        assignMediaToQuad(es_, &sources_, *wall_, *item);

        Project::SourceData missing;
        missing.id = UUID::generate();
        missing.kind = "video";
        missing.path = utf8FromPath(dir_ / "gone" / "h264_closed_gop.mp4");
        missing.missing = true;
        es_.missingSources.emplace(missing.id, missing);
        back_->getTexture().mediaId = missing.id;

        QuadGroup* group = main.createGroup("Walls");
        group->addMember(wall_->getId());
        group->addMember(stage_->getId());

        statue_ = main.createObject("Statue");
        MeshData mesh;
        mesh.positions = {Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f)};
        mesh.uvs = {Vec2(0.0f), Vec2(1.0f, 0.0f), Vec2(0.0f, 1.0f)};
        mesh.indices = {0, 1, 2};
        statue_->setMesh(std::move(mesh));
        statue_->setTransform(glm::translate(glm::mat4(1.0f), glm::vec3(0.2f, 0.1f, -0.5f)));

        Scene* encore = es_.addScene("Encore");
        encore->getQuadManager().createQuad("Encore quad");
        es_.setActiveScene(main.getId());

        es_.outputResolver.setOutputManager(&om_);
        for (const char* name : {"Left", "Right"}) {
            OutputWindow* output = om_.createOutput(name);
            ASSERT_NE(output, nullptr);
            OutputManager::showScene(*output, &main, es_.scenes);
            const IVec2 canvas = main.getCanvasSize();
            Slice left = makeFullCanvasSlice(canvas);
            left.name = "Left half";
            left.input.width = canvas.x * 0.55f;
            left.blend[1].width = 96.0f;
            Slice right = makeFullCanvasSlice(canvas);
            right.id = UUID::generate();
            right.name = "Right half";
            right.input.x = canvas.x * 0.45f;
            right.input.width = canvas.x * 0.55f;
            right.blend[3].width = 96.0f;
            right.blackLevel = 0.02f;
            output->getSlices() = {left, right};
            output->setSlicesCanvas(canvas);
        }

        control::MidiMapping opacity;
        opacity.type = control::MidiControlType::CC;
        opacity.channel = 1;
        opacity.number = 7;
        opacity.mode = control::MidiMode::Value;
        opacity.target = "/qm/quad/" + wall_->getId().toString() + "/opacity";
        control::MidiMapping take;
        take.type = control::MidiControlType::Program;
        take.number = 1;
        take.target = "/qm/scene/take";
        es_.midiMappings = {opacity, take};
    }

    // ---- The app's actions, as Application::registerActions puts them --------------
    void registerActions() {
        const auto put = [this](const std::string& id, std::function<void(EditorContext&)> run) {
            std::optional<Action> action = makeAppAction(id, std::move(run));
            ASSERT_TRUE(action) << id;
            ASSERT_EQ(registry_.add(std::move(*action)), AddResult::Added) << id;
        };
        put(action_id::SAVE, [](EditorContext& c) {
            c.es.pendingFileAction = EditorState::FileAction::Save;
        });
        registerFileActions(registry_);
        registerEditActions(registry_, [this] { session_->invalidateUnsavedLabel(); });
        put(action_id::PREFERENCES, [](EditorContext& c) { c.es.requestPanelOpen(PreferencesDialog::ID); });
        put(action_id::BLACKOUT, [](EditorContext& c) { c.outputManager->toggleBlackout(); });
        put(action_id::PERFORM_TOGGLE, performToggle);
        put(action_id::PERFORM_ESCAPE, performEscape);
        put(action_id::PERFORM_LOCK, togglePerformLock);
        installPerformLockPolicy(registry_, es_);
        // The take as Application::takeSceneByNumber makes it: the OSC/MIDI fade, else
        // the Preferences one.
        for (int n = 1; n <= 9; ++n) {
            put(sceneTakeId(n), [this, n](EditorContext&) {
                takeSceneByNumber(es_, om_, n,
                                  takeQueue_.controlFade.value_or(prefsDialog_.getPreferences().general.sceneFadeSeconds));
            });
        }

        // External control, as Application wires it (ADR-048): the targets on this show,
        // a take queued for the editor frame (runExternalSceneTakes).
        host_.scenes = &es_.scenes;
        host_.sources = &sources_;
        host_.takeScene = [this](int number, std::optional<float> fadeSeconds) {
            takeQueue_.external.push_back({number, fadeSeconds});
        };
        host_.activeSceneNumber = [this] {
            return findSceneIndex(es_.scenes, es_.activeScene().getId()) + 1;
        };
        host_.isBlackout = [this] { return om_.isBlackout(); };
        host_.setBlackout = [this](bool on) { om_.setBlackout(on); };
        host_.refuse = [](const std::string&, const std::string&) {};
        registerControlTargets(control_.registry(), host_);
        refreshControlEntities(control_.registry(), es_.scenes, &sources_);

        // An output window's 1-9 (OutputManager's GLFW key callback) queue takes
        // (Application::queueSceneTake).
        om_.setSceneKeyCallback([this](int number) { takeQueue_.keys.push_back(number); });
    }

    // Lock from the menu, as an operator does, and take the baseline. The frame code runs
    // once before locking (what it does then is the show as locked) and is checked on
    // its first pass under the lock, so a change it makes only while locked is a leak.
    void lockShow() {
        boundary();
        ASSERT_EQ(registry_.invoke(action_id::PERFORM_LOCK, ctx_, Source::Menu), Outcome::Ran);
        ASSERT_TRUE(es_.performLock.isOn());
        ASSERT_FALSE(om_.isBlendTestPattern()) << "locking turns the blend test pattern off";
        rebaseline();
        boundary();
        unchanged("the first frame boundary after locking");
    }

    // ---- Checking -------------------------------------------------------------------
    std::string fingerprint(unsigned allow) {
        return showFingerprint(es_, om_, &sources_, allow);
    }

    // The new baseline after an allowed change was checked.
    void rebaseline() {
        for (unsigned allow : {unsigned(ALLOW_NONE), unsigned(ALLOW_TAKES), unsigned(ALLOW_TAKES | ALLOW_TARGETS)}) {
            baseline_[allow] = fingerprint(allow);
        }
        allowedBaseline_ = allowedValues(es_, om_, &sources_);
        prefsBaseline_ = preferencesFingerprint(prefsDialog_.getPreferences(), registry_);
    }

    // After every step. False (with one failure naming the step) once the show, the
    // preferences or the lock moved: the sweep then stops, so one leak is one failure.
    bool unchanged(const std::string& step, unsigned allow = ALLOW_NONE) {
        if (stopped_) return false;
        ++checks_;
        std::string problem;
        if (!es_.performLock.isOn()) {
            problem = "the lock came off";
        } else if (const std::string now = fingerprint(allow); now != baseline_[allow]) {
            problem = "the show changed\n" + firstDifference(baseline_[allow], now);
        } else if (const std::string prefs = preferencesFingerprint(prefsDialog_.getPreferences(), registry_);
                   prefs != prefsBaseline_) {
            problem = "the preferences changed\n" + firstDifference(prefsBaseline_, prefs);
        } else if (services_.locateRequests || services_.searchRequests) {
            problem = "a Missing Media locate or search was asked for";
        } else if (std::any_of(services_.aiRuns.begin(), services_.aiRuns.end(), [](auto a) {
                       return a != EditorServices::AiAction::ClearPattern;
                   })) {
            problem = "an AI assist action other than Clear Pattern ran";
        } else if (control_.dispatcher().isLearning()) {
            problem = "a MIDI learn was armed";
        }
        if (problem.empty()) {
            // An allowed change (a take, a target) is the new baseline.
            if (allow != ALLOW_NONE && allowedValues(es_, om_, &sources_) != allowedBaseline_) {
                ++allowedChanges_;
                rebaseline();
            }
            return true;
        }
        stopped_ = true;
        ADD_FAILURE() << "Under the perform lock, " << step << ": " << problem;
        return false;
    }

    // The application's frame boundary (Application::runFrameBoundaryTasks), in its
    // order: the session's file actions, OBJ adoption, external control (the queued
    // OSC/MIDI events applied, with the Control panel open so the entities are re-read),
    // the folder search, the source housekeeping, the projector fit; then what render()
    // does before the editor frame: the new-quad upkeep and the failed opens. Save As
    // would open the native Save dialog: it is allowed under the lock, so it is counted,
    // not shown. External control runs on controlNow_, which the sweep keeps still.
    void boundary() {
        if (es_.pendingFileAction == EditorState::FileAction::SaveAs) {
            es_.pendingFileAction = EditorState::FileAction::None;
            ++saveAsRequests_;
        }
        session_->processPendingFileAction();
        adoptObjectMeshes(es_, &om_);
        processExternalControl(control_, es_.scenes, es_.midiMappings, &sources_, controlNow_,
                               /*controlPanelShown=*/true);
        session_->pollMissingMediaSearch();
        releaseUnusedSources(es_, sources_, &library_);
        fitProjectorsToOutputs(es_, om_);
        prepareNewQuads(es_);
        adoptFailedOpens(es_, sources_);
        // Unlocking asks first; the sweep never answers "Unlock".
        if (es_.performLock.isConfirmingUnlock()) cancelPerformUnlock(es_);
    }

    // A MIDI message on the external control queue, as MIDI input pushes it.
    void pushMidi(control::MidiType type, int channel, int number, int value) {
        control::ControlEvent e;
        e.source = control::ControlEvent::Source::Midi;
        e.midi.type = type;
        e.midi.channel = static_cast<uint8_t>(channel);
        e.midi.number = static_cast<uint8_t>(number);
        e.midi.value = static_cast<uint8_t>(value);
        ASSERT_TRUE(control_.queue().push(std::move(e)));
    }

    // Unlock as an operator does (the prompt answered "Unlock").
    void unlockShow() {
        requestPerformUnlock(es_);
        ASSERT_TRUE(confirmPerformUnlock(es_));
    }

    // ---- Headless ImGui input ----------------------------------------------------------
    using Frame = std::function<void()>;

    ImGuiContext& g() { return *ImGui::GetCurrentContext(); }

    void mouseTo(ImVec2 p) { ImGui::GetIO().AddMousePosEvent(p.x, p.y); }
    void mouseAway() { ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX); }

    void button(ImGuiMouseButton b, bool down, const Frame& frame) {
        ImGui::GetIO().AddMouseButtonEvent(b, down);
        frame();
    }

    void click(const Frame& frame, ImGuiMouseButton b = ImGuiMouseButton_Left) {
        button(b, true, frame);
        button(b, false, frame);
    }

    void pressKey(ImGuiKey key, int mods, const Frame& frame) {
        ImGuiIO& io = ImGui::GetIO();
        const auto setMods = [&io, mods](bool down) {
            if (mods & ImGuiMod_Ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, down);
            if (mods & ImGuiMod_Shift) io.AddKeyEvent(ImGuiMod_Shift, down);
            if (mods & ImGuiMod_Alt) io.AddKeyEvent(ImGuiMod_Alt, down);
        };
        setMods(true);
        io.AddKeyEvent(key, true);
        frame();
        io.AddKeyEvent(key, false);
        setMods(false);
        frame();
    }

    // Every item the mouse finds over `rect`, one frame per point.
    // Each hit's point is the middle of the points that found it, clear of its edges.
    std::vector<Hit> scanRect(const ImRect& rect, float scroll, const Frame& frame, float dx, float dy,
                              std::unordered_set<ImGuiID>& seen) {
        std::vector<Hit> hits;
        std::map<ImGuiID, ImRect> extent;
        for (float y = rect.Min.y + 2.0f; y < rect.Max.y - 1.0f; y += dy) {
            for (float x = rect.Min.x + 3.0f; x < rect.Max.x - 1.0f; x += dx) {
                mouseTo(ImVec2(x, y));
                frame();
                const ImGuiID id = g().HoveredId;
                if (id == 0) continue;
                if (seen.insert(id).second) {
                    hits.push_back({id, ImVec2(x, y), scroll, g().HoveredIdIsDisabled});
                    extent[id] = ImRect(x, y, x, y);
                } else if (extent.count(id)) {
                    extent[id].Add(ImVec2(x, y));
                }
            }
        }
        for (Hit& hit : hits) hit.pos = extent[hit.id].GetCenter();
        mouseAway();
        frame();
        return hits;
    }

    // ---- Driving a window's items --------------------------------------------------------
    // One target of the panel sweep: a window (or modal) and how to get it drawn.
    struct Target {
        std::string label;
        std::string window;
        // Between frames, before each interaction: make the window appear as scanned.
        std::function<void()> prepare;
        bool placed = true;  // a panel window the sweep positions and sizes
    };

    // Everything a click, drag or key can do to `target`'s items, and to the popups
    // they open. Stops at the first change.
    void sweepWindow(const Target& target, const Frame& frame, std::vector<ImGuiID> skip = {},
                     std::set<ImGuiKey> skipKeys = {}) {
        if (stopped_) return;
        target.prepare();
        frame();
        frame();
        ImGuiWindow* window = ImGui::FindWindowByName(target.window.c_str());
        if (!window || !window->WasActive) {
            missing_.push_back(target.label);
            return;
        }
        const float visible = window->InnerRect.GetHeight();
        const float scrollMax = window->ScrollMax.y;
        std::unordered_set<ImGuiID> seen;
        std::vector<Hit> hits;
        for (float scroll = 0.0f;; scroll += std::max(40.0f, visible * 0.8f)) {
            scroll_ = std::min(scroll, scrollMax);
            target.prepare();
            frame();
            frame();
            window = ImGui::FindWindowByName(target.window.c_str());
            if (!window) break;
            // The content only: the title bar, scrollbars and resize grips are the
            // window's, not the panel's; and no further down than the content goes.
            ImRect area = window->InnerClipRect;
            area.Max.y = std::min(area.Max.y, window->DC.CursorMaxPos.y + 4.0f);
            const std::vector<Hit> found = scanRect(area, scroll_, frame, 16.0f, 7.0f, seen);
            hits.insert(hits.end(), found.begin(), found.end());
            if (scroll_ >= scrollMax) break;
        }
        scroll_ = 0.0f;
        itemsFound_[target.label] = hits.size();
        size_t enabled = 0;
        for (const Hit& hit : hits) {
            if (std::find(skip.begin(), skip.end(), hit.id) != skip.end()) {
                ++skipped_;
                continue;
            }
            if (!hit.disabled) ++enabled;
            interactWith(target, hit, frame);
            if (stopped_) return;
        }
        enabledFound_[target.label] = enabled;

        // Keys into the focused window.
        static const std::vector<std::pair<ImGuiKey, int>> keys = {
            {ImGuiKey_Delete, 0}, {ImGuiKey_Backspace, 0}, {ImGuiKey_Enter, 0}, {ImGuiKey_Space, 0},
            {ImGuiKey_Escape, 0}, {ImGuiKey_F2, 0}, {ImGuiKey_Tab, 0}, {ImGuiKey_DownArrow, 0},
            {ImGuiKey_UpArrow, 0}, {ImGuiKey_Space, 0}, {ImGuiKey_A, ImGuiMod_Ctrl},
            {ImGuiKey_D, ImGuiMod_Ctrl}, {ImGuiKey_V, ImGuiMod_Ctrl}, {ImGuiKey_Delete, ImGuiMod_Shift},
        };
        for (const auto& [key, mods] : keys) {
            if (skipKeys.count(key)) continue;
            target.prepare();
            focusWindow_ = target.window;
            frame();
            focusWindow_.clear();
            pressKey(key, mods, frame);
            boundary();
            if (!unchanged(target.label + ": key " + std::string(ImGui::GetKeyName(key)))) return;
        }
    }

    // Returns the UI to a resting state: no active item, no popup, mouse away.
    void settle(const Target& target, const Frame& frame) {
        mouseAway();
        resetUi_ = true;
        target.prepare();
        frame();
        frame();
    }

    // Moves onto `hit`, after putting its window back as it was scanned. False when the
    // item is not under the mouse there any more (the layout moved).
    bool reach(const Target& target, const Hit& hit, const Frame& frame) {
        // Twice: a menu bar stays live for a frame after a menu closes, and the first
        // hover may open the menu under it instead.
        for (int attempt = 0; attempt < 2; ++attempt) {
            settle(target, frame);
            scroll_ = hit.scroll;
            target.prepare();
            frame();
            mouseTo(hit.pos);
            frame();
            if (g().HoveredId == hit.id) return true;
        }
        ++unreached_;
        return false;
    }

    void interactWith(const Target& target, const Hit& hit, const Frame& frame) {
        char where[96];
        snprintf(where, sizeof(where), " item 0x%08X at (%.0f, %.0f)%s", hit.id, hit.pos.x, hit.pos.y,
                 hit.disabled ? " (disabled)" : "");
        const std::string step = target.label + where;
        ImGuiIO& io = ImGui::GetIO();

        // A disabled item cannot be activated (ImGui), so one pass of a click, a double
        // click, a right-click and a drag is enough to show the panel does not act on
        // it some other way; one check after them finds anything they did.
        if (hit.disabled) {
            if (!reach(target, hit, frame)) return;
            click(frame);
            click(frame);
            click(frame);
            click(frame, ImGuiMouseButton_Right);
            button(ImGuiMouseButton_Left, true, frame);
            mouseTo(ImVec2(hit.pos.x + 40.0f, hit.pos.y + 4.0f));
            frame();
            button(ImGuiMouseButton_Left, false, frame);
            gestures_ += 4;
            settle(target, frame);
            boundary();
            unchanged(step + ": click, double-click, right-click, drag");
            return;
        }

        // Each gesture, then whatever popup it opened, item by item.
        const std::vector<std::pair<const char*, std::function<void()>>> gestures = {
            {"click", [&] { click(frame); }},
            {"double-click", [&] { click(frame); click(frame); }},
            {"drag right", [&] {
                 button(ImGuiMouseButton_Left, true, frame);
                 mouseTo(ImVec2(hit.pos.x + 30.0f, hit.pos.y)); frame();
                 mouseTo(ImVec2(hit.pos.x + 70.0f, hit.pos.y + 4.0f)); frame();
                 button(ImGuiMouseButton_Left, false, frame);
             }},
            {"drag left", [&] {
                 button(ImGuiMouseButton_Left, true, frame);
                 mouseTo(ImVec2(hit.pos.x - 30.0f, hit.pos.y)); frame();
                 mouseTo(ImVec2(hit.pos.x - 70.0f, hit.pos.y + 30.0f)); frame();
                 button(ImGuiMouseButton_Left, false, frame);
             }},
            {"click and type", [&] {
                 click(frame);
                 if (io.WantTextInput) {
                     io.AddInputCharactersUTF8("7");
                     frame();
                     pressKey(ImGuiKey_Enter, 0, frame);
                 }
             }},
            {"ctrl+click and type", [&] {
                 io.AddKeyEvent(ImGuiMod_Ctrl, true);
                 click(frame);
                 io.AddKeyEvent(ImGuiMod_Ctrl, false);
                 frame();
                 if (io.WantTextInput) {
                     io.AddInputCharactersUTF8("3");
                     frame();
                     pressKey(ImGuiKey_Enter, 0, frame);
                 }
             }},
            {"right-click", [&] { click(frame, ImGuiMouseButton_Right); }},
        };
        // An enabled item is checked after each gesture: a second click could undo the
        // first.
        for (const auto& [name, gesture] : gestures) {
            if (!reach(target, hit, frame)) break;
            const int popupsBefore = g().OpenPopupStack.Size;
            gesture();
            ++gestures_;
            const bool popupOpened = g().OpenPopupStack.Size > popupsBefore;
            ImGuiWindow* popup = popupOpened ? g().OpenPopupStack.back().Window : nullptr;
            const std::string popupName = popup ? popup->Name : "";
            boundary();
            if (!unchanged(step + ": " + name)) return;
            if (popupName.empty()) continue;

            // The popup's items, each reached again through the same gesture.
            std::unordered_set<ImGuiID> seen;
            ImGuiWindow* open = ImGui::FindWindowByName(popupName.c_str());
            if (!open) continue;
            const std::vector<Hit> items = scanRect(open->Rect(), 0.0f, frame, 12.0f, 5.0f, seen);
            for (const Hit& item : items) {
                if (!reach(target, hit, frame)) break;
                gesture();
                mouseTo(item.pos);
                frame();
                click(frame);
                ++gestures_;
                boundary();
                if (!unchanged(step + ": " + name + ", then popup item 0x" + std::to_string(item.id))) return;
            }
        }
        settle(target, frame);
        boundary();
        unchanged(step + ": after all its gestures");
    }

    // ---- The frames ---------------------------------------------------------------------
    // The panels alone (PanelHost), the window being swept placed where it can be read.
    void panelFrame(const Target& target) {
        ImGui::NewFrame();
        applyFrameRequests(target);
        ui_->panels().drawWindows(ctx_);
        ui_->panels().drawOverlays(ctx_);
        ImGui::Render();
    }

    void applyFrameRequests(const Target& target) {
        if (resetUi_) {
            resetUi_ = false;
            ImGui::ClearActiveID();
            ImGui::ClosePopupsExceptModals();
        }
        if (target.placed && ImGui::FindWindowByName(target.window.c_str())) {
            ImGui::SetWindowPos(target.window.c_str(), ImVec2(40.0f, 30.0f));
            ImGui::SetWindowSize(target.window.c_str(), ImVec2(520.0f, 940.0f));
            ImGui::SetWindowCollapsed(target.window.c_str(), false);
            if (ImGuiWindow* window = ImGui::FindWindowByName(target.window.c_str())) {
                ImGui::SetScrollY(window, scroll_);
            }
        }
        if (!focusWindow_.empty()) ImGui::SetWindowFocus(focusWindow_.c_str());
    }

    // The whole editor frame, as Application runs it, with the scene takes queued in
    // takeQueue_ (Application::runSceneTakes) and, while dragSource_ is set, a window
    // that holds a drag.
    void uiFrame() {
        ImGui::NewFrame();
        if (resetUi_) {
            resetUi_ = false;
            ImGui::ClearActiveID();
            ImGui::ClosePopupsExceptModals();
        }
        if (dragSource_) dragSource_();
        ui_->frame(ctx_, [this](EditorContext& c) {
            runSceneTakeKeys(registry_, c, takeQueue_.keys);
            runExternalSceneTakes(registry_, c, takeQueue_);
        });
        ImGui::Render();
    }

    // Shows only `panel` (and the overlays) in the panel sweep.
    void showOnly(const std::string& panel) {
        for (Panel* p : ui_->panels().panels()) {
            if (p->kind() == PanelKind::Window) p->visible = panel == p->id();
        }
    }

    void selectQuad(Quad* quad) {
        es_.activeScene().getQuadManager().deselectAll();
        es_.quadManager().selectQuad(quad->getId());
        es_.currentSelectionType = EditorState::SelectionType::Quad;
    }

    // ---- State ----------------------------------------------------------------------------
    GLFWwindow* window_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    fs::path dir_;
    std::string projectFile_;

    EditorState es_;
    OutputManager om_;
    SourceManager sources_;
    MediaLibrary library_;
    Project project_;
    ActionRegistry registry_;
    control::ExternalControl control_;
    ControlBindingHost host_;
    PreferencesDialog prefsDialog_;
    SweepServices services_;
    EditorContext ctx_{es_, &project_, &library_, &sources_, &om_, nullptr, nullptr};
    std::unique_ptr<ProjectSession> session_;
    std::unique_ptr<EditorUi> ui_;
    VideoPlayerPanel* player_ = nullptr;

    Quad* wall_ = nullptr;
    Quad* stage_ = nullptr;
    Quad* back_ = nullptr;
    MeshObject* statue_ = nullptr;
    UUID mediaItemId_;

    std::map<unsigned, std::string> baseline_;
    std::string allowedBaseline_;
    std::string prefsBaseline_;
    bool stopped_ = false;
    bool resetUi_ = false;
    float scroll_ = 0.0f;
    std::string focusWindow_;
    SceneTakeQueue takeQueue_;
    double controlNow_ = 100.0;  // external control's clock (seconds)
    std::function<void()> dragSource_;

    // Counts for the report.
    size_t checks_ = 0;
    size_t gestures_ = 0;
    size_t skipped_ = 0;
    size_t unreached_ = 0;
    size_t allowedChanges_ = 0;  // checks after which an allowed value had changed
    int saveAsRequests_ = 0;
    std::map<std::string, size_t> itemsFound_;
    std::map<std::string, size_t> enabledFound_;
    std::vector<std::string> missing_;

    void report(const char* what) {
        std::printf("[sweep] %s: %zu checks, %zu gestures, %zu items skipped, %zu not reached again\n",
                    what, checks_, gestures_, skipped_, unreached_);
        for (const auto& [label, count] : itemsFound_) {
            if (enabledFound_.count(label)) {
                std::printf("[sweep]   %-48s %3zu items, %3zu enabled\n", label.c_str(), count, enabledFound_[label]);
            } else {
                std::printf("[sweep]   %-48s %3zu items\n", label.c_str(), count);
            }
        }
        for (const std::string& label : missing_) std::printf("[sweep]   not drawn: %s\n", label.c_str());
    }
};

}  // namespace

// ---- Registered actions ---------------------------------------------------------------

// Every action in the registry, from the menu, a key and external control, in several
// selections: the Edit class and New / Open are refused; the Show and View classes and
// Save / Save As run, and change only what the list above allows.
TEST_F(PerformLockSweepTest, RegisteredActionsChangeOnlyWhatTheLockKeeps) {
    const std::set<std::string> allowed = {
        action_id::SAVE, action_id::SAVE_AS, action_id::PREFERENCES, action_id::BLACKOUT,
        action_id::PERFORM_TOGGLE, action_id::PERFORM_ESCAPE, action_id::PERFORM_LOCK,
    };
    std::vector<std::string> ids;
    for (const Action& action : registry_.actions()) ids.push_back(action.id);

    const std::vector<std::pair<const char*, std::function<void()>>> selections = {
        {"corner-pin quad", [this] { selectQuad(wall_); }},
        {"mesh-edit quad", [this] { selectQuad(stage_); }},
        {"every quad", [this] { es_.quadManager().selectAll(); }},
        {"3D object", [this] {
             es_.activeScene().getQuadManager().deselectAll();
             es_.selectedObjectId = statue_->getId();
             es_.currentSelectionType = EditorState::SelectionType::Object;
         }},
    };
    size_t invoked = 0;
    for (const auto& [selection, select] : selections) {
        for (const std::string& id : ids) {
            for (Source source : {Source::Menu, Source::Key, Source::Control}) {
                select();
                const Action* action = registry_.find(id);
                ASSERT_NE(action, nullptr);
                const Outcome outcome = registry_.invoke(id, ctx_, source);
                ++invoked;
                boundary();
                const bool isTake = id.rfind("scene.take.", 0) == 0;
                const std::string step = "action " + id + " (" + selection + ")";
                if (allowed.count(id) || isTake) {
                    if (!unchanged(step, ALLOW_TAKES)) return;
                    continue;
                }
                EXPECT_NE(outcome, Outcome::Ran) << step << " ran under the lock";
                if (!unchanged(step)) return;
            }
        }
    }

    // F11 with no outputs would create "Projector 1" (outputs are show content).
    OutputManager none;
    ctx_.outputManager = &none;
    EXPECT_EQ(registry_.invoke(action_id::PERFORM_TOGGLE, ctx_, Source::Key), Outcome::Ran);
    EXPECT_EQ(none.getOutputCount(), 0u) << "F11 created an output under the lock";
    ctx_.outputManager = &om_;
    ++invoked;
    std::printf("[sweep] registry: %zu actions x 3 sources x %zu selections = %zu invocations, "
                "%d Save As requests\n",
                ids.size(), selections.size(), invoked, saveAsRequests_);
    report("registry");
}

// ---- Panels -----------------------------------------------------------------------------

// Every panel window, every Resources tab, the Missing Media and Preferences windows,
// the Properties panel in each selection and mode, and the session modals.
TEST_F(PerformLockSweepTest, PanelWidgetsChangeNothing) {
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_DockingEnable;  // each window where it is put
    PanelHost& host = ui_->panels();
    Panel* resources = host.find(ResourcesPanel::ID);
    ASSERT_NE(resources, nullptr);
    es_.requestPanelOpen(EditorState::MISSING_MEDIA_PANEL_ID);

    std::vector<Target> targets;
    const auto windowTarget = [&](const std::string& label, const std::string& id, EditorState::UiMode mode,
                                  std::function<void()> select) {
        Target t;
        t.label = label;
        t.window = id;
        t.prepare = [this, id, mode, select] {
            es_.uiMode = mode;
            showOnly(id);
            if (select) select();
        };
        targets.push_back(std::move(t));
    };
    using Mode = EditorState::UiMode;
    const auto quadSel = [this] { selectQuad(wall_); };
    for (Mode mode : {Mode::Advanced, Mode::Simple}) {
        const std::string m = mode == Mode::Advanced ? " (Advanced)" : " (Simple)";
        for (Panel* panel : host.panels()) {
            if (panel->kind() != PanelKind::Window || panel == resources) continue;
            windowTarget(std::string(panel->id()) + m, panel->id(), mode, quadSel);
        }
        // The Properties panel follows the selection.
        windowTarget(std::string("Properties, mesh-edit quad") + m, "Properties", mode,
                     [this] { selectQuad(stage_); });
        windowTarget(std::string("Properties, quad with missing media") + m, "Properties", mode,
                     [this] { selectQuad(back_); });
        windowTarget(std::string("Properties, 3D object") + m, "Properties", mode, [this] {
            es_.activeScene().getQuadManager().deselectAll();
            es_.selectedObjectId = statue_->getId();
            es_.currentSelectionType = EditorState::SelectionType::Object;
        });
        windowTarget(std::string("Properties, output") + m, "Properties", mode, [this] {
            es_.selectedOutputId = om_.getOutputs().front()->getId();
            es_.selectedSliceId = om_.getOutputs().front()->getSlices().front().id;
            es_.currentSelectionType = EditorState::SelectionType::Output;
        });
        windowTarget(std::string("Properties, media") + m, "Properties", mode, [this] {
            es_.selectedMediaId = mediaItemId_;
            es_.currentSelectionType = EditorState::SelectionType::Media;
        });
        // Each tab of the Resources window.
        for (Panel* panel : host.panels()) {
            if (panel->kind() != PanelKind::Tab) continue;
            es_.uiMode = mode;
            if (!panel->available(ctx_)) continue;  // not drawn in this mode
            const std::string tab = panel->id();
            Target t;
            t.label = "Resources > " + tab + m;
            t.window = ResourcesPanel::ID;
            t.prepare = [this, mode, tab, quadSel] {
                es_.uiMode = mode;
                showOnly(ResourcesPanel::ID);
                quadSel();
                ImGuiWindow* window = ImGui::FindWindowByName(ResourcesPanel::ID);
                if (!window) return;
                if (ImGuiTabBar* tabs = g().TabBars.GetByKey(ImHashStr("ResourcesTabs", 0, window->ID))) {
                    tabs->NextSelectedTabId = ImHashStr(tab.c_str(), 0, tabs->ID);
                }
            };
            targets.push_back(std::move(t));
        }
    }

    for (const Target& target : targets) {
        const Frame frame = [this, &target] { panelFrame(target); };
        sweepWindow(target, frame);
        if (stopped_) break;
    }
    report("panels");
}

// The session modals the lock leaves live (SessionDialogs, editsShow false): an
// Unsaved Changes prompt opened before locking, for New and for Load, answered after;
// the recovery prompt; the unlock prompt (its Unlock button is the way out, left alone).
TEST_F(PerformLockSweepTest, SessionModalsChangeNothing) {
    ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_DockingEnable;
    // A recovery file and another show to load, each different from this show.
    Project other;
    syncEditorToProject(es_, &om_, &sources_, other);
    other.getScenes().front().quads.pop_back();
    other.getScenes().front().texturePaths.pop_back();
    const std::string otherFile = utf8FromPath(dir_ / "other.qmap");
    ASSERT_TRUE(ProjectSerializer::save(other, pathFromUtf8(otherFile)));
    const std::string recovery = utf8FromPath(dir_ / "show.autosave.qmap");
    ASSERT_TRUE(ProjectSerializer::save(other, pathFromUtf8(recovery)));

    using FileAction = EditorState::FileAction;
    // One modal at a time: the others are answered away first.
    const auto modal = [&](const std::string& label, const std::string& window, std::function<void()> open) {
        Target t;
        t.label = label;
        t.window = window;
        t.placed = false;
        t.prepare = [this, open, window] {
            showOnly("");
            if (window != "Unsaved Changes##session") es_.unsavedPromptFor = FileAction::None;
            if (window != "Recover Unsaved Work##session") es_.recoveryFilePath.clear();
            if (window != "Unlock the Show?##performlock") cancelPerformUnlock(es_);
            open();
        };
        return t;
    };
    const std::vector<Target> targets = {
        modal("Unsaved Changes for New", "Unsaved Changes##session",
              [this] { if (es_.unsavedPromptFor == FileAction::None) es_.unsavedPromptFor = FileAction::New; }),
        modal("Unsaved Changes for Load", "Unsaved Changes##session", [this, otherFile] {
            if (es_.unsavedPromptFor != FileAction::None) return;
            es_.unsavedPromptFor = FileAction::Load;
            es_.pendingLoadPath = otherFile;
        }),
        modal("Recover Unsaved Work", "Recover Unsaved Work##session", [this, recovery] {
            if (!es_.recoveryFilePath.empty()) return;
            es_.recoveryFilePath = recovery;
            es_.recoveryProjectPath = projectFile_;
        }),
        modal("Unlock the Show?", "Unlock the Show?##performlock",
              [this] { requestPerformUnlock(es_); }),
    };
    for (const Target& target : targets) {
        const Frame frame = [this, &target] { panelFrame(target); };
        std::vector<ImGuiID> skip;
        std::set<ImGuiKey> skipKeys;
        if (target.window == "Unlock the Show?##performlock") {
            // Unlock, by click or by Enter / Space once the arrows or Tab have moved
            // keyboard focus onto it (it starts on Stay Locked), is the confirmation
            // the lock asks for.
            skip.push_back(ImHashStr("Unlock", 0, ImHashStr(target.window.c_str())));
            skipKeys = {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space};
        }
        sweepWindow(target, frame, skip, skipKeys);
        if (stopped_) break;
    }
    EXPECT_GE(skipped_, 1u) << "the Unlock button was not found: the skip list is stale";
    report("session modals");
}

// ---- Menu bar ---------------------------------------------------------------------------

// Every menu, every item in it (and in its submenus), and every button and control on
// the bar, through the whole editor frame. Report Bug opens a browser and is skipped.
TEST_F(PerformLockSweepTest, MenuBarChangesOnlyWhatTheLockKeeps) {
    showOnly("");
    const Frame frame = [this] { uiFrame(); };
    frame();
    frame();
    ImGuiWindow* bar = ImGui::FindWindowByName(MAIN_MENU_BAR);
    ASSERT_NE(bar, nullptr);
    const ImGuiID reportBug = ImHashStr("Report Bug", 0, ImHashStr("##MenuBar", 0, bar->ID));

    // Advanced with the AI sidecar connected (the One-Flash menu), and Simple, whose bar
    // has Apply Test Grid and Send to Projector, with it not running.
    using Mode = EditorState::UiMode;
    for (const auto& [mode, aiActive] : {std::pair<Mode, bool>{Mode::Advanced, true},
                                         std::pair<Mode, bool>{Mode::Simple, false}}) {
        services_.active = aiActive;
        Target target;
        target.label = std::string("menu bar, ") + (mode == Mode::Simple ? "Simple" : "Advanced") +
                       ", AI " + (aiActive ? "connected" : "not running");
        target.window = MAIN_MENU_BAR;
        target.placed = false;
        target.prepare = [this, mode = mode] {
            es_.uiMode = mode;
            showOnly("");
            // File > Exit asks about the unsaved changes (allowed; the prompt is swept in
            // SessionModalsChangeNothing): answered Cancel, so the bar stays reachable.
            es_.unsavedPromptFor = EditorState::FileAction::None;
        };
        target.prepare();
        frame();
        frame();
        // The bar's items; then each menu's items, reached by opening the menu each time.
        std::unordered_set<ImGuiID> seen;
        const std::vector<Hit> hits = scanRect(bar->Rect(), 0.0f, frame, 6.0f, 4.0f, seen);
        itemsFound_[target.label] = hits.size();
        size_t found = 0;
        for (const Hit& hit : hits) {
            if (hit.id == reportBug) {
                ++skipped_;
                continue;
            }
            ++found;
            // Open the menu (or press the button) ...
            if (!reach(target, hit, frame)) continue;
            const int popupsBefore = g().OpenPopupStack.Size;
            click(frame);
            ++gestures_;
            boundary();
            if (!unchanged(target.label + ": bar item 0x" + std::to_string(hit.id), ALLOW_TAKES)) return;
            if (g().OpenPopupStack.Size <= popupsBefore) {
                // A slider or a button: drag it too.
                if (!reach(target, hit, frame)) continue;
                button(ImGuiMouseButton_Left, true, frame);
                mouseTo(ImVec2(hit.pos.x + 40.0f, hit.pos.y));
                frame();
                button(ImGuiMouseButton_Left, false, frame);
                boundary();
                if (!unchanged(target.label + ": bar item drag 0x" + std::to_string(hit.id), ALLOW_TAKES)) return;
                continue;
            }
            // ... then each item of the menu, and of each submenu it opens.
            const std::string menuName = g().OpenPopupStack.back().Window->Name;
            ImGuiWindow* menu = ImGui::FindWindowByName(menuName.c_str());
            std::unordered_set<ImGuiID> menuSeen;
            const std::vector<Hit> items = scanRect(menu->Rect(), 0.0f, frame, 10.0f, 4.0f, menuSeen);
            itemsFound_[target.label + " > " + menuName.substr(0, menuName.find("###"))] = items.size();
            for (const Hit& item : items) {
                int depth = 0;  // popups open with only the menu up
                const auto openMenu = [&] {
                    if (!reach(target, hit, frame)) return false;
                    click(frame);
                    depth = g().OpenPopupStack.Size;
                    mouseTo(item.pos);
                    for (int i = 0; i < 8; ++i) frame();  // a submenu opens after a short hover
                    return true;
                };
                if (!openMenu()) continue;
                click(frame);
                ++gestures_;
                boundary();
                if (!unchanged(target.label + ": " + menuName + " item 0x" + std::to_string(item.id), ALLOW_TAKES)) {
                    return;
                }
                // A submenu opens on hover: its items too.
                if (!openMenu() || g().OpenPopupStack.Size <= depth) continue;
                const std::string subName = g().OpenPopupStack.back().Window->Name;
                if (subName == menuName) continue;
                ImGuiWindow* sub = ImGui::FindWindowByName(subName.c_str());
                std::unordered_set<ImGuiID> subSeen;
                const std::vector<Hit> subItems = scanRect(sub->Rect(), 0.0f, frame, 10.0f, 4.0f, subSeen);
                itemsFound_[target.label + " > " + subName.substr(0, subName.find("###"))] = subItems.size();
                for (const Hit& subItem : subItems) {
                    if (!openMenu()) break;
                    mouseTo(ImVec2(sub->Pos.x + 4.0f, item.pos.y));  // into the submenu
                    frame();
                    mouseTo(subItem.pos);
                    frame();
                    frame();
                    click(frame);
                    ++gestures_;
                    boundary();
                    if (!unchanged(target.label + ": " + subName + " item 0x" + std::to_string(subItem.id),
                                   ALLOW_TAKES)) {
                        return;
                    }
                }
            }
            settle(target, frame);
        }
        enabledFound_[target.label] = found;
    }
    EXPECT_GE(skipped_, 1u) << "Report Bug was not found: the skip id is stale";
    report("menu bar");
}

// ---- Keys ---------------------------------------------------------------------------------

// Every key, alone and with Ctrl, Shift, Alt, Ctrl+Shift and Ctrl+Alt, through the
// editor frame (the shortcut router, the canvas), and the scene take keys 1-9 as the
// application queues them. perform.lock's own chords are left out: they only ask to
// unlock (the prompt is checked in SessionModalsChangeNothing).
TEST_F(PerformLockSweepTest, KeyboardChangesOnlyWhatTheLockKeeps) {
    showOnly("");
    const Frame frame = [this] { uiFrame(); };
    frame();
    const Action* lock = registry_.find(action_id::PERFORM_LOCK);
    ASSERT_NE(lock, nullptr);
    const std::vector<int> modifiers = {0, ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt,
                                        ImGuiMod_Ctrl | ImGuiMod_Shift, ImGuiMod_Ctrl | ImGuiMod_Alt};
    size_t presses = 0;
    for (Quad* selected : {wall_, stage_}) {
        selectQuad(selected);
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; ++k) {
            const ImGuiKey key = static_cast<ImGuiKey>(k);
            if (key >= ImGuiKey_MouseLeft) break;
            for (int mods : modifiers) {
                uint8_t chordMods = 0;
                if (mods & ImGuiMod_Ctrl) chordMods |= MOD_CTRL;
                if (mods & ImGuiMod_Shift) chordMods |= MOD_SHIFT;
                if (mods & ImGuiMod_Alt) chordMods |= MOD_ALT;
                const Chord chord{k, chordMods};
                if (std::find(lock->chords.begin(), lock->chords.end(), chord) != lock->chords.end()) continue;
                pressKey(key, mods, frame);
                ++presses;
                boundary();
            }
            if (!unchanged(std::string("key ") + chordText(Chord{k, 0}) + " with each modifier", ALLOW_TAKES)) {
                return;
            }
        }
    }
    for (int n = 1; n <= 9; ++n) {
        takeQueue_.keys.push_back(n);
        frame();
        boundary();
        if (!unchanged("scene take key " + std::to_string(n), ALLOW_TAKES)) return;
    }
    std::printf("[sweep] keys: %zu presses, 9 scene take keys\n", presses);
    report("keys");
}

// ---- Canvas -------------------------------------------------------------------------------

// The canvas (ViewportEditor, driven with a Pointer) with each quad selected: a press
// and a drag on every corner, mesh vertex and grid handle and over a grid of points,
// and the canvas keys (Tab, the arrows with Shift and Alt, H) at each. The canvas
// never claims its keys under the lock (ADR-051 §4, "no canvas key owner").
TEST_F(PerformLockSweepTest, CanvasMouseAndKeysChangeNothing) {
    ViewportEditor viewport;
    ViewportEditor::Pointer pointer;
    const ImGuiID canvasOwner = ImHashStr("QUADmapper.CanvasEditingKeys");
    bool claimed = false;
    const Frame frame = [&] {
        es_.canvasRegionPos = Vec2(0.0f, 0.0f);
        es_.canvasRegionSize = Vec2(800.0f, 600.0f);
        ImGui::NewFrame();
        claimCanvasEditingKeys(canvasKeyOwner(es_, &om_) != CanvasKeyOwner::None);
        viewport.render(ctx_, pointer);
        ImGui::Render();
        for (ImGuiKey key : {ImGuiKey_Tab, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow,
                             ImGuiKey_RightArrow, ImGuiKey_H}) {
            if (ImGui::GetKeyOwner(key) == canvasOwner) claimed = true;
        }
    };
    frame();
    const auto toWindow = [this](Vec2 c) {
        return Vec2(es_.canvasRegionPos.x + (c.x * es_.lastViewScale.x + es_.lastViewOffset.x) * es_.canvasRegionSize.x,
                    es_.canvasRegionPos.y + (c.y * es_.lastViewScale.y + es_.lastViewOffset.y) * es_.canvasRegionSize.y);
    };
    size_t drags = 0;
    for (Quad* selected : {wall_, stage_, back_}) {
        selectQuad(selected);
        frame();
        std::vector<Vec2> points;
        const QuadGeometry& geometry = selected->getGeometry();
        for (const Vec2& c : geometry.getCorners()) points.push_back(c);
        if (geometry.getMeshMode() == MeshMode::MESH_EDIT) {
            const std::vector<Vec2> mesh = geometry.generateMeshPositions();
            for (size_t i = 0; i < mesh.size(); ++i) {
                points.push_back(mesh[i]);
                const unsigned handles = geometry.availableHandles(static_cast<int>(i));
                for (int h = 0; h < GRID_HANDLE_COUNT; ++h) {
                    if ((handles >> h) & 1u) points.push_back(geometry.gridHandlePosition(static_cast<int>(i), h));
                }
            }
        }
        // Over the whole canvas too: picking another quad, the canvas body, off the canvas.
        for (float y = -0.05f; y <= 1.05f; y += 0.1f) {
            for (float x = -0.05f; x <= 1.05f; x += 0.1f) points.push_back(Vec2(x, y));
        }
        for (const Vec2& point : points) {
            const Vec2 at = toWindow(point);
            pointer = {at, false};
            frame();
            pointer.leftDown = true;
            frame();
            pointer.position = toWindow(point + Vec2(0.02f, 0.015f));
            frame();
            pointer.position = toWindow(point + Vec2(0.05f, 0.03f));
            frame();
            pointer.leftDown = false;
            frame();
            ++drags;
            if (!unchanged("canvas drag at (" + std::to_string(point.x) + ", " + std::to_string(point.y) + ")")) {
                return;
            }
        }
        for (int mods : {0, int(ImGuiMod_Shift), int(ImGuiMod_Alt)}) {
            for (ImGuiKey key : {ImGuiKey_Tab, ImGuiKey_RightArrow, ImGuiKey_LeftArrow, ImGuiKey_UpArrow,
                                 ImGuiKey_DownArrow, ImGuiKey_H}) {
                pressKey(key, mods, frame);
                pressKey(key, mods, frame);
                if (!unchanged(std::string("canvas key ") + ImGui::GetKeyName(key))) return;
            }
        }
    }
    EXPECT_FALSE(claimed) << "the canvas claimed its editing keys under the lock";
    EXPECT_EQ(es_.cornerSelectedVertex, -1) << "a vertex was selected under the lock";
    std::printf("[sweep] canvas: %zu presses and drags, 54 key presses\n", drags);
    report("canvas");
}

// ---- Drops ----------------------------------------------------------------------------------

// A drag held when the lock was taken (a bin clip, a quad row, a scene row) dropped on
// the canvas and on every item of the panels that take drops, through the editor frame.
TEST_F(PerformLockSweepTest, DropsChangeNothing) {
    struct Payload {
        const char* type;
        std::vector<unsigned char> data;
    };
    const UUID quadId = back_->getId();
    const int sceneIndex = 1;
    std::vector<Payload> payloads = {
        {"MEDIA_ITEM_ID", std::vector<unsigned char>(reinterpret_cast<const unsigned char*>(&mediaItemId_),
                                                     reinterpret_cast<const unsigned char*>(&mediaItemId_) + sizeof(UUID))},
        {"QUAD_DND", std::vector<unsigned char>(reinterpret_cast<const unsigned char*>(&quadId),
                                                reinterpret_cast<const unsigned char*>(&quadId) + sizeof(UUID))},
        {"SCENE_INDEX", std::vector<unsigned char>(reinterpret_cast<const unsigned char*>(&sceneIndex),
                                                   reinterpret_cast<const unsigned char*>(&sceneIndex) + sizeof(int))},
    };
    const Payload* held = nullptr;
    ImVec2 sourceButton;
    dragSource_ = [&] {
        ImGui::SetNextWindowPos(ImVec2(700.0f, 40.0f));
        ImGui::SetNextWindowSize(ImVec2(120.0f, 60.0f));
        ImGui::Begin(DRAG_SOURCE_WINDOW, nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
        ImGui::Button("Held");
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        sourceButton = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        if (held && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(held->type, held->data.data(), held->data.size());
            ImGui::EndDragDropSource();
        }
        ImGui::End();
    };
    const Frame frame = [this] { uiFrame(); };
    size_t drops = 0;
    const auto dropAt = [&](ImVec2 target, const std::string& step) {
        mouseTo(sourceButton);
        frame();
        button(ImGuiMouseButton_Left, true, frame);
        mouseTo(ImVec2(sourceButton.x + 30.0f, sourceButton.y + 30.0f));
        frame();
        mouseTo(target);
        frame();
        frame();
        button(ImGuiMouseButton_Left, false, frame);
        mouseAway();
        frame();
        ++drops;
        boundary();
        return unchanged(step);
    };

    // The default dock layout (Quad List, Properties, Resources) over the canvas.
    for (Panel* p : ui_->panels().panels()) {
        if (p->kind() == PanelKind::Window) {
            const std::string id = p->id();
            p->visible = id == "Quad List" || id == "Properties" || id == ResourcesPanel::ID;
        }
    }
    selectQuad(wall_);
    frame();
    frame();
    ASSERT_GT(es_.canvasRegionSize.x, 1.0f);

    // The panels' items, found once.
    std::vector<std::pair<std::string, Hit>> targets;
    for (const char* name : {"Quad List", "Properties", ResourcesPanel::ID}) {
        ImGuiWindow* window = ImGui::FindWindowByName(name);
        if (!window || !window->WasActive) {
            missing_.push_back(name);
            continue;
        }
        std::unordered_set<ImGuiID> seen;
        for (const Hit& hit : scanRect(window->Rect(), 0.0f, frame, 20.0f, 6.0f, seen)) {
            targets.push_back({name, hit});
        }
        itemsFound_[std::string("drop targets in ") + name] = seen.size();
    }
    // The canvas: each quad's centre.
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    for (Quad* quad : {wall_, stage_, back_}) {
        Vec2 centre(0.0f);
        for (const Vec2& c : quad->getGeometry().getCorners()) centre += c * 0.25f;
        Hit hit;
        hit.pos = ImVec2(
            origin.x + es_.canvasRegionPos.x + (centre.x * es_.lastViewScale.x + es_.lastViewOffset.x) * es_.canvasRegionSize.x,
            origin.y + es_.canvasRegionPos.y + (centre.y * es_.lastViewScale.y + es_.lastViewOffset.y) * es_.canvasRegionSize.y);
        targets.push_back({"canvas, " + quad->getName(), hit});
    }
    for (const Payload& payload : payloads) {
        held = &payload;
        for (const auto& [where, hit] : targets) {
            char step[160];
            snprintf(step, sizeof(step), "%s dropped on %s at (%.0f, %.0f)", payload.type, where.c_str(),
                     hit.pos.x, hit.pos.y);
            if (!dropAt(hit.pos, step)) return;
        }
    }

    // Each other tab of the Resources window (the Scenes rows take scene drops).
    for (Panel* panel : ui_->panels().panels()) {
        if (panel->kind() != PanelKind::Tab || !panel->available(ctx_)) continue;
        const std::string tab = panel->id();
        const auto selectTab = [this, &tab] {
            ImGuiWindow* window = ImGui::FindWindowByName(ResourcesPanel::ID);
            if (!window) return;
            if (ImGuiTabBar* tabs = g().TabBars.GetByKey(ImHashStr("ResourcesTabs", 0, window->ID))) {
                tabs->NextSelectedTabId = ImHashStr(tab.c_str(), 0, tabs->ID);
            }
        };
        held = nullptr;
        selectTab();
        frame();
        frame();
        ImGuiWindow* window = ImGui::FindWindowByName(ResourcesPanel::ID);
        ASSERT_NE(window, nullptr);
        std::unordered_set<ImGuiID> seen;
        const std::vector<Hit> items = scanRect(window->InnerClipRect, 0.0f, frame, 20.0f, 6.0f, seen);
        itemsFound_["drop targets in Resources > " + tab] = items.size();
        for (const Payload& payload : payloads) {
            held = &payload;
            for (const Hit& item : items) {
                selectTab();
                if (!dropAt(item.pos, std::string(payload.type) + " dropped on Resources > " + tab)) return;
            }
        }
    }
    dragSource_ = nullptr;
    std::printf("[sweep] drops: %zu drops of %zu payload types\n", drops, payloads.size());
    report("drops");
}

// ---- Pickers and background work answered after locking ----------------------------------

// The file dialogs have no owner, so one opened before locking can be answered after
// it: each answer is refused. An OBJ import begun before locking that finishes after it
// keeps its object (ADR-051 E7) and moves no projector.
TEST_F(PerformLockSweepTest, PickersAnsweredAfterLockingChangeNothing) {
    // A small OBJ to import or relink to.
    const fs::path obj = dir_ / "box.obj";
    {
        std::ofstream out(obj);
        out << "v 0 0 0\nv 2 0 0\nv 0 2 0\nv 0 0 2\nf 1 2 3\nf 1 3 4\nf 1 2 4\n";
    }
    session_->objectFileChosen(utf8FromPath(obj));
    boundary();
    if (!unchanged("Import 3D Object answered after locking")) return;

    const std::vector<MissingMediaItem> missing = collectMissingMedia(es_);
    ASSERT_FALSE(missing.empty());
    session_->locateFileChosen(missing.front(), utf8FromPath(clip()));
    boundary();
    if (!unchanged("Relocate answered after locking")) return;

    session_->searchFolderChosen(utf8FromPath(fixturesDir() / "videos"));
    EXPECT_FALSE(session_->isSearchingMedia()) << "a folder search started under the lock";
    boundary();
    if (!unchanged("Search Folder answered after locking")) return;

    for (const bool simple : {true, false}) {
        snprintf(es_.videoPath, sizeof(es_.videoPath), "%s",
                 utf8FromPath(fixturesDir() / "videos" / "hevc_closed_gop.mp4").c_str());
        player_->loadVideo(ctx_, simple);
        boundary();
        if (!unchanged(std::string("Video Player load answered after locking") + (simple ? " (Simple)" : ""))) {
            return;
        }
    }

    prefsDialog_.mediaFolderChosen(ctx_, utf8FromPath(dir_));
    if (!unchanged("Preferences media folder answered after locking")) return;

    // An import begun before locking: unlock, import, lock again; then its parse lands.
    unlockShow();
    importObjectFile(es_, utf8FromPath(obj));
    lockShow();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!es_.meshLoads.tickets.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        boundary();
    }
    ASSERT_TRUE(es_.meshLoads.tickets.empty()) << "the OBJ did not parse";
    if (!unchanged("an OBJ import finishing under the lock")) return;
    report("pickers");
}

// ---- External control ------------------------------------------------------------------------

// Every OSC target (globals and every entity's actions, at 0, 0.5 and 1) and the show's
// MIDI mappings, each event queued as the OSC and MIDI threads queue it and applied at
// the frame boundary (processExternalControl), its take made in the editor frame
// (runExternalSceneTakes): they act, as the lock keeps every target, and change nothing
// else: no mapping, no learn, nothing a target does not name.
TEST_F(PerformLockSweepTest, ExternalControlChangesOnlyItsTargets) {
    showOnly("");
    const Frame frame = [this] { uiFrame(); };
    frame();
    size_t events = 0;
    const auto apply = [&] {
        ++events;
        boundary();  // the event applied (processExternalControl)
        frame();     // a take it queued is made (runExternalSceneTakes)
        boundary();
    };
    const auto osc = [&](const std::string& address, float value) {
        control::ControlEvent e;
        e.source = control::ControlEvent::Source::Osc;
        e.osc.address = address;
        control::OscArg arg;
        arg.type = control::OscArg::Type::Float;
        arg.f = value;
        e.osc.args.push_back(arg);
        ASSERT_TRUE(control_.queue().push(std::move(e)));
        apply();
    };

    // An OSC take reaches the show through the queue, the boundary and the editor frame.
    const UUID encore = es_.scenes[1]->getId();
    osc("/qm/scene/take", 2.0f);
    EXPECT_EQ(es_.activeScene().getId(), encore) << "the OSC take of scene 2 was not made";
    if (!unchanged("OSC /qm/scene/take 2", ALLOW_TAKES | ALLOW_TARGETS)) return;

    std::vector<std::string> addresses = {"/qm/scene/take", "/qm/scene/active"};
    for (const auto& target : control_.registry().listTargets()) addresses.push_back(target.path);
    for (const std::string& address : addresses) {
        for (float value : {0.0f, 0.5f, 1.0f}) {
            osc(address, value);
            if (!unchanged("OSC " + address + " " + std::to_string(value), ALLOW_TAKES | ALLOW_TARGETS)) return;
        }
    }
    for (const control::MidiMapping& mapping : std::vector<control::MidiMapping>(es_.midiMappings)) {
        for (int value : {0, 64, 127}) {
            pushMidi(mapping.type == control::MidiControlType::CC ? control::MidiType::ControlChange
                                                                 : control::MidiType::ProgramChange,
                     mapping.channel > 0 ? mapping.channel : 1, mapping.number, value);
            apply();
            if (!unchanged("MIDI " + mapping.target, ALLOW_TAKES | ALLOW_TARGETS)) return;
        }
    }
    EXPECT_GT(allowedChanges_, 1u) << "no OSC/MIDI event reached a target";
    std::printf("[sweep] external control: %zu addresses, %zu events, %zu allowed changes\n", addresses.size(),
                events, allowedChanges_);
    report("external control");
}

// A MIDI learn armed before locking: locking cancels it, so a control moved under the
// lock binds nothing.
TEST_F(PerformLockSweepTest, ALearnArmedBeforeLockingBindsNothingUnderTheLock) {
    unlockShow();
    control_.dispatcher().beginLearn("/qm/quad/" + stage_->getId().toString() + "/opacity");
    lockShow();
    pushMidi(control::MidiType::ControlChange, 1, 20, 64);
    boundary();
    unchanged("a control moved under the lock after a learn armed before it");
}

// A 14-bit pair whose first half (CC 20) was learned just before locking: its second
// half (CC 52) arriving under the lock, within the pairing time, must not make the
// mapping a 14-bit one (TD-36: locking cancelled the learn but not the pair).
TEST_F(PerformLockSweepTest, AFourteenBitPairHalfLearnedBeforeLockingDoesNotCompleteUnderIt) {
    unlockShow();
    control_.dispatcher().beginLearn("/qm/quad/" + stage_->getId().toString() + "/opacity");
    pushMidi(control::MidiType::ControlChange, 1, 20, 64);
    boundary();
    ASSERT_EQ(es_.midiMappings.size(), 3u) << "the learn made no mapping";
    ASSERT_EQ(es_.midiMappings.back().bits, 7);
    lockShow();
    pushMidi(control::MidiType::ControlChange, 1, 20 + 32, 1);  // controlNow_ has not moved
    boundary();
    unchanged("the other half of a 14-bit pair learned before locking");
}

// ---- Output windows ----------------------------------------------------------------------------

// Every key on a focused output window (OutputManager's GLFW key callback): Esc / F11
// leave perform mode, Ctrl+B is blackout, 1-9 queue scene takes, made in the next
// editor frame; nothing else does anything.
TEST_F(PerformLockSweepTest, OutputWindowKeysChangeOnlyWhatTheLockKeeps) {
    showOnly("");
    const Frame frame = [this] { uiFrame(); };
    size_t presses = 0;
    for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; ++key) {
        for (int mods : {0, GLFW_MOD_CONTROL, GLFW_MOD_SHIFT, GLFW_MOD_ALT, GLFW_MOD_CONTROL | GLFW_MOD_SHIFT}) {
            om_.handleOutputWindowKey(key, GLFW_PRESS, mods);
            om_.applyPendingPerformExit();
            om_.handleOutputWindowKey(key, GLFW_RELEASE, mods);
            ++presses;
            frame();
            boundary();
        }
        // Once per key: a take is the only allowed change, and two cannot cancel out.
        if (!unchanged("output window key " + std::to_string(key) + " (plain, Ctrl, Shift, Alt, Ctrl+Shift)",
                       ALLOW_TAKES)) {
            return;
        }
    }
    std::printf("[sweep] output window keys: %zu presses\n", presses);
    report("output window keys");
}
