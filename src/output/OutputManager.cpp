#include "OutputManager.hpp"
#include "core/Logger.hpp"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <sstream>

namespace quadmapper {

// Static instance pointer for GLFW callback
OutputManager* OutputManager::instance_ = nullptr;

std::string MonitorInfo::getDisplayString() const {
    std::ostringstream oss;
    oss << name << " (" << resolution.x << "x" << resolution.y << " @ " << refreshRate << "Hz)";
    if (isPrimary) {
        oss << " [Primary]";
    }
    return oss.str();
}

OutputManager::OutputManager() = default;

OutputManager::~OutputManager() {
    shutdown();
}

Result<void, Error> OutputManager::initialize(GLFWwindow* mainWindow) {
    if (mainWindow == nullptr) {
        return Err("Cannot initialize OutputManager with null main window");
    }

    if (mainWindow_ != nullptr) {
        LOG_WARNING("OutputManager already initialized, re-initializing");
        shutdown();
    }

    mainWindow_ = mainWindow;

    // Set the static instance for GLFW callbacks
    instance_ = this;

    // Enumerate available monitors
    refreshMonitors();

    LOG_INFO("OutputManager initialized with {} monitors available", monitors_.size());

    return Ok();
}

void OutputManager::shutdown() {
    // Disable monitor change detection
    disableMonitorChangeDetection();

    // Exit perform mode if active
    if (performMode_) {
        exitPerformMode();
    }

    // The frames and the fence live in the main context.
    if (mainWindow_ != nullptr) {
        GLFWwindow* previousContext = glfwGetCurrentContext();
        glfwMakeContextCurrent(mainWindow_);
        frames_.clear();
        canvases_.clear();
        if (framesDrawn_ != nullptr) {
            glDeleteSync(framesDrawn_);
            framesDrawn_ = nullptr;
        }
        glfwMakeContextCurrent(previousContext);
    }

    // Destroy all output windows
    for (auto& output : outputs_) {
        output->destroy();
    }
    outputs_.clear();

    mainWindow_ = nullptr;
    outputCounter_ = 0;

    // Clear static instance
    if (instance_ == this) {
        instance_ = nullptr;
    }

    LOG_INFO("OutputManager shutdown complete");
}

OutputWindow* OutputManager::createOutput(const std::string& name) {
    auto output = std::make_unique<OutputWindow>();

    // Generate unique name if not provided or is default
    std::string outputName = name;
    if (outputName == "Output" || outputName.empty()) {
        outputCounter_++;
        outputName = "Output " + std::to_string(outputCounter_);
    }
    output->setName(outputName);

    // Create the GLFW window if manager is initialized
    if (mainWindow_ != nullptr) {
        auto result = output->create(mainWindow_);
        if (!result) {
            LOG_ERROR("Failed to create output window '{}': {}",
                      outputName, result.error().message);
            return nullptr;
        }
    }

    OutputWindow* ptr = output.get();
    outputs_.push_back(std::move(output));

    LOG_INFO("Created output '{}' (total: {})", outputName, outputs_.size());

    return ptr;
}

void OutputManager::deleteOutput(const UUID& id) {
    auto it = std::find_if(outputs_.begin(), outputs_.end(),
        [&id](const std::unique_ptr<OutputWindow>& output) {
            return output->getId() == id;
        });

    if (it != outputs_.end()) {
        std::string name = (*it)->getName();

        // Its frame dies in the main context (its copier goes with its window).
        if (frames_.count(id) != 0 && mainWindow_ != nullptr) {
            GLFWwindow* previousContext = glfwGetCurrentContext();
            glfwMakeContextCurrent(mainWindow_);
            frames_.erase(id);
            glfwMakeContextCurrent(previousContext);
        }

        (*it)->destroy();
        outputs_.erase(it);
        releaseUnshownCanvases();
        LOG_INFO("Deleted output '{}' (remaining: {})", name, outputs_.size());
        if (outputRemovedCallback_) {
            outputRemovedCallback_(id);
        }
    }
}

OutputWindow* OutputManager::getOutput(const UUID& id) {
    auto it = std::find_if(outputs_.begin(), outputs_.end(),
        [&id](const std::unique_ptr<OutputWindow>& output) {
            return output->getId() == id;
        });

    return it != outputs_.end() ? it->get() : nullptr;
}

const OutputWindow* OutputManager::getOutput(const UUID& id) const {
    auto it = std::find_if(outputs_.begin(), outputs_.end(),
        [&id](const std::unique_ptr<OutputWindow>& output) {
            return output->getId() == id;
        });

    return it != outputs_.end() ? it->get() : nullptr;
}

void OutputManager::refreshMonitors() {
    monitors_.clear();

    int monitorCount = 0;
    GLFWmonitor** glfwMonitors = glfwGetMonitors(&monitorCount);

    if (glfwMonitors == nullptr || monitorCount == 0) {
        LOG_WARNING("No monitors detected");
        return;
    }

    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();

    for (int i = 0; i < monitorCount; ++i) {
        GLFWmonitor* monitor = glfwMonitors[i];
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        const char* name = glfwGetMonitorName(monitor);

        MonitorInfo info;
        info.index = i;
        info.name = name ? name : ("Monitor " + std::to_string(i + 1));
        info.isPrimary = (monitor == primaryMonitor);

        if (mode != nullptr) {
            info.resolution = IVec2(mode->width, mode->height);
            info.refreshRate = mode->refreshRate;
        }

        // Get monitor position in virtual screen space
        int xpos, ypos;
        glfwGetMonitorPos(monitor, &xpos, &ypos);
        info.position = IVec2(xpos, ypos);

        monitors_.push_back(info);

        LOG_DEBUG("Monitor {}: '{}' {}x{}@{}Hz at ({},{}) {}",
                  i, info.name, info.resolution.x, info.resolution.y,
                  info.refreshRate, info.position.x, info.position.y,
                  info.isPrimary ? "[Primary]" : "");
    }

    LOG_INFO("Detected {} monitors", monitors_.size());
}

std::vector<VideoModeInfo> OutputManager::getMonitorVideoModes(int monitorIndex) const {
    std::vector<VideoModeInfo> modes;

    int monitorCount = 0;
    GLFWmonitor** glfwMonitors = glfwGetMonitors(&monitorCount);

    if (glfwMonitors == nullptr || monitorIndex < 0 || monitorIndex >= monitorCount) {
        return modes;
    }

    GLFWmonitor* monitor = glfwMonitors[monitorIndex];
    int modeCount = 0;
    const GLFWvidmode* glfwModes = glfwGetVideoModes(monitor, &modeCount);

    if (glfwModes == nullptr) {
        return modes;
    }

    // Collect unique resolutions (different refresh rates for same resolution)
    for (int i = 0; i < modeCount; ++i) {
        VideoModeInfo mode;
        mode.width = glfwModes[i].width;
        mode.height = glfwModes[i].height;
        mode.refreshRate = glfwModes[i].refreshRate;

        // Only add if not already in list (avoid duplicate resolutions at different refresh rates)
        bool exists = false;
        for (const auto& existing : modes) {
            if (existing.width == mode.width && existing.height == mode.height) {
                // Keep the higher refresh rate
                exists = true;
                break;
            }
        }
        if (!exists) {
            modes.push_back(mode);
        }
    }

    // Sort by resolution (descending - highest first)
    std::sort(modes.begin(), modes.end(), [](const VideoModeInfo& a, const VideoModeInfo& b) {
        if (a.width != b.width) return a.width > b.width;
        return a.height > b.height;
    });

    return modes;
}

MonitorResolution OutputManager::resolveMonitor(const std::vector<MonitorInfo>& monitors,
                                                const MonitorIdentity& desired,
                                                int editorMonitor,
                                                const std::vector<int>& taken) {
    const int count = static_cast<int>(monitors.size());
    auto isFree = [&](int i) {
        return i != editorMonitor && std::find(taken.begin(), taken.end(), i) == taken.end();
    };

    if (!desired.name.empty()) {
        for (int i = 0; i < count; ++i) {
            if (monitors[i].name == desired.name && monitors[i].position == desired.position) {
                return {i, MonitorMatch::Exact};
            }
        }
        for (int i = 0; i < count; ++i) {
            if (monitors[i].position == desired.position && isFree(i)) {
                return {i, MonitorMatch::Position};
            }
        }
        // Displays are often all "Generic PnP Monitor": a shared name says nothing.
        int named = -1;
        int sameName = 0;
        for (int i = 0; i < count; ++i) {
            if (monitors[i].name == desired.name) {
                named = i;
                ++sameName;
            }
        }
        if (sameName == 1 && isFree(named)) {
            return {named, MonitorMatch::UniqueName};
        }
    }

    if (desired.savedIndex >= 0 && desired.savedIndex < count && isFree(desired.savedIndex)) {
        return {desired.savedIndex, MonitorMatch::SavedIndex};
    }
    for (int i = 0; i < count; ++i) {
        if (isFree(i)) {
            return {i, MonitorMatch::FirstFree};
        }
    }
    return {};
}

std::vector<MonitorResolution> OutputManager::resolveMonitors(
        const std::vector<MonitorInfo>& monitors,
        const std::vector<MonitorIdentity>& desired,
        int editorMonitor) {
    std::vector<MonitorResolution> results(desired.size());
    std::vector<bool> done(desired.size(), false);
    std::vector<int> taken;

    // Each pass accepts matches up to `weakest`; the last accepts everything.
    for (MonitorMatch weakest : {MonitorMatch::Exact, MonitorMatch::UniqueName,
                                 MonitorMatch::Unresolved}) {
        for (size_t i = 0; i < desired.size(); ++i) {
            if (done[i]) {
                continue;
            }
            const MonitorResolution r = resolveMonitor(monitors, desired[i], editorMonitor, taken);
            if (r.match > weakest) {
                continue;
            }
            results[i] = r;
            done[i] = true;
            if (r.index >= 0) {
                taken.push_back(r.index);
            }
        }
    }
    return results;
}

int OutputManager::monitorContaining(const std::vector<MonitorInfo>& monitors, IVec2 point) {
    for (int i = 0; i < static_cast<int>(monitors.size()); ++i) {
        const MonitorInfo& m = monitors[i];
        if (point.x >= m.position.x && point.x < m.position.x + m.resolution.x &&
            point.y >= m.position.y && point.y < m.position.y + m.resolution.y) {
            return i;
        }
    }
    return -1;
}

int OutputManager::vsyncHolder(const std::vector<bool>& iconified) {
    for (size_t i = 0; i < iconified.size(); ++i) {
        if (!iconified[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int OutputManager::editorMonitorIndex() const {
    if (mainWindow_ == nullptr) {
        return -1;
    }
    int x = 0, y = 0, width = 0, height = 0;
    glfwGetWindowPos(mainWindow_, &x, &y);
    glfwGetWindowSize(mainWindow_, &width, &height);
    const int index = monitorContaining(monitors_, IVec2(x + width / 2, y + height / 2));
    if (index >= 0) {
        return index;
    }
    const MonitorInfo* primary = getPrimaryMonitor();
    return primary ? primary->index : -1;
}

void OutputManager::resolveOutputs() {
    const int editorMonitor = editorMonitorIndex();

    std::vector<OutputWindow*> enabled;
    std::vector<MonitorIdentity> desired;
    for (auto& output : outputs_) {
        if (output->isEnabled()) {
            enabled.push_back(output.get());
            desired.push_back(output->getDesiredMonitor());
        }
    }
    const std::vector<MonitorResolution> results =
        resolveMonitors(monitors_, desired, editorMonitor);

    auto apply = [&](OutputWindow& output, const MonitorResolution& r) {
        if (r.index != output.getMonitor()) {
            const MonitorIdentity& want = output.getDesiredMonitor();
            if (r.index < 0) {
                LOG_WARNING("Output '{}': no display available (wanted '{}'); not shown",
                            output.getName(), want.name);
            } else if (r.match >= MonitorMatch::SavedIndex && !want.name.empty()) {
                LOG_WARNING("Output '{}': display '{}' not connected; using '{}' (monitor {})",
                            output.getName(), want.name, monitors_[r.index].name, r.index);
            } else {
                LOG_INFO("Output '{}' resolved to '{}' (monitor {})",
                         output.getName(), monitors_[r.index].name, r.index);
            }
        }
        output.setMonitor(r.index);
    };

    std::vector<int> taken;
    for (size_t i = 0; i < enabled.size(); ++i) {
        apply(*enabled[i], results[i]);
        if (results[i].index >= 0) {
            taken.push_back(results[i].index);
        }
    }
    for (auto& output : outputs_) {
        if (!output->isEnabled()) {
            apply(*output, resolveMonitor(monitors_, output->getDesiredMonitor(),
                                          editorMonitor, taken));
        }
    }
}

std::string OutputManager::describeMonitorChoice(const std::vector<MonitorInfo>& monitors,
                                                const OutputWindow& output) {
    const int current = output.getMonitor();
    if (current >= 0 && static_cast<size_t>(current) < monitors.size()) {
        std::string label = monitors[current].getDisplayString();
        if (output.isMonitorAutomatic()) label += " (automatic)";
        return label;
    }
    const MonitorIdentity& desired = output.getDesiredMonitor();
    if (!desired.name.empty()) return desired.name + " (not connected)";
    return "Not connected";
}

void OutputManager::assignMonitor(OutputWindow& output, int monitorIndex) {
    const MonitorInfo* monitor = getMonitor(monitorIndex);
    MonitorIdentity identity;
    if (monitor != nullptr) {
        identity = {monitor->name, monitor->position, monitorIndex};
    }
    output.setDesiredMonitor(identity);

    if (mainWindow_ == nullptr) {
        output.setMonitor(monitor ? monitorIndex : -1);
        return;
    }
    // "Automatic" picks a display now; a chosen monitor resolves Exact to itself.
    // Other outputs may give way to it, so all of them are re-placed.
    reresolveOutputs(/*refreshMonitorList=*/false);
}

void OutputManager::setNoticeCallback(NoticeCallback callback) {
    noticeCallback_ = std::move(callback);
}

void OutputManager::notify(const std::string& message) {
    LOG_WARNING("{}", message);
    if (noticeCallback_) {
        noticeCallback_(message);
    }
}

bool OutputManager::showOnResolvedMonitor(OutputWindow& output) {
    const int monitorIndex = output.getMonitor();
    if (monitorIndex < 0) {
        return false;
    }
    output.show();
    auto result = useBorderlessFullscreen_
        ? output.goBorderlessFullscreen(monitorIndex)
        : output.goFullscreen(monitorIndex);
    if (!result) {
        LOG_ERROR("Failed to go fullscreen for output '{}': {}",
                  output.getName(), result.error().message);
        output.goWindowed();
        output.hide();
        return false;
    }
    LOG_INFO("Output '{}' {} on monitor {}", output.getName(),
             useBorderlessFullscreen_ ? "borderless fullscreen" : "fullscreen", monitorIndex);
    return true;
}

Result<void, Error> OutputManager::enterPerformMode(bool borderless) {
    if (performMode_) {
        return Ok();  // Already in perform mode
    }

    if (outputs_.empty()) {
        return Err("No outputs to display in perform mode");
    }

    LOG_INFO("Entering perform mode with {} outputs (borderless={})", outputs_.size(), borderless);

    // Resolve at the last moment: the projector may have been plugged in since
    // the show was loaded.
    refreshMonitors();
    resolveOutputs();

    useBorderlessFullscreen_ = borderless;
    bool anySucceeded = false;

    for (auto& output : outputs_) {
        if (!output->isEnabled()) {
            continue;
        }

        if (!output->isCreated()) {
            LOG_WARNING("Output '{}' not created, skipping", output->getName());
            continue;
        }

        if (output->getMonitor() < 0) {
            // Never fall back onto the operator's own screen by itself; say how to do it
            // on purpose (e.g. rehearsing on a laptop with no projector attached).
            notify("Projector '" + output->getName() + "' not connected; its output is not shown."
                   " To preview it on this screen, pick this display for it in Outputs.");
            continue;
        }
        if (showOnResolvedMonitor(*output)) {
            anySucceeded = true;
        }
    }

    if (!anySucceeded) {
        return Err("Failed to put any outputs in fullscreen mode");
    }

    performMode_ = true;

    // Setup key callbacks for Escape to exit
    setupPerformModeKeyCallbacks();

    // Outputs are shown without activation (GLFW_FOCUS_ON_SHOW off); make sure
    // no window manager quirk left one holding the keyboard.
    returnFocusToEditor();

    LOG_INFO("Perform mode active");

    return Ok();
}

Result<void, Error> OutputManager::exitPerformMode() {
    if (!performMode_) {
        return Ok();  // Already not in perform mode
    }

    LOG_INFO("Exiting perform mode");

    // Clear key callbacks before exiting
    clearPerformModeKeyCallbacks();

    for (auto& output : outputs_) {
        if (!output->isCreated()) {
            continue;
        }

        // Return to windowed mode
        auto result = output->goWindowed();
        if (!result) {
            LOG_WARNING("Failed to return output '{}' to windowed mode: {}",
                        output->getName(), result.error().message);
        }

        // Hide the window
        output->hide();
    }

    performMode_ = false;
    performExitPending_ = false;  // a request made before this exit is spent

    // No output paces the loop any more; the editor is vsync'd again.
    setEditorSwapInterval(1);
    LOG_INFO("Perform mode exited");

    return Ok();
}

Result<void, Error> OutputManager::togglePerformMode(bool borderless) {
    if (performMode_) {
        return exitPerformMode();
    } else {
        return enterPerformMode(borderless);
    }
}

const Scene* OutputManager::sceneForOutput(
        const std::vector<std::unique_ptr<Scene>>& scenes, const OutputWindow& output) {
    const UUID& sceneId = output.getSceneId();
    if (sceneId.isNull()) {
        return nullptr;
    }
    for (const auto& scene : scenes) {
        if (scene && scene->getId() == sceneId) {
            return scene.get();
        }
    }
    return nullptr;
}

std::vector<Quad> makeBlendTestPattern() {
    std::vector<Quad> quads;
    auto rect = [&](float x0, float y0, float x1, float y1) {
        Quad quad;
        quad.setName("Blend test pattern");
        quad.getGeometry().setCorners({Vec2(x0, y0), Vec2(x1, y0), Vec2(x1, y1), Vec2(x0, y1)});
        quads.push_back(std::move(quad));
    };
    // A 16 x 9 grid of lines about 4 px thick on a 1920 x 1080 canvas: alignment.
    constexpr int COLUMNS = 16;
    constexpr int ROWS = 9;
    constexpr float HALF_X = 0.001f;
    constexpr float HALF_Y = 0.0018f;
    for (int i = 0; i <= COLUMNS; ++i) {
        const float x = static_cast<float>(i) / COLUMNS;
        rect(x - HALF_X, 0.0f, x + HALF_X, 1.0f);
    }
    for (int j = 0; j <= ROWS; ++j) {
        const float y = static_cast<float>(j) / ROWS;
        rect(0.0f, y - HALF_Y, 1.0f, y + HALF_Y);
    }
    // Solid white bands across the whole width and height: every left/right overlap
    // crosses the first, every top/bottom overlap the second, so a seam shows as a
    // brighter or darker stripe in flat white.
    rect(0.0f, 4.0f / ROWS, 1.0f, 5.0f / ROWS);
    rect(7.0f / COLUMNS, 0.0f, 9.0f / COLUMNS, 1.0f);
    return quads;
}

void OutputManager::showScene(OutputWindow& output, const Scene* scene,
                              const std::vector<std::unique_ptr<Scene>>& scenes) {
    if (!scene) {
        output.setSceneId(UUID::null());
        return;
    }
    const IVec2 canvas = scene->getCanvasSize();
    auto& slices = output.getSlices();
    IVec2 from = output.getSlicesCanvas();
    if (from.x <= 0 || from.y <= 0) {
        // Not recorded (slices loaded without a scene): the scene shown now, if any.
        const Scene* shown = sceneForOutput(scenes, output);
        from = shown ? shown->getCanvasSize() : IVec2(0, 0);
    }
    if (slices.empty() || from.x <= 0 || from.y <= 0) {
        slices.assign(1, makeFullCanvasSlice(canvas));
    } else {
        rescaleSlices(slices, from, canvas);
    }
    output.setSlicesCanvas(canvas);
    output.setSceneId(scene->getId());
}

bool OutputManager::routeScene(OutputWindow& output, Scene* scene,
                               const std::vector<std::unique_ptr<Scene>>& scenes) {
    showScene(output, scene, scenes);
    if (!scene || scene->isCanvasLocked() ||
        !isDefaultSlicing(output.getSlices(), scene->getCanvasSize())) {
        return false;
    }
    const bool alone = std::none_of(outputs_.begin(), outputs_.end(), [&](const auto& o) {
        return o.get() != &output && o->getSceneId() == scene->getId();
    });
    if (!alone) return false;
    const IVec2 before = scene->getCanvasSize();
    scene->setCanvasSize(output.getEffectiveResolution());
    const IVec2 canvas = scene->getCanvasSize();
    output.getSlices().front().input =
        CanvasRect{0.0f, 0.0f, static_cast<float>(canvas.x), static_cast<float>(canvas.y)};
    output.setSlicesCanvas(canvas);
    return canvas != before;
}

bool OutputManager::followOutputSizes(std::vector<std::unique_ptr<Scene>>& scenes) {
    bool changed = false;
    for (const auto& output : outputs_) {
        const IVec2 now = output->getEffectiveResolution();
        const bool presenting = output->isFullscreen() || output->isBorderless();
        auto known = knownSizes_.find(output->getId());
        if (known == knownSizes_.end()) {
            knownSizes_.emplace(output->getId(), KnownOutputSize{now, presenting});
            continue;
        }
        const KnownOutputSize before = known->second;
        known->second = KnownOutputSize{now, presenting};
        if (before.size == now) continue;

        Scene* scene = nullptr;
        for (auto& s : scenes) {
            if (s && s->getId() == output->getSceneId()) { scene = s.get(); break; }
        }
        if (!scene || scene->isCanvasLocked() || scene->getCanvasSize() != before.size ||
            !isDefaultSlicing(output->getSlices(), before.size)) {
            continue;
        }
        const bool alone = std::none_of(outputs_.begin(), outputs_.end(), [&](const auto& o) {
            return o.get() != output.get() && o->getSceneId() == scene->getId();
        });
        if (!alone) continue;
        scene->setCanvasSize(now);
        const IVec2 canvas = scene->getCanvasSize();
        output->getSlices().front().input =
            CanvasRect{0.0f, 0.0f, static_cast<float>(canvas.x), static_cast<float>(canvas.y)};
        output->setSlicesCanvas(canvas);
        if (before.presenting == presenting) changed = true;  // not an F11 enter/exit
    }
    for (auto it = knownSizes_.begin(); it != knownSizes_.end();) {
        it = getOutput(it->first) ? std::next(it) : knownSizes_.erase(it);
    }
    return changed;
}

OutputWindow* OutputManager::firstOutputShowing(const UUID& sceneId) const {
    if (sceneId.isNull()) {
        return nullptr;
    }
    for (const auto& output : outputs_) {
        if (output && output->getSceneId() == sceneId) {
            return output.get();
        }
    }
    return nullptr;
}

const Framebuffer* OutputManager::sceneCanvas(const UUID& sceneId) const {
    auto it = canvases_.find(sceneId);
    if (it == canvases_.end() || !it->second.target || !it->second.target->valid()) {
        return nullptr;
    }
    return it->second.target.get();
}

Rotation OutputManager::sceneCanvasRotation(const UUID& sceneId) const {
    auto it = canvases_.find(sceneId);
    return it != canvases_.end() ? it->second.rotation : Rotation::R0;
}

void OutputManager::releaseUnshownCanvases() {
    if (canvases_.empty() || mainWindow_ == nullptr) {
        return;
    }
    GLFWwindow* previousContext = glfwGetCurrentContext();
    glfwMakeContextCurrent(mainWindow_);
    for (auto it = canvases_.begin(); it != canvases_.end();) {
        if (!firstOutputShowing(it->first)) {
            it = canvases_.erase(it);
        } else {
            ++it;
        }
    }
    glfwMakeContextCurrent(previousContext);
}

void OutputManager::renderCanvases(const std::vector<std::unique_ptr<Scene>>& scenes,
                                   const std::unordered_map<UUID, Texture*>& textures,
                                   Renderer& renderer) {
    // The scenes outputs show, each once.
    canvasScenes_.clear();
    for (const auto& output : outputs_) {
        const Scene* scene = sceneForOutput(scenes, *output);
        if (scene && std::find(canvasScenes_.begin(), canvasScenes_.end(), scene->getId()) ==
                         canvasScenes_.end()) {
            canvasScenes_.push_back(scene->getId());
        }
    }
    for (auto it = canvases_.begin(); it != canvases_.end();) {
        if (std::find(canvasScenes_.begin(), canvasScenes_.end(), it->first) == canvasScenes_.end()) {
            it = canvases_.erase(it);
        } else {
            ++it;
        }
    }
    if (blackout_) {
        return;  // every frame is black; the canvases are kept for when it lifts
    }

    // Colour blends over black exactly as the quads used to blend into the frame;
    // alpha accumulates coverage, so the frame can composite the canvas over the
    // 3D objects it draws first.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (const UUID& sceneId : canvasScenes_) {
        const Scene* scene = nullptr;
        for (const auto& s : scenes) {
            if (s && s->getId() == sceneId) { scene = s.get(); break; }
        }
        SceneCanvas& canvas = canvases_[sceneId];
        if (!canvas.target) {
            // Colour only: quads are painter-ordered, and 3D objects are drawn per
            // output into its frame, never into the canvas.
            canvas.target = std::make_unique<Framebuffer>(Framebuffer::Depth::Without);
        }
        // The rotation all its outputs share, else none.
        bool first = true;
        for (const auto& output : outputs_) {
            if (output->getSceneId() != sceneId) continue;
            if (first) {
                canvas.rotation = output->getRotation();
                first = false;
            } else if (output->getRotation() != canvas.rotation) {
                canvas.rotation = Rotation::R0;
                break;
            }
        }
        const IVec2 size = scene->getCanvasSize();
        const bool quarterTurn = canvas.rotation == Rotation::R90 || canvas.rotation == Rotation::R270;
        const IVec2 texSize = quarterTurn ? IVec2(size.y, size.x) : size;
        if (texSize == canvas.failedSize) {
            continue;  // failed at this size before: not retried every frame
        }
        if (maxTextureSize_ == 0) {
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize_);
        }
        const bool fits = texSize.x <= maxTextureSize_ && texSize.y <= maxTextureSize_;
        if (!fits || !canvas.target->resize(texSize.x, texSize.y)) {
            canvas.failedSize = texSize;
            LOG_ERROR("Scene '{}': cannot allocate its {}x{} canvas ({}); its outputs show no quads",
                      scene->getName(), size.x, size.y,
                      fits ? "out of GPU memory" : "larger than the GPU's texture limit");
            continue;
        }
        canvas.failedSize = IVec2(0, 0);
        renderer.setOutputRotation(canvas.rotation);
        canvas.target->bind();  // also sets the viewport to the canvas size
        glDisable(GL_DEPTH_TEST);
        if (blendTestPattern_) {
            // Opaque: the pattern covers the 3D objects too.
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (testPattern_.empty()) testPattern_ = makeBlendTestPattern();
            for (const Quad& quad : testPattern_) {
                renderer.renderQuad(quad, nullptr, /*selected=*/false);  // white
            }
            continue;
        }
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        // Clean: no selection outlines or editor handles.
        for (const Quad* quad : scene->getQuadsByZOrder()) {
            if (!quad || !quad->isVisible()) {
                continue;
            }
            auto tex = textures.find(quad->getId());
            renderer.renderQuad(*quad, tex != textures.end() ? tex->second : nullptr,
                                /*selected=*/false);
        }
    }
    renderer.setOutputRotation(Rotation::R0);
    renderer.endFrame();
}

void OutputManager::renderOutputFrames(const std::vector<std::unique_ptr<Scene>>& scenes,
                                       const std::unordered_map<UUID, Texture*>& textures,
                                       Renderer& renderer) {
    if (!mainWindow_) {
        return;
    }
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevBlend[4] = {GL_ONE, GL_ZERO, GL_ONE, GL_ZERO};
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlend[0]);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevBlend[1]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlend[2]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlend[3]);
    renderCanvases(scenes, textures, renderer);
    // renderFrames saves and restores this state again around the frames.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glBlendFuncSeparate(static_cast<GLenum>(prevBlend[0]), static_cast<GLenum>(prevBlend[1]),
                        static_cast<GLenum>(prevBlend[2]), static_cast<GLenum>(prevBlend[3]));

    // Each output: its scene's objects from its projector (ADR-052), then its slices
    // of the scene's canvas over them. No scene stays black.
    renderFrames(renderer, [&](const OutputWindow& output, IVec2 effectiveSize) {
        const Scene* scene = sceneForOutput(scenes, output);
        if (!scene) {
            return;
        }
        renderer.renderMeshObjects(scene->getObjects(), output.getProjector(), textures);
        const Framebuffer* canvas = sceneCanvas(scene->getId());
        if (!canvas || output.getSlices().empty()) {
            return;
        }
        const Rotation canvasRotation = canvases_.at(scene->getId()).rotation;
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        for (const Slice& slice : output.getSlices()) {
            renderer.renderSlice(slice, canvas->colorTexture(), scene->getCanvasSize(),
                                 canvasRotation, effectiveSize);
        }
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    });
}

void OutputManager::renderPatternFrames(const Quad& patternQuad, const Texture* texture,
                                        Renderer& renderer) {
    // One fullscreen identity quad carrying the calibration texture.
    renderFrames(renderer, [&](const OutputWindow&, IVec2) {
        renderer.renderQuad(patternQuad, texture, /*selected=*/false);
    });
}

IVec2 OutputManager::frameSize(const OutputWindow& output) {
    if (output.isCreated()) {
        int width = 0, height = 0;
        glfwGetFramebufferSize(output.getGLFWwindow(), &width, &height);
        if (width > 0 && height > 0) {
            return IVec2(width, height);
        }
    }
    return output.getResolution();  // no window yet, or it is minimised (0 x 0)
}

void OutputManager::renderFrames(
        Renderer& renderer, const std::function<void(const OutputWindow&, IVec2)>& draw) {
    if (!mainWindow_) {
        return;
    }

    // Save GL state so the surrounding main-context render is undisturbed.
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLint prevBlend[4] = {GL_ONE, GL_ZERO, GL_ONE, GL_ZERO};
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlend[0]);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevBlend[1]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlend[2]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlend[3]);

    // Colour blends as on the projector; alpha keeps the clear's 1, so the preview,
    // which ImGui draws blended by the texture's alpha, is not darkened where quads
    // are translucent.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    // Drop the frames of outputs that no longer exist.
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (!getOutput(it->first)) {
            it = frames_.erase(it);
        } else {
            ++it;
        }
    }

    // Every output, shown or not: the preview panel shows hidden projectors too.
    for (auto& output : outputs_) {
        auto& frame = frames_[output->getId()];
        if (!frame) {
            frame = std::make_unique<Framebuffer>();
        }
        const IVec2 size = frameSize(*output);
        if (!frame->resize(size.x, size.y)) {
            continue;
        }

        frame->bind();  // also sets the viewport to the frame size
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (blackout_) {
            continue;  // the projector shows black, and so does its preview
        }
        const Rotation rotation = output->getRotation();
        renderer.setOutputRotation(rotation);
        const bool quarterTurn = rotation == Rotation::R90 || rotation == Rotation::R270;
        draw(*output, quarterTurn ? IVec2(size.y, size.x) : size);
    }

    // The editor canvas shares this Renderer and is never rotated.
    renderer.setOutputRotation(Rotation::R0);
    renderer.endFrame();

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    glBlendFuncSeparate(static_cast<GLenum>(prevBlend[0]), static_cast<GLenum>(prevBlend[1]),
                        static_cast<GLenum>(prevBlend[2]), static_cast<GLenum>(prevBlend[3]));

    // The output contexts wait for this on the GPU before copying the frames.
    if (framesDrawn_ != nullptr) {
        glDeleteSync(framesDrawn_);  // frames drawn but never presented
    }
    framesDrawn_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();  // the fence must reach the GPU before another context waits on it
}

const Framebuffer* OutputManager::outputFrame(const UUID& outputId) const {
    auto it = frames_.find(outputId);
    if (it == frames_.end() || !it->second || !it->second->valid()) {
        return nullptr;
    }
    return it->second.get();
}

void OutputManager::presentOutputs() {
    returnFocusToEditor();

    // Hidden outputs (not in perform mode, or hidden by the user) are neither
    // copied nor swapped: nobody sees them, and each swap costs frame time.
    std::vector<OutputWindow*> presented;
    std::vector<bool> iconified;
    for (auto& output : outputs_) {
        if (!output->isEnabled() || !output->isCreated() || !output->isVisible()) {
            continue;
        }
        presented.push_back(output.get());
        iconified.push_back(
            glfwGetWindowAttrib(output->getGLFWwindow(), GLFW_ICONIFIED) == GLFW_TRUE);
    }
    const int holder = vsyncHolder(iconified);

    GLFWwindow* previousContext = glfwGetCurrentContext();
    for (size_t i = 0; i < presented.size(); ++i) {
        OutputWindow* output = presented[i];

        output->makeContextCurrent();
        output->setSwapInterval(static_cast<int>(i) == holder ? 1 : 0);

        int width = 0, height = 0;
        glfwGetFramebufferSize(output->getGLFWwindow(), &width, &height);
        glViewport(0, 0, width, height);

        // Black unless there is a frame to show (in blackout the frame is black).
        const Framebuffer* frame = outputFrame(output->getId());
        bool shown = false;
        if (frame != nullptr) {
            if (framesDrawn_ != nullptr) {
                glWaitSync(framesDrawn_, 0, GL_TIMEOUT_IGNORED);
            }
            shown = output->frameCopy().copy(*frame, width, height);
        }
        if (!shown) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }

        output->swapBuffers();
    }

    // The fence belongs to the main context, which deletes it.
    if (framesDrawn_ != nullptr) {
        glfwMakeContextCurrent(mainWindow_);
        glDeleteSync(framesDrawn_);
        framesDrawn_ = nullptr;
    }
    if (previousContext != nullptr) {
        glfwMakeContextCurrent(previousContext);
    }

    // A presented, non-minimised projector paces the loop and the editor swaps
    // without waiting. Otherwise the editor is vsync'd as before.
    setEditorSwapInterval(holder >= 0 ? 0 : 1);
}

void OutputManager::setEditorSwapInterval(int interval) {
    if (mainWindow_ == nullptr || interval == editorSwapInterval_) {
        return;
    }
    GLFWwindow* previousContext = glfwGetCurrentContext();
    glfwMakeContextCurrent(mainWindow_);
    glfwSwapInterval(interval);
    glfwMakeContextCurrent(previousContext);
    editorSwapInterval_ = interval;
}

void OutputManager::returnFocusToEditor() {
    if (!performMode_) {
        return;
    }
    for (const auto& output : outputs_) {
        GLFWwindow* window = output->getGLFWwindow();
        if (window != nullptr && glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE) {
            LOG_DEBUG("Output '{}' took keyboard focus; returning it to the editor",
                      output->getName());
            glfwFocusWindow(mainWindow_);
            return;
        }
    }
}

const MonitorInfo* OutputManager::getMonitor(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= monitors_.size()) {
        return nullptr;
    }
    return &monitors_[index];
}

const MonitorInfo* OutputManager::getPrimaryMonitor() const {
    for (const auto& monitor : monitors_) {
        if (monitor.isPrimary) {
            return &monitor;
        }
    }
    return monitors_.empty() ? nullptr : &monitors_[0];
}

void OutputManager::setMonitorChangeCallback(MonitorChangeCallback callback) {
    monitorChangeCallback_ = std::move(callback);
}

void OutputManager::enableMonitorChangeDetection() {
    if (monitorChangeDetectionEnabled_) {
        return;
    }

    glfwSetMonitorCallback(&OutputManager::glfwMonitorCallback);
    monitorChangeDetectionEnabled_ = true;

    LOG_DEBUG("Monitor change detection enabled");
}

void OutputManager::disableMonitorChangeDetection() {
    if (!monitorChangeDetectionEnabled_) {
        return;
    }

    glfwSetMonitorCallback(nullptr);
    monitorChangeDetectionEnabled_ = false;

    LOG_DEBUG("Monitor change detection disabled");
}

void OutputManager::glfwMonitorCallback(GLFWmonitor* monitor, int event) {
    if (instance_ == nullptr) {
        return;
    }

    const char* monitorName = glfwGetMonitorName(monitor);
    if (event == GLFW_CONNECTED) {
        LOG_INFO("Monitor connected: {}", monitorName ? monitorName : "Unknown");
    } else if (event == GLFW_DISCONNECTED) {
        LOG_INFO("Monitor disconnected: {}", monitorName ? monitorName : "Unknown");
    }

    instance_->reresolveOutputs(/*refreshMonitorList=*/true);

    // Invoke the user callback if set
    if (instance_->monitorChangeCallback_) {
        instance_->monitorChangeCallback_();
    }
}

void OutputManager::reresolveOutputs(bool refreshMonitorList) {
    // Monitor indexes shift on hotplug, so remember each shown output's display
    // by identity to tell a real move from a renumbering.
    std::unordered_map<UUID, MonitorIdentity> shownOn;
    for (const auto& output : outputs_) {
        const MonitorInfo* monitor = getMonitor(output->getMonitor());
        if (monitor != nullptr && output->isVisible()) {
            shownOn[output->getId()] = {monitor->name, monitor->position, monitor->index};
        }
    }

    if (refreshMonitorList) {
        refreshMonitors();
    }
    resolveOutputs();
    if (!performMode_) {
        return;
    }

    for (auto& output : outputs_) {
        if (!output->isEnabled() || !output->isCreated()) {
            continue;
        }
        const MonitorInfo* monitor = getMonitor(output->getMonitor());
        if (monitor == nullptr) {
            if (output->isVisible()) {
                output->goWindowed();
                output->hide();
                notify("Projector '" + output->getName() +
                       "' has no display connected; its output is hidden");
            }
            continue;
        }
        auto was = shownOn.find(output->getId());
        if (was != shownOn.end() && was->second.name == monitor->name &&
            was->second.position == monitor->position) {
            continue;  // still on the same display
        }
        if (showOnResolvedMonitor(*output)) {
            notify("Projector '" + output->getName() + "' now showing on '" + monitor->name + "'");
        }
    }
    returnFocusToEditor();
}

void OutputManager::handleF11Key() {
    LOG_DEBUG("F11 key pressed - toggling perform mode");

    // If there are no outputs yet, auto-create one on the best secondary monitor
    // so a single F11 press immediately sends the scene fullscreen to a projector.
    if (!performMode_ && outputs_.empty()) {
        // No monitor is chosen: enterPerformMode picks the first display that is
        // not the editor's.
        OutputWindow* output = createOutput("Projector 1");
        if (output) {
            LOG_INFO("F11 auto-created output '{}'", output->getName());
        } else {
            LOG_ERROR("F11 auto-create output failed — cannot enter perform mode");
            return;
        }
    }

    togglePerformMode(useBorderlessFullscreen_);
}

void OutputManager::handleEscapeKey() {
    if (performMode_) {
        LOG_DEBUG("Escape key pressed - exiting perform mode");
        exitPerformMode();
    }
}

void OutputManager::handleOutputWindowKey(int key, int action, int mods) {
    if (action == GLFW_PRESS && (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_F11)) {
        performExitPending_ = true;
        return;
    }
    if (isBlackoutKey(key, action, mods)) {
        toggleBlackout();
        return;
    }
    const int sceneNumber = sceneTakeNumber(key, action, mods);
    if (sceneNumber > 0 && sceneKeyCallback_) {
        sceneKeyCallback_(sceneNumber);
    }
}

void OutputManager::applyPendingPerformExit() {
    if (!performExitPending_) return;
    performExitPending_ = false;
    LOG_DEBUG("Output window asked to leave perform mode");
    exitPerformMode();
}

void OutputManager::setupPerformModeKeyCallbacks() {
    for (auto& output : outputs_) {
        if (!output->isCreated()) {
            continue;
        }
        output->setKeyCallback([this](int key, int /*scancode*/, int action, int mods) {
            handleOutputWindowKey(key, action, mods);
        });
    }
}

int OutputManager::sceneTakeNumber(int key, int action, int mods) {
    constexpr int modifiers = GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER;
    if (action != GLFW_PRESS || (mods & modifiers) != 0) return 0;
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) return key - GLFW_KEY_0;
    if (key >= GLFW_KEY_KP_1 && key <= GLFW_KEY_KP_9) return key - GLFW_KEY_KP_0;
    return 0;
}

bool OutputManager::isBlackoutKey(int key, int action, int mods) {
    constexpr int others = GLFW_MOD_SHIFT | GLFW_MOD_ALT | GLFW_MOD_SUPER;
    return key == GLFW_KEY_B && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL) != 0 &&
           (mods & others) == 0;
}

void OutputManager::setBlackout(bool on) {
    if (on == blackout_) return;
    blackout_ = on;
    Logger::info("Blackout {}", on ? "ON: every output is black" : "off");
}

void OutputManager::clearPerformModeKeyCallbacks() {
    for (auto& output : outputs_) {
        if (!output->isCreated()) {
            continue;
        }
        // Clear the callback by setting an empty one
        output->setKeyCallback(nullptr);
    }
}

} // namespace quadmapper
