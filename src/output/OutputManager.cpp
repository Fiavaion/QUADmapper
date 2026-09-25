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

    // Destroy per-output renderers in their respective GL contexts before windows close.
    GLFWwindow* previousContext = glfwGetCurrentContext();
    for (auto& output : outputs_) {
        auto rendererIt = outputRenderers_.find(output->getId());
        if (rendererIt != outputRenderers_.end() && output->isCreated()) {
            output->makeContextCurrent();
            rendererIt->second->shutdown();
        }
    }
    outputRenderers_.clear();
    if (previousContext != nullptr) {
        glfwMakeContextCurrent(previousContext);
    }

    // Destroy the preview FBOs (they live in the main context, current now).
    previewTargets_.clear();

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

        // Create a Renderer for this output in its own GL context.
        // VAOs are NOT shared across contexts, so each output needs its own.
        // Shaders and VBOs are shared and will be re-created per-context (cheap).
        GLFWwindow* previousContext = glfwGetCurrentContext();
        output->makeContextCurrent();

        auto renderer = std::make_unique<Renderer>();
        if (!renderer->initialize()) {
            LOG_ERROR("Failed to initialize renderer for output '{}'", outputName);
            glfwMakeContextCurrent(previousContext);
            output->destroy();
            return nullptr;
        }

        outputRenderers_[output->getId()] = std::move(renderer);
        glfwMakeContextCurrent(previousContext);
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

        // Destroy the per-output renderer in its own GL context before the window goes away.
        auto rendererIt = outputRenderers_.find(id);
        if (rendererIt != outputRenderers_.end()) {
            GLFWwindow* previousContext = glfwGetCurrentContext();
            (*it)->makeContextCurrent();
            rendererIt->second->shutdown();
            outputRenderers_.erase(rendererIt);
            glfwMakeContextCurrent(previousContext);
        }

        (*it)->destroy();
        outputs_.erase(it);
        LOG_INFO("Deleted output '{}' (remaining: {})", name, outputs_.size());
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
        const std::vector<std::unique_ptr<Scene>>& scenes, const UUID& outputId) {
    if (outputId.isNull()) {
        return nullptr;
    }
    for (const auto& scene : scenes) {
        if (scene && scene->getAssignedOutputId() == outputId) {
            return scene.get();
        }
    }
    return nullptr;
}

void OutputManager::renderToOutputs(const std::vector<std::unique_ptr<Scene>>& scenes,
                                     const std::unordered_map<UUID, Texture*>& textures) {
    if (!mainWindow_) {
        return;
    }

    // Render only the scene assigned to THIS output (ADR-039 routing). An output
    // with no assigned scene stays black. renderQuad(...false) keeps it clean
    // (no selection outlines / editor handles).
    presentToOutputs([&](const OutputWindow& output, Renderer& renderer) {
        const Scene* scene = sceneForOutput(scenes, output.getId());
        if (!scene) {
            return;
        }
        for (const Quad* quad : scene->getQuadsByZOrder()) {
            if (!quad || !quad->isVisible()) {
                continue;
            }
            const Texture* tex = nullptr;
            auto it = textures.find(quad->getId());
            if (it != textures.end()) {
                tex = it->second;
            }
            renderer.renderQuad(*quad, tex, /*selected=*/false);
        }
    });
}

void OutputManager::updatePreviews(const std::vector<std::unique_ptr<Scene>>& scenes,
                                   const std::unordered_map<UUID, Texture*>& textures,
                                   Renderer& renderer) {
    if (!mainWindow_) {
        return;
    }

    // Save GL state so the surrounding main-context render is undisturbed.
    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    // Drop thumbnails for outputs that no longer exist.
    for (auto it = previewTargets_.begin(); it != previewTargets_.end();) {
        if (!getOutput(it->first)) {
            it = previewTargets_.erase(it);
        } else {
            ++it;
        }
    }

    for (auto& output : outputs_) {
        auto& target = previewTargets_[output->getId()];
        if (!target) {
            target = std::make_unique<Framebuffer>();
        }
        if (!target->resize(kPreviewWidth, kPreviewHeight)) {
            continue;
        }

        target->bind();  // also sets the viewport to the thumbnail size
        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Same routing + clean-render + rotation rules as renderToOutputs, but
        // into the FBO, so the thumbnail shows what the projector shows.
        renderer.setOutputRotation(output->getRotation());
        const Scene* scene = sceneForOutput(scenes, output->getId());
        if (scene) {
            for (const Quad* quad : scene->getQuadsByZOrder()) {
                if (!quad || !quad->isVisible()) {
                    continue;
                }
                const Texture* tex = nullptr;
                auto texIt = textures.find(quad->getId());
                if (texIt != textures.end()) {
                    tex = texIt->second;
                }
                renderer.renderQuad(*quad, tex, /*selected=*/false);
            }
        }
    }

    // The editor canvas shares this Renderer and is never rotated.
    renderer.setOutputRotation(Rotation::R0);

    // Restore the previous framebuffer + viewport for the rest of the frame.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

unsigned int OutputManager::previewTextureFor(const UUID& outputId) const {
    auto it = previewTargets_.find(outputId);
    if (it == previewTargets_.end() || !it->second) {
        return 0;
    }
    return it->second->colorTexture();
}

void OutputManager::renderPatternToOutputs(const Quad& patternQuad,
                                           const Texture* texture) {
    if (!mainWindow_) {
        return;
    }

    // One fullscreen identity quad carrying the calibration texture.
    presentToOutputs([&](const OutputWindow&, Renderer& renderer) {
        renderer.renderQuad(patternQuad, texture, /*selected=*/false);
    });
}

void OutputManager::presentToOutputs(
        const std::function<void(const OutputWindow&, Renderer&)>& draw) {
    returnFocusToEditor();

    // Hidden outputs (not in perform mode, or hidden by the user) are neither
    // drawn nor swapped: nobody sees them, and each swap costs frame time.
    std::vector<std::pair<OutputWindow*, Renderer*>> presented;
    std::vector<bool> iconified;
    for (auto& output : outputs_) {
        if (!output->isEnabled() || !output->isCreated() || !output->isVisible()) {
            continue;
        }
        auto rendererIt = outputRenderers_.find(output->getId());
        if (rendererIt == outputRenderers_.end()) {
            continue;
        }
        presented.emplace_back(output.get(), rendererIt->second.get());
        iconified.push_back(
            glfwGetWindowAttrib(output->getGLFWwindow(), GLFW_ICONIFIED) == GLFW_TRUE);
    }
    const int holder = vsyncHolder(iconified);

    GLFWwindow* previousContext = glfwGetCurrentContext();
    for (size_t i = 0; i < presented.size(); ++i) {
        OutputWindow* output = presented[i].first;
        Renderer* renderer = presented[i].second;

        output->makeContextCurrent();
        output->setSwapInterval(static_cast<int>(i) == holder ? 1 : 0);

        int width, height;
        glfwGetFramebufferSize(output->getGLFWwindow(), &width, &height);
        glViewport(0, 0, width, height);

        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        renderer->setOutputRotation(output->getRotation());
        draw(*output, *renderer);

        glEnable(GL_DEPTH_TEST);

        output->swapBuffers();
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

void OutputManager::setupPerformModeKeyCallbacks() {
    for (auto& output : outputs_) {
        if (!output->isCreated()) {
            continue;
        }

        output->setKeyCallback([this](int key, int /*scancode*/, int action, int /*mods*/) {
            if (action == GLFW_PRESS) {
                if (key == GLFW_KEY_ESCAPE) {
                    handleEscapeKey();
                } else if (key == GLFW_KEY_F11) {
                    handleF11Key();
                }
            }
        });
    }
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
