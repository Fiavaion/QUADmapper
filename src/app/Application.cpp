#include "Application.hpp"
#include "../editor/ActionRegistry.hpp"
#include "../editor/EditActions.hpp"
#include "../editor/EditorContext.hpp"
#include "../editor/EditorState.hpp"
#include "../editor/TextureResolve.hpp"
#include "../editor/MediaAssign.hpp"
#include "../editor/MissingMedia.hpp"
#include "../editor/NewQuads.hpp"
#include "../editor/ProjectorFit.hpp"
#include "../editor/ProjectMapping.hpp"
#include "../editor/SceneTakes.hpp"
#include "../editor/ReplaceMedia.hpp"
#include "../editor/MeshLoads.hpp"
#include "../editor/SliceEditing.hpp"
#include "../editor/ControlBindings.hpp"
#include "AiAssist.hpp"
#include "ProjectSession.hpp"
#include "../editor/StatusLine.hpp"
#include "../control/ExternalControl.hpp"
#include "../core/Logger.hpp"
#include "../core/FileSystem.hpp"
#include "../core/GlObjectStats.hpp"
#include "../core/MainThreadTimings.hpp"
#include "../core/Version.hpp"
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
#include "../renderer/Renderer.hpp"
#include "../renderer/Texture.hpp"
#include "../renderer/GpuCapabilities.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../ui/Theme.hpp"
#include "../ui/EditorUi.hpp"
#include "../ui/ShortcutRouter.hpp"
#include "../ui/dialogs/PreferencesDialog.hpp"
#include "../audio/AudioEngine.hpp"
#include "../audio/AudioMixer.hpp"
#include "../editor/SourceAudio.hpp"
#include "../video/VideoPlayer.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
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

static bool isProjectFile(const std::filesystem::path& path) {
    std::string ext = utf8FromPath(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".qmap";
}

// The log of a normal run: quadmapper.log in the logs folder under userLocalDir(). Once
// it is large, the next start keeps it as quadmapper.previous.log and begins a new one.
static void startLogFile() {
    namespace fs = std::filesystem;
    constexpr std::uintmax_t ROTATE_BYTES = 2 * 1024 * 1024;
    std::error_code ec;
    const fs::path dir = userLocalDir() / "logs";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "quadmapper.log";
    const std::uintmax_t size = fs::file_size(file, ec);
    if (!ec && size > ROTATE_BYTES) fs::rename(file, dir / "quadmapper.previous.log", ec);
    Logger::enableFileOutput(utf8FromPath(file));
}

// GLFW file drop callback for external drag-drop: media goes into the library, a .qmap
// is opened as the show.
static void dropCallback(GLFWwindow* /*window*/, int count, const char** paths) {
    Application& app = Application::instance();
    MediaLibrary* library = app.getMediaLibrary();
    if (!library) return;

    int imported = 0;
    for (int i = 0; i < count; i++) {
        const std::filesystem::path filePath = pathFromUtf8(paths[i]);  // GLFW drops are UTF-8
        if (isProjectFile(filePath)) {
            app.queueProjectLoad(paths[i]);
            continue;
        }
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

Application::Application() : m_sceneTakes(std::make_unique<SceneTakeQueue>()) {
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
    // Headless harness flags: --stress (QM-081) and --bench (PRD §5 benchmark). Any
    // other argument naming a .qmap is the show to open (a double-clicked file).
    std::string startupProject;
    for (int i = 1; i < argc; ++i) {
        if (parseStressArg(argv[i], m_stressConfig) || parseBenchArg(argv[i], m_benchConfig)) continue;
        if (startupProject.empty() && isProjectFile(pathFromUtf8(argv[i]))) startupProject = argv[i];
    }
    if (!isHarnessRun()) startLogFile();
    // Only the harnesses measure the main thread (TD-35): a show never pays for the
    // samples' lock and allocations, nor keeps them for its whole run.
    if (m_stressConfig || m_benchConfig) MainThreadTimings::markMainThread();
    if (m_benchConfig) {
        // stdout carries only the JSON report; the log goes to a file beside it.
        Logger::setConsoleOutput(false);
        Logger::enableFileOutput(m_benchConfig->logPath());
    }

    Logger::info("Initializing QUADmapper {}...", APP_VERSION);

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
    // run) or an unreadable one leaves the defaults. An unreadable file is kept aside,
    // because the next preference change saves over it.
    m_preferences = std::make_unique<PreferencesDialog>();
    std::string prefsWarning;
    if (!m_preferences->loadPreferences()) {
        const std::filesystem::path prefsPath = AppPreferences::getDefaultConfigPath();
        std::error_code ec;
        if (std::filesystem::exists(prefsPath, ec)) {
            std::filesystem::path kept = prefsPath;
            kept += ".bad";
            std::filesystem::copy_file(prefsPath, kept, std::filesystem::copy_options::overwrite_existing, ec);
            prefsWarning = ec ? "Preferences could not be read; using the defaults"
                              : "Preferences could not be read; using the defaults (old file kept as preferences.json.bad)";
        }
    }

    if (!initializeComponents()) {
        Logger::error("Failed to initialize components");
        return false;
    }
    if (!prefsWarning.empty()) postStatus(*m_editorState, prefsWarning);

    // The stress and bench harnesses measure the render/perform path; they open no
    // ports or devices.
    if (!isHarnessRun()) {
        initializeExternalControl();
        if (!startupProject.empty()) queueProjectLoad(startupProject);
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
    // Maximised for a person; the harnesses keep the fixed size their numbers assume.
    glfwWindowHint(GLFW_MAXIMIZED, isHarnessRun() ? GLFW_FALSE : GLFW_TRUE);

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
    // claimCanvasEditingKeys(), which EditorUi::frame() calls first after NewFrame().
    io.ConfigNavCaptureKeyboard = false;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;    // Enable multi-viewport

    // The window layout is the user's, kept with their settings: never in the folder
    // the app happened to be started from. The harnesses use the default layout.
    if (isHarnessRun()) {
        io.IniFilename = nullptr;
    } else {
        m_layoutFile = utf8FromPath(userConfigDir() / "layout.ini");
        io.IniFilename = m_layoutFile.c_str();
    }

    // Apply QUADmapper "Neon Void" theme
    Theme::apply();

    // Load custom fonts (will fall back to default if not found)
    glfwGetWindowContentScale(m_window, &m_monitorScale, nullptr);
    if (m_monitorScale <= 0.0f) m_monitorScale = 1.0f;
    Theme::loadFonts(m_monitorScale);

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
    m_sourceManager->prepareGpu();

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
        m_editorState->vertexSnapping = prefs.general.vertexSnapping;
        m_editorState->uvGridOnNewQuads = prefs.general.testGridOnNewQuads;
        m_editorState->showOtherScenes = prefs.general.showOtherScenes;
        m_editorState->newGridPointsLinear = prefs.general.newGridPointsLinear;
        m_editorState->cropInPercent = prefs.general.cropInPercent;

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
            // On the watcher's thread: the library belongs to the main thread, which
            // is reading it for the media bin, so the file is only queued here.
            m_folderWatcher->setCallback([this](const std::filesystem::path& path, bool isNew) {
                if (!isNew) return;
                std::lock_guard<std::mutex> lock(m_watchedFilesMutex);
                m_watchedFiles.push_back(path);
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
        services.dropQueuedSceneTakes = [this]() { m_sceneTakes->clear(); };
        services.projectReplaced = [this]() {
            EditorContext ctx = editorContext();
            m_ui->projectReplaced(ctx);
        };
        services.projectUsed = [this](const std::string& path) {
            if (isHarnessRun()) return;  // a harness run must not touch the user's preferences
            addRecentProject(m_preferences->getPreferences().recentProjects, path);
            savePreferences();
        };
        services.confirmExit = [this]() {
            m_exitConfirmed = true;
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        };
        m_session = std::make_unique<ProjectSession>(*m_project, *m_editorState,
                                                     m_preferences->getPreferences(),
                                                     std::move(services));
    }

    // Projectors found at startup (owner, 2026-11-23): every display gets an output at
    // its own resolution and a scene of its own (Scene 1 on Projector 1), the editor's
    // screen last (F11 covers the editor too). With the editor's screen the only one, a single Automatic
    // "Projector 1" (ADR-039 P2) resolves to none (QM-090) and F11 previews it on that
    // screen. There is always >= 1 output, so the F11 lazy-create path is a no-op.
    // Harness runs keep the single default output, whatever this computer has.
    {
        if (!isHarnessRun()) m_outputManager->addOutputsForUnusedDisplays(/*includeEditorScreen=*/true);
        if (m_outputManager->getOutputCount() == 0) {
            m_outputManager->createOutput("Projector 1");
            m_outputManager->resolveOutputs();
        }
        std::vector<OutputWindow*> outputs;
        for (auto& output : m_outputManager->getOutputs()) outputs.push_back(output.get());
        giveOutputsTheirOwnScenes(*m_editorState, *m_outputManager, outputs);
        for (OutputWindow* output : outputs) {
            Logger::info("Output '{}' on monitor {} at {}x{}", output->getName(),
                         output->getMonitor(), output->getResolution().x, output->getResolution().y);
        }
    }
    m_outputManager->setMonitorChangeCallback([this]() { m_displaysChanged = true; });

    registerActions();

    // The editor's UI (ADR-051 §2): its panels take what they need from the context.
    m_ui = std::make_unique<EditorUi>(*m_preferences);
    {
        EditorContext ctx = editorContext();
        m_ui->attach(ctx);
    }

    Logger::debug("All components initialized");
    return true;
}

EditorContext Application::editorContext() {
    EditorContext ctx{*m_editorState, m_project.get(), m_mediaLibrary.get(),
                      m_sourceManager.get(), m_outputManager.get(), m_renderer.get(), m_window};
    ctx.audio = m_audioEngine.get();
    ctx.thumbnails = m_thumbnailCache.get();
    ctx.control = m_externalControl.get();
    ctx.prefs = &m_preferences->getPreferences();
    ctx.services = this;
    ctx.actions = m_actions.get();
    return ctx;
}

// ---- EditorServices: what the panels ask of the app --------------------------------

bool Application::unsavedLabelShown() { return m_session->unsavedLabelShown(glfwGetTime()); }
bool Application::isSearchingMedia() const { return m_session->isSearchingMedia(); }
bool Application::isFileDialogBusy() const { return m_session->isFileDialogBusy(); }
std::string Application::mediaSearchFolder() const { return m_session->mediaSearchFolder(); }
void Application::requestLocateMissingMedia(const MissingMediaItem& item) {
    m_session->requestLocateMissingMedia(item);
}
void Application::requestMissingMediaSearch() { m_session->requestMissingMediaSearch(); }
void Application::savePreferences() { m_preferences->savePreferences(); }
bool Application::aiActive() const { return m_aiAssist->isActive(); }
bool Application::aiBusy() const { return m_aiAssist->isBusy(); }

void Application::runAi(AiAction action) { m_aiAssist->run(action, m_outputManager.get()); }
bool Application::aiCanSegment() const { return m_aiAssist->canSegment(); }
const AiPhoto& Application::aiPhoto() const { return m_aiAssist->photo(); }
void Application::aiSegmentAt(float x, float y) { m_aiAssist->segmentAt(x, y); }
bool Application::aiCanScan() const { return m_aiAssist->canScan(); }
bool Application::aiScanning() const { return m_aiAssist->isScanning(); }
bool Application::aiHasScan(const UUID& outputId) const {
    return m_aiAssist->currentGrayMap(outputId, m_outputManager.get()) != nullptr;
}
void Application::aiCalibrationPixelAt(float x, float y) {
    m_aiAssist->calibrationPixelAt(x, y, m_outputManager.get());
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

        // AI Assist is a beta: turning it off in Preferences gives the show back and
        // stops the engine.
        const bool aiBeta = m_preferences->getPreferences().general.aiAssistBeta;
        if (m_aiBetaWasOn && !aiBeta) m_aiAssist->disable();
        m_aiBetaWasOn = aiBeta;

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
        m_outputManager->retireIdentify();
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

    // The first quad of a fresh session and the UV test grid on new quads (NewQuads.hpp).
    prepareNewQuads(*m_editorState);

    float deltaTime = ImGui::GetIO().DeltaTime;

    // Update all media sources (ADR-040 P2) and upload any new frames to GPU —
    // main thread / main GL context, once per frame, before any quad is drawn.
    if (m_sourceManager) {
        m_sourceManager->updateAll(static_cast<double>(deltaTime));
        adoptFailedOpens(*m_editorState, *m_sourceManager);  // undecodable clips: missing media
        // Each video's sound follows the clock it just advanced (ADR-050 §2).
        syncAudio();
    }

    // Interface size: the fonts were loaded at the monitor's scale, so a fixed choice
    // scales them by choice / monitor scale. Fonts are dynamic (ImGui 1.92): no atlas
    // rebuild, and it is set here, outside any frame.
    {
        const int percent = m_preferences->getPreferences().general.uiScalePercent;
        ImGui::GetStyle().FontScaleMain = percent == 0 ? 1.0f : (percent / 100.0f) / m_monitorScale;
    }

    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // The editor's frame (EditorUi::frame): canvas keys, the canvas itself (straight
    // after NewFrame, TD-21), the dockspace, the menu bar, the canvas overlay, the
    // shortcuts, this frame's scene takes, every panel, then calibration, the banners
    // and the session modals on top.
    EditorContext ctx = editorContext();
    m_ui->frame(ctx, [this](EditorContext& frameCtx) { runSceneTakes(frameCtx); });

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
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* backupContext = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backupContext);
    }
}

void Application::queueSceneTake(int number) { m_sceneTakes->keys.push_back(number); }

// Scene takes (US-008, ADR-048; editor/SceneTakes.hpp): the 1-9 presses queued since the
// last frame, unless typed into a field or behind a popup, then the OSC/MIDI takes.
void Application::runSceneTakes(EditorContext& ctx) {
    runSceneTakeKeys(*m_actions, ctx, m_sceneTakes->keys);
    runExternalSceneTakes(*m_actions, ctx, *m_sceneTakes);
}

void Application::takeSceneByNumber(int number, std::optional<float> fadeSeconds) {
    // The harnesses cut whatever this machine's preference is, so their scene-switch
    // timings and same-frame readbacks do not depend on it.
    const bool harness = m_stressConfig.has_value() || m_benchConfig.has_value();
    const float fade = fadeSeconds ? *fadeSeconds
                                   : (harness ? 0.0f : m_preferences->getPreferences().general.sceneFadeSeconds);
    quadmapper::takeSceneByNumber(*m_editorState, *m_outputManager, number, fade);
}

void Application::initializeExternalControl() {
    m_externalControl = std::make_unique<control::ExternalControl>();

    ControlBindingHost host;
    host.scenes = &m_editorState->scenes;
    host.sources = m_sourceManager.get();
    host.takeScene = [this](int number, std::optional<float> fadeSeconds) {
        m_sceneTakes->external.push_back({number, fadeSeconds});
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

void Application::saveProject() {
    m_editorState->pendingFileAction = EditorState::FileAction::Save;
}

void Application::requestExit() {
    if (m_exitConfirmed || !m_editorState) return;  // our own confirmed close: let it through
    glfwSetWindowShouldClose(m_window, GLFW_FALSE);
    m_editorState->pendingFileAction = EditorState::FileAction::Exit;
}

void Application::queueProjectLoad(const std::string& path) {
    EditorState& es = *m_editorState;
    // A file action already waiting, or its unsaved-changes prompt, wins.
    if (es.pendingFileAction != EditorState::FileAction::None ||
        es.unsavedPromptFor != EditorState::FileAction::None) {
        postStatus(es, "Not opened: another file action is pending");
        return;
    }
    es.pendingLoadPath = path;
    es.pendingFileAction = EditorState::FileAction::Load;
}

void Application::importWatchedFiles() {
    std::vector<std::filesystem::path> files;
    {
        std::lock_guard<std::mutex> lock(m_watchedFilesMutex);
        files.swap(m_watchedFiles);
    }
    for (const std::filesystem::path& file : files) {
        if (m_mediaLibrary->addItem(file)) {
            Logger::info("FolderWatcher: Auto-imported new file: {}", utf8FromPath(file.filename()));
        }
    }
}

void Application::updateWindowTitle() {
    // The unsaved check serializes the show (at most twice a second, UnsavedIndicator):
    // not while the show is on the projectors, and never in a harness run.
    if (isHarnessRun() || (m_outputManager && m_outputManager->isInPerformMode())) return;
    const std::string& project = m_project->getFilePath();
    const bool dirty = unsavedLabelShown();
    if (m_titleShown && dirty == m_titleDirty && project == m_titleProject) return;
    m_titleShown = true;
    m_titleDirty = dirty;
    m_titleProject = project;
    const std::string name =
        project.empty() ? "Untitled" : utf8FromPath(pathFromUtf8(project).filename());
    m_windowTitle = name + (dirty ? "*" : "") + " - QUADmapper";
    glfwSetWindowTitle(m_window, m_windowTitle.c_str());
}

void Application::runFrameBoundaryTasks() {
    importWatchedFiles();
    if (!m_session->ensureCleanBaseline()) return;
    m_session->processPendingFileAction();
    adoptObjectMeshes(*m_editorState, m_outputManager.get());
    if (m_externalControl) {
        processExternalControl(*m_externalControl, m_editorState->scenes, m_editorState->midiMappings,
                               m_sourceManager.get(), glfwGetTime(),
                               m_ui && m_ui->panels().isVisible(ControlPanel::ID));
    }
    m_session->pollMissingMediaSearch();
    if (m_sourceManager) releaseUnusedSources(*m_editorState, *m_sourceManager, m_mediaLibrary.get());
    fitProjectorsToOutputs();
    adoptConnectedDisplays();
    m_session->autosaveIfDue();
    updateWindowTitle();
}

void Application::adoptConnectedDisplays() {
    if (!m_displaysChanged || !m_outputManager || isHarnessRun()) return;
    EditorState& es = *m_editorState;
    // Outputs are show content: leave them alone while the show runs; done afterwards.
    if (m_outputManager->isInPerformMode() || es.performLock.isOn() || es.aiScan.running) return;
    m_displaysChanged = false;

    std::string message;
    auto say = [&message](const std::string& part) {
        message += (message.empty() ? "" : "; ") + part;
    };
    for (const UUID& id : m_outputManager->takeOutputsPlacedByHotplug()) {
        OutputWindow* output = m_outputManager->getOutput(id);
        const MonitorInfo* monitor = output ? m_outputManager->getMonitor(output->getMonitor()) : nullptr;
        if (monitor && m_outputManager->adoptDisplayResolution(*output)) {
            say(output->getName() + " now uses " + monitor->name + " at " +
                std::to_string(monitor->resolution.x) + "x" + std::to_string(monitor->resolution.y));
        }
    }
    const std::vector<OutputWindow*> added = m_outputManager->addOutputsForUnusedDisplays();
    giveOutputsTheirOwnScenes(es, *m_outputManager, added);
    for (OutputWindow* output : added) {
        const MonitorInfo* monitor = m_outputManager->getMonitor(output->getMonitor());
        const Scene* scene = OutputManager::sceneForOutput(es.scenes, *output);
        say(output->getName() + " added for " + (monitor ? monitor->name : std::string("a new display")) +
            (scene ? ", showing " + scene->getName() : std::string()));
    }
    if (!message.empty()) postStatus(es, message);
}

// See editor/ProjectorFit.hpp (kept framing under the perform lock, TD-36).
void Application::fitProjectorsToOutputs() {
    if (m_outputManager) quadmapper::fitProjectorsToOutputs(*m_editorState, *m_outputManager);
}

// Once per frame after the sources advance (ADR-050 §2). Heard: sources of the active
// scene and of scenes on enabled outputs, cross-faded with the picture during a scene
// fade (ADR-047 §5); the rest are gated silent, still in step.
void Application::syncAudio() {
    ScopedMainThreadTiming timing("syncAudio");
    if (!m_audioEngine || !m_sourceManager || !m_editorState || !m_outputManager) return;
    const EditorState& es = *m_editorState;
    syncSourceAudio(*m_audioEngine, *m_sourceManager,
                    heardSourceGains(es.scenes, es.activeSceneId, *m_outputManager));
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
    // Media-bin thumbnails and the calibration view, then quad textures and the
    // missing-media looks (the panels refer to the editor state, so go first).
    m_ui.reset();
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
