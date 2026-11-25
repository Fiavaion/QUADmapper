#include "OutputWindow.hpp"
#include "core/Logger.hpp"
#include "renderer/Framebuffer.hpp"  // glad, before GLFW
#include <GLFW/glfw3.h>
#include <algorithm>

namespace quadmapper {

OutputWindow::OutputWindow()
    : id_(UUID::generate())
    , name_("Output")
{
}

OutputWindow::~OutputWindow() {
    destroy();
}

Result<void, Error> OutputWindow::create(GLFWwindow* shareContext) {
    if (window_ != nullptr) {
        return Err("OutputWindow already created");
    }

    shareContext_ = shareContext;

    // Configure window hints
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // Start hidden
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE);  // Window decorations
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);  // Allow resize
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);   // Don't steal focus on creation
    // Keyboard focus stays with the editor in perform mode (vertex nudging), so
    // showing the output must not activate it, and a fullscreen output must not
    // iconify when it loses focus back to the editor.
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_FALSE);

    // OpenGL context hints (must match the main window: GL 3.3 core)
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);  // 3D objects depth-test (ADR-052); GLFW's default, made explicit

    // Create the window with shared context
    window_ = glfwCreateWindow(
        resolution_.x,
        resolution_.y,
        name_.c_str(),
        nullptr,       // No monitor (windowed mode)
        shareContext   // Share context for texture sharing
    );

    // The window hints above are global GLFW state; restore the ones that would
    // otherwise leak into windows created later (e.g. by ImGui).
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_TRUE);
    glfwWindowHint(GLFW_AUTO_ICONIFY, GLFW_TRUE);

    if (!window_) {
        LOG_ERROR("Failed to create GLFW window for OutputWindow '{}'", name_);
        return Err("Failed to create GLFW window");
    }

    glfwSetWindowPos(window_, position_.x, position_.y);

    savedWindowedPos_ = position_;
    savedWindowedSize_ = resolution_;

    LOG_INFO("Created OutputWindow '{}' ({}x{}) with shared context",
             name_, resolution_.x, resolution_.y);

    return Ok();
}

void OutputWindow::destroy() {
    if (window_ != nullptr) {
        LOG_DEBUG("Destroying OutputWindow '{}'", name_);
        if (frameCopy_) {
            // Its framebuffer object belongs to this context: free it there, first.
            GLFWwindow* previous = glfwGetCurrentContext();
            glfwMakeContextCurrent(window_);
            frameCopy_.reset();
            glfwMakeContextCurrent(previous != window_ ? previous : nullptr);
        }
        glfwDestroyWindow(window_);
        window_ = nullptr;
        shareContext_ = nullptr;
        swapInterval_ = -1;
    }
}

FrameCopy& OutputWindow::frameCopy() {
    if (!frameCopy_) {
        frameCopy_ = std::make_unique<FrameCopy>();
    }
    return *frameCopy_;
}

void OutputWindow::show() {
    if (window_ != nullptr) {
        glfwShowWindow(window_);
        LOG_DEBUG("OutputWindow '{}': shown", name_);
    }
}

void OutputWindow::hide() {
    if (window_ != nullptr) {
        glfwHideWindow(window_);
        LOG_DEBUG("OutputWindow '{}': hidden", name_);
    }
}

bool OutputWindow::isVisible() const {
    if (window_ == nullptr) {
        return false;
    }
    return glfwGetWindowAttrib(window_, GLFW_VISIBLE) == GLFW_TRUE;
}

void OutputWindow::makeContextCurrent() {
    if (window_ != nullptr) {
        glfwMakeContextCurrent(window_);
    }
}

void OutputWindow::swapBuffers() {
    if (window_ != nullptr) {
        glfwSwapBuffers(window_);
    }
}

void OutputWindow::setSwapInterval(int interval) {
    if (window_ != nullptr && interval != swapInterval_) {
        glfwSwapInterval(interval);
        swapInterval_ = interval;
    }
}

void OutputWindow::setPosition(IVec2 pos) {
    position_ = pos;
    if (window_ != nullptr && !fullscreen_) {
        glfwSetWindowPos(window_, pos.x, pos.y);
    }
}

Result<void, Error> OutputWindow::goFullscreen(int monitorIndex) {
    if (window_ == nullptr) {
        return Err("Window not created");
    }

    // Use the specified monitor or the stored one
    int targetMonitor = (monitorIndex >= 0) ? monitorIndex : monitor_;

    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);

    if (monitors == nullptr || monitorCount == 0) {
        return Err("No monitors available");
    }

    // Never guess: monitor 0 is usually the operator's own screen (QM-090).
    if (targetMonitor < 0 || targetMonitor >= monitorCount) {
        return Err("Monitor " + std::to_string(targetMonitor) + " is not connected");
    }

    GLFWmonitor* monitor = monitors[targetMonitor];
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);

    if (mode == nullptr) {
        return Err("Failed to get video mode for monitor");
    }

    // Save current windowed position and size before going fullscreen
    if (!fullscreen_) {
        int wx, wy;
        glfwGetWindowPos(window_, &wx, &wy);
        savedWindowedPos_ = IVec2(wx, wy);

        int ww, wh;
        glfwGetWindowSize(window_, &ww, &wh);
        savedWindowedSize_ = IVec2(ww, wh);
    }

    // Switch to fullscreen
    glfwSetWindowMonitor(
        window_,
        monitor,
        0, 0,
        mode->width, mode->height,
        mode->refreshRate
    );

    fullscreen_ = true;
    monitor_ = targetMonitor;
    resolution_ = IVec2(mode->width, mode->height);

    LOG_INFO("OutputWindow '{}': switched to fullscreen on monitor {} ({}x{}@{}Hz)",
             name_, targetMonitor, mode->width, mode->height, mode->refreshRate);

    return Ok();
}

Result<void, Error> OutputWindow::goWindowed() {
    if (window_ == nullptr) {
        return Err("Window not created");
    }

    if (!fullscreen_ && !borderless_) {
        // Already windowed
        return Ok();
    }

    // Switch to windowed mode, restoring previous size and position
    glfwSetWindowMonitor(
        window_,
        nullptr,  // No monitor = windowed mode
        savedWindowedPos_.x, savedWindowedPos_.y,
        savedWindowedSize_.x, savedWindowedSize_.y,
        GLFW_DONT_CARE
    );

    // Restore window decorations if we were in borderless mode
    if (borderless_) {
        glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_TRUE);
    }

    fullscreen_ = false;
    borderless_ = false;
    position_ = savedWindowedPos_;
    resolution_ = savedWindowedSize_;

    LOG_INFO("OutputWindow '{}': switched to windowed mode ({}x{} at {},{})",
             name_, savedWindowedSize_.x, savedWindowedSize_.y,
             savedWindowedPos_.x, savedWindowedPos_.y);

    return Ok();
}

Result<void, Error> OutputWindow::goBorderlessFullscreen(int monitorIndex) {
    if (window_ == nullptr) {
        return Err("Window not created");
    }

    // Use the specified monitor or the stored one
    int targetMonitor = (monitorIndex >= 0) ? monitorIndex : monitor_;

    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);

    if (monitors == nullptr || monitorCount == 0) {
        return Err("No monitors available");
    }

    // Never guess: monitor 0 is usually the operator's own screen (QM-090).
    if (targetMonitor < 0 || targetMonitor >= monitorCount) {
        return Err("Monitor " + std::to_string(targetMonitor) + " is not connected");
    }

    GLFWmonitor* monitor = monitors[targetMonitor];
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);

    if (mode == nullptr) {
        return Err("Failed to get video mode for monitor");
    }

    int monitorX, monitorY;
    glfwGetMonitorPos(monitor, &monitorX, &monitorY);

    // Save current windowed position and size before going fullscreen
    if (!fullscreen_ && !borderless_) {
        int wx, wy;
        glfwGetWindowPos(window_, &wx, &wy);
        savedWindowedPos_ = IVec2(wx, wy);

        int ww, wh;
        glfwGetWindowSize(window_, &ww, &wh);
        savedWindowedSize_ = IVec2(ww, wh);
    }

    // Remove window decorations for borderless mode
    glfwSetWindowAttrib(window_, GLFW_DECORATED, GLFW_FALSE);

    // Position window to cover the entire monitor (borderless fullscreen)
    glfwSetWindowMonitor(
        window_,
        nullptr,  // No monitor = windowed mode (but we're making it borderless)
        monitorX, monitorY,
        mode->width, mode->height,
        GLFW_DONT_CARE
    );

    fullscreen_ = false;
    borderless_ = true;
    monitor_ = targetMonitor;
    resolution_ = IVec2(mode->width, mode->height);

    LOG_INFO("OutputWindow '{}': switched to borderless fullscreen on monitor {} ({}x{} at {},{})",
             name_, targetMonitor, mode->width, mode->height, monitorX, monitorY);

    return Ok();
}

IVec2 OutputWindow::getEffectiveResolution() const {
    // For 90 or 270 degree rotations, swap width and height
    if (rotation_ == Rotation::R90 || rotation_ == Rotation::R270) {
        return IVec2(resolution_.y, resolution_.x);
    }
    return resolution_;
}

int OutputWindow::getRotationDegrees() const {
    return static_cast<int>(rotation_);
}

void OutputWindow::setRotationDegrees(int degrees) {
    // Normalize to 0-360 range and round to nearest valid value
    degrees = degrees % 360;
    if (degrees < 0) degrees += 360;

    if (degrees < 45) {
        rotation_ = Rotation::R0;
    } else if (degrees < 135) {
        rotation_ = Rotation::R90;
    } else if (degrees < 225) {
        rotation_ = Rotation::R180;
    } else if (degrees < 315) {
        rotation_ = Rotation::R270;
    } else {
        rotation_ = Rotation::R0;
    }
}

Result<void, Error> OutputWindow::applyResolution() {
    if (window_ == nullptr) {
        return Err("Window not created");
    }

    if (fullscreen_ || borderless_) {
        // In fullscreen mode, resolution is determined by the monitor
        LOG_DEBUG("OutputWindow '{}': cannot apply resolution while in fullscreen/borderless mode", name_);
        return Ok();
    }

    glfwSetWindowSize(window_, resolution_.x, resolution_.y);

    LOG_INFO("OutputWindow '{}': applied resolution {}x{}", name_, resolution_.x, resolution_.y);

    return Ok();
}

void OutputWindow::setKeyCallback(KeyCallback callback) {
    keyCallback_ = std::move(callback);

    if (window_ != nullptr) {
        // Store this instance as user pointer for the static callback
        glfwSetWindowUserPointer(window_, this);
        glfwSetKeyCallback(window_, &OutputWindow::glfwKeyCallback);
    }
}

void OutputWindow::glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    OutputWindow* self = static_cast<OutputWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->keyCallback_) {
        // Call a copy: the callback may clear or replace keyCallback_, which must not
        // destroy the function object while it is running.
        const KeyCallback callback = self->keyCallback_;
        callback(key, scancode, action, mods);
    }
}

} // namespace quadmapper
