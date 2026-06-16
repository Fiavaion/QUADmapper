#include "Application.hpp"
#include "EditorContext.hpp"
#include "EditorState.hpp"
#include "../core/Logger.hpp"
#include "../core/FileSystem.hpp"
#include "../project/Project.hpp"
#include "../project/ProjectSerializer.hpp"
#include "../scene/Scene.hpp"
#include "../scene/Quad.hpp"
#include "../scene/QuadManager.hpp"
#include "../scene/QuadGroup.hpp"
#include "../media/MediaLibrary.hpp"
#include "../media/MediaItem.hpp"
#include "../media/FolderWatcher.hpp"
#include "../media/ThumbnailCache.hpp"
#include "../source/SourceManager.hpp"
#include "../source/procedural/ProceduralSource.hpp"
#include "../renderer/Renderer.hpp"
#include "../renderer/Texture.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../input/InputManager.hpp"
#include "../ui/Theme.hpp"
#include "../ui/panels/OutputPanel.hpp"
#include "../ui/panels/MediaBinPanel.hpp"
#include "../ui/panels/QuadListPanel.hpp"
#include "../ui/dialogs/PreferencesDialog.hpp"
#include "../video/VideoPlayer.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <filesystem>
#include <imgui_internal.h>  // For DockBuilder API
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

namespace quadmapper {

Application* Application::s_instance = nullptr;

// GLFW error callback
static void glfwErrorCallback(int error, const char* description) {
    Logger::error("GLFW Error {}: {}", error, description);
}

// ---- Editor scene <-> flat Project persistence sync (ADR-033) ----
// The live scene of truth is es.quadManager; the flat Project is only the serialization DTO.
// These two helpers are the single place that copies between them, replacing the save/load
// blocks that were duplicated 3x/2x across render(). They copy the WHOLE geometry and texture
// binding, fixing the latent bug where the hand-rolled copies dropped mesh-edit vertex offsets
// and flipH/flipV on save/load.
static void syncSceneToProject(EditorState& es, Project& project) {
    project.clearQuads();
    project.getTexturePaths().clear();
    for (const auto& quadPtr : es.quadManager.getQuads()) {
        project.addQuad(*quadPtr);
        auto pathIt = es.quadTexturePaths.find(quadPtr->getId());
        project.getTexturePaths().push_back(
            pathIt != es.quadTexturePaths.end() ? pathIt->second : std::string());
    }
}

static void syncProjectToScene(EditorState& es, const Project& project) {
    es.quadManager.clear();
    es.quadTextures.clear();
    es.quadTexturePaths.clear();

    const auto& quads = project.getQuads();
    const auto& texPaths = project.getTexturePaths();
    for (size_t i = 0; i < quads.size(); ++i) {
        Quad* newQuad = es.quadManager.createQuad(quads[i].getName());
        newQuad->setVisible(quads[i].isVisible());
        newQuad->setLocked(quads[i].isLocked());
        newQuad->setOpacity(quads[i].getOpacity());
        newQuad->setZOrder(quads[i].getZOrder());
        newQuad->getGeometry() = quads[i].getGeometry();  // corners + resolution + mode + mesh offsets
        newQuad->getTexture() = quads[i].getTexture();    // UV transform + flip flags

        if (i < texPaths.size() && !texPaths[i].empty()) {
            es.quadTexturePaths[newQuad->getId()] = texPaths[i];
            auto texture = std::make_unique<Texture>();
            if (texture->loadFromFile(texPaths[i])) {
                es.quadTextures[newQuad->getId()] = std::move(texture);
            }
        }
    }

    // Select the first quad and sync the texture-path edit buffer.
    if (es.quadManager.getQuadCount() > 0) {
        es.quadManager.selectQuad(es.quadManager.getQuads()[0]->getId());
        auto pathIt = es.quadTexturePaths.find(es.quadManager.getQuads()[0]->getId());
        if (pathIt != es.quadTexturePaths.end()) {
            strncpy(es.currentTexturePath, pathIt->second.c_str(), sizeof(es.currentTexturePath) - 1);
            es.currentTexturePath[sizeof(es.currentTexturePath) - 1] = '\0';
        } else {
            es.currentTexturePath[0] = '\0';
        }
    }
}

// GLFW framebuffer size callback
static void framebufferSizeCallback(GLFWwindow* /*window*/, int width, int height) {
    glViewport(0, 0, width, height);
}

// GLFW file drop callback for external drag-drop
static void dropCallback(GLFWwindow* /*window*/, int count, const char** paths) {
    Application& app = Application::instance();
    MediaLibrary* library = app.getMediaLibrary();
    if (!library) return;

    int imported = 0;
    for (int i = 0; i < count; i++) {
        std::filesystem::path filePath(paths[i]);
        std::string ext = filePath.extension().string();
        if (MediaLibrary::isExtensionSupported(ext)) {
            if (library->addItem(filePath)) {
                imported++;
            }
        }
    }
    if (imported > 0) {
        Logger::info("Imported {} media file(s) via drag-drop", imported);
    }
}

Application::Application() {
    s_instance = this;
}

Application::~Application() {
    shutdown();
    s_instance = nullptr;
}

Application& Application::instance() {
    return *s_instance;
}

bool Application::initialize(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    Logger::info("Initializing QUADmapper...");

    if (!initializeGLFW()) {
        Logger::error("Failed to initialize GLFW");
        return false;
    }

    if (!initializeOpenGL()) {
        Logger::error("Failed to initialize OpenGL");
        return false;
    }

    if (!initializeImGui()) {
        Logger::error("Failed to initialize ImGui");
        return false;
    }

    if (!initializeComponents()) {
        Logger::error("Failed to initialize components");
        return false;
    }

    Logger::info("QUADmapper initialized successfully");
    m_running = true;
    return true;
}

bool Application::initializeGLFW() {
    Logger::debug("Initializing GLFW...");

    glfwSetErrorCallback(glfwErrorCallback);

    if (!glfwInit()) {
        Logger::error("Failed to initialize GLFW");
        return false;
    }

    // Configure OpenGL 3.3 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    // Window hints
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_TRUE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_FALSE);

    // Create window
    m_window = glfwCreateWindow(m_windowWidth, m_windowHeight,
                                 m_windowTitle.c_str(), nullptr, nullptr);
    if (!m_window) {
        Logger::error("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    // Make OpenGL context current
    glfwMakeContextCurrent(m_window);

    // Enable vsync
    glfwSwapInterval(1);

    // Set callbacks
    glfwSetFramebufferSizeCallback(m_window, framebufferSizeCallback);
    glfwSetDropCallback(m_window, dropCallback);

    // Get actual framebuffer size (may differ from window size on high-DPI)
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
    Logger::info("Window created: {}x{}, Framebuffer: {}x{}",
                 m_windowWidth, m_windowHeight, fbWidth, fbHeight);

    return true;
}

bool Application::initializeOpenGL() {
    Logger::debug("Initializing OpenGL with GLAD...");

    // Load OpenGL function pointers with GLAD
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        Logger::error("Failed to initialize GLAD");
        return false;
    }

    // Log OpenGL info
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* glslVersion = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));

    Logger::info("OpenGL Vendor: {}", vendor ? vendor : "unknown");
    Logger::info("OpenGL Renderer: {}", renderer ? renderer : "unknown");
    Logger::info("OpenGL Version: {}", version ? version : "unknown");
    Logger::info("GLSL Version: {}", glslVersion ? glslVersion : "unknown");

    // Set initial viewport
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
    glViewport(0, 0, fbWidth, fbHeight);

    // Enable depth testing (will be useful later)
    glEnable(GL_DEPTH_TEST);

    // Enable blending for transparency
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Set clear color (dark gray)
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    return true;
}

bool Application::initializeImGui() {
    Logger::debug("Initializing Dear ImGui...");

    // Check ImGui version
    IMGUI_CHECKVERSION();

    // Create ImGui context
    ImGui::CreateContext();

    // Configure ImGui
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard navigation
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;    // Enable multi-viewport

    // Apply QUADmapper "Neon Void" theme
    Theme::apply();

    // Load custom fonts (will fall back to default if not found)
    float dpiScale = 1.0f;
    glfwGetWindowContentScale(m_window, &dpiScale, nullptr);
    Theme::loadFonts(dpiScale);

    // When viewports are enabled, ensure consistent appearance
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        // Maintain our rounded corners in viewport windows
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Initialize platform/renderer backends
    const char* glslVersion = "#version 330 core";
    if (!ImGui_ImplGlfw_InitForOpenGL(m_window, true)) {
        Logger::error("Failed to initialize ImGui GLFW backend");
        return false;
    }

    if (!ImGui_ImplOpenGL3_Init(glslVersion)) {
        Logger::error("Failed to initialize ImGui OpenGL3 backend");
        return false;
    }

    Logger::info("Dear ImGui {} initialized with docking support", IMGUI_VERSION);

    return true;
}

bool Application::initializeComponents() {
    Logger::debug("Initializing application components...");

    // Create project first (other components may depend on it)
    m_project = std::make_unique<Project>();

    // Per-session editor/scene state (replaces render()'s function-statics; see ADR-031 TD-02)
    m_editorState = std::make_unique<EditorState>();

    // Create media library
    m_mediaLibrary = std::make_unique<MediaLibrary>();

    // Create thumbnail cache for media bin
    m_thumbnailCache = std::make_unique<ThumbnailCache>();

    // Create source manager for texture sources (procedural, video, image)
    m_sourceManager = std::make_unique<SourceManager>();

    // Auto-load media from default folder if configured
    {
        PreferencesDialog prefsDialog;
        prefsDialog.loadPreferences();
        const auto& prefs = prefsDialog.getPreferences();

        if (prefs.general.autoLoadMediaOnStartup &&
            !prefs.general.defaultMediaFolder.empty()) {
            std::filesystem::path mediaFolder(prefs.general.defaultMediaFolder);
            if (std::filesystem::exists(mediaFolder)) {
                int count = m_mediaLibrary->scanFolder(
                    mediaFolder,
                    prefs.general.recursiveFolderScan
                );
                if (count > 0) {
                    Logger::info("Auto-loaded {} media files from {}",
                                count, mediaFolder.string());
                }
            }
        }

        // Set up folder watcher for live file monitoring
        if (!prefs.general.defaultMediaFolder.empty()) {
            m_folderWatcher = std::make_unique<FolderWatcher>();
            m_folderWatcher->setCallback([this](const std::filesystem::path& path, bool isNew) {
                if (isNew) {
                    if (m_mediaLibrary->addItem(path)) {
                        Logger::info("FolderWatcher: Auto-imported new file: {}",
                                    path.filename().string());
                    }
                } else {
                    // File was modified - could reload texture here in the future
                    Logger::debug("FolderWatcher: File modified: {}",
                                 path.filename().string());
                }
            });
            m_folderWatcher->watch(
                prefs.general.defaultMediaFolder,
                prefs.general.recursiveFolderScan
            );
        }
    }

    // Create renderer and initialize it
    m_renderer = std::make_unique<Renderer>();
    if (!m_renderer->initialize()) {
        Logger::error("Failed to initialize renderer");
        return false;
    }

    // Create output manager and initialize it
    m_outputManager = std::make_unique<OutputManager>();
    auto outputResult = m_outputManager->initialize(m_window);
    if (!outputResult) {
        Logger::error("Failed to initialize output manager: {}", outputResult.error().message);
        return false;
    }
    m_outputManager->enableMonitorChangeDetection();
    Logger::info("Output manager initialized with {} monitors", m_outputManager->getMonitorCount());

    // Create input manager
    m_inputManager = std::make_unique<InputManager>();

    Logger::debug("All components initialized");
    return true;
}

int Application::run() {
    Logger::info("Starting main loop");

    m_lastFrameTime = static_cast<float>(glfwGetTime());

    while (!glfwWindowShouldClose(m_window) && m_running) {
        // Calculate delta time
        float currentTime = static_cast<float>(glfwGetTime());
        float deltaTime = currentTime - m_lastFrameTime;
        m_lastFrameTime = currentTime;

        // Poll events
        glfwPollEvents();

        // Process input
        processInput();

        // Update
        update(deltaTime);

        // Render
        render();

        // Swap buffers
        glfwSwapBuffers(m_window);

        // Push the warped scene to all projector outputs (after the main frame
        // is committed so we never run inside an ImGui frame).
        renderToProjectorOutputs();
    }

    Logger::info("Main loop ended");
    return 0;
}

void Application::processInput() {
    // Check for escape key to close window
    if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        // For now, just log - later we'll show a confirmation dialog
        // glfwSetWindowShouldClose(m_window, true);
    }

    // Input manager will handle detailed input processing
    if (m_inputManager) {
        // m_inputManager->processInput(m_window);
    }
}

void Application::update(float deltaTime) {
    // Update output manager
    if (m_outputManager) {
        // m_outputManager->update(deltaTime);
    }

    (void)deltaTime; // Suppress unused warning for now
    // Note: Video player update is done in render() for now since we need access to static state
}

void Application::render() {
    // Clear the framebuffer
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Per-session editor state now lives in m_editorState (owned member; see ADR-031 TD-02).
    // Each field is aliased to a same-named local reference so the body below is unchanged.
    EditorState& es = *m_editorState;
    using SelectionType = EditorState::SelectionType;
    auto& quadManager = es.quadManager;
    auto& quadTextures = es.quadTextures;
    auto& quadTexturePaths = es.quadTexturePaths;
    auto& projectPath = es.projectPath;
    auto& statusMessage = es.statusMessage;
    auto& statusTimer = es.statusTimer;
    auto& initialized = es.initialized;

    // Corner/mesh/snap editing state + videoAssignedQuadId now live in ViewportEditor
    // (the pre-NewFrame warp surface); their aliases moved there with the block (ADR-0034).

    auto& videoPlayer = es.videoPlayer;
    auto& videoTextureId = es.videoTextureId;
    auto& videoTextureWidth = es.videoTextureWidth;
    auto& videoTextureHeight = es.videoTextureHeight;

    auto& mediaBinPanel = es.mediaBinPanel;
    auto& mediaBinInitialized = es.mediaBinInitialized;

    auto& outputPanel = es.outputPanel;
    auto& outputPanelInitialized = es.outputPanelInitialized;

    auto& showResources = es.showResources;

    auto& currentSelectionType = es.currentSelectionType;

    // Initialize with one default quad
    if (!initialized) {
        quadManager.createQuad("Quad 1");
        if (quadManager.getQuadCount() > 0) {
            quadManager.selectQuad(quadManager.getQuads()[0]->getId());
        }
        initialized = true;
    }

    // Initialize Media Bin Panel
    if (!mediaBinInitialized && m_mediaLibrary) {
        mediaBinPanel.setMediaLibrary(m_mediaLibrary.get());
        mediaBinPanel.setThumbnailCache(m_thumbnailCache.get());
        mediaBinPanel.setSourceManager(m_sourceManager.get());
        mediaBinPanel.setVisible(true);  // Make visible by default
        mediaBinInitialized = true;
    }

    // Initialize Output Panel
    if (!outputPanelInitialized && m_outputManager) {
        outputPanel.setOutputManager(m_outputManager.get());
        outputPanelInitialized = true;
    }

    // Initialize Main Menu Bar callbacks (set once; lambdas capture this)
    if (!es.mainMenuBarInitialized) {
        es.mainMenuBar.setOnNewProject( [this]() { newProject();  });
        es.mainMenuBar.setOnSaveProject([this]() { saveProject(); });
        es.mainMenuBar.setOnLoadProject([this]() { loadProject(); });
        es.mainMenuBarInitialized = true;
    }

    // Update video player
    float deltaTime = ImGui::GetIO().DeltaTime;
    if (videoPlayer.isLoaded()) {
        bool hasNewFrame = videoPlayer.update(static_cast<double>(deltaTime));

        if (hasNewFrame && videoPlayer.hasNewFrame()) {
            const VideoFrame& frame = videoPlayer.getCurrentFrame();
            if (frame.isValid()) {
                // Create texture if needed
                if (videoTextureId == 0) {
                    glGenTextures(1, &videoTextureId);
                }

                // Check if size changed
                if (videoTextureWidth != frame.width || videoTextureHeight != frame.height) {
                    videoTextureWidth = frame.width;
                    videoTextureHeight = frame.height;

                    glBindTexture(GL_TEXTURE_2D, videoTextureId);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame.width, frame.height,
                                 0, GL_RGB, GL_UNSIGNED_BYTE, frame.data.data());
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                } else {
                    // Just update texture data
                    glBindTexture(GL_TEXTURE_2D, videoTextureId);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                                   GL_RGB, GL_UNSIGNED_BYTE, frame.data.data());
                }
                glBindTexture(GL_TEXTURE_2D, 0);

                videoPlayer.consumeFrame();
            }
        }
    }

    // On-canvas viewport: render every quad to the main GL framebuffer + the
    // corner-pin / mesh-edit warp interaction. Runs BEFORE ImGui::NewFrame() so it
    // owns the fullscreen GL surface and captures the mouse/keyboard ahead of any
    // panel (the success-metric vertex-warp UX). See ADR-0034 / ViewportEditor.
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.viewportEditor.render(ctx);
    }

    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Create dockspace over the entire viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags dockspaceFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpace", nullptr, dockspaceFlags);
    ImGui::PopStyleVar(3);

    // Create the dockspace
    ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    // Set up default dock layout on first run
    static bool dockLayoutInitialized = false;
    if (!dockLayoutInitialized) {
        dockLayoutInitialized = true;

        // Clear any existing layout
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        // Split the dockspace into regions
        ImGuiID dockLeft, dockRight, dockCenter, dockBottom;

        // First split: left panel (20% width)
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.20f, &dockLeft, &dockCenter);

        // Second split: right panel from center (25% of remaining)
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.30f, &dockRight, &dockCenter);

        // Third split: bottom panel from center (25% height)
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.25f, &dockBottom, &dockCenter);

        // Dock windows to their respective nodes
        ImGui::DockBuilderDockWindow("Quad List", dockLeft);
        ImGui::DockBuilderDockWindow("Video Player", dockLeft);  // Tabbed with Quad List
        ImGui::DockBuilderDockWindow("Properties", dockRight);
        ImGui::DockBuilderDockWindow("Resources", dockBottom);  // Tabbed panel with Project, Media, Outputs

        // Finish the layout
        ImGui::DockBuilderFinish(dockspaceId);
    }

    ImGui::End();

    ImGuiIO& io = ImGui::GetIO();

    // Main menu bar
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.mainMenuBar.render(ctx);
    }

    // Handle keyboard shortcuts
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject();
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        quadManager.selectAll();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && quadManager.hasSelection()) {
        for (const UUID& id : quadManager.getSelectedIds()) {
            quadTextures.erase(id);
            quadTexturePaths.erase(id);
        }
        quadManager.deleteSelectedQuads();
    }

    // Z-order keyboard shortcuts (only when not typing in text field)
    if (!io.WantTextInput) {
        // Ctrl+] : Move selected quad(s) up (bring forward - render later = on top)
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedUp();
                m_project->setModified(true);
            }
        }

        // Ctrl+[ : Move selected quad(s) down (send backward - render earlier = behind)
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedDown();
                m_project->setModified(true);
            }
        }

        // Ctrl+Shift+] : Move to top (bring to front)
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedToTop();
                m_project->setModified(true);
            }
        }

        // Ctrl+Shift+[ : Move to bottom (send to back)
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedToBottom();
                m_project->setModified(true);
            }
        }

        // F11 : Toggle Perform Mode
        if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
            m_outputManager->handleF11Key();
        }

        // Escape : Exit Perform Mode (if active)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            m_outputManager->handleEscapeKey();
        }

        // Mesh editing shortcuts
        Quad* meshQuad = quadManager.getPrimarySelectedQuad();
        if (meshQuad) {
            QuadGeometry& geo = meshQuad->getGeometry();

            // Ctrl+M : Toggle mesh edit mode
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
                bool isEdit = (geo.getMeshMode() == MeshMode::MESH_EDIT);
                geo.setMeshMode(isEdit ? MeshMode::CORNERS_ONLY : MeshMode::MESH_EDIT);
                m_project->setModified(true);
            }

            // Alt+= : Increase mesh resolution
            if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Equal, false)) {
                int res = geo.getMeshResolution();
                if (res < 64) {
                    geo.setMeshResolution(res + 1);
                    m_project->setModified(true);
                }
            }

            // Alt+- : Decrease mesh resolution
            if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Minus, false)) {
                int res = geo.getMeshResolution();
                if (res > 2) {
                    geo.setMeshResolution(res - 1);
                    m_project->setModified(true);
                }
            }

            // Alt+R : Reset mesh to bilinear
            if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                if (geo.getMeshMode() == MeshMode::MESH_EDIT && geo.hasMeshModifications()) {
                    geo.resetMesh();
                    m_project->setModified(true);
                }
            }
        }
    }

    // ============ Quad List Panel ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.quadListPanel.render(ctx);
    }

    // ============ Properties Panel (Context-Sensitive) ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.inspectorPanel.render(ctx);
    }

    // ============ Resources Panel (Tabbed: Project, Media, Outputs) ============
    if (showResources) {
    ImGui::Begin("Resources");

    if (ImGui::BeginTabBar("ResourcesTabs")) {
        // -------- Project Tab --------
        if (ImGui::BeginTabItem("Project")) {
            ImGui::Text("Project: %s", m_project->getName().c_str());
            if (m_project->isModified()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "(modified)");
            }
            ImGui::Separator();

            ImGui::Text("File Path:");
            ImGui::InputText("##projectpath", projectPath, sizeof(projectPath));
            ImGui::SameLine();
            if (ImGui::Button("Save")) {
                saveProject();
            }
            ImGui::SameLine();
            if (ImGui::Button("Load")) {
                loadProject();
            }

            // Status message display
            if (statusTimer > 0.0f) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", statusMessage);
                statusTimer -= ImGui::GetIO().DeltaTime;
            }

            ImGui::EndTabItem();
        }

        // -------- Media Bin Tab --------
        if (ImGui::BeginTabItem("Media")) {
            UUID prevMediaSelection = mediaBinPanel.getSelectedItemId();
            mediaBinPanel.renderContent();
            // Switch to Media properties if a media item was just selected
            if (mediaBinPanel.hasSelection() && mediaBinPanel.getSelectedItemId() != prevMediaSelection) {
                currentSelectionType = SelectionType::Media;
                quadManager.deselectAll();
                outputPanel.clearSelection();
            }
            ImGui::EndTabItem();
        }

        // -------- Outputs Tab --------
        if (ImGui::BeginTabItem("Outputs")) {
            UUID prevOutputSelection = outputPanel.getSelectedOutputId();
            outputPanel.renderContent();
            // Switch to Output properties if an output was just selected
            if (outputPanel.hasSelection() && outputPanel.getSelectedOutputId() != prevOutputSelection) {
                currentSelectionType = SelectionType::Output;
                quadManager.deselectAll();
                mediaBinPanel.clearSelection();
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    } // end showResources

    // ============ Video Player Panel ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.videoPlayerPanel.render(ctx);
    }

    // Demo window for testing ImGui (can be removed later)
    static bool showDemo = false;
    if (showDemo) {
        ImGui::ShowDemoWindow(&showDemo);
    }

    // Finalize ImGui rendering
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Handle multi-viewport platform windows
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backupContext = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backupContext);
    }
}

// ---- Project lifecycle actions (TD-02) ----
// Single home for New/Save/Load, replacing the blocks previously duplicated
// across render() (File menu, Ctrl+S shortcut, Resources>Project tab). Save and
// load read the editor's project-path buffer and no-op when it is empty.
void Application::newProject() {
    EditorState& es = *m_editorState;

    es.quadManager.clear();
    es.quadTextures.clear();
    es.quadTexturePaths.clear();
    es.currentTexturePath[0] = '\0';

    es.quadManager.createQuad("Quad 1");
    if (es.quadManager.getQuadCount() > 0) {
        es.quadManager.selectQuad(es.quadManager.getQuads()[0]->getId());
    }

    m_project->setName("Untitled Project");
    m_project->setFilePath("");
    m_project->clearQuads();
    m_project->getTexturePaths().clear();

    snprintf(es.statusMessage, sizeof(es.statusMessage), "New project created");
    es.statusTimer = 3.0f;
}

void Application::saveProject() {
    EditorState& es = *m_editorState;
    if (strlen(es.projectPath) == 0) return;

    syncSceneToProject(es, *m_project);

    auto result = ProjectSerializer::save(*m_project, es.projectPath);
    if (result) {
        m_project->setFilePath(es.projectPath);
        m_project->setModified(false);
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Project saved: %s", es.projectPath);
    } else {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Save failed: %s", result.error().message.c_str());
    }
    es.statusTimer = 3.0f;
}

void Application::loadProject() {
    EditorState& es = *m_editorState;
    if (strlen(es.projectPath) == 0) return;

    auto result = ProjectSerializer::load(es.projectPath);
    if (result) {
        *m_project = std::move(result.value());
        syncProjectToScene(es, *m_project);
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Project loaded: %s", es.projectPath);
    } else {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Load failed: %s", result.error().message.c_str());
    }
    es.statusTimer = 3.0f;
}

void Application::renderToProjectorOutputs() {
    if (!m_outputManager || !m_editorState) {
        return;
    }

    EditorState& es = *m_editorState;

    // Mirror the texture-map construction from render(): static image textures
    // plus the video wrapper for the video-assigned quad.
    std::unordered_map<UUID, Texture*> textures;
    textures.reserve(es.quadTextures.size() + 1);
    for (const auto& [id, texPtr] : es.quadTextures) {
        textures[id] = texPtr.get();
    }
    if (!es.videoAssignedQuadId.isNull() && es.videoTextureId != 0) {
        textures[es.videoAssignedQuadId] = &es.videoTexWrapper;
    }

    m_outputManager->renderToOutputs(es.quadManager, textures);
}

void Application::shutdown() {
    if (!m_window) {
        return; // Already shut down
    }

    Logger::info("Shutting down QUADmapper...");

    // Cleanup components in reverse order of creation
    m_inputManager.reset();
    m_outputManager.reset();
    m_thumbnailCache.reset();  // Clear GL textures before context destroyed
    m_renderer.reset();
    m_folderWatcher.reset();  // Stop watching before destroying media library
    m_mediaLibrary.reset();
    m_project.reset();

    // Shutdown ImGui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // Destroy window and terminate GLFW
    glfwDestroyWindow(m_window);
    glfwTerminate();

    m_window = nullptr;
    m_running = false;
    Logger::info("Shutdown complete");
}

} // namespace quadmapper
