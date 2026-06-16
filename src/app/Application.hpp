#pragma once

#include <array>
#include <memory>
#include <string>

struct GLFWwindow;

namespace quadmapper {

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

    // Project lifecycle actions, invoked from the menu bar, the Ctrl+N/S/O
    // shortcuts, and the Resources>Project tab. Single home for New/Save/Load,
    // replacing the blocks that were duplicated across render() (TD-02). Save
    // and load no-op when the editor's project path is empty.
    void newProject();
    void saveProject();
    void loadProject();

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

    // QM-058 self-test hook (QM_AI_SELFTEST). Off in normal runs.
    bool m_aiSelfTestPending = false;
    int m_frameCount = 0;

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
