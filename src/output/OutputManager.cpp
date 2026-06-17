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

Result<void, Error> OutputManager::enterPerformMode(bool borderless) {
    if (performMode_) {
        return Ok();  // Already in perform mode
    }

    if (outputs_.empty()) {
        return Err("No outputs to display in perform mode");
    }

    LOG_INFO("Entering perform mode with {} outputs (borderless={})", outputs_.size(), borderless);

    // Refresh monitors in case configuration changed
    refreshMonitors();

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

        // Show the window
        output->show();

        // Go fullscreen on the assigned monitor
        int monitorIndex = output->getMonitor();
        if (monitorIndex < 0 && !monitors_.empty()) {
            // Default to primary monitor for outputs without assignment
            for (size_t i = 0; i < monitors_.size(); ++i) {
                if (monitors_[i].isPrimary) {
                    monitorIndex = static_cast<int>(i);
                    break;
                }
            }
            if (monitorIndex < 0) {
                monitorIndex = 0;  // Fallback to first monitor
            }
        }

        auto result = borderless
            ? output->goBorderlessFullscreen(monitorIndex)
            : output->goFullscreen(monitorIndex);

        if (result) {
            anySucceeded = true;
            LOG_INFO("Output '{}' {} on monitor {}",
                     output->getName(),
                     borderless ? "borderless fullscreen" : "fullscreen",
                     monitorIndex);
        } else {
            LOG_ERROR("Failed to go fullscreen for output '{}': {}",
                      output->getName(), result.error().message);
        }
    }

    if (!anySucceeded) {
        return Err("Failed to put any outputs in fullscreen mode");
    }

    performMode_ = true;

    // Setup key callbacks for Escape to exit
    setupPerformModeKeyCallbacks();

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

    GLFWwindow* previousContext = glfwGetCurrentContext();

    for (auto& output : outputs_) {
        if (!output->isEnabled() || !output->isCreated()) {
            continue;
        }

        auto rendererIt = outputRenderers_.find(output->getId());
        if (rendererIt == outputRenderers_.end()) {
            continue;
        }
        Renderer* renderer = rendererIt->second.get();

        output->makeContextCurrent();

        int width, height;
        glfwGetFramebufferSize(output->getGLFWwindow(), &width, &height);
        glViewport(0, 0, width, height);

        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Render only the scene assigned to THIS output (ADR-039 routing). An output
        // with no assigned scene stays black. renderQuad(...false) keeps it clean
        // (no selection outlines / editor handles).
        const Scene* scene = sceneForOutput(scenes, output->getId());
        if (scene) {
            for (const Quad* quad : scene->getQuadsByZOrder()) {
                if (!quad || !quad->isVisible()) {
                    continue;
                }
                const Texture* tex = nullptr;
                auto it = textures.find(quad->getId());
                if (it != textures.end()) {
                    tex = it->second;
                }
                renderer->renderQuad(*quad, tex, /*selected=*/false);
            }
        }

        glEnable(GL_DEPTH_TEST);

        output->swapBuffers();
    }

    if (previousContext != nullptr) {
        glfwMakeContextCurrent(previousContext);
    }
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

        // Same routing + clean-render rules as renderToOutputs, but into the FBO.
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

    GLFWwindow* previousContext = glfwGetCurrentContext();

    for (auto& output : outputs_) {
        if (!output->isEnabled() || !output->isCreated()) {
            continue;
        }

        auto rendererIt = outputRenderers_.find(output->getId());
        if (rendererIt == outputRenderers_.end()) {
            continue;
        }
        Renderer* renderer = rendererIt->second.get();

        output->makeContextCurrent();

        int width, height;
        glfwGetFramebufferSize(output->getGLFWwindow(), &width, &height);
        glViewport(0, 0, width, height);

        glDisable(GL_DEPTH_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // One fullscreen identity quad carrying the calibration texture.
        renderer->renderQuad(patternQuad, texture, /*selected=*/false);

        glEnable(GL_DEPTH_TEST);

        output->swapBuffers();
    }

    if (previousContext != nullptr) {
        glfwMakeContextCurrent(previousContext);
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

    // Refresh the monitor list
    instance_->refreshMonitors();

    // Invoke the user callback if set
    if (instance_->monitorChangeCallback_) {
        instance_->monitorChangeCallback_();
    }
}

void OutputManager::handleF11Key() {
    LOG_DEBUG("F11 key pressed - toggling perform mode");

    // If there are no outputs yet, auto-create one on the best secondary monitor
    // so a single F11 press immediately sends the scene fullscreen to a projector.
    if (!performMode_ && outputs_.empty()) {
        refreshMonitors();
        int monitorCount = static_cast<int>(monitors_.size());
        int targetMonitor = (monitorCount > 1) ? 1 : 0;

        OutputWindow* output = createOutput("Projector 1");
        if (output) {
            output->setMonitor(targetMonitor);
            LOG_INFO("F11 auto-created output on monitor {} ({})",
                     targetMonitor,
                     targetMonitor < monitorCount ? monitors_[targetMonitor].name : "unknown");
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
