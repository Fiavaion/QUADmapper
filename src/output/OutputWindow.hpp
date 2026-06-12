#pragma once

#include "core/UUID.hpp"
#include "core/Types.hpp"
#include "core/Result.hpp"
#include <string>
#include <vector>
#include <functional>

// Forward declare GLFW types
struct GLFWwindow;
struct GLFWmonitor;

namespace quadmapper {

/**
 * Common resolution presets for output windows.
 */
struct ResolutionPreset {
    const char* name;
    int width;
    int height;
};

/**
 * Standard resolution presets available for output windows.
 */
inline const ResolutionPreset RESOLUTION_PRESETS[] = {
    {"720p (HD)", 1280, 720},
    {"1080p (Full HD)", 1920, 1080},
    {"1440p (QHD)", 2560, 1440},
    {"4K (UHD)", 3840, 2160},
    {"WXGA", 1280, 800},
    {"WUXGA", 1920, 1200},
    {"Custom", 0, 0}  // Placeholder for custom resolution
};

inline constexpr size_t RESOLUTION_PRESET_COUNT = sizeof(RESOLUTION_PRESETS) / sizeof(ResolutionPreset);

/**
 * OutputWindow represents a single output window for projection mapping.
 * Each output window has its own GLFW window with a shared OpenGL context
 * for efficient texture sharing with the main application window.
 */
class OutputWindow {
public:
    OutputWindow();
    ~OutputWindow();

    // Non-copyable, movable
    OutputWindow(const OutputWindow&) = delete;
    OutputWindow& operator=(const OutputWindow&) = delete;
    OutputWindow(OutputWindow&& other) noexcept;
    OutputWindow& operator=(OutputWindow&& other) noexcept;

    // Identity
    const UUID& getId() const { return id_; }
    const std::string& getName() const { return name_; }
    void setName(const std::string& n) { name_ = n; }

    // Resolution and display settings
    IVec2 getResolution() const { return resolution_; }
    void setResolution(IVec2 r) { resolution_ = r; }
    Rotation getRotation() const { return rotation_; }
    void setRotation(Rotation r) { rotation_ = r; }
    int getMonitor() const { return monitor_; }
    void setMonitor(int m) { monitor_ = m; }

    /**
     * Get the effective resolution accounting for rotation.
     * For 90 or 270 degree rotations, width and height are swapped.
     */
    IVec2 getEffectiveResolution() const;

    /**
     * Get the rotation angle in degrees (0, 90, 180, or 270).
     */
    int getRotationDegrees() const;

    /**
     * Set rotation from degrees (rounds to nearest valid value).
     */
    void setRotationDegrees(int degrees);

    /**
     * Apply the current resolution to the window if created.
     * @return Result indicating success or error
     */
    Result<void, Error> applyResolution();

    // Window state
    bool isFullscreen() const { return fullscreen_; }
    void setFullscreen(bool f) { fullscreen_ = f; }
    bool isEnabled() const { return enabled_; }
    void setEnabled(bool e) { enabled_ = e; }

    // Scene assignments - which scenes are rendered to this output
    const std::vector<UUID>& getAssignedScenes() const { return assignedScenes_; }
    void assignScene(const UUID& sceneId);
    void removeScene(const UUID& sceneId);
    bool hasScene(const UUID& sceneId) const;
    void clearScenes() { assignedScenes_.clear(); }

    // GLFW window management
    /**
     * Create the GLFW window with shared OpenGL context.
     * @param shareContext The main window to share OpenGL context with (for texture sharing)
     * @return Result indicating success or error
     */
    Result<void, Error> create(GLFWwindow* shareContext);

    /**
     * Destroy the GLFW window and clean up resources.
     */
    void destroy();

    /**
     * Check if the GLFW window has been created.
     */
    bool isCreated() const { return window_ != nullptr; }

    // Window visibility
    void show();
    void hide();
    bool isVisible() const;

    // OpenGL context management
    /**
     * Make this window's OpenGL context current for rendering.
     */
    void makeContextCurrent();

    /**
     * Swap the front and back buffers (present the frame).
     */
    void swapBuffers();

    // Fullscreen control
    /**
     * Switch to fullscreen mode on the specified monitor.
     * @param monitorIndex Index of the monitor to use (-1 for current monitor setting)
     * @return Result indicating success or error
     */
    Result<void, Error> goFullscreen(int monitorIndex = -1);

    /**
     * Switch back to windowed mode, restoring the previous window position.
     * @return Result indicating success or error
     */
    Result<void, Error> goWindowed();

    /**
     * Switch to borderless fullscreen mode on the specified monitor.
     * This creates a decorated window covering the entire monitor area.
     * @param monitorIndex Index of the monitor to use (-1 for current monitor setting)
     * @return Result indicating success or error
     */
    Result<void, Error> goBorderlessFullscreen(int monitorIndex = -1);

    /**
     * Check if window is in borderless fullscreen mode.
     */
    bool isBorderless() const { return borderless_; }

    // Window position management
    IVec2 getPosition() const { return position_; }
    void setPosition(IVec2 pos);

    // Get the underlying GLFW window (for advanced use)
    GLFWwindow* getGLFWwindow() const { return window_; }

    // Keyboard callback support for perform mode
    using KeyCallback = std::function<void(int key, int scancode, int action, int mods)>;

    /**
     * Set a callback for key events on this output window.
     * Used primarily for perform mode escape handling.
     */
    void setKeyCallback(KeyCallback callback);

    /**
     * Get the current key callback.
     */
    const KeyCallback& getKeyCallback() const { return keyCallback_; }

private:
    // Static GLFW key callback handler
    static void glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    // Identity
    UUID id_;
    std::string name_;

    // Display settings
    IVec2 resolution_ = IVec2(1920, 1080);
    Rotation rotation_ = Rotation::R0;
    int monitor_ = -1;

    // State flags
    bool fullscreen_ = false;
    bool borderless_ = false;
    bool enabled_ = true;

    // Scene assignments
    std::vector<UUID> assignedScenes_;

    // GLFW window
    GLFWwindow* window_ = nullptr;
    GLFWwindow* shareContext_ = nullptr;

    // Window position (saved for fullscreen toggle)
    IVec2 position_ = IVec2(100, 100);
    IVec2 savedWindowedPos_ = IVec2(100, 100);
    IVec2 savedWindowedSize_ = IVec2(1920, 1080);

    // Keyboard callback for perform mode
    KeyCallback keyCallback_;
};

} // namespace quadmapper
