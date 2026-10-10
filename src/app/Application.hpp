#pragma once

#include "Bench.hpp"
#include "StressHarness.hpp"
#include "../editor/EditorContext.hpp"
#include "../editor/EditorServices.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace quadmapper {

class Renderer;
class MediaLibrary;
class OutputManager;
class Project;
class FolderWatcher;
class ThumbnailCache;
class SourceManager;
class AudioEngine;
struct EditorState;
class EditorUi;
class PreferencesDialog;
class ProjectSession;
class AiAssist;
class ActionRegistry;
struct SceneTakeQueue;
namespace control { class ExternalControl; }

// Also the editor's services (ADR-051 §1): the panels reach the session, the
// preferences file and the AI assist through EditorContext::services.
class Application : private EditorServices {
public:
    Application();
    ~Application() override;

    // Non-copyable
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Initialize application
    bool initialize(int argc, char* argv[]);

    // Run main loop
    int run();

    // Shutdown
    void shutdown();

    // Window close button / Alt+F4 (GLFW close callback): veto the close and route it
    // through the deferred Exit action so the unsaved-changes prompt can run (ADR-046).
    void requestExit();

    // Crash guard (ADR-046 §4): best-effort write of the live project to
    // <project>.crash.qmap (or the recovery folder when untitled). Never touches the
    // real project file and never throws; called from main()'s catch-all.
    void emergencySave() noexcept;

    // Live-show scene take (US-008). Key events from the editor window and, in perform
    // mode, from a focused output window only queue the scene number (GLFW callbacks
    // run inside glfwPollEvents); runSceneTakes() takes them under the editing-key guard.
    void queueSceneTake(int number);

    // Accessors
    GLFWwindow* getWindow() const { return m_window; }
    Renderer* getRenderer() const { return m_renderer.get(); }
    MediaLibrary* getMediaLibrary() const { return m_mediaLibrary.get(); }
    OutputManager* getOutputManager() const { return m_outputManager.get(); }
    Project* getProject() const { return m_project.get(); }
    SourceManager* getSourceManager() const { return m_sourceManager.get(); }

    // Singleton access
    static Application& instance();

private:
    bool initializeGLFW();
    bool initializeOpenGL();
    bool initializeImGui();
    bool initializeComponents();

    // One iteration of the interactive loop, in two halves so the headless bench can
    // act between them exactly where OS input lands. pollFrame: delta time, events,
    // frame-boundary tasks; false once a confirmed Exit closed the window.
    // presentFrame: render, editor swap, projector outputs; `marks`,
    // when given, receives the time each stage finished.
    struct FrameMarks {
        std::chrono::steady_clock::time_point rendered, editorSwapped, outputsPresented;
    };
    bool pollFrame(float& deltaTime);
    void presentFrame(FrameMarks* marks = nullptr);

    void render();
    // This frame's view of the editor and the components, for the UI (ADR-051 §2).
    EditorContext editorContext();

    // ---- EditorServices ------------------------------------------------------------
    bool unsavedLabelShown() override;
    bool isSearchingMedia() const override;
    bool isFileDialogBusy() const override;
    std::string mediaSearchFolder() const override;
    void requestLocateMissingMedia(const MissingMediaItem& item) override;
    void requestMissingMediaSearch() override;
    void savePreferences() override;
    bool aiActive() const override;
    bool aiBusy() const override;
    void runAi(AiAction action) override;
    bool aiCanSegment() const override;
    const AiPhoto& aiPhoto() const override;
    void aiSegmentAt(float x, float y) override;
    bool aiCanScan() const override;
    bool aiScanning() const override;
    bool aiHasScan(const UUID& outputId) const override;
    void aiCalibrationPixelAt(float x, float y) override;

    // Ctrl+S: only queues a deferred EditorState::FileAction (as the File menu does);
    // the work runs in processPendingFileAction() at the next frame boundary (ADR-046).
    void saveProject();   // Save; an untitled project asks for a path

    // The editor's actions (ADR-051 §3, AppActions.cpp): the edit.* actions and the
    // app's own (Save, Preferences, Blackout, F11, Esc, scene takes 1-9), in the order
    // the old shortcut block ran them. Once, from initializeComponents().
    void registerActions();

    // Live-show scene take (editor/SceneTakes.hpp), fading over `fadeSeconds` (nullopt =
    // the Preferences default, ADR-047 §5; the harnesses cut).
    void takeSceneByNumber(int number, std::optional<float> fadeSeconds = std::nullopt);
    // The takes queued since the last frame, through the scene.take.N actions, after the
    // shortcuts (EditorUi::frame). Key presses only under the Editing key guard (not
    // while a text field is active or a popup is open; presses made then are dropped,
    // never replayed); OSC/MIDI takes skip it (Source::Control).
    void runSceneTakes(EditorContext& ctx);

    // ---- External control (ADR-048) ---------------------------------------------
    // Created at startup (not for the stress or bench harness); OSC/MIDI settings come
    // from the preferences. Each frame boundary runs processExternalControl
    // (editor/ControlBindings.hpp) before render() takes any scene references.
    void initializeExternalControl();

    // ---- Show-safe session (ADR-046 §4, ProjectSession) ---------------------------
    // Runs once per frame between glfwPollEvents() and render(): establishes the
    // clean baseline, offers startup recovery, runs any pending file action, autosaves.
    void runFrameBoundaryTasks();
    void fitProjectorsToOutputs();

    // Draw every output's frame (its routed scene, or the armed One-Flash pattern)
    // into the OutputManager's render targets (ADR-054). Called from render() after
    // every edit of the frame (canvas, shortcuts, scene takes, panels), just before
    // ImGui::Render(), so the frames include them; presentFrame() then shows them on
    // the outputs after the editor's swap.
    void renderProjectorFrames();

    // Headless perform-mode stress harness (QM-081 H2). Driven by --stress CLI
    // flags; runs in place of the interactive loop. Returns 0 on clean (flat GL
    // object counts), non-zero on any detected leak.
    int runStress(const StressConfig& cfg);

    // Headless PRD §5 performance benchmark (--bench, Bench.cpp): builds synthetic
    // shows in code, drives the real frame path and prints a JSON report.
    int runBench(const BenchConfig& cfg);
    friend class BenchRunner;
    // Bench only (null otherwise): an edit render() makes where the panels make
    // theirs, after all of them and before the projector frames are drawn.
    std::function<void()> m_benchInFrameEdit;

    // Window
    GLFWwindow* m_window = nullptr;
    int m_windowWidth = 1920;
    int m_windowHeight = 1080;
    std::string m_windowTitle = "QUADmapper";

    // Components
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<MediaLibrary> m_mediaLibrary;
    std::unique_ptr<OutputManager> m_outputManager;
    std::unique_ptr<Project> m_project;
    std::unique_ptr<FolderWatcher> m_folderWatcher;
    std::unique_ptr<ThumbnailCache> m_thumbnailCache;
    std::unique_ptr<SourceManager> m_sourceManager;

    // Optional AI pre-show assist (ADR-035, ADR-051 §1): the sidecar and One-Flash.
    std::unique_ptr<AiAssist> m_aiAssist;

    // Per-session editor/scene state (was ~40 function-statics in render(); see ADR-031 TD-02)
    std::unique_ptr<EditorState> m_editorState;
    // The editor's UI (ADR-051 §2): canvas, dock layout, menu and panels. Its panels
    // refer to m_editorState and m_preferences, so it goes before them.
    std::unique_ptr<EditorUi> m_ui;
    // Scene takes by key and by OSC/MIDI since the last frame (editor/SceneTakes.hpp).
    std::unique_ptr<SceneTakeQueue> m_sceneTakes;

    // The editor's actions: the keys, the menu and the scene takes all invoke them.
    std::unique_ptr<ActionRegistry> m_actions;

    // External control (ADR-048); null in the stress harness.
    std::unique_ptr<control::ExternalControl> m_externalControl;

    // The machine preferences (loaded at startup, saved on every change) and the
    // Edit > Preferences window that edits them, which EditorUi draws. The Control
    // window and the menu bar's audio device list change the same copy.
    std::unique_ptr<PreferencesDialog> m_preferences;

    // Sound from video sources (ADR-050 §2): the output device, mixer and each video's
    // audio. The stress harness runs it on miniaudio's silent null device.
    std::unique_ptr<AudioEngine> m_audioEngine;
    void syncAudio();                // each video's sound onto its clock, once per frame

    // State
    bool m_running = false;
    float m_lastFrameTime = 0.0f;

    // Show-safe session (ADR-046 §4): file actions, autosave, recovery, dirty state.
    std::unique_ptr<ProjectSession> m_session;
    bool m_exitConfirmed = false;      // Exit passed the unsaved-changes check

    // Perform-mode stress harness config (QM-081 H2). Set by --stress CLI flags;
    // absent in normal (interactive) runs.
    std::optional<StressConfig> m_stressConfig;
    // Benchmark config (--bench flags); absent in normal runs.
    std::optional<BenchConfig> m_benchConfig;

    // Singleton
    static Application* s_instance;
};

} // namespace quadmapper
