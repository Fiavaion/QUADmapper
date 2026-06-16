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
    auto& currentTexturePath = es.currentTexturePath;
    auto& projectPath = es.projectPath;
    auto& statusMessage = es.statusMessage;
    auto& statusTimer = es.statusTimer;
    auto& initialized = es.initialized;

    auto& cornerHoveredVertex = es.cornerHoveredVertex;
    auto& cornerDraggedVertex = es.cornerDraggedVertex;
    auto& cornerSelectedVertex = es.cornerSelectedVertex;
    auto& cornerDragStartMousePos = es.cornerDragStartMousePos;
    auto& cornerDragStartPos = es.cornerDragStartPos;

    auto& meshHoveredVertex = es.meshHoveredVertex;
    auto& meshDraggedVertex = es.meshDraggedVertex;
    auto& meshSelectedVertex = es.meshSelectedVertex;
    auto& meshDragStartMousePos = es.meshDragStartMousePos;

    auto& snapActive = es.snapActive;
    auto& snapTargetPos = es.snapTargetPos;
    auto& meshDragStartOffset = es.meshDragStartOffset;

    auto& videoPlayer = es.videoPlayer;
    auto& videoPath = es.videoPath;
    auto& videoTextureId = es.videoTextureId;
    auto& videoTextureWidth = es.videoTextureWidth;
    auto& videoTextureHeight = es.videoTextureHeight;
    auto& videoAssignedQuadId = es.videoAssignedQuadId;

    auto& mediaBinPanel = es.mediaBinPanel;
    auto& mediaBinInitialized = es.mediaBinInitialized;

    auto& outputPanel = es.outputPanel;
    auto& outputPanelInitialized = es.outputPanelInitialized;

    auto& showProperties = es.showProperties;
    auto& showResources = es.showResources;
    auto& showVideoPlayer = es.showVideoPlayer;

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

    // Disable depth test for 2D rendering
    glDisable(GL_DEPTH_TEST);

    // Render all quads
    if (m_renderer) {
        m_renderer->beginFrame();

        // Build texture pointer map for rendering
        std::unordered_map<UUID, Texture*> texturePtrs;
        for (const auto& [id, texPtr] : quadTextures) {
            texturePtrs[id] = texPtr.get();
        }

        // Handle video texture rendering for assigned quad
        auto& videoTexWrapper = es.videoTexWrapper;  // Owned by EditorState (see ADR-031 TD-02)
        if (!videoAssignedQuadId.isNull() && videoTextureId != 0) {
            // Use raw OpenGL texture binding for video
            videoTexWrapper.setGLTextureId(videoTextureId, videoTextureWidth, videoTextureHeight);
            texturePtrs[videoAssignedQuadId] = &videoTexWrapper;
        }

        m_renderer->renderQuads(quadManager, texturePtrs);

        // Render handles for selected quad based on mode
        Quad* selectedQuadForHandles = quadManager.getPrimarySelectedQuad();
        if (selectedQuadForHandles) {
            MeshMode mode = selectedQuadForHandles->getGeometry().getMeshMode();
            if (mode == MeshMode::MESH_EDIT) {
                // Full mesh editing - show all mesh vertex handles
                m_renderer->renderMeshHandles(*selectedQuadForHandles, meshHoveredVertex, meshDraggedVertex, meshSelectedVertex);
            } else {
                // CORNERS_ONLY mode (default) - show just the 4 corner handles
                m_renderer->renderCornerHandles(*selectedQuadForHandles, cornerHoveredVertex, cornerDraggedVertex, cornerSelectedVertex);
            }

            // Render snap indicator if actively snapping to another quad's vertex
            if (snapActive) {
                m_renderer->renderSnapIndicator(snapTargetPos);
            }
        }

        m_renderer->endFrame();
    }

    // Handle vertex interaction (before ImGui to capture mouse before UI)
    {
        Quad* selectedQuad = quadManager.getPrimarySelectedQuad();
        if (selectedQuad) {
            QuadGeometry& geometry = selectedQuad->getGeometry();
            MeshMode mode = geometry.getMeshMode();

            // Get mouse position in normalized coordinates
            double mouseX, mouseY;
            glfwGetCursorPos(m_window, &mouseX, &mouseY);

            int fbWidth, fbHeight;
            glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);

            Vec2 mouseNorm(
                static_cast<float>(mouseX) / fbWidth,
                static_cast<float>(mouseY) / fbHeight
            );

            bool mouseDown = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            constexpr float HIT_RADIUS = 0.015f;  // Slightly larger hit radius for corners
            constexpr float SNAP_RADIUS = 0.02f;  // Snap threshold for vertex snapping

            // Reset snap state at start of each frame
            snapActive = false;

            // Helper lambda to find snap target from other quads
            auto findSnapTarget = [&](const Vec2& pos, const UUID& excludeQuadId) -> Vec2 {
                Vec2 snappedPos = pos;
                float closestSnapDist = SNAP_RADIUS;

                // Check all other quads for snap targets
                const auto& allQuads = quadManager.getQuads();
                for (const auto& otherQuad : allQuads) {
                    if (otherQuad->getId() == excludeQuadId) continue;  // Skip same quad

                    const auto& otherCorners = otherQuad->getGeometry().getCorners();
                    for (int i = 0; i < 4; ++i) {
                        float dist = glm::length(pos - otherCorners[i]);
                        if (dist < closestSnapDist) {
                            closestSnapDist = dist;
                            snappedPos = otherCorners[i];
                        }
                    }

                    // Also check mesh vertices if in mesh edit mode
                    if (otherQuad->getGeometry().getMeshMode() == MeshMode::MESH_EDIT) {
                        auto meshPositions = otherQuad->getGeometry().generateMeshPositions();
                        for (const auto& meshPos : meshPositions) {
                            float dist = glm::length(pos - meshPos);
                            if (dist < closestSnapDist) {
                                closestSnapDist = dist;
                                snappedPos = meshPos;
                            }
                        }
                    }
                }

                // Set snap indicator state if snapping occurred
                if (snappedPos != pos) {
                    snapActive = true;
                    snapTargetPos = snappedPos;
                }

                return snappedPos;
            };

            if (mode == MeshMode::CORNERS_ONLY) {
                // ===== CORNER PIN MODE (Default) =====
                const auto& corners = geometry.getCorners();

                if (cornerDraggedVertex >= 0) {
                    // Currently dragging a corner
                    if (mouseDown) {
                        Vec2 mouseDelta = mouseNorm - cornerDragStartMousePos;
                        Vec2 newPos = cornerDragStartPos + mouseDelta;

                        // Apply snapping to other quads' vertices
                        newPos = findSnapTarget(newPos, selectedQuad->getId());

                        geometry.setCorner(cornerDraggedVertex, newPos);
                        m_project->setModified(true);
                    } else {
                        // Mouse released
                        cornerSelectedVertex = cornerDraggedVertex;
                        cornerDraggedVertex = -1;
                    }
                } else {
                    // Check for hover over corners
                    cornerHoveredVertex = -1;
                    float closestDist = HIT_RADIUS;

                    for (int i = 0; i < 4; ++i) {
                        float dist = glm::length(mouseNorm - corners[i]);
                        if (dist < closestDist) {
                            closestDist = dist;
                            cornerHoveredVertex = i;
                        }
                    }

                    // Start drag if mouse pressed on a corner
                    if (mouseDown && cornerHoveredVertex >= 0) {
                        cornerDraggedVertex = cornerHoveredVertex;
                        cornerSelectedVertex = cornerHoveredVertex;
                        cornerDragStartMousePos = mouseNorm;
                        cornerDragStartPos = corners[cornerHoveredVertex];
                    }
                }

                // Keyboard controls for corners
                ImGuiIO& cornerIO = ImGui::GetIO();
                if (!cornerIO.WantTextInput && !cornerIO.WantCaptureKeyboard) {
                    // Tab: Cycle through corners (TL, TR, BR, BL)
                    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                        if (cornerIO.KeyShift) {
                            cornerSelectedVertex = (cornerSelectedVertex <= 0) ? 3 : cornerSelectedVertex - 1;
                        } else {
                            cornerSelectedVertex = (cornerSelectedVertex + 1) % 4;
                        }
                    }

                    // Arrow keys: Move selected corner
                    if (cornerSelectedVertex >= 0 && cornerSelectedVertex < 4) {
                        float moveAmount = 0.001f;
                        if (cornerIO.KeyShift) moveAmount *= 10.0f;
                        if (cornerIO.KeyAlt) moveAmount *= 0.1f;

                        Vec2 pos = corners[cornerSelectedVertex];
                        bool moved = false;

                        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                            pos.y -= moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                            pos.y += moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                            pos.x -= moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                            pos.x += moveAmount;
                            moved = true;
                        }

                        if (moved) {
                            geometry.setCorner(cornerSelectedVertex, pos);
                            m_project->setModified(true);
                        }
                    }
                }

                // Reset mesh state when in corner mode
                meshHoveredVertex = -1;
                meshDraggedVertex = -1;
                meshSelectedVertex = -1;

            } else {
                // ===== MESH EDIT MODE =====
                auto positions = geometry.generateMeshPositions();
                auto bilinearPositions = geometry.generateBilinearPositions();
                int vertexCount = geometry.getVertexCount();

                if (meshDraggedVertex >= 0) {
                    if (mouseDown) {
                        Vec2 mouseDelta = mouseNorm - meshDragStartMousePos;
                        Vec2 newOffset = meshDragStartOffset + mouseDelta;

                        // Calculate actual position, apply snapping, then compute offset
                        Vec2 bilinearPos = bilinearPositions[meshDraggedVertex];
                        Vec2 actualPos = bilinearPos + newOffset;
                        Vec2 snappedPos = findSnapTarget(actualPos, selectedQuad->getId());
                        newOffset = snappedPos - bilinearPos;

                        geometry.setMeshVertexByIndex(meshDraggedVertex, newOffset);
                        m_project->setModified(true);
                    } else {
                        meshSelectedVertex = meshDraggedVertex;
                        meshDraggedVertex = -1;
                    }
                } else {
                    meshHoveredVertex = -1;
                    float closestDist = 0.012f;  // Mesh handles are smaller

                    for (size_t i = 0; i < positions.size(); ++i) {
                        float dist = glm::length(mouseNorm - positions[i]);
                        if (dist < closestDist) {
                            closestDist = dist;
                            meshHoveredVertex = static_cast<int>(i);
                        }
                    }

                    if (mouseDown && meshHoveredVertex >= 0) {
                        meshDraggedVertex = meshHoveredVertex;
                        meshSelectedVertex = meshHoveredVertex;
                        meshDragStartMousePos = mouseNorm;
                        meshDragStartOffset = geometry.getMeshVertexByIndex(meshHoveredVertex);
                    }
                }

                // Keyboard controls for mesh editing
                ImGuiIO& meshIO = ImGui::GetIO();
                if (!meshIO.WantTextInput && !meshIO.WantCaptureKeyboard) {
                    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                        if (meshIO.KeyShift) {
                            meshSelectedVertex = (meshSelectedVertex <= 0) ? vertexCount - 1 : meshSelectedVertex - 1;
                        } else {
                            meshSelectedVertex = (meshSelectedVertex + 1) % vertexCount;
                        }
                    }

                    if (meshSelectedVertex >= 0 && meshSelectedVertex < vertexCount) {
                        float moveAmount = 0.001f;
                        if (meshIO.KeyShift) moveAmount *= 10.0f;
                        if (meshIO.KeyAlt) moveAmount *= 0.1f;

                        Vec2 offset = geometry.getMeshVertexByIndex(meshSelectedVertex);
                        bool moved = false;

                        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
                            offset.y -= moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
                            offset.y += moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) {
                            offset.x -= moveAmount;
                            moved = true;
                        }
                        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) {
                            offset.x += moveAmount;
                            moved = true;
                        }

                        if (moved) {
                            geometry.setMeshVertexByIndex(meshSelectedVertex, offset);
                            m_project->setModified(true);
                        }
                    }
                }

                // Reset corner state when in mesh mode
                cornerHoveredVertex = -1;
                cornerDraggedVertex = -1;
                cornerSelectedVertex = -1;
            }
        } else {
            // No quad selected, reset all state
            cornerHoveredVertex = -1;
            cornerDraggedVertex = -1;
            cornerSelectedVertex = -1;
            meshHoveredVertex = -1;
            meshDraggedVertex = -1;
            meshSelectedVertex = -1;
        }
    }

    // Re-enable depth test
    glEnable(GL_DEPTH_TEST);

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
    if (showProperties) {
    ImGui::Begin("Properties");

    // Show header indicating current selection type
    switch (currentSelectionType) {
        case SelectionType::Quad: ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Quad Properties"); break;
        case SelectionType::Output: ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Output Properties"); break;
        case SelectionType::Media: ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "Media Properties"); break;
        default: ImGui::TextDisabled("Properties"); break;
    }
    ImGui::Separator();

    // -------- Quad Properties --------
    if (currentSelectionType == SelectionType::Quad) {
    Quad* selectedQuad = quadManager.getPrimarySelectedQuad();

    if (selectedQuad) {
        // Editable name
        static char quadName[128];
        strncpy(quadName, selectedQuad->getName().c_str(), sizeof(quadName) - 1);
        quadName[sizeof(quadName) - 1] = '\0';

        if (ImGui::InputText("Name", quadName, sizeof(quadName), ImGuiInputTextFlags_EnterReturnsTrue)) {
            selectedQuad->setName(quadName);
            m_project->setModified(true);
        }

        ImGui::Separator();

        // Texture section
        ImGui::Text("Texture:");

        // Drag-drop target for media from Media Bin
        ImGui::BeginGroup();
        ImGui::InputText("Path", currentTexturePath, sizeof(currentTexturePath));
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            if (strlen(currentTexturePath) > 0) {
                auto texture = std::make_unique<Texture>();
                if (texture->loadFromFile(currentTexturePath)) {
                    quadTextures[selectedQuad->getId()] = std::move(texture);
                    quadTexturePaths[selectedQuad->getId()] = currentTexturePath;
                    Logger::info("Loaded texture for quad '{}': {}", selectedQuad->getName(), currentTexturePath);
                    snprintf(statusMessage, sizeof(statusMessage), "Loaded texture: %s", currentTexturePath);
                    statusTimer = 3.0f;
                    m_project->setModified(true);
                } else {
                    Logger::error("Failed to load texture: {}", currentTexturePath);
                    snprintf(statusMessage, sizeof(statusMessage), "Failed to load texture: %s", currentTexturePath);
                    statusTimer = 5.0f;
                }
            } else {
                snprintf(statusMessage, sizeof(statusMessage), "Please enter a texture path");
                statusTimer = 3.0f;
            }
        }

        auto texIt = quadTextures.find(selectedQuad->getId());
        if (texIt != quadTextures.end() && texIt->second) {
            ImGui::Text("Size: %dx%d", texIt->second->getWidth(), texIt->second->getHeight());
            if (ImGui::Button("Clear Texture")) {
                quadTextures.erase(selectedQuad->getId());
                quadTexturePaths.erase(selectedQuad->getId());
                currentTexturePath[0] = '\0';
                m_project->setModified(true);
            }
        }
        ImGui::EndGroup();

        // Drag-drop target for media from Media Bin
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_ITEM_ID")) {
                UUID mediaId = *(const UUID*)payload->Data;
                MediaItem* item = m_mediaLibrary->getItem(mediaId);
                if (item && item->getType() != MediaType::PROCEDURAL) {
                    // Load texture from media item path
                    auto texture = std::make_unique<Texture>();
                    if (texture->loadFromFile(item->getPath().string())) {
                        quadTextures[selectedQuad->getId()] = std::move(texture);
                        quadTexturePaths[selectedQuad->getId()] = item->getPath().string();
                        strncpy(currentTexturePath, item->getPath().string().c_str(),
                                sizeof(currentTexturePath) - 1);
                        currentTexturePath[sizeof(currentTexturePath) - 1] = '\0';
                        Logger::info("Assigned media '{}' to quad '{}'",
                                    item->getName(), selectedQuad->getName());
                        snprintf(statusMessage, sizeof(statusMessage),
                                "Assigned: %s", item->getName().c_str());
                        statusTimer = 3.0f;
                        m_project->setModified(true);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        // UV Controls section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("UV Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
            TextureBinding& texBinding = selectedQuad->getTexture();

            // Flip buttons at the top
            if (ImGui::Button(texBinding.flipH ? "Flip H [ON]" : "Flip H")) {
                texBinding.flipH = !texBinding.flipH;
                m_project->setModified(true);
            }
            ImGui::SameLine();
            if (ImGui::Button(texBinding.flipV ? "Flip V [ON]" : "Flip V")) {
                texBinding.flipV = !texBinding.flipV;
                m_project->setModified(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset UV")) {
                texBinding.resetUV();
                texBinding.flipH = false;
                texBinding.flipV = false;
                m_project->setModified(true);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Offset (Pan) - DragFloat with Ctrl+Click for input
            ImGui::Text("Offset (Pan):");
            ImGui::TextDisabled("Ctrl+Click to type value");
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::DragFloat("Offset X", &texBinding.uvOffset.x, 0.01f, -2.0f, 2.0f, "%.3f")) {
                m_project->setModified(true);
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::DragFloat("Offset Y", &texBinding.uvOffset.y, 0.01f, -2.0f, 2.0f, "%.3f")) {
                m_project->setModified(true);
            }

            ImGui::Spacing();

            // Coverage (easier for multi-quad spanning) - inverse of scale
            // Coverage 100% = full texture, 50% = half texture visible (scale = 2.0)
            ImGui::Text("Coverage (for spanning):");
            ImGui::TextDisabled("50%% = texture spans 2 quads");

            float coverageU = (texBinding.uvScale.x > 0.001f) ? (100.0f / texBinding.uvScale.x) : 100.0f;
            float coverageV = (texBinding.uvScale.y > 0.001f) ? (100.0f / texBinding.uvScale.y) : 100.0f;

            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::DragFloat("U Coverage", &coverageU, 1.0f, 10.0f, 400.0f, "%.0f%%")) {
                texBinding.uvScale.x = 100.0f / std::max(coverageU, 1.0f);
                m_project->setModified(true);
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::DragFloat("V Coverage", &coverageV, 1.0f, 10.0f, 400.0f, "%.0f%%")) {
                texBinding.uvScale.y = 100.0f / std::max(coverageV, 1.0f);
                m_project->setModified(true);
            }

            ImGui::Spacing();

            // Scale (direct control)
            ImGui::Text("Scale (Zoom):");
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::DragFloat("Scale X", &texBinding.uvScale.x, 0.01f, 0.1f, 10.0f, "%.3f")) {
                m_project->setModified(true);
            }
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::DragFloat("Scale Y", &texBinding.uvScale.y, 0.01f, 0.1f, 10.0f, "%.3f")) {
                m_project->setModified(true);
            }

            ImGui::Spacing();

            // Rotation with direct input
            float rotationDegrees = texBinding.uvRotation * 180.0f / 3.14159265f;
            ImGui::Text("Rotation:");
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::DragFloat("Rotation##uv", &rotationDegrees, 0.5f, -180.0f, 180.0f, "%.1f deg")) {
                texBinding.uvRotation = rotationDegrees * 3.14159265f / 180.0f;
                m_project->setModified(true);
            }
            // Quick rotation buttons
            ImGui::SameLine();
            if (ImGui::Button("0##rot")) {
                texBinding.uvRotation = 0.0f;
                m_project->setModified(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("90##rot")) {
                texBinding.uvRotation = 90.0f * 3.14159265f / 180.0f;
                m_project->setModified(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("180##rot")) {
                texBinding.uvRotation = 180.0f * 3.14159265f / 180.0f;
                m_project->setModified(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("270##rot")) {
                texBinding.uvRotation = 270.0f * 3.14159265f / 180.0f;
                m_project->setModified(true);
            }
        }

        // Geometry section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto& geometry = selectedQuad->getGeometry();
            auto corners = geometry.getCorners();
            bool cornersChanged = false;

            ImGui::Text("Corners (normalized 0-1):");
            if (ImGui::DragFloat2("Top Left", &corners[0].x, 0.01f, 0.0f, 1.0f)) cornersChanged = true;
            if (ImGui::DragFloat2("Top Right", &corners[1].x, 0.01f, 0.0f, 1.0f)) cornersChanged = true;
            if (ImGui::DragFloat2("Bottom Right", &corners[2].x, 0.01f, 0.0f, 1.0f)) cornersChanged = true;
            if (ImGui::DragFloat2("Bottom Left", &corners[3].x, 0.01f, 0.0f, 1.0f)) cornersChanged = true;

            if (cornersChanged) {
                geometry.setCorners(corners);
                m_project->setModified(true);
            }

            if (ImGui::Button("Reset Corners")) {
                geometry.resetCorners();
                m_project->setModified(true);
            }

            ImGui::Separator();
            int meshRes = geometry.getMeshResolution();
            if (ImGui::SliderInt("Mesh Resolution", &meshRes, 2, 64)) {
                geometry.setMeshResolution(meshRes);
                m_project->setModified(true);
            }

            // Mesh Edit Mode
            ImGui::Separator();
            ImGui::Text("Mesh Editing:");

            bool meshEditMode = (geometry.getMeshMode() == MeshMode::MESH_EDIT);
            if (ImGui::Checkbox("Edit Mesh Vertices", &meshEditMode)) {
                geometry.setMeshMode(meshEditMode ? MeshMode::MESH_EDIT : MeshMode::CORNERS_ONLY);
                // Reset all handle state when toggling mode
                meshDraggedVertex = -1;
                meshHoveredVertex = -1;
                meshSelectedVertex = -1;
                cornerDraggedVertex = -1;
                cornerHoveredVertex = -1;
                cornerSelectedVertex = -1;
                m_project->setModified(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Enable to drag individual mesh vertices for fine-tuned warping");
            }

            if (meshEditMode) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "Click and drag vertex handles to edit");

                if (geometry.hasMeshModifications()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Reset Mesh to Bilinear")) {
                        geometry.resetMesh();
                        m_project->setModified(true);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Clear all mesh vertex offsets and return to bilinear interpolation");
                    }
                }

                // Show vertex count info
                ImGui::Text("Vertices: %d (%dx%d grid)", geometry.getVertexCount(),
                           meshRes + 1, meshRes + 1);
            }
        }

        // Display section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
            float opacity = selectedQuad->getOpacity();
            if (ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f)) {
                selectedQuad->setOpacity(opacity);
                m_project->setModified(true);
            }

            ImGui::Separator();
            ImGui::Text("Z-Order");
            int zOrder = selectedQuad->getZOrder();
            int maxZ = static_cast<int>(quadManager.getQuadCount()) - 1;
            ImGui::Text("Current: %d / %d (higher = on top)", zOrder, maxZ);

            // Z-order buttons with proper labels
            if (ImGui::Button("Bring to Front")) {
                quadManager.moveQuadToBottom(selectedQuad->getId()); // To highest z-order
                m_project->setModified(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Ctrl+Shift+]");
            }
            ImGui::SameLine();
            if (ImGui::Button("Send to Back")) {
                quadManager.moveQuadToTop(selectedQuad->getId()); // To lowest z-order
                m_project->setModified(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Ctrl+Shift+[");
            }

            if (ImGui::Button("Bring Forward")) {
                quadManager.moveQuadDown(selectedQuad->getId()); // Increase z-order by 1
                m_project->setModified(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Ctrl+]");
            }
            ImGui::SameLine();
            if (ImGui::Button("Send Backward")) {
                quadManager.moveQuadUp(selectedQuad->getId()); // Decrease z-order by 1
                m_project->setModified(true);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Ctrl+[");
            }
        }
    } else {
        ImGui::TextDisabled("No quad selected");
        ImGui::TextDisabled("Select a quad from the Quad List");
    }
    } // end SelectionType::Quad

    // -------- Output Properties --------
    else if (currentSelectionType == SelectionType::Output) {
        OutputWindow* selectedOutput = nullptr;
        if (outputPanel.hasSelection() && m_outputManager) {
            selectedOutput = m_outputManager->getOutput(outputPanel.getSelectedOutputId());
        }

        if (selectedOutput) {
            // Editable name
            static char outputName[128];
            strncpy(outputName, selectedOutput->getName().c_str(), sizeof(outputName) - 1);
            outputName[sizeof(outputName) - 1] = '\0';
            if (ImGui::InputText("Name", outputName, sizeof(outputName), ImGuiInputTextFlags_EnterReturnsTrue)) {
                selectedOutput->setName(outputName);
            }

            ImGui::Separator();

            // Monitor dropdown
            ImGui::Text("Display:");
            const auto& monitors = m_outputManager->getMonitors();
            int currentMonitor = selectedOutput->getMonitor();
            if (currentMonitor < 0) currentMonitor = 0;

            // Build monitor combo string
            std::string monitorPreview = "Monitor " + std::to_string(currentMonitor + 1);
            if (currentMonitor < static_cast<int>(monitors.size())) {
                monitorPreview = monitors[currentMonitor].name + " (" +
                    std::to_string(monitors[currentMonitor].resolution.x) + "x" +
                    std::to_string(monitors[currentMonitor].resolution.y) + ")";
            }

            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##Monitor", monitorPreview.c_str())) {
                for (size_t i = 0; i < monitors.size(); ++i) {
                    const auto& mon = monitors[i];
                    std::string label = mon.name + " (" +
                        std::to_string(mon.resolution.x) + "x" +
                        std::to_string(mon.resolution.y) + ")";
                    if (mon.isPrimary) label += " [Primary]";

                    bool isSelected = (static_cast<int>(i) == currentMonitor);
                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        selectedOutput->setMonitor(static_cast<int>(i));
                        // Update resolution to match monitor's native resolution
                        selectedOutput->setResolution(mon.resolution);
                        if (selectedOutput->isCreated()) {
                            selectedOutput->applyResolution();
                        }
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();

            // Resolution dropdown
            ImGui::Text("Resolution:");
            IVec2 currentRes = selectedOutput->getResolution();
            std::string resPreview = std::to_string(currentRes.x) + " x " + std::to_string(currentRes.y);

            // Get available resolutions for the selected monitor
            auto videoModes = m_outputManager->getMonitorVideoModes(currentMonitor);

            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##Resolution", resPreview.c_str())) {
                for (const auto& mode : videoModes) {
                    std::string label = std::to_string(mode.width) + " x " + std::to_string(mode.height);
                    bool isSelected = (mode.width == currentRes.x && mode.height == currentRes.y);
                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        selectedOutput->setResolution(IVec2(mode.width, mode.height));
                        if (selectedOutput->isCreated()) {
                            selectedOutput->applyResolution();
                        }
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();

            // Rotation dropdown
            ImGui::Text("Rotation:");
            int rotationDeg = selectedOutput->getRotationDegrees();
            const char* rotLabels[] = { "0 degrees", "90 degrees", "180 degrees", "270 degrees" };
            int rotIndex = rotationDeg / 90;
            if (rotIndex < 0 || rotIndex > 3) rotIndex = 0;

            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##Rotation", rotLabels[rotIndex])) {
                for (int i = 0; i < 4; ++i) {
                    bool isSelected = (i == rotIndex);
                    if (ImGui::Selectable(rotLabels[i], isSelected)) {
                        selectedOutput->setRotationDegrees(i * 90);
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            // Enabled checkbox
            ImGui::Separator();
            bool enabled = selectedOutput->isEnabled();
            if (ImGui::Checkbox("Enabled", &enabled)) {
                selectedOutput->setEnabled(enabled);
            }

            // State info
            ImGui::Separator();
            ImGui::TextDisabled("Status:");
            ImGui::Text("Window: %s", selectedOutput->isCreated() ? "Created" : "Not Created");
            ImGui::Text("Fullscreen: %s", selectedOutput->isFullscreen() ? "Yes" : "No");

            // Window control buttons
            ImGui::Separator();
            if (selectedOutput->isCreated()) {
                if (selectedOutput->isVisible()) {
                    if (ImGui::Button("Hide Window")) {
                        selectedOutput->hide();
                    }
                } else {
                    if (ImGui::Button("Show Window")) {
                        selectedOutput->show();
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Destroy Window")) {
                    selectedOutput->destroy();
                }
            } else {
                if (ImGui::Button("Create Window")) {
                    selectedOutput->create(m_outputManager->getMainWindow());
                }
            }
        } else {
            ImGui::TextDisabled("No output selected");
            ImGui::TextDisabled("Select an output from the Outputs tab");
        }
    }

    // -------- Media Properties --------
    else if (currentSelectionType == SelectionType::Media) {
        MediaItem* selectedMedia = nullptr;
        if (mediaBinPanel.hasSelection() && m_mediaLibrary) {
            selectedMedia = m_mediaLibrary->getItem(mediaBinPanel.getSelectedItemId());
        }

        if (selectedMedia) {
            // Editable name
            static char mediaName[128];
            strncpy(mediaName, selectedMedia->getName().c_str(), sizeof(mediaName) - 1);
            mediaName[sizeof(mediaName) - 1] = '\0';
            if (ImGui::InputText("Name", mediaName, sizeof(mediaName), ImGuiInputTextFlags_EnterReturnsTrue)) {
                selectedMedia->setName(mediaName);
            }

            ImGui::Separator();

            // Type
            const char* typeStr = "Unknown";
            switch (selectedMedia->getType()) {
                case MediaType::IMAGE: typeStr = "Image"; break;
                case MediaType::VIDEO: typeStr = "Video"; break;
                case MediaType::AUDIO: typeStr = "Audio"; break;
                case MediaType::PROCEDURAL: typeStr = "Procedural"; break;
            }
            ImGui::Text("Type: %s", typeStr);

            // Path (for file-based media)
            if (selectedMedia->hasPath()) {
                ImGui::Separator();
                ImGui::Text("File:");
                ImGui::TextWrapped("%s", selectedMedia->getPath().string().c_str());
            }

            // Procedural source parameters
            if (selectedMedia->getType() == MediaType::PROCEDURAL && selectedMedia->hasSource()) {
                ImGui::Separator();
                const char* procTypeStr = "Unknown";
                switch (selectedMedia->getProceduralType()) {
                    case ProceduralType::SOLID_COLOR: procTypeStr = "Solid Color"; break;
                    case ProceduralType::GRADIENT: procTypeStr = "Gradient"; break;
                    case ProceduralType::CHECKERBOARD: procTypeStr = "Checkerboard"; break;
                    case ProceduralType::TEST_PATTERN: procTypeStr = "Test Pattern"; break;
                    case ProceduralType::NOISE: procTypeStr = "Noise"; break;
                    case ProceduralType::PLASMA: procTypeStr = "Plasma"; break;
                    case ProceduralType::GRID: procTypeStr = "Grid"; break;
                }
                ImGui::Text("Procedural Type: %s", procTypeStr);

                // Get the procedural source and show editable parameters
                TextureSource* source = m_sourceManager->getSource(selectedMedia->getSourceId());
                ProceduralSource* procSource = dynamic_cast<ProceduralSource*>(source);
                if (procSource) {
                    ImGui::Separator();
                    ImGui::Text("Parameters:");
                    auto params = procSource->getParameters();
                    for (const auto& param : params) {
                        ImGui::PushID(param.name.c_str());
                        switch (param.type) {
                            case SourceParameter::Type::Float: {
                                float val = std::get<float>(param.value);
                                if (ImGui::SliderFloat(param.displayName.c_str(), &val, param.minValue, param.maxValue)) {
                                    procSource->setParameter(param.name, val);
                                }
                                break;
                            }
                            case SourceParameter::Type::Int: {
                                int val = std::get<int>(param.value);
                                if (ImGui::SliderInt(param.displayName.c_str(), &val, (int)param.minValue, (int)param.maxValue)) {
                                    procSource->setParameter(param.name, val);
                                }
                                break;
                            }
                            case SourceParameter::Type::Bool: {
                                bool val = std::get<bool>(param.value);
                                if (ImGui::Checkbox(param.displayName.c_str(), &val)) {
                                    procSource->setParameter(param.name, val);
                                }
                                break;
                            }
                            case SourceParameter::Type::Color: {
                                Color col = std::get<Color>(param.value);
                                float colArr[4] = { col.r, col.g, col.b, col.a };
                                if (ImGui::ColorEdit4(param.displayName.c_str(), colArr,
                                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
                                    procSource->setParameter(param.name, Color(colArr[0], colArr[1], colArr[2], colArr[3]));
                                }
                                break;
                            }
                        }
                        ImGui::PopID();
                    }

                    // Reset button
                    if (ImGui::Button("Reset to Defaults")) {
                        procSource->resetParameters();
                    }

                    // Show dimensions
                    ImGui::Separator();
                    ImGui::Text("Size: %dx%d", procSource->getWidth(), procSource->getHeight());
                }
            }
        } else {
            ImGui::TextDisabled("No media selected");
            ImGui::TextDisabled("Select media from the Media tab");
        }
    }

    // -------- No Selection --------
    else {
        ImGui::TextDisabled("Nothing selected");
        ImGui::TextDisabled("Select a quad, output, or media item");
    }

    ImGui::End();
    } // end showProperties

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
    if (showVideoPlayer) {
    ImGui::Begin("Video Player");

    ImGui::Text("Video File:");
    ImGui::InputText("##videopath", videoPath, sizeof(videoPath));
    ImGui::SameLine();
    if (ImGui::Button("Load Video")) {
        if (strlen(videoPath) > 0) {
            if (videoPlayer.load(videoPath)) {
                snprintf(statusMessage, sizeof(statusMessage), "Video loaded: %s", videoPath);
            } else {
                snprintf(statusMessage, sizeof(statusMessage), "Failed to load video: %s", videoPath);
            }
            statusTimer = 3.0f;
        }
    }

    if (videoPlayer.isLoaded()) {
        const VideoInfo& info = videoPlayer.getVideoInfo();
        const VideoPlaybackState& state = videoPlayer.getState();

        ImGui::Separator();

        // Video info
        ImGui::Text("Size: %dx%d | FPS: %.2f | Duration: %.1fs",
                   info.width, info.height, info.frameRate, info.duration);
        if (info.hasAudio) {
            ImGui::Text("Audio: %d ch @ %d Hz", info.audioChannels, info.audioSampleRate);
        }

        ImGui::Separator();

        // Playback controls
        if (state.isPlaying()) {
            if (ImGui::Button("Pause")) {
                videoPlayer.pause();
            }
        } else {
            if (ImGui::Button("Play")) {
                videoPlayer.play();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop")) {
            videoPlayer.stop();
        }
        ImGui::SameLine();
        if (ImGui::Button("<< 5s")) {
            videoPlayer.skip(-5.0);
        }
        ImGui::SameLine();
        if (ImGui::Button(">> 5s")) {
            videoPlayer.skip(5.0);
        }

        // Progress slider
        float progress = state.getProgress();
        if (ImGui::SliderFloat("Progress", &progress, 0.0f, 1.0f, "%.1f%%")) {
            videoPlayer.seekProgress(progress);
        }

        // Time display
        ImGui::Text("Time: %.1f / %.1f s  Frame: %lld / %lld",
                   state.position, state.duration,
                   static_cast<long long>(state.currentFrame),
                   static_cast<long long>(state.totalFrames));

        ImGui::Separator();

        // Speed control
        float speed = videoPlayer.getSpeed();
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 4.0f, "%.1fx")) {
            videoPlayer.setSpeed(speed);
        }
        ImGui::SameLine();
        if (ImGui::Button("1x")) {
            videoPlayer.setSpeed(1.0f);
        }

        // Loop mode
        const char* loopModes[] = { "Once", "Loop", "Ping-Pong" };
        int loopMode = static_cast<int>(videoPlayer.getLoopMode());
        if (ImGui::Combo("Loop Mode", &loopMode, loopModes, 3)) {
            videoPlayer.setLoopMode(static_cast<LoopMode>(loopMode));
        }

        ImGui::Separator();

        // Assign to quad section
        ImGui::Text("Assign Video to Quad:");
        Quad* selected = quadManager.getPrimarySelectedQuad();
        if (selected) {
            bool isAssigned = (videoAssignedQuadId == selected->getId());
            if (ImGui::Button(isAssigned ? "Remove from Quad" : "Assign to Selected Quad")) {
                if (isAssigned) {
                    // Remove video texture from quad
                    quadTextures.erase(selected->getId());
                    videoAssignedQuadId = UUID::null();  // Clear
                } else {
                    // Create a texture wrapper for the video
                    // Note: We'll use the raw OpenGL texture ID
                    videoAssignedQuadId = selected->getId();
                }
                m_project->setModified(true);
            }
            if (isAssigned) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
                                  "Video assigned to: %s", selected->getName().c_str());
            }
        } else {
            ImGui::TextDisabled("Select a quad to assign video");
        }

        // Unload button
        ImGui::Separator();
        if (ImGui::Button("Unload Video")) {
            videoPlayer.unload();
            if (videoTextureId != 0) {
                glDeleteTextures(1, &videoTextureId);
                videoTextureId = 0;
                videoTextureWidth = 0;
                videoTextureHeight = 0;
            }
            videoAssignedQuadId = UUID::null();
        }
    } else {
        ImGui::TextDisabled("No video loaded");
        ImGui::TextDisabled("Enter a video file path and click Load");
    }

    ImGui::End();
    } // end showVideoPlayer

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
