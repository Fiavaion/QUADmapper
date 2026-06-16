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

    // Per-session editor/scene state (was ~40 function-statics in render(); see ADR-031 TD-02)
    std::unique_ptr<EditorState> m_editorState;

    // State
    bool m_running = false;
    float m_lastFrameTime = 0.0f;

    // Singleton
    static Application* s_instance;
};

} // namespace quadmapper
