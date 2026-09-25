#pragma once

#include "../core/Result.hpp"

#include <array>
#include <future>
#include <memory>
#include <optional>
#include <string>

struct GLFWwindow;

namespace quadmapper {

// ---------------------------------------------------------------------------
// StressConfig — CLI-driven perform-mode stress harness (QM-081 H2).
// Fields mirror the --stress sub-flags. H3/H4 fields (pauseAfter, capture)
// are stubbed here so the struct is stable; only H2 fields are consumed now.
// ---------------------------------------------------------------------------
struct StressConfig {
    int  cycles         = 50;
    int  outputs        = 1;
    int  framesPerCycle = 120;
    bool fullscreen     = false;
    std::string source  = "procedural";  // "procedural" | "image" | "video" | "hap"
    // H4: path to a media file when --source=video or --source=hap.
    // Required for those source types; empty = error.
    std::string media   = "";
    // H3: hidden mode — exercise the resource lifecycle (per-output Renderer
    // create/destroy) without taking over the screen. Windows stay hidden, and
    // hidden outputs are neither drawn nor swapped (QM-087), so this mode does not
    // exercise the output draw path or vsync pacing.
    bool hidden         = false;
    // H3: working-set bound — FAIL if post-warm-up working set grows more than
    // this many MB across all cycles.
    long workingSetBoundMB = 64;
    // H3: per-cycle wall-clock budget (ms). A cycle that hangs longer than this
    // is flagged as a wedge and aborts the run.
    int  cycleTimeoutMs = 5000;
    // H4: pause mid-cycle then resume to exercise the decode-thread / ring-buffer
    // on resume (MultiMon LESSON-BUG-006 repro). 0 = disabled.
    int  pauseAfter     = 0;
    std::string capture = "";  // "" = disabled
};

class Renderer;
class MediaLibrary;
class OutputManager;
class InputManager;
class Project;
class FolderWatcher;
class ThumbnailCache;
class SourceManager;
class Quad;
class Texture;
struct EditorState;
namespace ai { class SidecarClient; enum class SidecarState; }

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

    // Accessors
    GLFWwindow* getWindow() const { return m_window; }
    Renderer* getRenderer() const { return m_renderer.get(); }
    MediaLibrary* getMediaLibrary() const { return m_mediaLibrary.get(); }
    OutputManager* getOutputManager() const { return m_outputManager.get(); }
    InputManager* getInputManager() const { return m_inputManager.get(); }
    Project* getProject() const { return m_project.get(); }
    SourceManager* getSourceManager() const { return m_sourceManager.get(); }

    // Singleton access
    static Application& instance();

private:
    bool initializeGLFW();
    bool initializeOpenGL();
    bool initializeImGui();
    bool initializeComponents();

    void processInput();
    void update(float deltaTime);
    void render();

    // Project lifecycle requests from Ctrl+S and the Resources>Project tab. They only
    // queue a deferred EditorState::FileAction (as the File menu does); the work runs
    // in processPendingFileAction() at the next frame boundary (ADR-046).
    void saveProject();   // Save; an untitled project asks for a path
    void loadProject();   // Load the path typed in the Project tab

    // ---- Show-safe session (ADR-046 §4) -----------------------------------------
    // Runs once per frame between glfwPollEvents() and render(): establishes the
    // clean baseline, offers startup recovery, runs any pending file action, autosaves.
    void runFrameBoundaryTasks();
    void processPendingFileAction();
    void performNewProject();
    bool performLoadProject(const std::string& path);
    bool performSaveProject(bool chooseNewPath);
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
    // Offer a recovery file for `projectPath` ("" = untitled) newer than the project.
    void offerRecovery(const std::string& projectPath);
    // Unsaved-changes and recovery modals; called inside the ImGui frame.
    void renderSessionDialogs();

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

    // Build the per-frame texture map from m_editorState and push the warped
    // scene to all enabled output windows.  Called from run() after each main
    // frame's render()+ImGui+swap so it never runs mid-ImGui-frame.
    void renderToProjectorOutputs();

    // Headless perform-mode stress harness (QM-081 H2). Driven by --stress CLI
    // flags; runs in place of the interactive loop. Returns 0 on clean (flat GL
    // object counts), non-zero on any detected leak.
    int runStress(const StressConfig& cfg);

    // Window
    GLFWwindow* m_window = nullptr;
    int m_windowWidth = 1920;
    int m_windowHeight = 1080;
    std::string m_windowTitle = "QUADmapper";

    // Components
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<MediaLibrary> m_mediaLibrary;
    std::unique_ptr<OutputManager> m_outputManager;
    std::unique_ptr<InputManager> m_inputManager;
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

    // State
    bool m_running = false;
    float m_lastFrameTime = 0.0f;

    // Show-safe session state (ADR-046 §4).
    bool m_haveCleanBaseline = false;  // m_savedSnapshot captured after the first frame
    std::string m_savedSnapshot;       // editorSnapshot() as of the last save/load/new
    std::string m_autosavedSnapshot;   // editorSnapshot() last written to the autosave
    bool m_autoSaveEnabled = true;
    double m_autoSaveIntervalSec = 60.0;
    double m_lastAutosaveTime = 0.0;
    std::future<Result<void, Error>> m_autosaveWrite;  // in-flight autosave disk write
    bool m_exitConfirmed = false;      // Exit passed the unsaved-changes check

    // QM-058 self-test hook (QM_AI_SELFTEST). Off in normal runs.
    bool m_aiSelfTestPending = false;
    int m_frameCount = 0;

    // Perform-mode stress harness config (QM-081 H2). Set by --stress CLI flags;
    // absent in normal (interactive) runs.
    std::optional<StressConfig> m_stressConfig;

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
