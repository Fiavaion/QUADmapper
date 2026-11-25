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
        fadeLayers_.clear();
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

    // What was kept per output or per session goes with them: a second initialize()
    // starts clean.
    sceneFades_.clear();
    knownSizes_.clear();
    endIdentify();
    placedByHotplug_.clear();
    lastPresents_.clear();
    presentedRefresh_.clear();
    mixedRefreshNotice_.clear();
    failedFrameSizes_.clear();
    canvasScenes_.clear();
    maxTextureSize_ = 0;  // the next main window's context is asked again
    blackout_ = false;
    performExitPending_ = false;

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
        sceneFades_.cancelOutput(id);
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

int OutputManager::vsyncHolder(const std::vector<bool>& iconified, const std::vector<int>& refreshHz) {
    auto rate = [&](size_t i) { return i < refreshHz.size() && refreshHz[i] > 0 ? refreshHz[i] : 0; };
    int holder = -1;
    for (size_t i = 0; i < iconified.size(); ++i) {
        if (iconified[i]) {
            continue;
        }
        if (holder < 0) {
            holder = static_cast<int>(i);  // with no rate known: the first one
            continue;
        }
        const int hz = rate(i);
        const int current = rate(static_cast<size_t>(holder));
        if (hz > 0 && (current == 0 || hz < current)) {
            holder = static_cast<int>(i);
        }
    }
    return holder;
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

const OutputWindow* OutputManager::onlyScreenPreview(
        const std::vector<MonitorInfo>& monitors,
        const std::vector<std::unique_ptr<OutputWindow>>& outputs) {
    if (monitors.size() != 1) return nullptr;
    for (const auto& output : outputs) {
        if (output->isEnabled() && output->getMonitor() == 0) return nullptr;  // taken
    }
    for (const auto& output : outputs) {
        if (output->isEnabled() && output->isMonitorAutomatic() && output->getMonitor() < 0) {
            return output.get();
        }
    }
    return nullptr;
}

std::vector<int> OutputManager::displaysWithoutOutput(
        const std::vector<MonitorInfo>& monitors,
        const std::vector<std::unique_ptr<OutputWindow>>& outputs, int editorMonitor,
        bool includeEditorScreen) {
    auto used = [&outputs](int i) {
        return std::any_of(outputs.begin(), outputs.end(),
            [i](const std::unique_ptr<OutputWindow>& o) { return o && o->getMonitor() == i; });
    };
    const int count = static_cast<int>(monitors.size());
    std::vector<int> displays;
    for (int i = 0; i < count; ++i) {
        if (i != editorMonitor && !used(i)) displays.push_back(i);
    }
    if (includeEditorScreen && count > 1 && editorMonitor >= 0 && editorMonitor < count &&
        !used(editorMonitor)) {
        displays.push_back(editorMonitor);
    }
    return displays;
}

std::string OutputManager::nextProjectorName(const std::vector<std::unique_ptr<OutputWindow>>& outputs) {
    for (int n = 1; ; ++n) {
        const std::string name = "Projector " + std::to_string(n);
        const bool taken = std::any_of(outputs.begin(), outputs.end(),
            [&name](const std::unique_ptr<OutputWindow>& o) { return o && o->getName() == name; });
        if (!taken) return name;
    }
}

std::vector<OutputWindow*> OutputManager::addOutputsForUnusedDisplays(bool includeEditorScreen) {
    std::vector<OutputWindow*> added;
    if (performMode_) return added;
    resolveOutputs();  // an output waiting for a display takes it first
    const bool hadNone = outputs_.empty();
    const int editorMonitor = editorMonitorIndex();
    for (int index : displaysWithoutOutput(monitors_, outputs_, editorMonitor, includeEditorScreen)) {
        OutputWindow* output = createOutput(nextProjectorName(outputs_));
        if (!output) continue;
        // The very first output stays Automatic; it resolves to this display below,
        // since every other new output is chosen for its own. Automatic never picks
        // the editor's screen, so that screen's output is always chosen.
        if (!(hadNone && added.empty()) || index == editorMonitor) assignMonitor(*output, index);
        added.push_back(output);
        LOG_INFO("Display '{}' found: added output '{}'", monitors_[index].name, output->getName());
    }
    resolveOutputs();
    for (OutputWindow* output : added) adoptDisplayResolution(*output);
    return added;
}

std::vector<UUID> OutputManager::takeOutputsPlacedByHotplug() {
    std::vector<UUID> placed;
    placed.swap(placedByHotplug_);
    return placed;
}

bool OutputManager::adoptDisplayResolution(OutputWindow& output) {
    const MonitorInfo* monitor = getMonitor(output.getMonitor());
    if (monitor == nullptr || monitor->resolution.x <= 0 || monitor->resolution.y <= 0 ||
        output.getResolution() == monitor->resolution) {
        return false;
    }
    output.setResolution(monitor->resolution);
    if (output.isCreated()) {
        auto result = output.applyResolution();
        if (!result) {
            LOG_WARNING("Output '{}': could not apply {}x{}: {}", output.getName(),
                        monitor->resolution.x, monitor->resolution.y, result.error().message);
        }
    }
    return true;
}

std::string OutputManager::describeMonitorChoice(const std::vector<MonitorInfo>& monitors,
                                                const OutputWindow& output,
                                                bool previewsOnOnlyScreen) {
    const int current = output.getMonitor();
    if (current >= 0 && static_cast<size_t>(current) < monitors.size()) {
        std::string label = monitors[current].getDisplayString();
        if (output.isMonitorAutomatic()) label += " (automatic)";
        return label;
    }
    const MonitorIdentity& desired = output.getDesiredMonitor();
    if (!desired.name.empty()) return desired.name + " (not connected)";
    if (previewsOnOnlyScreen && monitors.size() == 1) {
        return monitors[0].getDisplayString() + " (automatic: the only display)";
    }
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
    // An output whose window was made after the show started has no keys yet: Esc,
    // F11, blackout and the scene takes must work on it too.
    if (performMode_) setPerformModeKeyCallback(output);
    return true;
}

Result<void, Error> OutputManager::enterPerformMode(bool borderless) {
    if (performMode_) {
        // Asked for again (a Gray-code scan, a calibration) while Identify holds perform
        // mode: it now stays on when Identify ends.
        identifyLeavesPerform_ = false;
        return Ok();
    }

    if (performRefusal_) {
        const std::string reason = performRefusal_();
        if (!reason.empty()) {
            notify(reason);
            return Err(reason);
        }
    }

    if (outputs_.empty()) {
        notify("Perform mode needs an output: add one in the Outputs tab");
        return Err("No outputs to display in perform mode");
    }

    LOG_INFO("Entering perform mode with {} outputs (borderless={})", outputs_.size(), borderless);

    // Resolve at the last moment: the projector may have been plugged in since
    // the show was loaded.
    refreshMonitors();
    resolveOutputs();

    useBorderlessFullscreen_ = borderless;
    bool anySucceeded = false;

    const OutputWindow* preview = onlyScreenPreview(monitors_, outputs_);
    bool reasonShown = false;
    for (auto& output : outputs_) {
        if (!output->isEnabled()) {
            continue;
        }

        if (!output->isCreated()) {
            LOG_WARNING("Output '{}' not created, skipping", output->getName());
            continue;
        }

        if (output.get() == preview) {
            output->setMonitor(0);
        }
        if (output->getMonitor() < 0) {
            notify("Projector '" + output->getName() + "' has no display; its output is not shown."
                   " Choose one for it in the Outputs tab.");
            reasonShown = true;
            continue;
        }
        if (showOnResolvedMonitor(*output)) {
            anySucceeded = true;
        }
    }

    if (!anySucceeded) {
        // The status line holds one message: the one naming the projector stays.
        if (!reasonShown) notify("Perform mode did not start: no output could be shown on a display");
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
    endIdentify();  // the projectors are gone: nothing left to identify

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

bool OutputManager::identifyOutputs() {
    if (!performMode_) {
        if (!enterPerformMode(useBorderlessFullscreen_)) return false;
        identifyLeavesPerform_ = true;
    }
    identifyUntil_ = frameTime_ + IDENTIFY_SECONDS;
    return true;
}

void OutputManager::retireIdentify() {
    if (!isIdentifying() || frameTime_ < identifyUntil_) return;
    if (identifyLeavesPerform_) {
        exitPerformMode();  // ends Identify too
    } else {
        endIdentify();
    }
}

void OutputManager::endIdentify() {
    identifyUntil_ = 0.0;
    identifyLeavesPerform_ = false;
    identifyOverlays_.clear();
}

void OutputManager::renderIdentifyOverlay(const OutputWindow& output, int number, IVec2 size,
                                          Renderer& renderer) {
    IdentifyOverlay& overlay = identifyOverlays_[output.getId()];
    if (overlay.quads.empty() || overlay.number != number || overlay.size != size) {
        overlay = IdentifyOverlay{number, size, makeIdentifyOverlay(number, size)};
    }
    glDisable(GL_DEPTH_TEST);  // over the scene's 3D objects too
    for (const Quad& quad : overlay.quads) {
        renderer.renderQuad(quad, nullptr, /*selected=*/false);
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

std::vector<Quad> makeIdentifyOverlay(int number, IVec2 frameSize) {
    const float w = static_cast<float>(std::max(frameSize.x, 1));
    const float h = static_cast<float>(std::max(frameSize.y, 1));
    std::vector<Quad> quads;
    // A rectangle given in pixels of the frame, stored normalized.
    auto rect = [&](float x0, float y0, float x1, float y1) {
        Quad quad;
        quad.setName("Identify");
        quad.getGeometry().setCorners({Vec2(x0 / w, y0 / h), Vec2(x1 / w, y0 / h),
                                       Vec2(x1 / w, y1 / h), Vec2(x0 / w, y1 / h)});
        quads.push_back(std::move(quad));
    };

    const std::string digits = std::to_string(std::clamp(number, 0, 99));
    // Digits sized to the frame, centred, on a panel a little larger.
    const float digitH = std::min(h * 0.4f, w * 0.3f);
    const float digitW = digitH * 0.5f;
    const float t = digitH * 0.12f;  // segment thickness
    const float gap = digitW * 0.3f;
    const float pad = digitH * 0.15f;
    const float count = static_cast<float>(digits.size());
    const float textW = count * digitW + (count - 1.0f) * gap;
    const float left = (w - textW) * 0.5f;
    const float top = (h - digitH) * 0.5f;
    rect(left - pad, top - pad, left + textW + pad, top + digitH + pad);

    // The frame's edge, so the projector's extent shows.
    const float edge = std::max(4.0f, std::min(w, h) * 0.008f);
    rect(0.0f, 0.0f, w, edge);
    rect(0.0f, h - edge, w, h);
    rect(0.0f, edge, edge, h - edge);
    rect(w - edge, edge, w, h - edge);

    // Segments a b c d e f g, as bits 0-6.
    static constexpr std::array<unsigned char, 10> SEGMENTS = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    for (size_t i = 0; i < digits.size(); ++i) {
        const unsigned char on = SEGMENTS[static_cast<size_t>(digits[i] - '0')];
        const float x0 = left + static_cast<float>(i) * (digitW + gap);
        const float x1 = x0 + digitW;
        const float y0 = top;
        const float ym = top + digitH * 0.5f;
        const float y1 = top + digitH;
        if (on & 0x01) rect(x0, y0, x1, y0 + t);                    // a: top
        if (on & 0x02) rect(x1 - t, y0, x1, ym + t * 0.5f);         // b: top right
        if (on & 0x04) rect(x1 - t, ym - t * 0.5f, x1, y1);         // c: bottom right
        if (on & 0x08) rect(x0, y1 - t, x1, y1);                    // d: bottom
        if (on & 0x10) rect(x0, ym - t * 0.5f, x0 + t, y1);         // e: bottom left
        if (on & 0x20) rect(x0, y0, x0 + t, ym + t * 0.5f);         // f: top left
        if (on & 0x40) rect(x0, ym - t * 0.5f, x1, ym + t * 0.5f);  // g: middle
    }
    // The panel: dark and nearly opaque, so the number reads over any picture.
    ColorCorrection dark;
    dark.rgbGain = Vec3(0.0f);
    quads[0].setColorCorrection(dark);
    quads[0].setOpacity(0.85f);
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

bool OutputManager::takeScene(OutputWindow& output, Scene* scene,
                              const std::vector<std::unique_ptr<Scene>>& scenes, double fadeSeconds) {
    const UUID from = output.getSceneId();
    const bool canvasChanged = routeScene(output, scene, scenes);
    sceneFades_.begin(output.getId(), from, output.getSceneId(), frameTime_, fadeSeconds);
    return canvasChanged;
}

bool OutputManager::retireSceneFades(const std::vector<std::unique_ptr<Scene>>& scenes) {
    auto known = [&](const UUID& id) {
        return std::any_of(scenes.begin(), scenes.end(),
                           [&](const auto& s) { return s && s->getId() == id; });
    };
    return sceneFades_.retire(frameTime_, [&](const UUID& outputId, const SceneFade& fade) {
        const OutputWindow* output = getOutput(outputId);
        return output && output->getSceneId() == fade.toSceneId && known(fade.toSceneId) &&
               (fade.fromSceneId.isNull() || known(fade.fromSceneId)) &&
               (fade.fromNextSceneId.isNull() || known(fade.fromNextSceneId));
    });
}

const SceneFade* OutputManager::sceneFade(const OutputWindow& output) const {
    const SceneFade* fade = sceneFades_.find(output.getId());
    if (!fade || fade->toSceneId != output.getSceneId() || output.getSceneId().isNull() ||
        sceneFadeMix(*fade, frameTime_) >= 1.0f) {
        return nullptr;
    }
    return fade;
}

bool OutputManager::sceneOnAnOutput(const UUID& sceneId) const {
    if (firstOutputShowing(sceneId)) {
        return true;
    }
    return std::any_of(outputs_.begin(), outputs_.end(), [&](const auto& output) {
        const SceneFade* fade = sceneFade(*output);
        return fade && (fade->fromSceneId == sceneId || fade->fromNextSceneId == sceneId);
    });
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
        if (!sceneOnAnOutput(it->first)) {
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
    // The scenes outputs show, and those they are fading out of, each once.
    canvasScenes_.clear();
    auto addCanvasScene = [&](const UUID& id) {
        const bool exists = !id.isNull() && std::any_of(scenes.begin(), scenes.end(),
                                                        [&](const auto& s) { return s && s->getId() == id; });
        if (exists && std::find(canvasScenes_.begin(), canvasScenes_.end(), id) == canvasScenes_.end()) {
            canvasScenes_.push_back(id);
        }
    };
    for (const auto& output : outputs_) {
        if (const Scene* scene = sceneForOutput(scenes, *output)) {
            addCanvasScene(scene->getId());
        }
        if (const SceneFade* fade = sceneFade(*output)) {
            addCanvasScene(fade->fromSceneId);
            addCanvasScene(fade->fromNextSceneId);
        }
    }
    for (auto it = canvases_.begin(); it != canvases_.end();) {
        if (std::find(canvasScenes_.begin(), canvasScenes_.end(), it->first) == canvasScenes_.end()) {
            it = canvases_.erase(it);
        } else {
            ++it;
        }
    }
    if (blackout_ && !exportRendering_) {
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
        // The rotation all its outputs (showing it or fading out of it) share, else none.
        bool first = true;
        for (const auto& output : outputs_) {
            const SceneFade* fade = sceneFade(*output);
            const bool fadingOut = fade && (fade->fromSceneId == sceneId || fade->fromNextSceneId == sceneId);
            if (output->getSceneId() != sceneId && !fadingOut) continue;
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

    // Object layers of outputs that no longer fade (or no longer exist) go.
    for (auto it = fadeLayers_.begin(); it != fadeLayers_.end();) {
        const OutputWindow* output = getOutput(it->first);
        it = output && sceneFade(*output) ? std::next(it) : fadeLayers_.erase(it);
    }

    // Each output: its scene's objects from its projector (ADR-052), then its slices
    // of the scene's canvas over them. No scene stays black. During a crossfade the
    // output shows the weighted mix of each scene's own frame: the slices mix the
    // canvases when no scene has objects, and otherwise each scene's frame is drawn
    // whole into its own layer and the layers are mixed (renderFadeLayers).
    auto findScene = [&](const UUID& id) -> const Scene* {
        if (id.isNull()) return nullptr;
        for (const auto& s : scenes) {
            if (s && s->getId() == id) return s.get();
        }
        return nullptr;
    };
    auto drawScene = [&](const OutputWindow& output, IVec2 effectiveSize) {
        const Scene* scene = sceneForOutput(scenes, output);
        if (!scene) {
            return;
        }
        const SceneFade* fade = sceneFade(output);
        if (!fade) {
            renderSceneFrame(output, *scene, effectiveSize, textures, renderer);
            return;
        }
        Renderer::SliceFade sliceFade;
        sliceFade.mix = sceneFadeMix(*fade, frameTime_);
        sliceFade.fromNextWeight = fade->fromNextWeight;
        const Scene* from = findScene(fade->fromSceneId);
        const Scene* fromNext = findScene(fade->fromNextSceneId);
        if (renderFadeLayers(output, effectiveSize, {from, fromNext, scene}, sliceFade, textures, renderer)) {
            return;
        }
        auto outgoing = [&](const Scene* s) {
            Renderer::SliceFadeCanvas c;
            if (const Framebuffer* target = s ? sceneCanvas(s->getId()) : nullptr) {
                c.texture = target->colorTexture();
                c.size = s->getCanvasSize();
                c.rotation = canvases_.at(s->getId()).rotation;
            }
            return c;
        };
        sliceFade.from = outgoing(from);
        sliceFade.fromNext = outgoing(fromNext);
        renderSceneSlices(output, *scene, effectiveSize, &sliceFade, renderer);
    };
    renderFrames(renderer, [&](const OutputWindow& output, IVec2 effectiveSize) {
        drawScene(output, effectiveSize);
        if (!isIdentifying() || exportRendering_) return;
        for (size_t i = 0; i < outputs_.size(); ++i) {
            if (outputs_[i].get() == &output) {
                renderIdentifyOverlay(output, static_cast<int>(i) + 1, effectiveSize, renderer);
                break;
            }
        }
    });
}

void OutputManager::renderSceneFrame(const OutputWindow& output, const Scene& scene,
                                     IVec2 effectiveSize,
                                     const std::unordered_map<UUID, Texture*>& textures,
                                     Renderer& renderer) {
    renderer.renderMeshObjects(scene.getObjects(), output.getProjector(), textures);
    renderSceneSlices(output, scene, effectiveSize, nullptr, renderer);
}

void OutputManager::renderSceneSlices(const OutputWindow& output, const Scene& scene,
                                      IVec2 effectiveSize, const Renderer::SliceFade* fade,
                                      Renderer& renderer) {
    const Framebuffer* canvas = sceneCanvas(scene.getId());
    if (!canvas || output.getSlices().empty()) {
        return;
    }
    const Rotation canvasRotation = canvases_.at(scene.getId()).rotation;
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    for (const Slice& slice : output.getSlices()) {
        renderer.renderSlice(slice, canvas->colorTexture(), scene.getCanvasSize(), canvasRotation,
                             effectiveSize, fade);
    }
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
}

bool OutputManager::renderFadeLayers(const OutputWindow& output, IVec2 effectiveSize,
                                     const std::array<const Scene*, 3>& legs,
                                     const Renderer::SliceFade& weights,
                                     const std::unordered_map<UUID, Texture*>& textures,
                                     Renderer& renderer) {
    const bool anyObjects = std::any_of(legs.begin(), legs.end(), [](const Scene* s) {
        return s && !s->getObjects().empty();
    });
    if (!anyObjects) {
        return false;
    }
    // The frame renderFrames bound (its viewport is the frame's size).
    GLint frameFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &frameFbo);
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);

    // Each scene's frame alone, exactly as the output shows it without a fade: its
    // objects, then its canvas through the output's slices, blend zones and black
    // level, over transparent black. An absent scene needs no layer (it is black).
    FadeLayers& layers = fadeLayers_[output.getId()];
    std::array<uint32_t, 3> textureOf{0, 0, 0};
    for (size_t i = 0; i < legs.size(); ++i) {
        const Scene* scene = legs[i];
        if (!scene) {
            continue;
        }
        std::unique_ptr<Framebuffer>& layer = layers.legs[i];
        if (!layer) {
            layer = std::make_unique<Framebuffer>(Framebuffer::Depth::With);
        }
        if (!layer->resize(viewport[2], viewport[3])) {
            continue;
        }
        layer->bind();
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        renderSceneFrame(output, *scene, effectiveSize, textures, renderer);
        textureOf[i] = layer->colorTexture();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(frameFbo));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (textureOf[2] == 0) {
        return false;  // could not allocate: the canvases alone rather than a wrong mix
    }

    // Mix them into the black frame: a whole-frame slice of layers drawn exactly as the
    // frame, so every pixel is the weighted sum of the texels under it (one rounding).
    // The layers' alpha stays 0 (objects and slices blend colour only), so the sum
    // lands as is.
    const Rotation rotation = output.getRotation();
    Renderer::SliceFade mix = weights;
    mix.from = Renderer::SliceFadeCanvas{textureOf[0], effectiveSize, rotation};
    mix.fromNext = Renderer::SliceFadeCanvas{textureOf[1], effectiveSize, rotation};
    fadeLayerSlice_.input = CanvasRect{0.0f, 0.0f, static_cast<float>(effectiveSize.x),
                                       static_cast<float>(effectiveSize.y)};
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    renderer.renderSlice(fadeLayerSlice_, textureOf[2], effectiveSize, rotation, effectiveSize, &mix);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    return true;
}

void OutputManager::renderPatternFrames(const Quad& patternQuad, const Texture* texture,
                                        Renderer& renderer, const UUID& only) {
    // One fullscreen identity quad carrying the calibration texture.
    renderFrames(renderer, [&](const OutputWindow& output, IVec2) {
        if (!only.isNull() && output.getId() != only) return;  // left black
        renderer.renderQuad(patternQuad, texture, /*selected=*/false);
    });
}

IVec2 OutputManager::exportFrameSize(const OutputWindow& output) {
    const IVec2 r = output.getResolution();
    return IVec2(std::max(r.x, 2) & ~1, std::max(r.y, 2) & ~1);
}

IVec2 OutputManager::frameSize(const OutputWindow& output) const {
    if (exportRendering_) return exportFrameSize(output);
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
            failedFrameSizes_.erase(it->first);
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
            auto& failed = failedFrameSizes_[output->getId()];
            if (failed != size) {
                failed = size;
                LOG_ERROR("Output '{}': cannot allocate its {}x{} frame; its projector shows black",
                          output->getName(), size.x, size.y);
                notify("Projector '" + output->getName() + "': not enough video memory for " +
                       std::to_string(size.x) + "x" + std::to_string(size.y) +
                       "; lower its resolution in Outputs");
            }
            continue;
        }
        failedFrameSizes_.erase(output->getId());

        frame->bind();  // also sets the viewport to the frame size
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (blackout_ && !exportRendering_) {
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
    lastPresents_.clear();

    // Hidden outputs (not in perform mode, or hidden by the user) are neither
    // copied nor swapped: nobody sees them, and each swap costs frame time.
    std::vector<OutputWindow*> presented;
    std::vector<bool> iconified;
    presentedRefresh_.clear();
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    for (auto& output : outputs_) {
        if (!output->isEnabled() || !output->isCreated() || !output->isVisible()) {
            continue;
        }
        presented.push_back(output.get());
        iconified.push_back(
            glfwGetWindowAttrib(output->getGLFWwindow(), GLFW_ICONIFIED) == GLFW_TRUE);
        // The display it is on, when it covers one (perform mode); a window's rate is unknown.
        int hz = 0;
        const int m = output->getMonitor();
        if ((output->isFullscreen() || output->isBorderless()) && m >= 0 && m < monitorCount) {
            if (const GLFWvidmode* mode = glfwGetVideoMode(monitors[m])) hz = mode->refreshRate;
        }
        presentedRefresh_.push_back(hz);
    }
    const int holder = vsyncHolder(iconified, presentedRefresh_);
    noticeMixedRefresh(presented, holder);

    GLFWwindow* previousContext = glfwGetCurrentContext();
    for (size_t i = 0; i < presented.size(); ++i) {
        OutputWindow* output = presented[i];

        output->makeContextCurrent();
        const int interval = static_cast<int>(i) == holder ? 1 : 0;
        output->setSwapInterval(interval);

        int width = 0, height = 0;
        glfwGetFramebufferSize(output->getGLFWwindow(), &width, &height);
        glViewport(0, 0, width, height);

        // Black unless there is a frame to show (in blackout the frame is black).
        const Framebuffer* frame = outputFrame(output->getId());
        bool shown = false;
        // Blackout: black, also over an export's frame (drawn without blackout).
        if (frame != nullptr && !blackout_) {
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
        lastPresents_.push_back({output->getId(), std::chrono::steady_clock::now(), interval});
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

void OutputManager::noticeMixedRefresh(const std::vector<OutputWindow*>& presented, int holder) {
    std::vector<int> rates;
    for (int hz : presentedRefresh_) {
        if (hz > 0 && std::find(rates.begin(), rates.end(), hz) == rates.end()) rates.push_back(hz);
    }
    std::string text;
    if (rates.size() > 1 && holder >= 0) {
        std::sort(rates.begin(), rates.end());
        std::string list;
        for (int hz : rates) list += (list.empty() ? "" : ", ") + std::to_string(hz) + " Hz";
        text = "Projector outputs run at different refresh rates (" + list + "): the show is paced at " +
               std::to_string(presentedRefresh_[static_cast<size_t>(holder)]) + " Hz by '" +
               presented[static_cast<size_t>(holder)]->getName() +
               "'; outputs at another rate repeat or drop frames";
    }
    if (text != mixedRefreshNotice_) {
        mixedRefreshNotice_ = text;
        if (!text.empty()) notify(text);
    }
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

    std::vector<UUID> waiting;  // Automatic outputs with no display before a hotplug
    for (const auto& output : outputs_) {
        if (output->getMonitor() < 0 && output->isMonitorAutomatic()) {
            waiting.push_back(output->getId());
        }
    }

    if (refreshMonitorList) {
        refreshMonitors();
    }
    resolveOutputs();
    if (refreshMonitorList) {
        for (const UUID& id : waiting) {
            const OutputWindow* output = getOutput(id);
            if (output != nullptr && output->getMonitor() >= 0 &&
                std::find(placedByHotplug_.begin(), placedByHotplug_.end(), id) == placedByHotplug_.end()) {
                placedByHotplug_.push_back(id);
            }
        }
    }
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
        // not the editor's, or the editor's when it is the only one.
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
    bool leavesPerformMode = key == GLFW_KEY_ESCAPE || key == GLFW_KEY_F11;
#ifdef __APPLE__
    // Option+P is the perform-mode chord on macOS (ADR-062).
    constexpr int chordMods = GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER;
    if (key == GLFW_KEY_P && (mods & chordMods) == GLFW_MOD_ALT) leavesPerformMode = true;
#endif
    if (action == GLFW_PRESS && leavesPerformMode) {
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
        setPerformModeKeyCallback(*output);
    }
}

void OutputManager::setPerformModeKeyCallback(OutputWindow& output) {
    output.setKeyCallback([this](int key, int /*scancode*/, int action, int mods) {
        handleOutputWindowKey(key, action, mods);
    });
}

int OutputManager::sceneTakeNumber(int key, int action, int mods) {
    constexpr int modifiers = GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER;
    if (action != GLFW_PRESS || (mods & modifiers) != 0) return 0;
    if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) return key - GLFW_KEY_0;
    if (key >= GLFW_KEY_KP_1 && key <= GLFW_KEY_KP_9) return key - GLFW_KEY_KP_0;
    return 0;
}

bool OutputManager::isBlackoutKey(int key, int action, int mods) {
#ifdef __APPLE__
    // Cmd+B, as in the editor (where Dear ImGui reads Cmd as Ctrl), or Control+B (ADR-062).
    constexpr int chordMods = GLFW_MOD_SHIFT | GLFW_MOD_CONTROL | GLFW_MOD_ALT | GLFW_MOD_SUPER;
    const int held = mods & chordMods;
    return key == GLFW_KEY_B && action == GLFW_PRESS &&
           (held == GLFW_MOD_SUPER || held == GLFW_MOD_CONTROL);
#else
    constexpr int others = GLFW_MOD_SHIFT | GLFW_MOD_ALT | GLFW_MOD_SUPER;
    return key == GLFW_KEY_B && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL) != 0 &&
           (mods & others) == 0;
#endif
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
