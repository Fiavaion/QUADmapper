#include "Application.hpp"
#include "EditorContext.hpp"
#include "EditorState.hpp"
#include "TextureResolve.hpp"
#include "MediaAssign.hpp"
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
#include "../source/CameraSource.hpp"
#include "../source/CameraDevices.hpp"
#include "../ai/SidecarClient.hpp"
#include "../renderer/UvTestGrid.hpp"
#include "../renderer/Renderer.hpp"
#include "../renderer/Texture.hpp"
#include "../renderer/GpuCapabilities.hpp"
#include "../output/OutputManager.hpp"
#include "../output/OutputWindow.hpp"
#include "../input/InputManager.hpp"
#include "../ui/Theme.hpp"
#include "../ui/panels/OutputPanel.hpp"
#include "../ui/panels/MediaBinPanel.hpp"
#include "../ui/panels/QuadListPanel.hpp"
#include "../ui/dialogs/PreferencesDialog.hpp"
#include "../video/VideoPlayer.hpp"

#include "../core/GlObjectStats.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <filesystem>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <imgui_internal.h>  // For DockBuilder API
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
// H3: working-set backstop (spec §2).  psapi is a system lib — no vcpkg dep.
// NOMINMAX prevents windows.h from defining min/max macros that clash with std::max.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

namespace quadmapper {

Application* Application::s_instance = nullptr;

// GLFW error callback
static void glfwErrorCallback(int error, const char* description) {
    Logger::error("GLFW Error {}: {}", error, description);
}

// ---- Editor <-> Project persistence bridge (ADR-039 P5) ----
// The live state of truth is EditorState's scene list (Scene/QuadManager) plus the
// OutputManager's output config; the Project DTO is only the serialization shape.
// These two helpers are the single place that copies between them. Save gathers EVERY
// scene + output into the DTO (the serializer then writes .qmap v3); load rebuilds the
// live scenes/outputs and remaps each scene's output assignment onto the freshly
// created output ids. Quad geometry + texture binding (incl. mesh offsets, flipH/V,
// colour, soft edge) copy whole, so nothing is dropped on round-trip.
static void syncEditorToProject(EditorState& es, OutputManager* om, Project& project) {
    project.getScenes().clear();
    project.getOutputs().clear();

    for (const auto& scenePtr : es.scenes) {
        if (!scenePtr) continue;
        Scene& sc = *scenePtr;
        Project::SceneData sd;
        sd.id = sc.getId();
        sd.name = sc.getName();
        sd.assignedOutputId = sc.getAssignedOutputId();
        const SceneViewState& vs = sc.getViewState();
        sd.view.pan = vs.panOffset;
        sd.view.zoom = vs.zoomLevel;
        sd.view.gridVisible = vs.gridVisible;
        sd.view.snapToGrid = vs.snapToGrid;
        for (const auto& quadPtr : sc.getQuadManager().getQuads()) {
            sd.quads.push_back(*quadPtr);
            auto it = es.quadTexturePaths.find(quadPtr->getId());
            sd.texturePaths.push_back(
                it != es.quadTexturePaths.end() ? it->second : std::string());
        }
        for (const auto& groupPtr : sc.getGroups()) {
            if (groupPtr) sd.groups.push_back(*groupPtr);
        }
        project.getScenes().push_back(std::move(sd));
    }

    if (om) {
        for (const auto& outPtr : om->getOutputs()) {
            if (!outPtr) continue;
            Project::OutputData od;
            od.id = outPtr->getId();
            od.name = outPtr->getName();
            od.monitor = outPtr->getMonitor();
            od.resolution = outPtr->getResolution();
            od.rotationDegrees = outPtr->getRotationDegrees();
            od.enabled = outPtr->isEnabled();
            project.getOutputs().push_back(std::move(od));
        }
    }
}

// Copy a DTO quad's full state onto a freshly created live quad.
static void applyQuadData(const Quad& src, Quad* dst) {
    dst->setVisible(src.isVisible());
    dst->setLocked(src.isLocked());
    dst->setOpacity(src.getOpacity());
    dst->setZOrder(src.getZOrder());
    dst->getGeometry() = src.getGeometry();  // corners + grid + mode + mesh offsets
    dst->getTexture() = src.getTexture();    // UV transform + flip flags
    dst->setColorCorrection(src.getColorCorrection());  // ADR-036
    dst->setSoftEdge(src.getSoftEdge());                // ADR-037
}

static void syncProjectToEditor(EditorState& es, OutputManager* om, const Project& project,
                                SourceManager* sm) {
    es.scenes.clear();
    es.quadTextures.clear();
    es.quadTexturePaths.clear();
    if (sm) sm->clear();  // drop the previous project's media sources (ADR-040 P2)

    // Rebuild outputs from the file (v3) so scene assignments remap onto the freshly
    // created output ids. v1/v2 files carry no outputs — keep the existing ones (e.g.
    // the startup default Projector 1) untouched.
    std::unordered_map<UUID, UUID> outputRemap;  // saved output id -> live output id
    if (om && !project.getOutputs().empty()) {
        std::vector<UUID> existing;
        for (const auto& o : om->getOutputs()) {
            if (o) existing.push_back(o->getId());
        }
        for (const UUID& id : existing) om->deleteOutput(id);

        for (const auto& od : project.getOutputs()) {
            OutputWindow* ow = om->createOutput(od.name);
            if (!ow) continue;
            ow->setName(od.name);
            ow->setMonitor(od.monitor);
            ow->setResolution(od.resolution);
            ow->setRotationDegrees(od.rotationDegrees);
            ow->setEnabled(od.enabled);
            outputRemap[od.id] = ow->getId();
        }
    }

    for (const auto& sd : project.getScenes()) {
        auto scene = std::make_unique<Scene>(sd.id);
        scene->setName(sd.name);
        if (!sd.assignedOutputId.isNull()) {
            auto it = outputRemap.find(sd.assignedOutputId);
            scene->setAssignedOutputId(it != outputRemap.end() ? it->second : UUID::null());
        }
        SceneViewState& vs = scene->getViewState();
        vs.panOffset = sd.view.pan;
        vs.zoomLevel = sd.view.zoom;
        vs.gridVisible = sd.view.gridVisible;
        vs.snapToGrid = sd.view.snapToGrid;

        QuadManager& qm = scene->getQuadManager();
        std::unordered_map<UUID, UUID> quadRemap;  // saved quad id -> fresh live quad id
        for (size_t i = 0; i < sd.quads.size(); ++i) {
            Quad* nq = qm.createQuad(sd.quads[i].getName());
            quadRemap[sd.quads[i].getId()] = nq->getId();
            applyQuadData(sd.quads[i], nq);
            if (i < sd.texturePaths.size() && !sd.texturePaths[i].empty()) {
                // ADR-040 P2: route the media through SourceManager (deduped by
                // path, so the same file on many quads decodes once) and bind the
                // quad by mediaId. quadTexturePaths is kept as the P2 persistence
                // carrier (P5 serializes mediaId/sources directly).
                es.quadTexturePaths[nq->getId()] = sd.texturePaths[i];
                if (sm) {
                    const std::filesystem::path mediaPath = sd.texturePaths[i];
                    const std::string ext = mediaPath.extension().string();
                    UUID sid;
                    if (MediaLibrary::getTypeFromExtension(ext) == MediaType::VIDEO) {
                        sid = sm->getOrCreateVideoSource(mediaPath);
                        if (!sid.isNull()) es.activeVideoSourceId = sid;  // panel controls it
                    } else {
                        sid = sm->getOrCreateImageSource(mediaPath);
                    }
                    if (!sid.isNull()) nq->getTexture().mediaId = sid;
                }
            }
        }

        // Re-apply quad groups onto the live scene. createQuad/createGroup mint fresh
        // ids, so member + parent references are remapped off the saved ids.
        std::unordered_map<UUID, UUID> groupRemap;  // saved group id -> fresh live group id
        for (const QuadGroup& sg : sd.groups) {
            QuadGroup* ng = scene->createGroup(sg.getName());
            if (!ng) continue;
            groupRemap[sg.getId()] = ng->getId();
            ng->setVisible(sg.isVisible());
            ng->setLocked(sg.isLocked());
            ng->setCollapsed(sg.isCollapsed());
            for (const UUID& memberId : sg.getMemberQuadIds()) {
                auto it = quadRemap.find(memberId);
                if (it != quadRemap.end()) ng->addMember(it->second);
            }
        }
        // Second pass: rewire nested-group parents once every group exists.
        for (const QuadGroup& sg : sd.groups) {
            if (!sg.getParentGroup().has_value()) continue;
            auto childIt = groupRemap.find(sg.getId());
            auto parentIt = groupRemap.find(*sg.getParentGroup());
            if (childIt != groupRemap.end() && parentIt != groupRemap.end()) {
                if (QuadGroup* live = scene->getGroup(childIt->second)) {
                    live->setParentGroup(parentIt->second);
                }
            }
        }

        es.scenes.push_back(std::move(scene));
    }

    // Guarantee >= 1 scene, then make the first one active.
    if (es.scenes.empty()) {
        es.activeScene();  // lazily creates a default scene
    }
    es.setActiveScene(es.scenes.front()->getId());

    // Preserve "launch is aimed at the projector": if outputs exist but no scene is
    // assigned to any (e.g. a v1/v2 file), route the active scene to the first output
    // so it still projects on F11.
    if (om && !om->getOutputs().empty()) {
        bool anyAssigned = false;
        for (const auto& scenePtr : es.scenes) {
            if (scenePtr && scenePtr->hasAssignedOutput()) { anyAssigned = true; break; }
        }
        if (!anyAssigned) {
            es.activeScene().setAssignedOutputId(om->getOutputs().front()->getId());
        }
    }

    // Select the first quad of the active scene + sync the texture-path edit buffer.
    QuadManager& aqm = es.quadManager();
    if (aqm.getQuadCount() > 0) {
        aqm.selectQuad(aqm.getQuads()[0]->getId());
        auto pathIt = es.quadTexturePaths.find(aqm.getQuads()[0]->getId());
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
    m_lastSidecarState = ai::SidecarState::Stopped;  // TD-10 watcher seed
}

Application::~Application() {
    shutdown();
    s_instance = nullptr;
}

Application& Application::instance() {
    return *s_instance;
}

bool Application::initialize(int argc, char* argv[]) {
    // QM-081 H2: parse --stress and its sub-flags into m_stressConfig.
    // Intentionally minimal: no getopt dependency, no env-var fallback.
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--stress") == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
        } else if (std::strncmp(arg, "--cycles=", 9) == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->cycles = std::atoi(arg + 9);
        } else if (std::strncmp(arg, "--outputs=", 10) == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->outputs = std::atoi(arg + 10);
        } else if (std::strncmp(arg, "--frames-per-cycle=", 19) == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->framesPerCycle = std::atoi(arg + 19);
        } else if (std::strcmp(arg, "--fullscreen") == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->fullscreen = true;
        } else if (std::strncmp(arg, "--source=", 9) == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->source = arg + 9;
        } else if (std::strcmp(arg, "--hidden") == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->hidden = true;
        } else if (std::strncmp(arg, "--ws-bound-mb=", 14) == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->workingSetBoundMB = std::atol(arg + 14);
        } else if (std::strncmp(arg, "--cycle-timeout-ms=", 19) == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->cycleTimeoutMs = std::atoi(arg + 19);
        } else if (std::strncmp(arg, "--pause-after=", 14) == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->pauseAfter = std::atoi(arg + 14);
        } else if (std::strncmp(arg, "--capture=", 10) == 0) {
            if (!m_stressConfig) m_stressConfig = StressConfig{};
            m_stressConfig->capture = arg + 10;
        }
    }

    Logger::info("Initializing QUADmapper...");

    m_aiSelfTestPending = (std::getenv("QM_AI_SELFTEST") != nullptr);

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

    // Query BCn texture-compression support once now that a context is current
    // (ADR-044); the HAP upload path gates on this and degrades gracefully.
    GpuCapabilities::get().queryOnce();

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

    // Optional AI pre-show sidecar (ADR-035 / QM-056). Constructed here but not
    // started — the user enables it on demand from the AI menu. We resolve the
    // sidecar script next to the working dir or the repo root; if it is missing,
    // scriptPath stays empty and start() fails cleanly (AI menu shows disabled).
    {
        ai::SidecarConfig sidecarCfg;
        const std::filesystem::path rel("tools/ai-sidecar/sidecar.py");
        for (const std::filesystem::path& base :
             {std::filesystem::current_path(),
              std::filesystem::current_path() / ".." / "..",
              std::filesystem::current_path() / ".." / ".." / ".."}) {
            std::error_code ec;
            std::filesystem::path candidate = base / rel;
            if (std::filesystem::exists(candidate, ec)) {
                sidecarCfg.scriptPath =
                    std::filesystem::weakly_canonical(candidate, ec).string();
                break;
            }
        }
        m_sidecarClient = std::make_unique<ai::SidecarClient>(sidecarCfg);
    }

    // Auto-load media from default folder if configured
    {
        PreferencesDialog prefsDialog;
        prefsDialog.loadPreferences();
        const auto& prefs = prefsDialog.getPreferences();

        // UI complexity mode (ADR-042): default Simple; persisted choice wins.
        m_editorState->uiMode = (prefs.general.uiMode == 1)
            ? EditorState::UiMode::Advanced : EditorState::UiMode::Simple;

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

    // ADR-039 P2: create the default output and route the default scene to it. The
    // default scene targets the 2nd monitor (the projector) when present, else the
    // only display — "launch already aimed at the projector". There is always >= 1
    // output, so the F11 lazy-create path becomes a no-op.
    {
        int monitorCount = static_cast<int>(m_outputManager->getMonitorCount());
        int targetMonitor = (monitorCount > 1) ? 1 : 0;
        OutputWindow* defaultOutput = m_outputManager->createOutput("Projector 1");
        if (defaultOutput) {
            defaultOutput->setMonitor(targetMonitor);
            m_editorState->activeScene().setAssignedOutputId(defaultOutput->getId());
            Logger::info("Default scene assigned to output '{}' on monitor {}",
                         defaultOutput->getName(), targetMonitor);
        }
    }

    // Create input manager
    m_inputManager = std::make_unique<InputManager>();

    Logger::debug("All components initialized");
    return true;
}

int Application::run() {
    // QM-081 H2: stress harness runs as a sibling branch, not a hijack of the UI loop.
    if (m_stressConfig) {
        return runStress(*m_stressConfig);
    }

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

        // TD-10: report the outcome of an async sidecar connect (startAsync runs
        // off the UI thread). When the cached state leaves Starting, post a
        // one-shot status so the user sees connected/failed without a freeze.
        if (m_sidecarClient) {
            const ai::SidecarState s = m_sidecarClient->state();
            if (s != m_lastSidecarState) {
                if (m_lastSidecarState == ai::SidecarState::Starting) {
                    EditorState& es = *m_editorState;
                    const bool ok = (s == ai::SidecarState::Attached ||
                                     s == ai::SidecarState::Launched);
                    snprintf(es.statusMessage, sizeof(es.statusMessage), "%s",
                             ok ? "AI assist enabled (sidecar connected)"
                                : "AI assist unavailable - sidecar could not start");
                    es.statusTimer = 5.0f;
                    if (ok) Logger::info(es.statusMessage);
                    else    Logger::warning(es.statusMessage);
                }
                m_lastSidecarState = s;
            }
        }

        // QM-058 verification hook (off unless QM_AI_SELFTEST is set): once the
        // first frames have built the default scene, enable the sidecar, select a
        // quad, and run one "Detect Surface" headlessly so the full chain can be
        // asserted from the log without driving ImGui menus. No-op in production.
        if (m_aiSelfTestPending && ++m_frameCount >= 3) {
            m_aiSelfTestPending = false;
            runAiSelfTest();
        }
    }

    Logger::info("Main loop ended");
    return 0;
}

void Application::runAiSelfTest() {
    EditorState& es = *m_editorState;
    if (!es.quadManager().getPrimarySelectedQuad() && !es.quadManager().getQuads().empty()) {
        es.quadManager().selectQuad(es.quadManager().getQuads().front()->getId());
    }
    if (m_sidecarClient) {
        m_sidecarClient->start();
    }

    const char* modeEnv = std::getenv("QM_AI_SELFTEST");
    const std::string mode = modeEnv ? modeEnv : "";
    if (mode == "oneflash") {
        // QM-059: arm the pattern, flash it on the projector, capture a photo.
        oneFlashEmitPattern();
        m_outputManager->handleF11Key();   // enter perform mode -> pattern shows
        oneFlashCapturePhoto();
    } else if (mode == "solve") {
        // QM-060: solve the projector->surface homography and apply the warp.
        oneFlashSolveAndApply();
    } else if (mode == "anamorphic") {
        // QM-062: audience-perspective (frontal-from-viewpoint) warp.
        oneFlashAnamorphic();
    } else {
        aiDetectSurface();
    }
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
    auto& quadManager = es.quadManager();
    auto& quadTextures = es.quadTextures;
    auto& quadTexturePaths = es.quadTexturePaths;
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

        // AI pre-show assist (QM-056 skeleton). Active state reads cached client
        // state (no per-frame I/O). Enable launches the sidecar on demand; Detect
        // runs the stub /surface round-trip. Both no-op gracefully if absent.
        es.mainMenuBar.setAiActiveQuery([this]() {
            return m_sidecarClient &&
                   (m_sidecarClient->state() == ai::SidecarState::Attached ||
                    m_sidecarClient->state() == ai::SidecarState::Launched);
        });
        es.mainMenuBar.setOnEnableAi([this]() {
            if (!m_sidecarClient) return;
            // TD-10: connect off the UI thread so the menu never freezes 1-3s.
            // The run loop watches state() and reports the outcome.
            m_sidecarClient->startAsync();
            EditorState& est = *m_editorState;
            snprintf(est.statusMessage, sizeof(est.statusMessage), "%s",
                     "AI: connecting to sidecar...");
            est.statusTimer = 5.0f;
        });
        es.mainMenuBar.setOnDetectSurface([this]() { aiDetectSurface(); });
        es.mainMenuBar.setOnEmitPattern([this]()  { oneFlashEmitPattern();  });
        es.mainMenuBar.setOnCapturePhoto([this]() { oneFlashCapturePhoto(); });
        es.mainMenuBar.setOnClearPattern([this]() { oneFlashClearPattern(); });
        es.mainMenuBar.setOnSolveApply([this]()   { oneFlashSolveAndApply(); });
        es.mainMenuBar.setOnAnamorphic([this]()   { oneFlashAnamorphic(); });
        es.mainMenuBarInitialized = true;
    }

    float deltaTime = ImGui::GetIO().DeltaTime;

    // Update all media sources (ADR-040 P2) and upload any new frames to GPU —
    // main thread / main GL context, once per frame, before any quad is drawn.
    if (m_sourceManager) {
        m_sourceManager->updateAll(static_cast<double>(deltaTime));
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

    // Render each output's assigned scene to an offscreen thumbnail (ADR-039 P4)
    // so the Resources > Preview tab shows a live view of every projector. Runs in
    // the main GL context; saves/restores the framebuffer + viewport itself.
    if (m_outputManager && m_renderer) {
        auto previewTextures = buildQuadTextureMap(es, m_sourceManager.get());
        m_outputManager->updatePreviews(es.scenes, previewTextures, *m_renderer);
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
        es.mainMenuBar.render(ctx);
    }

    // Persist a UI-mode change to preferences (ADR-042). Static cache avoids a disk
    // write every frame; mode toggles are rare. Initialized to the startup mode so
    // the first frame never re-saves.
    {
        static int s_lastUiMode = static_cast<int>(es.uiMode);
        const int cur = static_cast<int>(es.uiMode);
        if (cur != s_lastUiMode) {
            PreferencesDialog d;
            d.loadPreferences();
            d.getPreferences().general.uiMode = cur;
            d.savePreferences();
            s_lastUiMode = cur;
        }
    }

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
                                                 m_project.get(), *target, *item);
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

    // Undo-history safety guard (ADR-042 follow-up): the command stack holds raw quad
    // pointers, so invalidate it whenever a quad is DELETED (count drops) or the active
    // scene changes. Creating a quad (count rises) is safe and keeps the history.
    {
        static UUID s_histScene;
        static size_t s_histCount = 0;
        static bool s_histInit = false;
        const size_t count = quadManager.getQuadCount();
        if (!s_histInit) { s_histScene = es.activeSceneId; s_histCount = count; s_histInit = true; }
        if (es.activeSceneId != s_histScene || count < s_histCount) {
            es.commandHistory.clear();
        }
        s_histScene = es.activeSceneId;
        s_histCount = count;
    }

    // Handle keyboard shortcuts
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        saveProject();
    }

    // Undo / Redo (not while typing in a text field).
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        es.commandHistory.undo();
    }
    if (!io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        es.commandHistory.redo();
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
        // Simple mode (ADR-042) shows only the Media bin; Project/Scenes/Preview/
        // Outputs are Advanced-only (single-projector output is auto + the F11 button).
        const bool advanced = (es.uiMode == EditorState::UiMode::Advanced);

        // -------- Project Tab --------
        if (advanced && ImGui::BeginTabItem("Project")) {
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

    es.quadManager().clear();
    es.quadTextures.clear();
    es.quadTexturePaths.clear();
    es.currentTexturePath[0] = '\0';

    es.quadManager().createQuad("Quad 1");
    if (es.quadManager().getQuadCount() > 0) {
        es.quadManager().selectQuad(es.quadManager().getQuads()[0]->getId());
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

    syncEditorToProject(es, m_outputManager.get(), *m_project);

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
        syncProjectToEditor(es, m_outputManager.get(), *m_project, m_sourceManager.get());
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Project loaded: %s", es.projectPath);
    } else {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "Load failed: %s", result.error().message.c_str());
    }
    es.statusTimer = 3.0f;
}

void Application::aiDetectSurface() {
    EditorState& es = *m_editorState;
    auto setStatus = [&](const std::string& msg, float secs = 5.0f) {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
        es.statusTimer = secs;
        Logger::info(msg);
    };

    // Preconditions: a running sidecar and a target quad to warp.
    if (!m_sidecarClient ||
        (m_sidecarClient->state() != ai::SidecarState::Attached &&
         m_sidecarClient->state() != ai::SidecarState::Launched)) {
        setStatus("AI: enable the sidecar first (AI > Enable AI Assist)");
        return;
    }
    Quad* quad = es.quadManager().getPrimarySelectedQuad();
    if (!quad) {
        setStatus("AI: select a quad to warp first");
        return;
    }

    // Resolve the input image. QM_AI_TEST_IMAGE feeds a fixed photo (verification,
    // and the "detect from a phone photo on disk" path); otherwise capture a live
    // frame off the first camera (the QM-055 rail). NOTE: this blocks the UI for
    // ~1-3s (camera open + capture + inference) — acceptable pre-show; async is
    // tracked as TD-10.
    std::filesystem::path imgPath;
    if (const char* testImg = std::getenv("QM_AI_TEST_IMAGE")) {
        imgPath = testImg;
        if (!std::filesystem::exists(imgPath)) {
            setStatus("AI: QM_AI_TEST_IMAGE not found");
            return;
        }
    } else {
        imgPath = std::filesystem::temp_directory_path() / "quadmapper_ai_capture.png";
        std::string err;
        if (!captureCameraFrameToPng(imgPath.string(), err)) {
            setStatus("AI: " + err);
            return;
        }
    }

    auto surface = m_sidecarClient->requestSurface(imgPath.string());
    if (!surface) {
        setStatus("AI: detection failed (sidecar error)");
        return;
    }

    // Low confidence: do NOT auto-warp onto a possibly-wrong surface — ask the
    // user to disambiguate instead (the spike's load-bearing human-confirm step).
    constexpr float kConfidenceThreshold = 0.35f;
    if (surface->confidence < kConfidenceThreshold) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "AI: low confidence (%.2f) - tap the surface to retry",
                 surface->confidence);
        setStatus(msg);
        return;
    }

    // Apply the proposed corners (TL,TR,BR,BL, normalized [0,1] — same order and
    // space as QuadGeometry). The result is an ordinary warp: drawn on canvas,
    // F11-projectable, and refinable with Tab-cycle + arrow-nudge.
    std::array<Vec2, 4> corners{
        Vec2{surface->corners[0][0], surface->corners[0][1]},
        Vec2{surface->corners[1][0], surface->corners[1][1]},
        Vec2{surface->corners[2][0], surface->corners[2][1]},
        Vec2{surface->corners[3][0], surface->corners[3][1]},
    };
    quad->getGeometry().setCorners(corners);
    m_project->setModified(true);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "AI: surface applied (confidence %.2f) - nudge with Tab + arrows",
             surface->confidence);
    setStatus(msg);
}

bool Application::captureCameraFrameToPng(const std::string& pngPath,
                                          std::string& error) {
    const auto cameras = enumerateCameraDevices();
    if (cameras.empty()) {
        error = "no camera found (connect one or set QM_AI_TEST_IMAGE)";
        return false;
    }
    CameraSource camera(cameras.front().name);
    if (!camera.initialize()) {
        error = "failed to open camera";
        return false;
    }
    // Pump updates until the first frame lands (or give up after ~1s).
    for (int i = 0; i < 60 && !camera.hasNewFrame(); ++i) {
        camera.update(0.016);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const bool captured = camera.captureCurrentFrameToPng(pngPath);
    camera.shutdown();
    if (!captured) {
        error = "camera capture failed (no frame)";
        return false;
    }
    return true;
}

void Application::oneFlashEmitPattern() {
    EditorState& es = *m_editorState;
    auto setStatus = [&](const std::string& msg) {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
        es.statusTimer = 5.0f;
        Logger::info(msg);
    };

    // Lazily load the calibration pattern + build the fullscreen identity quad.
    if (!m_calibTexture) {
        std::filesystem::path patternPath;
        const std::filesystem::path rel("resources/patterns/charuco.png");
        for (const std::filesystem::path& base :
             {std::filesystem::current_path(),
              std::filesystem::current_path() / "..",
              std::filesystem::current_path() / ".." / ".."}) {
            std::error_code ec;
            if (std::filesystem::exists(base / rel, ec)) {
                patternPath = base / rel;
                break;
            }
        }
        auto tex = std::make_unique<Texture>();
        if (patternPath.empty() || !tex->loadFromFile(patternPath)) {
            setStatus("One-Flash: pattern asset not found (resources/patterns/charuco.png)");
            return;
        }
        m_calibTexture = std::move(tex);
        m_calibQuad = std::make_unique<Quad>();
        m_calibQuad->getGeometry().setCorners(
            {Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)});
    }

    m_emitPattern = true;
    if (!m_outputManager->isInPerformMode()) {
        setStatus("One-Flash: pattern armed - press F11 to flash it on the projector");
    } else {
        setStatus("One-Flash: pattern on projector - capture a photo, then Clear Pattern");
    }
}

void Application::oneFlashCapturePhoto() {
    EditorState& es = *m_editorState;
    auto setStatus = [&](const std::string& msg) {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
        es.statusTimer = 6.0f;
        Logger::info(msg);
    };

    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "quadmapper_oneflash_capture.png";
    std::string err;
    if (!captureCameraFrameToPng(out.string(), err)) {
        setStatus("One-Flash: " + err);
        return;
    }
    setStatus("One-Flash: captured " + out.string());
}

void Application::oneFlashClearPattern() {
    EditorState& es = *m_editorState;
    m_emitPattern = false;
    snprintf(es.statusMessage, sizeof(es.statusMessage), "One-Flash: pattern cleared");
    es.statusTimer = 4.0f;
    Logger::info("One-Flash: pattern cleared");
}

void Application::oneFlashSolveAndApply() {
    runOneFlash(nullptr, "warp applied");
}

void Application::oneFlashAnamorphic() {
    // A centered rectangle in the audience (photo) view: content mapped here
    // looks like a frontal rectangle from the captured viewpoint.
    static const std::array<std::array<float, 2>, 4> kAudienceTarget{{
        {0.15f, 0.15f}, {0.85f, 0.15f}, {0.85f, 0.85f}, {0.15f, 0.85f}}};
    runOneFlash(&kAudienceTarget, "anamorphic warp applied");
}

void Application::runOneFlash(const std::array<std::array<float, 2>, 4>* audienceTarget,
                              const char* label) {
    EditorState& es = *m_editorState;
    auto setStatus = [&](const std::string& msg) {
        snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", msg.c_str());
        es.statusTimer = 6.0f;
        Logger::info(msg);
    };

    if (!m_sidecarClient ||
        (m_sidecarClient->state() != ai::SidecarState::Attached &&
         m_sidecarClient->state() != ai::SidecarState::Launched)) {
        setStatus("One-Flash: enable the sidecar first (AI > Enable AI Assist)");
        return;
    }
    Quad* quad = es.quadManager().getPrimarySelectedQuad();
    if (!quad) {
        setStatus("One-Flash: select a quad to warp first");
        return;
    }

    // The pattern photo: QM_AI_TEST_IMAGE override (a synthetic capture for
    // verification) else a live camera shot of the projected pattern.
    std::filesystem::path patternPath;
    if (const char* testImg = std::getenv("QM_AI_TEST_IMAGE")) {
        patternPath = testImg;
        if (!std::filesystem::exists(patternPath)) {
            setStatus("One-Flash: QM_AI_TEST_IMAGE not found");
            return;
        }
    } else {
        patternPath = std::filesystem::temp_directory_path() /
                      "quadmapper_oneflash_capture.png";
        std::string err;
        if (!captureCameraFrameToPng(patternPath.string(), err)) {
            setStatus("One-Flash: " + err);
            return;
        }
    }

    auto result = m_sidecarClient->requestOneFlash(patternPath.string(), audienceTarget);
    if (!result || result->stub) {
        setStatus("One-Flash: pattern not detected (need the ChArUco visible)");
        return;
    }
    constexpr float kConfidenceThreshold = 0.35f;
    if (result->confidence < kConfidenceThreshold) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "One-Flash: low confidence (%.2f) - reframe the pattern",
                 result->confidence);
        setStatus(msg);
        return;
    }

    std::array<Vec2, 4> corners{
        Vec2{result->corners[0][0], result->corners[0][1]},
        Vec2{result->corners[1][0], result->corners[1][1]},
        Vec2{result->corners[2][0], result->corners[2][1]},
        Vec2{result->corners[3][0], result->corners[3][1]},
    };
    quad->getGeometry().setCorners(corners);
    m_project->setModified(true);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "One-Flash: %s (confidence %.2f) - send to projector (F11)",
             label, result->confidence);
    setStatus(msg);
}

void Application::renderToProjectorOutputs() {
    if (!m_outputManager || !m_editorState) {
        return;
    }

    EditorState& es = *m_editorState;

    // One-Flash: when a calibration pattern is armed, flash it fullscreen on the
    // outputs instead of the scene (QM-059).
    if (m_emitPattern && m_calibQuad) {
        m_outputManager->renderPatternToOutputs(*m_calibQuad, m_calibTexture.get());
        return;
    }

    // Resolve every quad's texture (image via SourceManager/mediaId, video via the
    // legacy wrapper) through the shared builder — same map the editor/preview use.
    auto textures = buildQuadTextureMap(es, m_sourceManager.get());

    m_outputManager->renderToOutputs(es.scenes, textures);
}

// ---------------------------------------------------------------------------
// runStress — QM-081 H2/H3 headless perform-mode stress / leak harness.
//
// Control flow:
//   1. Build cfg.outputs output windows + one scene/quad + a procedural
//      TestPattern source (no file dependency).  Built once, before the loop.
//   2. Run ONE warm-up cycle (enter → N frames → exit) to let per-output
//      Renderers and textures settle.
//   3. Snapshot GlObjectStats::Snapshot::capture() + working set immediately
//      after warm-up exit — the consistent "post-exit" phase.
//   4. Loop cfg.cycles: enter → N frames (with wedge watchdog) → exit →
//      snapshot post-exit.  Compare every snapshot to baseline; track worst.
//   5. Print the diff table + working-set delta + STRESS PASS / STRESS FAIL.
//      Return 0 only when all gates pass.
//
// H3 — --hidden mode:
//   Exercises the same GL-object churn (per-output Renderer create/destroy via
//   enterPerformMode/exitPerformMode + renderToOutputs) WITHOUT touching
//   borderless-fullscreen or bringing output windows on-screen.  Output
//   windows stay hidden via OutputWindow::hide(); the main window is also
//   hidden by glfwHideWindow at the start of the run.
//   What --hidden skips vs the real path:
//     - No glfwShowWindow / goBorderlessFullscreen / goWindowed calls.
//     - No monitor assignment / DWM composition swap.
//   What it still exercises (the leak-relevant part):
//     - Per-output Renderer lifecycle (created in enterPerformMode, destroyed in
//       exitPerformMode) — the most common resource-leak site.
//     - renderToOutputs: context-switch, GL clear, quad render, swapBuffers.
//     - All RAII wrapper create/destroy paths tracked by GlObjectStats.
// ---------------------------------------------------------------------------

// Helper: query process working set in bytes via psapi (H3 §2).
// Returns 0 on any error — caller treats 0 as "unavailable".
static SIZE_T stressWorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return 0;
    }
    return pmc.WorkingSetSize;
}

int Application::runStress(const StressConfig& cfg) {
    Logger::info("[STRESS] QM-081 H3 — cycles={} outputs={} frames={} "
                 "fullscreen={} hidden={} ws-bound-mb={} cycle-timeout-ms={}",
                 cfg.cycles, cfg.outputs, cfg.framesPerCycle,
                 cfg.fullscreen ? "true" : "false",
                 cfg.hidden     ? "true" : "false",
                 cfg.workingSetBoundMB, cfg.cycleTimeoutMs);

    // ---- 1. Build outputs ---------------------------------------------------
    // The default output was already created by initializeComponents() for
    // monitor 0.  Create additional ones if cfg.outputs > 1.
    while (static_cast<int>(m_outputManager->getOutputCount()) < cfg.outputs) {
        int idx = static_cast<int>(m_outputManager->getOutputCount());
        OutputWindow* w = m_outputManager->createOutput("Stress-" + std::to_string(idx));
        if (!w) {
            Logger::error("[STRESS] Failed to create output window {}", idx);
            return 2;
        }
        // Assign to monitor 0 (primary) for all stress outputs — dev box is safe.
        w->setMonitor(0);
    }

    // H3: In --hidden mode keep the main editor window off-screen too.
    if (cfg.hidden) {
        glfwHideWindow(m_window);
    }

    // ---- 2. Build one scene with one quad + procedural source ---------------
    EditorState& es = *m_editorState;

    // Guarantee at least one scene and one quad.
    Scene& scene = es.activeScene();
    QuadManager& qm = scene.getQuadManager();
    if (qm.getQuads().empty()) {
        qm.createQuad("Stress-Quad");
    }
    Quad* quad = qm.getQuads().front().get();

    // Create a TestPattern source (no file, no network dependency).
    UUID sourceId = m_sourceManager->createProceduralSource(ProceduralType::TEST_PATTERN,
                                                            320, 180);
    if (sourceId.isNull()) {
        Logger::error("[STRESS] Failed to create TestPattern source");
        return 2;
    }
    quad->getTexture().mediaId = sourceId;

    // Assign the active scene to output 0.
    const auto& outputs = m_outputManager->getOutputs();
    if (!outputs.empty()) {
        es.assignSceneToOutput(scene.getId(), outputs.front()->getId());
    }

    // ---- Helper: one full cycle with wedge watchdog (H3 §3) -----------------
    // Returns false if the cycle exceeded cfg.cycleTimeoutMs.
    auto runCycle = [&]() -> bool {
        const double cycleStart = glfwGetTime();
        const double budgetSec  = cfg.cycleTimeoutMs / 1000.0;

        if (cfg.hidden) {
            // --hidden: call the same enter/exit path so the per-output Renderer
            // lifecycle fires (the leak-relevant part), but keep windows hidden.
            // enterPerformMode normally shows + fullscreens each window; we skip
            // that here by driving it ourselves on the OutputManager's internals
            // via the public API in the most minimal way: just call renderToOutputs
            // directly without the show/fullscreen step.  The per-cycle Renderer
            // create/destroy still fires because enterPerformMode creates per-output
            // renderers regardless of visibility.
            m_outputManager->enterPerformMode(/*borderless=*/false);
            // Keep windows hidden after enterPerformMode showed them.
            for (auto& ow : m_outputManager->getOutputs()) {
                if (ow->isCreated()) {
                    ow->hide();
                }
            }
        } else {
            m_outputManager->enterPerformMode(cfg.fullscreen);
        }

        double t = glfwGetTime();
        for (int f = 0; f < cfg.framesPerCycle; ++f) {
            // Wedge check: abort if we've been in this cycle too long.
            if ((glfwGetTime() - cycleStart) > budgetSec) {
                m_outputManager->exitPerformMode();
                return false;
            }

            glfwPollEvents();

            double now = glfwGetTime();
            double dt  = now - t;
            t = now;
            m_sourceManager->updateAll(dt);

            auto textures = buildQuadTextureMap(es, m_sourceManager.get());
            m_outputManager->renderToOutputs(es.scenes, textures);
        }

        m_outputManager->exitPerformMode();
        return true;
    };

    // ---- 3. Warm-up cycle ---------------------------------------------------
    Logger::info("[STRESS] Warm-up cycle...");
    if (!runCycle()) {
        Logger::error("[STRESS] STRESS FAIL (wedge) — warm-up cycle exceeded {}ms budget.",
                      cfg.cycleTimeoutMs);
        return 1;
    }

    // Baseline captured immediately after warm-up exit (consistent post-exit phase).
    auto baseline   = GlObjectStats::Snapshot::capture();
    SIZE_T wsBaseline = stressWorkingSetBytes();

    Logger::info("[STRESS] Baseline captured after warm-up.  Working set: {} MB",
                 wsBaseline / (1024 * 1024));

    // ---- 4. Stress loop (with wedge watchdog + working-set tracking) --------
    GlObjectStats::Snapshot worst = baseline;
    bool anyLeak   = false;
    bool anyWedge  = false;
    bool wsExceeded = false;
    SIZE_T wsMax   = wsBaseline;

    for (int c = 0; c < cfg.cycles; ++c) {
        if (!runCycle()) {
            Logger::error("[STRESS] STRESS FAIL (wedge) — cycle {} exceeded {}ms budget.",
                          c + 1, cfg.cycleTimeoutMs);
            anyWedge = true;
            break;
        }

        auto snap = GlObjectStats::Snapshot::capture();
        if (!snap.isFlat(baseline)) {
            anyLeak = true;
            // Track worst (highest counts in each category independently).
            worst.textures      = std::max(worst.textures,      snap.textures);
            worst.framebuffers  = std::max(worst.framebuffers,  snap.framebuffers);
            worst.vertexArrays  = std::max(worst.vertexArrays,  snap.vertexArrays);
            worst.vertexBuffers = std::max(worst.vertexBuffers, snap.vertexBuffers);
            worst.shaderPrograms= std::max(worst.shaderPrograms,snap.shaderPrograms);
        }

        // H3: working-set sample (spec §2).
        SIZE_T wsCurrent = stressWorkingSetBytes();
        if (wsCurrent > wsMax) {
            wsMax = wsCurrent;
        }
        long growthMB = static_cast<long>((wsCurrent - wsBaseline) / (1024 * 1024));
        if (growthMB > cfg.workingSetBoundMB) {
            wsExceeded = true;
            Logger::error("[STRESS] STRESS FAIL (working-set) — cycle {} grew {} MB "
                          "(bound {} MB).", c + 1, growthMB, cfg.workingSetBoundMB);
        }

        Logger::info("[STRESS] Cycle {}/{} complete — {} | WS={} MB (delta {} MB)",
                     c + 1, cfg.cycles,
                     snap.isFlat(baseline) ? "flat" : "DELTA DETECTED",
                     wsCurrent / (1024 * 1024),
                     growthMB);
    }

    // ---- 5. Report ----------------------------------------------------------
    Logger::info("[STRESS] Worst-case GL diff vs baseline:");
    Logger::info("{}", worst.diff(baseline));

    long finalGrowthMB = static_cast<long>((wsMax - wsBaseline) / (1024 * 1024));
    Logger::info("[STRESS] Working-set: baseline={} MB  max={} MB  peak-delta={} MB  "
                 "bound={} MB",
                 wsBaseline / (1024 * 1024),
                 wsMax      / (1024 * 1024),
                 finalGrowthMB,
                 cfg.workingSetBoundMB);

    // H3: optional GPU memory report via GL_NVX_gpu_memory_info.
    // The extension is trivially detected at runtime; silently skip on AMD /
    // Intel / non-NVIDIA drivers.  Do NOT load GLAD GL_NVX_ — the extension is
    // query-only (no new entry points), so a plain glGetIntegerv is sufficient
    // once we confirm the extension string is present.
    //
    // GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX = 0x9049 (Khronos registry).
    if (glfwExtensionSupported("GL_NVX_gpu_memory_info")) {
        GLint availKB = 0;
        glGetIntegerv(0x9049 /*GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX*/, &availKB);
        Logger::info("[STRESS] GPU VRAM available (NVX): {} MB", availKB / 1024);
    }

    if (anyLeak) {
        Logger::error("[STRESS] STRESS FAIL (leak) — GL object count grew across cycles.");
    }
    if (wsExceeded) {
        Logger::error("[STRESS] STRESS FAIL (working-set) — peak growth exceeded {} MB bound.",
                      cfg.workingSetBoundMB);
    }

    if (anyLeak || anyWedge || wsExceeded) {
        return 1;
    }

    Logger::info("[STRESS] STRESS PASS — {} cycles, GL flat, WS delta {} MB (bound {} MB).",
                 cfg.cycles, finalGrowthMB, cfg.workingSetBoundMB);
    return 0;
}

void Application::shutdown() {
    if (!m_window) {
        return; // Already shut down
    }

    Logger::info("Shutting down QUADmapper...");

    // Cleanup components in reverse order of creation
    m_sidecarClient.reset();   // stop()s / kills any launched sidecar (no orphan)
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
