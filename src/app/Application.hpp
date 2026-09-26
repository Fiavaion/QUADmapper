#pragma once

#include "../core/Result.hpp"
#include "../project/MediaRelocation.hpp"
#include "Bench.hpp"
#include "StressHarness.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
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
class Quad;
class Texture;
class AsyncFileDialog;
struct EditorState;
struct MissingMediaItem;
class PreferencesDialog;
namespace ai { class SidecarClient; enum class SidecarState; }
namespace control { class ExternalControl; }

class Application {
public:
    Application();
    ~Application();

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
    // run inside glfwPollEvents); render() takes them under the editing-key guards.
    void queueSceneTake(int number) { m_pendingSceneTakes.push_back(number); }

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
    // presentFrame: update, render, editor swap, projector outputs; `marks`,
    // when given, receives the time each stage finished.
    struct FrameMarks {
        std::chrono::steady_clock::time_point rendered, editorSwapped, outputsPresented;
    };
    bool pollFrame(float& deltaTime);
    void presentFrame(float deltaTime, FrameMarks* marks = nullptr);

    void update(float deltaTime);
    void render();

    // Project lifecycle requests from Ctrl+S and the Resources>Project tab. They only
    // queue a deferred EditorState::FileAction (as the File menu does); the work runs
    // in processPendingFileAction() at the next frame boundary (ADR-046).
    void saveProject();   // Save; an untitled project asks for a path
    void loadProject();   // Load the path typed in the Project tab

    void takeSceneByNumber(int number);  // live-show scene take; ignored while calibrating
    // New, Load and Restore drop takes still queued: they were pressed for the show
    // that was just replaced and must not put its scene N of the new one on air.
    void dropQueuedSceneTakes() {
        m_pendingSceneTakes.clear();
        m_pendingExternalSceneTakes.clear();
    }

    // ---- External control (ADR-048) ---------------------------------------------
    // Created at startup (not for the stress or bench harness); OSC/MIDI settings come
    // from the preferences. processExternalControl() runs at the frame boundary: MIDI hot-plug
    // upkeep, then at most ExternalControl::kMaxEventsPerFrame queued events are
    // applied on the main thread, before render() takes any scene references.
    void initializeExternalControl();
    void processExternalControl();
    void saveControlPreferences();
    void saveUiModePreference();  // the mode last used is the next session's (ADR-042)
    void drawBlackoutBanner();  // inside the ImGui frame, over the canvas

    // ---- Show-safe session (ADR-046 §4) -----------------------------------------
    // Runs once per frame between glfwPollEvents() and render(): establishes the
    // clean baseline, offers startup recovery, runs any pending file action, autosaves.
    void runFrameBoundaryTasks();
    void processPendingFileAction();
    void performNewProject();
    bool performLoadProject(const std::string& path);
    // Save to the project's path, or via the Save dialog when untitled / chooseNewPath.
    // onSaved runs once the file is written (after the dialog closes, if one was shown).
    void performSaveProject(bool chooseNewPath, std::function<void()> onSaved = {});
    bool writeProjectFile(const std::string& path);
    // Show the .qmap Open/Save dialog without blocking rendering (QM-088); onChosen
    // gets the path (empty = cancelled) from processPendingFileAction() once it closes.
    void openProjectDialog(bool save, std::function<void(const std::string&)> onChosen);
    void performRestoreRecovery();
    // The live editor as a serializable Project DTO, and its canonical JSON text
    // (compared against the last saved text to decide "unsaved changes").
    void buildEditorProject(Project& out);
    std::string editorSnapshot();
    bool isProjectDirty();
    void markProjectClean();
    void autosaveIfDue();
    // Collect the finished background autosave write (logs a failure). Blocks if
    // it is still running.
    void finishAutosaveWrite();
    void fitProjectorsToOutputs();
    // Offer a recovery file for `projectPath` ("" = untitled) newer than the project.
    void offerRecovery(const std::string& projectPath);
    // Unsaved-changes and recovery modals; called inside the ImGui frame.
    void renderSessionDialogs();

    // Missing media (PRD §4.1.8, MissingMedia.hpp). The window lists the files a loaded
    // project could not open; Locate and Search Folder use the non-blocking picker, and
    // the folder is searched on a worker thread. Relocation itself runs at the frame
    // boundary (the picker callback and pollMissingMediaSearch()).
    void renderMissingMediaDialog();
    void requestLocateMissingMedia(const MissingMediaItem& item);
    void requestMissingMediaSearch();
    void pollMissingMediaSearch();
    // Stop a folder search whose results belong to a project being replaced; waits
    // for the worker to notice.
    void cancelMissingMediaSearch();

    // AI pre-show "Detect Surface" (QM-058): capture a frame (live camera, or the
    // QM_AI_TEST_IMAGE override) -> sidecar /surface -> apply the proposed quad to
    // the selected quad when confident, else prompt "tap the surface". Geometry
    // only (no .qmap change); the applied corners are an ordinary warp the user
    // refines with the existing Tab-cycle + arrow-nudge UX.
    void aiDetectSurface();

    // One-shot headless driver for aiDetectSurface(), gated by m_aiSelfTestPending
    // (set from the QM_AI_SELFTEST env var). Verification only; no production path.
    void runAiSelfTest();

    // One-Flash capture flow (QM-059): flash a known ChArUco pattern fullscreen on
    // the chosen projector output, then capture a single photo (surface + pattern)
    // to disk for the sidecar's homography solve (QM-060).
    void oneFlashEmitPattern();
    void oneFlashCapturePhoto();
    void oneFlashClearPattern();
    // Capture a photo of the projected pattern, solve via POST /oneflash, and
    // apply the resulting corners to the selected quad. Geometry only.
    //   oneFlashSolveAndApply (QM-060): target = the detected surface.
    //   oneFlashAnamorphic   (QM-062): target = a centered audience rectangle ->
    //     content looks frontal from the captured viewpoint (anamorphosis).
    void oneFlashSolveAndApply();
    void oneFlashAnamorphic();
    // Shared core: solve One-Flash with an optional audience target and apply.
    void runOneFlash(const std::array<std::array<float, 2>, 4>* audienceTarget,
                     const char* label);

    // Open the first camera, grab one frame, and write it to pngPath. Returns
    // false with a human-readable reason in `error`. Shared by aiDetectSurface()
    // and the One-Flash capture.
    bool captureCameraFrameToPng(const std::string& pngPath, std::string& error);

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

    // Optional AI pre-show sidecar (ADR-035 / QM-056). Constructed but NOT
    // started: launched on demand from the AI menu, torn down at shutdown. The
    // app is fully usable when it is absent.
    std::unique_ptr<ai::SidecarClient> m_sidecarClient;

    // TD-10: last observed sidecar state, so the run loop can post a one-shot
    // status message when an async startAsync() connect resolves off-thread.
    // Initialized to Stopped in the constructor (enum defn lives in the .cpp).
    ai::SidecarState m_lastSidecarState;

    // Per-session editor/scene state (was ~40 function-statics in render(); see ADR-031 TD-02)
    std::unique_ptr<EditorState> m_editorState;
    std::vector<int> m_pendingSceneTakes;  // scene numbers pressed since the last frame
    // Scene takes asked for by OSC/MIDI since the last frame (ADR-048). Kept apart from
    // the key presses because they skip the typing/popup guard (see render()).
    std::vector<int> m_pendingExternalSceneTakes;

    // External control (ADR-048); null in the stress harness.
    std::unique_ptr<control::ExternalControl> m_externalControl;

    // The machine preferences (loaded at startup, saved on every change) and the
    // Edit > Preferences window that edits them. The Control window and the menu
    // bar's audio device list change the same copy.
    std::unique_ptr<PreferencesDialog> m_preferences;

    // Sound from video sources (ADR-050 §2): the output device, mixer and each video's
    // audio. The stress harness runs it on miniaudio's silent null device.
    std::unique_ptr<AudioEngine> m_audioEngine;
    void applyProjectAudio();        // project master volume/mute -> engine
    void syncAudio();                // each video's sound onto its clock, once per frame
    void saveAudioDevicePreference();

    // State
    bool m_running = false;
    float m_lastFrameTime = 0.0f;

    // Show-safe session state (ADR-046 §4).
    bool m_haveCleanBaseline = false;  // m_savedSnapshot captured after the first frame
    std::string m_savedSnapshot;       // editorSnapshot() as of the last save/load/new
    std::string m_autosavedSnapshot;   // editorSnapshot() last written to the autosave
    bool m_autoSaveEnabled = true;     // false: never this session (the bench), whatever the preferences
    double m_lastAutosaveTime = 0.0;
    std::future<Result<void, Error>> m_autosaveWrite;  // in-flight autosave disk write
    bool m_exitConfirmed = false;      // Exit passed the unsaved-changes check
    std::unique_ptr<AsyncFileDialog> m_projectDialog;  // Open/Save .qmap, off the render thread
    std::function<void(const std::string&)> m_onProjectFileChosen;
    // Missing-media folder search in flight (declared in this order so the future,
    // which waits for its worker, is destroyed before the flag the worker reads).
    std::atomic<bool> m_mediaSearchCancel{false};
    std::string m_mediaSearchFolder;
    std::future<relocate::FolderIndex> m_mediaSearch;

    // QM-058 self-test hook (QM_AI_SELFTEST). Off in normal runs.
    bool m_aiSelfTestPending = false;
    int m_frameCount = 0;

    // Perform-mode stress harness config (QM-081 H2). Set by --stress CLI flags;
    // absent in normal (interactive) runs.
    std::optional<StressConfig> m_stressConfig;
    // Benchmark config (--bench flags); absent in normal runs.
    std::optional<BenchConfig> m_benchConfig;

    // One-Flash calibration pattern state (QM-059). When m_emitPattern is set,
    // the projector outputs show m_calibQuad (identity, fullscreen) instead of the
    // scene. Lazily built on first emit.
    bool m_emitPattern = false;
    std::unique_ptr<Quad> m_calibQuad;
    std::unique_ptr<Texture> m_calibTexture;

    // Singleton
    static Application* s_instance;
};

} // namespace quadmapper
