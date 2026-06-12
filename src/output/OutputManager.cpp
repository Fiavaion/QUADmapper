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

void OutputManager::renderToOutputs() {
    if (!mainWindow_) {
        return;
    }

    // Store the current context so we can restore it
    GLFWwindow* previousContext = glfwGetCurrentContext();

    for (auto& output : outputs_) {
        if (!output->isEnabled() || !output->isCreated()) {
            continue;
        }

        // Make this output's context current
        output->makeContextCurrent();

        // Get the framebuffer size for the viewport
        int width, height;
        glfwGetFramebufferSize(output->getGLFWwindow(), &width, &height);

        // Set viewport
        glViewport(0, 0, width, height);

        // Clear with a dark color (placeholder rendering)
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // TODO: Actual rendering will be done by the Renderer
        // This is just placeholder code to verify the context is working

        // Swap buffers
        output->swapBuffers();
    }

    // Restore the previous context
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
