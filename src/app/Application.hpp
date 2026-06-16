#pragma once

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
struct EditorState;
namespace ai { class SidecarClient; }

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

    // Per-session editor/scene state (was ~40 function-statics in render(); see ADR-031 TD-02)
    std::unique_ptr<EditorState> m_editorState;

    // State
    bool m_running = false;
    float m_lastFrameTime = 0.0f;

    // QM-058 self-test hook (QM_AI_SELFTEST). Off in normal runs.
    bool m_aiSelfTestPending = false;
    int m_frameCount = 0;

    // Singleton
    static Application* s_instance;
};

} // namespace quadmapper
