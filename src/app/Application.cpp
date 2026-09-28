#include "Application.hpp"
#include "EditorContext.hpp"
#include "EditorState.hpp"
#include "TextureResolve.hpp"
#include "MediaAssign.hpp"
#include "MissingMedia.hpp"
#include "ProjectReplace.hpp"
#include "ReplaceMedia.hpp"
#include "MeshLoads.hpp"
#include "UndoRouting.hpp"
#include "SliceEditing.hpp"
#include "OutputPersistence.hpp"
#include "ControlBindings.hpp"
#include "AiAssist.hpp"
#include "ProjectSession.hpp"
#include "StatusLine.hpp"
#include "../control/ExternalControl.hpp"
#include "../core/Logger.hpp"
#include "../core/FileSystem.hpp"
#include "../core/GlObjectStats.hpp"
#include "../project/Project.hpp"
#include "../scene/Scene.hpp"
#include "../scene/Quad.hpp"
#include "../scene/QuadManager.hpp"
#include "../scene/ObjLoader.hpp"
#include "../scene/ProjectorCamera.hpp"
#include "../media/MediaLibrary.hpp"
#include "../media/MediaItem.hpp"
#include "../media/FolderWatcher.hpp"
#include "../media/ThumbnailCache.hpp"
#include "../source/SourceManager.hpp"
#include "../ai/SidecarClient.hpp"
#include "../renderer/UvTestGrid.hpp"
#include "../renderer/Renderer.hpp"
#include "../renderer/Texture.hpp"
#include "../renderer/GpuCapabilities.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../input/VertexKeyboard.hpp"
#include "../ui/Theme.hpp"
#include "../ui/panels/OutputPanel.hpp"
#include "../ui/panels/MediaBinPanel.hpp"
#include "../ui/panels/QuadListPanel.hpp"
#include "../ui/dialogs/PreferencesDialog.hpp"
#include "../audio/AudioEngine.hpp"
#include "../audio/AudioMixer.hpp"
#include "SourceAudio.hpp"
#include "../video/VideoPlayer.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <filesystem>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <imgui_internal.h>  // For DockBuilder API
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

namespace quadmapper {

Application* Application::s_instance = nullptr;

// GLFW error callback
static void glfwErrorCallback(int error, const char* description) {
    Logger::error("GLFW Error {}: {}", error, description);
}

// GLFW window close callback: route the close through the deferred Exit action.
static void windowCloseCallback(GLFWwindow* /*window*/) {
    Application::instance().requestExit();
}

// Editor-window key callback, for the scene take keys only. It is installed before
// the ImGui backend, which chains to it with the untranslated (physical) GLFW key;
// ImGui's own keys are translated through the layout, which moves some top-row digits
// (AZERTY "4" arrives as the apostrophe key), so the take keys cannot use them.
static void editorKeyCallback(GLFWwindow* /*window*/, int key, int /*scancode*/, int action, int mods) {
    const int sceneNumber = OutputManager::sceneTakeNumber(key, action, mods);
    if (sceneNumber > 0) Application::instance().queueSceneTake(sceneNumber);
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
        const std::filesystem::path filePath = pathFromUtf8(paths[i]);  // GLFW drops are UTF-8
        std::string ext = utf8FromPath(filePath.extension());
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
    // Headless harness flags: --stress (QM-081) and --bench (PRD §5 benchmark).
    for (int i = 1; i < argc; ++i) {
        if (!parseStressArg(argv[i], m_stressConfig)) {
            parseBenchArg(argv[i], m_benchConfig);
        }
    }
    if (m_benchConfig) {
        // stdout carries only the JSON report; the log goes to a file beside it.
        Logger::setConsoleOutput(false);
        Logger::enableFileOutput(m_benchConfig->logPath());
    }

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

    // Machine preferences, before the components that read them. No file yet (first
    // run) or an unreadable one leaves the defaults.
    m_preferences = std::make_unique<PreferencesDialog>();
    m_preferences->loadPreferences();

    if (!initializeComponents()) {
        Logger::error("Failed to initialize components");
        return false;
    }

    // The stress and bench harnesses measure the render/perform path; they open no
    // ports or devices.
    if (!m_stressConfig && !m_benchConfig) initializeExternalControl();

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
    glfwSetKeyCallback(m_window, editorKeyCallback);  // before ImGui, which chains to it

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

    // Query BCn texture-compression support once now that a context is current
    // (ADR-044); the HAP upload path gates on this and degrades gracefully.
    GpuCapabilities::get().queryOnce();

    // Set initial viewport
    int fbWidth, fbHeight;
    glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
    glViewport(0, 0, fbWidth, fbHeight);

    // Enable depth testing (will be useful later)
    glEnable(GL_DEPTH_TEST);

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
    // A focused panel must not stop the canvas from taking Tab/arrows (the vertex
    // warp keys). Only a widget being edited captures the keyboard; see
    // claimCanvasEditingKeys() below NewFrame().
    io.ConfigNavCaptureKeyboard = false;
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

    // Sound (ADR-050 §2): the preferred output device, else the system default. A
    // device that will not open is logged and the app runs silent. The stress harness
    // uses the silent null device so it exercises the audio path without making sound.
    {
        AudioEngine::Options audioOptions;
        audioOptions.nullBackend = m_stressConfig.has_value() || m_benchConfig.has_value();
        m_audioEngine = std::make_unique<AudioEngine>(audioOptions);
        m_audioEngine->open(m_preferences->getPreferences().performance.audioDevice);
    }

    // Optional AI pre-show sidecar (ADR-035 / QM-056). Constructed here but not
    // started: the user enables it on demand from the AI menu.
    m_aiAssist = std::make_unique<AiAssist>(
        *m_editorState, std::make_unique<ai::SidecarClient>(AiAssist::locateSidecar()));

    // Auto-load media from default folder if configured
    {
        const AppPreferences& prefs = m_preferences->getPreferences();

        // UI complexity mode (ADR-042): default Simple; persisted choice wins.
        m_editorState->uiMode = (prefs.general.uiMode == 1)
            ? EditorState::UiMode::Advanced : EditorState::UiMode::Simple;

        if (prefs.general.autoLoadMediaOnStartup &&
            !prefs.general.defaultMediaFolder.empty()) {
            const std::filesystem::path mediaFolder = pathFromUtf8(prefs.general.defaultMediaFolder);
            if (std::filesystem::exists(mediaFolder)) {
                int count = m_mediaLibrary->scanFolder(
                    mediaFolder,
                    prefs.general.recursiveFolderScan
                );
                if (count > 0) {
                    Logger::info("Auto-loaded {} media files from {}",
                                count, utf8FromPath(mediaFolder));
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
                                    utf8FromPath(path.filename()));
                    }
                } else {
                    // File was modified - could reload texture here in the future
                    Logger::debug("FolderWatcher: File modified: {}",
                                 utf8FromPath(path.filename()));
                }
            });
            m_folderWatcher->watch(
                pathFromUtf8(prefs.general.defaultMediaFolder),
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
    m_editorState->outputResolver.setOutputManager(m_outputManager.get());  // calibration undo
    auto outputResult = m_outputManager->initialize(m_window);
    if (!outputResult) {
        Logger::error("Failed to initialize output manager: {}", outputResult.error().message);
        return false;
    }
    m_outputManager->enableMonitorChangeDetection();
    m_outputManager->setNoticeCallback([this](const std::string& message) {
        snprintf(m_editorState->statusMessage, sizeof(m_editorState->statusMessage), "%s",
                 message.c_str());
        m_editorState->statusTimer = 4.0f;
    });
    m_outputManager->setSceneKeyCallback([this](int sceneNumber) { queueSceneTake(sceneNumber); });
    Logger::info("Output manager initialized with {} monitors", m_outputManager->getMonitorCount());

    // Show-safe persistence (ADR-046): file actions, autosave, recovery, emergency save.
    {
        ProjectSession::Services services;
        services.sources = m_sourceManager.get();
        services.outputs = m_outputManager.get();
        services.audio = m_audioEngine.get();
        services.library = m_mediaLibrary.get();
        services.dropQueuedSceneTakes = [this]() { dropQueuedSceneTakes(); };
        services.confirmExit = [this]() {
            m_exitConfirmed = true;
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        };
        m_session = std::make_unique<ProjectSession>(*m_project, *m_editorState,
                                                     m_preferences->getPreferences(),
                                                     std::move(services));
    }

    // ADR-039 P2: create the default output and route the default scene to it. The
    // default output has no chosen monitor, so it resolves to the first display that
    // is not the editor's (the projector) — "launch already aimed at the projector" —
    // and to none when the editor's screen is the only one (QM-090). There is always
    // >= 1 output, so the F11 lazy-create path becomes a no-op.
    {
        OutputWindow* defaultOutput = m_outputManager->createOutput("Projector 1");
        if (defaultOutput) {
            m_outputManager->resolveOutputs();
            Scene& scene = m_editorState->activeScene();
            scene.setCanvasSize(defaultOutput->getEffectiveResolution());
            OutputManager::showScene(*defaultOutput, &scene, m_editorState->scenes);
            Logger::info("Default scene assigned to output '{}' on monitor {}",
                         defaultOutput->getName(), defaultOutput->getMonitor());
        }
    }

    Logger::debug("All components initialized");
    return true;
}

int Application::run() {
    // QM-081 H2: stress harness runs as a sibling branch, not a hijack of the UI loop.
    if (m_stressConfig) {
        return runStress(*m_stressConfig);
    }
    if (m_benchConfig) {
        return runBench(*m_benchConfig);
    }

    Logger::info("Starting main loop");

    // The close button / Alt+F4 goes through the unsaved-changes check (ADR-046).
    glfwSetWindowCloseCallback(m_window, windowCloseCallback);

    m_lastFrameTime = static_cast<float>(glfwGetTime());

    while (!glfwWindowShouldClose(m_window) && m_running) {
        float deltaTime = 0.0f;
        if (!pollFrame(deltaTime)) break;  // a confirmed Exit
        presentFrame();

        // AI assist: report an async sidecar connect; the QM_AI_SELFTEST hook.
        m_aiAssist->tick(m_outputManager.get());
    }

    Logger::info("Main loop ended");
    return 0;
}

bool Application::pollFrame(float& deltaTime) {
    // Calculate delta time
    float currentTime = static_cast<float>(glfwGetTime());
    deltaTime = currentTime - m_lastFrameTime;
    m_lastFrameTime = currentTime;

    // Poll events
    glfwPollEvents();
    // Esc/F11 on an output window only asked to leave perform mode (TD-13): leave it
    // here, outside the key callback that the exit clears.
    if (m_outputManager) m_outputManager->applyPendingPerformExit();

    // Frame boundary: pending file actions (New/Open/Save/Exit, recovery) and
    // autosave run here, before render() takes any scene references (ADR-046).
    runFrameBoundaryTasks();
    return !glfwWindowShouldClose(m_window);
}

void Application::presentFrame(FrameMarks* marks) {
    using Clock = std::chrono::steady_clock;

    // The frame clock scene crossfades run on (ADR-047 §5): wall time, sampled once,
    // before this frame's audio, takes and projector frames all read it.
    if (m_outputManager && m_editorState) {
        m_outputManager->setFrameTime(
            std::chrono::duration<double>(Clock::now().time_since_epoch()).count());
        m_outputManager->retireSceneFades(m_editorState->scenes);
    }

    // Render
    render();
    if (marks) marks->rendered = Clock::now();

    // Swap buffers
    glfwSwapBuffers(m_window);
    if (marks) marks->editorSwapped = Clock::now();

    // Show this frame's projector frames on the outputs (after the main frame
    // is committed so we never run inside an ImGui frame).
    if (m_outputManager) m_outputManager->presentOutputs();
    if (marks) marks->outputsPresented = Clock::now();
}

void Application::render() {
    // Clear the framebuffer
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Per-session editor state now lives in m_editorState (owned member; see ADR-031 TD-02).
    // Each field is aliased to a same-named local reference so the body below is unchanged.
    EditorState& es = *m_editorState;
    using SelectionType = EditorState::SelectionType;
    auto& quadManager = es.quadManager();
    auto& projectPath = es.projectPath;
    auto& statusMessage = es.statusMessage;
    auto& statusTimer = es.statusTimer;
    auto& initialized = es.initialized;

    // Corner/mesh/snap editing state moved into ViewportEditor (ADR-0034). Video is
    // now a SourceManager source updated by updateAll() below (ADR-040 P3) — no manual
    // decode/upload here.

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

    // UV test-grid (#2): apply the colored UV reference grid once to any quad we
    // haven't reconciled yet, when the toggle is on. Runs every frame so it
    // covers all creation paths (default, new-project, Quad List "+"). Existing
    // quads get marked seen, so toggling on later only affects future quads.
    for (const auto& qptr : quadManager.getQuads()) {
        const UUID& id = qptr->getId();
        if (es.seenQuadIds.insert(id).second && es.uvGridOnNewQuads) {
            assignUvTestGrid(es.quadTextures, es.quadTexturePaths, id, 1024, 8);
        }
    }

    // Initialize Media Bin Panel
    if (!mediaBinInitialized && m_mediaLibrary) {
        mediaBinPanel.setMediaLibrary(m_mediaLibrary.get());
        mediaBinPanel.setThumbnailCache(m_thumbnailCache.get());
        mediaBinPanel.setSourceManager(m_sourceManager.get());
        mediaBinPanel.setReplaceInShowCallback(
            [this](const std::filesystem::path& original, const std::filesystem::path& converted) {
                if (!m_sourceManager) return std::string("Media cannot be loaded right now.");
                std::string error = replaceClipInShow(*m_editorState, *m_sourceManager, original, converted);
                return error;
            });
        mediaBinPanel.setVisible(true);  // Make visible by default
        mediaBinInitialized = true;
    }

    // Initialize Output Panel
    if (!outputPanelInitialized && m_outputManager) {
        outputPanel.setOutputManager(m_outputManager.get());
        outputPanelInitialized = true;
    }

    // Initialize Main Menu Bar callbacks (set once; lambdas capture this). File items
    // queue es.pendingFileAction directly (ADR-046), so they need no callbacks.
    if (!es.mainMenuBarInitialized) {
        // AI pre-show assist (ADR-035): every item no-ops gracefully without a sidecar.
        es.mainMenuBar.setAiActiveQuery([this]() { return m_aiAssist->isActive(); });
        es.mainMenuBar.setOnEnableAi([this]()     { m_aiAssist->enable(); });
        es.mainMenuBar.setOnDetectSurface([this]() { m_aiAssist->detectSurface(); });
        es.mainMenuBar.setOnEmitPattern([this]()  { m_aiAssist->emitPattern(*m_outputManager); });
        es.mainMenuBar.setOnCapturePhoto([this]() { m_aiAssist->capturePhoto(m_outputManager.get()); });
        es.mainMenuBar.setOnClearPattern([this]() { m_aiAssist->clearPattern(); });
        es.mainMenuBar.setOnSolveApply([this]()   { m_aiAssist->solveAndApply(m_outputManager.get()); });
        es.mainMenuBar.setOnAnamorphic([this]()   { m_aiAssist->anamorphic(m_outputManager.get()); });
        es.mainMenuBarInitialized = true;
    }

    float deltaTime = ImGui::GetIO().DeltaTime;

    // Update all media sources (ADR-040 P2) and upload any new frames to GPU —
    // main thread / main GL context, once per frame, before any quad is drawn.
    if (m_sourceManager) {
        m_sourceManager->updateAll(static_cast<double>(deltaTime));
        // Each video's sound follows the clock it just advanced (ADR-050 §2).
        syncAudio();
    }

    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    // The canvas takes Tab/arrows for the selected quad's vertices or, while
    // calibrating the projector it shows, for the calibration points (ADR-052), or,
    // while a slice warp is edited, for its corners (ADR-047).
    claimCanvasEditingKeys(canvasKeyOwner(es, m_outputManager.get()) != CanvasKeyOwner::None);

    // On-canvas viewport: the corner-pin / mesh-edit warp interaction, then every
    // quad drawn to the main GL framebuffer. Runs right after NewFrame(), ahead of
    // every panel, so it acts on this frame's input first (the success-metric
    // vertex-warp UX) and a nudge shows in the very next presented frame (TD-21).
    // See ADR-0034 / ViewportEditor.
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.viewportEditor.render(ctx);
    }

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
        ImGui::DockBuilderDockWindow("Calibration", dockRight);  // Tabbed with Properties
        ImGui::DockBuilderDockWindow("Control", dockRight);      // Tabbed with Properties (ADR-048)
        ImGui::DockBuilderDockWindow("Resources", dockBottom);  // Tabbed panel with Project, Media, Outputs

        // Finish the layout
        ImGui::DockBuilderFinish(dockspaceId);
    }

    // Capture the central node rect (the transparent canvas region between panels)
    // for next frame's ViewportEditor view transform (ADR-041). Convert from ImGui
    // screen space to window-local coords so it matches glfwGetCursorPos.
    if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
        const ImVec2 vpPos = viewport->Pos;
        es.canvasRegionPos = Vec2(centralNode->Pos.x - vpPos.x,
                                  centralNode->Pos.y - vpPos.y);
        es.canvasRegionSize = Vec2(centralNode->Size.x, centralNode->Size.y);
    }

    ImGui::End();

    ImGuiIO& io = ImGui::GetIO();

    // Main menu bar
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        ctx.audio = m_audioEngine.get();
        es.mainMenuBar.render(ctx);
        saveAudioDevicePreference();
    }
    saveUiModePreference();

    // Viewport zoom/pan overlay + input (ADR-041). The central node is a passthrough
    // hole, so the mouse is over the canvas (not a panel) exactly when it is in the
    // region and no ImGui window has capture. Mutates canvasZoom/canvasPan, which the
    // next frame's ViewportEditor consumes; anchored zoom reuses last frame's view.
    if (es.canvasRegionSize.x > 1.0f && es.canvasRegionSize.y > 1.0f) {
        const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
        const ImVec2 regionAbs(es.canvasRegionPos.x + vpPos.x,
                               es.canvasRegionPos.y + vpPos.y);
        const ImVec2 regionSz(es.canvasRegionSize.x, es.canvasRegionSize.y);
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool overRegion =
            mouse.x >= regionAbs.x && mouse.x <= regionAbs.x + regionSz.x &&
            mouse.y >= regionAbs.y && mouse.y <= regionAbs.y + regionSz.y;

        // Scroll-wheel zoom, anchored on the cursor (uses last frame's transform).
        if (overRegion && !io.WantCaptureMouse && io.MouseWheel != 0.0f &&
            es.lastViewScale.x != 0.0f && es.lastViewScale.y != 0.0f) {
            const float oldZoom = es.canvasZoom;
            float newZoom = oldZoom * (1.0f + 0.12f * io.MouseWheel);
            newZoom = std::min(12.0f, std::max(0.1f, newZoom));
            const float cx = (mouse.x - regionAbs.x) / regionSz.x;
            const float cy = (mouse.y - regionAbs.y) / regionSz.y;
            const float px = (cx - es.lastViewOffset.x) / es.lastViewScale.x;
            const float py = (cy - es.lastViewOffset.y) / es.lastViewScale.y;
            const float nsx = es.lastViewScale.x * (newZoom / oldZoom);
            const float nsy = es.lastViewScale.y * (newZoom / oldZoom);
            es.canvasPan.x = (cx - px * nsx) - (1.0f - nsx) * 0.5f;
            es.canvasPan.y = (cy - py * nsy) - (1.0f - nsy) * 0.5f;
            es.canvasZoom = newZoom;
        }

        // Middle-drag pan (per-frame delta in region-normalized units).
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            es.canvasPan.x += d.x / regionSz.x;
            es.canvasPan.y += d.y / regionSz.y;
        }

        // Compact controls overlay in the top-left corner of the canvas region.
        ImGui::SetNextWindowPos(ImVec2(regionAbs.x + 8.0f, regionAbs.y + 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
        const ImGuiWindowFlags overlayFlags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        if (ImGui::Begin("##ViewportZoom", nullptr, overlayFlags)) {
            if (ImGui::SmallButton("Fit")) { es.canvasZoom = 1.0f; es.canvasPan = Vec2(0.0f, 0.0f); }
            ImGui::SameLine();
            if (ImGui::SmallButton("-")) es.canvasZoom = std::max(0.1f, es.canvasZoom / 1.12f);
            ImGui::SameLine();
            ImGui::Text("%3.0f%%", es.canvasZoom * 100.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton("+")) es.canvasZoom = std::min(12.0f, es.canvasZoom * 1.12f);
        }
        ImGui::End();

        // Drop media onto the quad under the cursor (ADR-042). A custom ImGui drop
        // target can't register over the passthrough central node (no window is hovered
        // there), so detect the drop manually: while a MEDIA_ITEM_ID drag is active,
        // on left-release over the canvas, assign to the topmost quad under the cursor.
        const ImGuiPayload* drag = ImGui::GetDragDropPayload();
        if (drag && drag->IsDataType("MEDIA_ITEM_ID") &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left) && overRegion &&
            es.lastViewScale.x != 0.0f && es.lastViewScale.y != 0.0f) {
            UUID mediaId = *(const UUID*)drag->Data;
            MediaItem* item = m_mediaLibrary ? m_mediaLibrary->getItem(mediaId) : nullptr;
            if (item) {
                const float cx = (mouse.x - regionAbs.x) / regionSz.x;
                const float cy = (mouse.y - regionAbs.y) / regionSz.y;
                const Vec2 canvasPt((cx - es.lastViewOffset.x) / es.lastViewScale.x,
                                    (cy - es.lastViewOffset.y) / es.lastViewScale.y);
                if (Quad* target = es.quadManager().quadAtPoint(canvasPt)) {
                    UUID sid = assignMediaToQuad(es, m_sourceManager.get(),
                                                 *target, *item);
                    if (!sid.isNull()) {
                        es.quadManager().selectQuad(target->getId());
                        snprintf(es.statusMessage, sizeof(es.statusMessage),
                                 "Assigned '%s' to %s", item->getName().c_str(),
                                 target->getName().c_str());
                        es.statusTimer = 3.0f;
                    }
                }
            }
        }
    }

    // Handle keyboard shortcuts. Editing shortcuts stay off while text is typed or a
    // popup/modal is open (e.g. the unsaved-changes prompt: Delete must not delete
    // quads behind it, Ctrl+S must not bypass it). F11/Esc are handled separately.
    const bool editingKeys = !io.WantTextInput &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // While calibrating a projector or editing a slice warp the quads are hidden and
    // not being edited, so the quad shortcuts (select all, Delete, z-order, mesh) must
    // not reach them.
    const bool quadKeys = editingKeys && es.calibratingOutputId.isNull() &&
                          es.sliceWarpOutputId.isNull();
    if (editingKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject();
    }

    // Undo/redo also run while calibrating or editing a slice warp, on that output's
    // calibration or slice steps only (UndoRouting.hpp, TD-17).
    const bool undoKey = editingKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false);
    const bool redoKey = editingKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false);
    if (undoKey || redoKey) {
        std::string heldBack;
        if (undoKey ? undoStep(es, &heldBack) : redoStep(es, &heldBack)) {
            m_session->invalidateUnsavedLabel();
        } else if (!heldBack.empty()) {
            snprintf(es.statusMessage, sizeof(es.statusMessage), "Leave %s to %s: %s",
                     undoScopeName(es), undoKey ? "undo" : "redo", heldBack.c_str());
            es.statusTimer = 3.0f;
        }
    }

    if (quadKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
        quadManager.selectAll();
    }

    // Edit > Preferences. Ctrl+, is no other shortcut's key.
    if (editingKeys && io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
        ImGui::IsKeyPressed(ImGuiKey_Comma, false)) {
        es.showPreferences = true;
    }

    if (editingKeys && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
        if (!es.calibratingOutputId.isNull()) {
            // Calibrating: Delete removes the selected calibration point, never quads.
            OutputWindow* calibrated = m_outputManager->getOutput(es.calibratingOutputId);
            if (calibrated && es.calibration.selected >= 0) {
                es.calibration.noteEdit("Delete calibration point " +
                                        std::to_string(es.calibration.selected + 1));
                es.calibration.removePoint(calibrated->getCalibrationPoints(),
                                           es.calibration.selected);
            }
        } else if (!quadKeys) {
            // Slice warp editing: the quads are hidden, Delete leaves them alone.
        } else if (quadManager.hasSelection()) {
            // Selecting an object deselects the quads, so a quad selection is newer.
            es.deleteSelectedQuads();
        } else {
            es.deleteSelectedObject();
        }
    }

    // Scene take (US-008): the 1-9 presses queued since the last frame put scene N on
    // air. Not while a text field is active (renaming a scene, typing a number) or a
    // popup is open; presses made then are dropped, never replayed later.
    if (editingKeys) {
        for (int n : m_pendingSceneTakes) takeSceneByNumber(n);
    }
    m_pendingSceneTakes.clear();
    // OSC/MIDI takes (ADR-048) go through the same take but skip that guard: it exists
    // so keys typed into a field or behind a modal do not act on the show, and an
    // operator's controller is not typing. Calibration still holds them off
    // (takeSceneByNumber), as it does the keys.
    for (const ExternalSceneTake& t : m_pendingExternalSceneTakes) {
        takeSceneByNumber(t.number, t.fadeSeconds);
        Logger::info("Control: scene take {} requested; active scene is now '{}'", t.number,
                     es.activeScene().getName());
    }
    m_pendingExternalSceneTakes.clear();

    // Blackout (ADR-048 §3): Ctrl+B, the panic key, works with a popup open like F11;
    // only a text field keeps it. Tab/arrows/Delete/1-9/Ctrl+S/Z/Y/A/M/[/] and Alt+=/-/R
    // are the editing keys, so Ctrl+B collides with none of them.
    if (!io.WantTextInput && io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper &&
        ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        m_outputManager->toggleBlackout();
    }

    // F11 toggles perform mode and Esc leaves it, even with a popup open: the way out
    // of fullscreen must never take two presses. Only a text field keeps them (its Esc
    // cancels the edit).
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
        m_outputManager->handleF11Key();
    }
    // While calibrating, the editor's Esc belongs to calibration: it cancels a pick in
    // progress, else leaves calibration, and never also leaves perform mode (Esc on an
    // output window still does).
    // Esc also cancels a pending MIDI learn, in the same press. Slice warp editing
    // takes Esc the same way: it leaves warp editing.
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (m_externalControl) m_externalControl->dispatcher().cancelLearn();
        if (!es.sliceWarpOutputId.isNull()) {
            es.endSliceWarp();
        } else if (es.calibratingOutputId.isNull()) {
            m_outputManager->handleEscapeKey();
        } else if (es.calibration.pick != CalibrationSession::Pick::None) {
            es.calibration.cancelPick();
        } else {
            es.endCalibration();
        }
    }

    // Z-order and mesh keyboard shortcuts
    if (quadKeys) {
        // Ctrl+] : Move selected quad(s) up (bring forward - render later = on top)
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedUp();
            }
        }

        // Ctrl+[ : Move selected quad(s) down (send backward - render earlier = behind)
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedDown();
            }
        }

        // Ctrl+Shift+] : Move to top (bring to front)
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedToTop();
            }
        }

        // Ctrl+Shift+[ : Move to bottom (send to back)
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) {
            if (quadManager.hasSelection()) {
                quadManager.moveSelectedToBottom();
            }
        }


        // Mesh editing shortcuts
        Quad* meshQuad = quadManager.getPrimarySelectedQuad();
        if (meshQuad) {
            QuadGeometry& geo = meshQuad->getGeometry();

            // Ctrl+M : Toggle mesh edit mode
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
                bool isEdit = (geo.getMeshMode() == MeshMode::MESH_EDIT);
                geo.setMeshMode(isEdit ? MeshMode::CORNERS_ONLY : MeshMode::MESH_EDIT);
            }

            // Alt+= : Increase mesh resolution
            if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Equal, false)) {
                int res = geo.getMeshResolution();
                if (res < 64) {
                    geo.setMeshResolution(res + 1);
                }
            }

            // Alt+- : Decrease mesh resolution
            if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Minus, false)) {
                int res = geo.getMeshResolution();
                if (res > 2) {
                    geo.setMeshResolution(res - 1);
                }
            }

            // Alt+R : Reset mesh (residuals and grid handles) to the corner-pin surface
            if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                if (geo.getMeshMode() == MeshMode::MESH_EDIT && geo.hasMeshModifications()) {
                    geo.resetMesh();
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

    // ============ Calibration Panel (only while calibrating, ADR-052) ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        es.calibrationPanel.render(ctx);
        es.calibrationPanel.drawCanvasOverlay(ctx);
    }

    // ============ Resources Panel (Tabbed: Project, Media, Outputs) ============
    if (showResources) {
    ImGui::Begin("Resources");

    if (ImGui::BeginTabBar("ResourcesTabs")) {
        // Simple mode (ADR-042) shows only the Media bin; Project/Scenes/Preview/
        // Outputs are Advanced-only (single-projector output is auto + the F11 button).
        const bool advanced = (es.uiMode == EditorState::UiMode::Advanced);

        // -------- Project Tab --------
        if (advanced && ImGui::BeginTabItem("Project")) {
            ImGui::Text("Project: %s", m_project->getName().c_str());
            if (m_session->unsavedLabelShown(glfwGetTime())) {
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
            UUID prevMediaSelection = es.selectedMediaId;
            mediaBinPanel.renderContent();
            // Switch to Media properties if a media item was just selected
            if (!es.selectedMediaId.isNull() && es.selectedMediaId != prevMediaSelection) {
                currentSelectionType = SelectionType::Media;
                quadManager.deselectAll();
                es.selectedOutputId = UUID();
            }
            ImGui::EndTabItem();
        }

        // -------- Scenes Tab --------
        if (advanced && ImGui::BeginTabItem("Scenes")) {
            EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                              m_sourceManager.get(), m_outputManager.get(),
                              m_renderer.get(), m_window};
            es.scenePanel.renderContent(ctx);
            ImGui::EndTabItem();
        }

        // -------- Preview Tab --------
        if (advanced && ImGui::BeginTabItem("Preview")) {
            EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                              m_sourceManager.get(), m_outputManager.get(),
                              m_renderer.get(), m_window};
            es.outputPreviewPanel.renderContent(ctx);
            ImGui::EndTabItem();
        }

        // -------- Outputs Tab --------
        if (advanced && ImGui::BeginTabItem("Outputs")) {
            UUID prevOutputSelection = es.selectedOutputId;
            outputPanel.renderContent();
            // Switch to Output properties if an output was just selected
            if (!es.selectedOutputId.isNull() && es.selectedOutputId != prevOutputSelection) {
                currentSelectionType = SelectionType::Output;
                quadManager.deselectAll();
                es.selectedMediaId = UUID();
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
        ctx.audio = m_audioEngine.get();
        es.videoPlayerPanel.render(ctx);
    }

    // ============ Control Panel (OSC / MIDI, ADR-048) ============
    if (m_externalControl) {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        if (es.controlPanel.render(ctx, *m_externalControl, m_preferences->getPreferences().control,
                                   glfwGetTime())) {
            saveControlPreferences();
        }
    }

    // ============ Preferences (Edit > Preferences..., Ctrl+,) ============
    {
        EditorContext ctx{es, m_project.get(), m_mediaLibrary.get(),
                          m_sourceManager.get(), m_outputManager.get(),
                          m_renderer.get(), m_window};
        ctx.audio = m_audioEngine.get();
        m_preferences->render(ctx, m_externalControl.get());
        saveAudioDevicePreference();  // a device picked in the Audio tab
        saveUiModePreference();       // a mode picked in the General tab
    }

    if (m_outputManager && m_outputManager->isBlackout()) drawBlackoutBanner();
    if (m_outputManager && m_outputManager->isBlendTestPattern()) drawBlendTestPatternBanner();

    renderMissingMediaDialog();
    es.mediaBinPanel.renderConversions();  // Convert to HAP progress (QM-082)
    // Unsaved-changes / recovery modals (ADR-046); drawn last so they sit on top.
    renderSessionDialogs();

    if (m_benchInFrameEdit) m_benchInFrameEdit();

    // Draw each projector's frame once (ADR-054), after every edit this frame made:
    // the canvas, the shortcuts, scene takes (keys 1-9, OSC/MIDI), blackout and every
    // panel, so each reaches the projector in the frame it was made (PRD: scene switch
    // < 16 ms). The output windows copy these after the editor's swap; the Preview tab
    // recorded the same textures above and ImGui samples them below, so it shows this
    // frame's image (a resize keeps the texture names, see Framebuffer::resize).
    renderProjectorFrames();

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

void Application::takeSceneByNumber(int number, std::optional<float> fadeSeconds) {
    EditorState& es = *m_editorState;
    // Calibration edits the scene routed to the calibrated output; keep it there.
    if (!es.calibratingOutputId.isNull()) return;

    // The harnesses cut whatever this machine's preference is, so their scene-switch
    // timings and same-frame readbacks do not depend on it.
    const bool harness = m_stressConfig.has_value() || m_benchConfig.has_value();
    const float fade = fadeSeconds ? *fadeSeconds
                                   : (harness ? 0.0f : m_preferences->getPreferences().general.sceneFadeSeconds);
    const SceneTake take = es.takeScene(number, *m_outputManager, fade);
    if (take.kind == SceneTake::Kind::None) return;
    // Takes run inside render(), after the frame-boundary fit and before this frame's
    // projector frames are drawn: re-fit now so those frames are framed for the scene
    // just put on air.
    fitProjectorsToOutputs();

    const Scene& scene = es.activeScene();
    if (take.kind == SceneTake::Kind::Route) {
        const OutputWindow* output = m_outputManager->getOutput(take.outputIds.front());
        const std::string where = take.outputIds.size() > 1
                                      ? std::to_string(take.outputIds.size()) + " outputs"
                                      : (output ? output->getName() : std::string("output"));
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Scene %d: %s on %s", number,
                 scene.getName().c_str(), where.c_str());
    } else {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Scene %d: %s", number,
                 scene.getName().c_str());
    }
    es.statusTimer = 3.0f;
}

void Application::initializeExternalControl() {
    m_externalControl = std::make_unique<control::ExternalControl>();

    ControlBindingHost host;
    host.scenes = &m_editorState->scenes;
    host.sources = m_sourceManager.get();
    host.takeScene = [this](int number, std::optional<float> fadeSeconds) {
        m_pendingExternalSceneTakes.push_back({number, fadeSeconds});
    };
    host.activeSceneNumber = [this]() {
        const EditorState& es = *m_editorState;
        for (size_t i = 0; i < es.scenes.size(); ++i) {
            if (es.scenes[i] && es.scenes[i]->getId() == es.activeSceneId) return static_cast<int>(i) + 1;
        }
        return 0;
    };
    host.isBlackout = [this]() { return m_outputManager && m_outputManager->isBlackout(); };
    host.setBlackout = [this](bool on) { if (m_outputManager) m_outputManager->setBlackout(on); };
    host.refuse = [this](const std::string& address, const std::string& reason) {
        m_externalControl->dispatcher().noteRefused(address, reason);
    };
    host.getMasterVolume = [this]() { return m_audioEngine ? m_audioEngine->masterVolume() : 1.0f; };
    host.setMasterVolume = [this](float v) { if (m_audioEngine) m_audioEngine->setMasterVolume(v); };
    host.isMasterMuted = [this]() { return m_audioEngine && m_audioEngine->masterMuted(); };
    host.setMasterMuted = [this](bool on) { if (m_audioEngine) m_audioEngine->setMasterMuted(on); };
    registerControlTargets(m_externalControl->registry(), host);

    const ControlPreferences& prefs = m_preferences->getPreferences().control;
    m_externalControl->setWantedMidiDevices(prefs.midiDevices);
    m_externalControl->setFeedback(ControlSettingsEditor::feedbackSettings(prefs));
    if (prefs.oscEnabled && !m_externalControl->startOsc(prefs.oscBindAddress, prefs.oscPort)) {
        postStatus(*m_editorState, "OSC input is off: " + m_externalControl->oscError());
    }
}

void Application::processExternalControl() {
    if (!m_externalControl) return;
    EditorState& es = *m_editorState;
    const double now = glfwGetTime();
    m_externalControl->maintainMidi(now);
    // Names and ids can change with any edit; re-read them when something will use them.
    if (m_externalControl->hasPendingEvents() || es.showControl || m_externalControl->feedbackDue(now)) {
        refreshControlEntities(m_externalControl->registry(), es.scenes, m_sourceManager.get());
    }
    m_externalControl->processEvents(es.midiMappings, now);
    m_externalControl->updateFeedback(now);
}

void Application::saveControlPreferences() {
    m_preferences->savePreferences();
}

void Application::saveUiModePreference() {
    const int mode = m_editorState->uiMode == EditorState::UiMode::Advanced ? 1 : 0;
    int& saved = m_preferences->getPreferences().general.uiMode;
    if (mode == saved) return;
    saved = mode;
    m_preferences->savePreferences();
}

void Application::drawBlackoutBanner() {
    const EditorState& es = *m_editorState;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool haveCanvas = es.canvasRegionSize.x > 1.0f && es.canvasRegionSize.y > 1.0f;
    // Across the top of the canvas, just below the zoom controls so they stay usable.
    const float top = haveCanvas ? es.canvasRegionPos.y : viewport->WorkPos.y - viewport->Pos.y;
    const ImVec2 origin(viewport->Pos.x + (haveCanvas ? es.canvasRegionPos.x : 0.0f),
                        viewport->Pos.y + top + ImGui::GetFrameHeight() + 16.0f);
    const float width = haveCanvas ? es.canvasRegionSize.x : viewport->WorkSize.x;

    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 2.2f;
    const char* title = "BLACKOUT";
    const char* detail = "every projector output is black  -  Ctrl+B or Output > Blackout to restore";
    const ImVec2 titleSize = font->CalcTextSizeA(size, FLT_MAX, 0.0f, title);
    const ImVec2 detailSize = ImGui::CalcTextSize(detail);
    const float height = titleSize.y + detailSize.y + 16.0f;

    ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(200, 20, 20, 235));
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
    draw->AddText(font, size, ImVec2(origin.x + (width - titleSize.x) * 0.5f, origin.y + 4.0f),
                  IM_COL32(255, 255, 255, 255), title);
    draw->AddText(ImVec2(origin.x + (width - detailSize.x) * 0.5f, origin.y + 8.0f + titleSize.y),
                  IM_COL32(255, 255, 255, 255), detail);
}

// While the blend test pattern is on (ADR-047 §3), a banner across the bottom of the
// canvas says so: the projectors show the pattern, not the show.
void Application::drawBlendTestPatternBanner() {
    const EditorState& es = *m_editorState;
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool haveCanvas = es.canvasRegionSize.x > 1.0f && es.canvasRegionSize.y > 1.0f;
    const char* text = "BLEND TEST PATTERN ON  -  the projectors show the test pattern, not the show";
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const float height = textSize.y + 12.0f;
    const float width = haveCanvas ? es.canvasRegionSize.x : viewport->WorkSize.x;
    const float bottom = haveCanvas ? es.canvasRegionPos.y + es.canvasRegionSize.y
                                    : viewport->WorkPos.y - viewport->Pos.y + viewport->WorkSize.y;
    const ImVec2 origin(viewport->Pos.x + (haveCanvas ? es.canvasRegionPos.x : 0.0f),
                        viewport->Pos.y + bottom - height - 8.0f);

    ImDrawList* draw = ImGui::GetForegroundDrawList(viewport);
    draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(230, 150, 0, 235));
    draw->AddRect(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(0, 0, 0, 255), 0.0f, 0, 2.0f);
    draw->AddText(ImVec2(origin.x + (width - textSize.x) * 0.5f, origin.y + 6.0f),
                  IM_COL32(0, 0, 0, 255), text);
}

void Application::saveProject() {
    m_editorState->pendingFileAction = EditorState::FileAction::Save;
}

void Application::loadProject() {
    m_editorState->pendingLoadPath = m_editorState->projectPath;
    m_editorState->pendingFileAction = EditorState::FileAction::Load;
}

void Application::requestExit() {
    if (m_exitConfirmed || !m_editorState) return;  // our own confirmed close: let it through
    glfwSetWindowShouldClose(m_window, GLFW_FALSE);
    m_editorState->pendingFileAction = EditorState::FileAction::Exit;
}

void Application::runFrameBoundaryTasks() {
    if (!m_session->ensureCleanBaseline()) return;
    m_session->processPendingFileAction();
    adoptObjectMeshes(*m_editorState, m_outputManager.get());
    processExternalControl();
    m_session->pollMissingMediaSearch();
    if (m_sourceManager) releaseUnusedSources(*m_editorState, *m_sourceManager, m_mediaLibrary.get());
    fitProjectorsToOutputs();
    m_session->autosaveIfDue();
}

// Keep each output's projector camera matched to what it shows now (ADR-052): its
// current rotation-aware size and, while uncalibrated, framed on its scene's objects.
// Every render and calibration path then reads a camera that is already right.
void Application::fitProjectorsToOutputs() {
    if (!m_outputManager) return;
    m_outputManager->followOutputSizes(m_editorState->scenes);
    for (const auto& output : m_outputManager->getOutputs()) {
        if (!output) continue;
        const IVec2 size = output->getEffectiveResolution();
        const Scene* scene = OutputManager::sceneForOutput(m_editorState->scenes, *output);
        Vec3 lo, hi;
        const bool framed = scene && scene->visibleObjectBounds(lo, hi);
        output->getProjector() = ProjectorCamera::fitToOutput(
            output->getProjector(), size.x, size.y, framed ? &lo : nullptr, framed ? &hi : nullptr);
    }
}

// Once per frame after the sources advance (ADR-050 §2). Heard: sources of the active
// scene and of scenes on enabled outputs, cross-faded with the picture during a scene
// fade (ADR-047 §5); the rest are gated silent, still in step.
void Application::syncAudio() {
    if (!m_audioEngine || !m_sourceManager || !m_editorState || !m_outputManager) return;
    const EditorState& es = *m_editorState;
    syncSourceAudio(*m_audioEngine, *m_sourceManager,
                    heardSourceGains(es.scenes, es.activeSceneId, *m_outputManager));
}

// The output device is a machine preference (ADR-050 §5): written when the user picks
// another one in the menu bar's audio menu or the Preferences window.
void Application::saveAudioDevicePreference() {
    std::string& saved = m_preferences->getPreferences().performance.audioDevice;
    if (!m_audioEngine || m_audioEngine->preferredDevice() == saved) return;
    saved = m_audioEngine->preferredDevice();
    m_preferences->savePreferences();
}

void Application::renderSessionDialogs() {
    using FileAction = EditorState::FileAction;
    EditorState& es = *m_editorState;

    constexpr const char* kUnsavedPopup = "Unsaved Changes##session";
    if (es.unsavedPromptFor != FileAction::None && !ImGui::IsPopupOpen(kUnsavedPopup)) {
        ImGui::OpenPopup(kUnsavedPopup);
    }
    if (ImGui::BeginPopupModal(kUnsavedPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This project has unsaved changes.");
        ImGui::TextUnformatted("Save them before continuing?");
        ImGui::Spacing();
        const FileAction deferred = es.unsavedPromptFor;
        if (ImGui::Button("Save")) {
            es.saveBeforeAction = true;
            es.pendingFileAction = deferred;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            es.discardConfirmed = true;
            es.pendingFileAction = deferred;
        }
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (es.pendingFileAction != FileAction::None || cancel) {
            es.unsavedPromptFor = FileAction::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    constexpr const char* kRecoveryPopup = "Recover Unsaved Work##session";
    if (!es.recoveryFilePath.empty() && !ImGui::IsPopupOpen(kRecoveryPopup)) {
        ImGui::OpenPopup(kRecoveryPopup);
    }
    if (ImGui::BeginPopupModal(kRecoveryPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(es.recoveryProjectPath.empty()
            ? "QUADmapper found unsaved work from an untitled project."
            : "QUADmapper found unsaved work newer than this project file.");
        ImGui::TextDisabled("%s", es.recoveryFilePath.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Restore")) {
            es.pendingFileAction = FileAction::RestoreRecovery;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            es.pendingFileAction = FileAction::DiscardRecovery;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// A plain window, not a modal: the unsaved-changes and recovery modals can open over
// it, and closed it draws nothing, so it never holds the canvas keys.
void Application::renderMissingMediaDialog() {
    EditorState& es = *m_editorState;
    if (!es.showMissingMedia) return;
    const std::vector<MissingMediaItem> items = collectMissingMedia(es);
    const bool searching = m_session->isSearchingMedia();
    if (items.empty() && !searching) {
        es.showMissingMedia = false;  // all found, or the project that had them is gone
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 380.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Missing Media##relocate", &open,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextWrapped("%zu file%s used by this project could not be opened. What uses them "
                           "shows a magenta checker here and black on the projectors until the "
                           "file is found. Saving keeps the original paths, so nothing is lost "
                           "if you skip this.",
                           items.size(), items.size() == 1 ? "" : "s");
        const bool busy = searching || m_session->isFileDialogBusy();
        if (searching) ImGui::TextDisabled("Searching %s ...", m_session->mediaSearchFolder().c_str());
        ImGui::Spacing();

        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##missing", 3, tableFlags, ImVec2(0.0f, -footer))) {
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < items.size(); ++i) {
                const MissingMediaItem& item = items[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(utf8FromPath(pathFromUtf8(item.path).filename()).c_str());
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", item.path.c_str());
                auto error = es.missingMediaErrors.find(item.path);
                if (error != es.missingMediaErrors.end()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error->second.c_str());
                }
                ImGui::PopTextWrapPos();

                ImGui::TableNextColumn();
                std::string usedBy;
                if (item.quadUses > 0) {
                    usedBy = std::to_string(item.quadUses) + (item.quadUses == 1 ? " quad" : " quads");
                }
                if (item.objectUses > 0) {
                    if (!usedBy.empty()) usedBy += ", ";
                    usedBy += std::to_string(item.objectUses) +
                              (item.objectUses == 1 ? " 3D object" : " 3D objects");
                }
                if (item.kind == MissingMediaItem::Kind::Mesh) usedBy += " (mesh)";
                ImGui::TextUnformatted(usedBy.c_str());

                ImGui::TableNextColumn();
                ImGui::BeginDisabled(busy);
                if (ImGui::Button("Locate...")) m_session->requestLocateMissingMedia(item);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Search Folder...")) m_session->requestMissingMediaSearch();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Pick a folder: every missing file with the same name in it,\n"
                              "or in any folder below it, is relocated.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip")) open = false;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Keep working without these files. File > Relocate Missing Media...\n"
                              "reopens this window.");
        }
    }
    ImGui::End();
    if (!open) es.showMissingMedia = false;
}

void Application::emergencySave() noexcept {
    if (m_session) m_session->emergencySave();
}

void Application::renderProjectorFrames() {
    if (!m_outputManager || !m_editorState || !m_renderer) {
        return;
    }

    EditorState& es = *m_editorState;

    // One-Flash: when a calibration pattern is armed, flash it fullscreen on the
    // outputs instead of the scene (QM-059).
    if (m_aiAssist->renderPatternFrames(*m_outputManager, *m_renderer)) return;

    // Resolve every quad's texture (image via SourceManager/mediaId, video via the
    // legacy wrapper) through the shared builder, with the projector's missing-media
    // look (black).
    auto textures = buildQuadTextureMap(es, m_sourceManager.get(), SurfaceView::Output);
    m_outputManager->renderOutputFrames(es.scenes, textures, *m_renderer);
}

void Application::shutdown() {
    if (!m_window) {
        return; // Already shut down
    }

    Logger::info("Shutting down QUADmapper...");
    if (m_session) {
        m_session->finishAutosaveWrite();  // never cut an autosave off mid-write
        m_session->cancelMissingMediaSearch();
    }
    m_session.reset();  // it references the components below

    // Cleanup components in reverse order of creation
    m_externalControl.reset(); // joins the OSC thread, closes MIDI devices
    m_audioEngine.reset();     // stops every video's audio, then closes the device
    if (m_aiAssist) m_aiAssist->stopSidecar();  // kills any launched sidecar (no orphan)
    m_outputManager.reset();

    // Everything below owns GL objects in the editor's context, so it goes while that
    // context is still alive and current (glfwTerminate below destroys it).
    glfwMakeContextCurrent(m_window);
    // Sources after the audio engine: each video's audio is freed once the mixer can no
    // longer reach it; then the decode and copier threads stop and the textures, YUV
    // planes and pixel buffers are deleted.
    m_sourceManager.reset();
    m_thumbnailCache.reset();
    m_aiAssist.reset();  // the One-Flash pattern texture; holds an EditorState&, so goes first
    // Quad textures, missing-media looks, the calibration view, media-bin thumbnails.
    m_editorState.reset();
    m_renderer.reset();
    m_folderWatcher.reset();  // Stop watching before destroying media library
    m_mediaLibrary.reset();
    m_project.reset();

    // Anything counted still alive now would be deleted after its context is gone.
    const GlObjectStats::Snapshot alive = GlObjectStats::Snapshot::capture();
    if (alive.textures != 0 || alive.framebuffers != 0 || alive.vertexArrays != 0 ||
        alive.vertexBuffers != 0 || alive.shaderPrograms != 0) {
        Logger::warning("GL objects still alive at context teardown:\n{}",
                        alive.diff(GlObjectStats::Snapshot{}));
    }

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
