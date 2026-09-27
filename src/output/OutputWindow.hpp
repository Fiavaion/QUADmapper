#pragma once

#include "core/UUID.hpp"
#include "core/Types.hpp"
#include "core/Result.hpp"
#include "scene/ProjectorCamera.hpp"
#include "scene/Slice.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Forward declare GLFW types
struct GLFWwindow;
struct GLFWmonitor;

namespace quadmapper {

class FrameCopy;

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
 * The display an output WANTS (QM-090): what the .qmap saved or the user last
 * chose. It is kept apart from the monitor the output currently resolves to, so
 * an automatic fallback never overwrites the projector's identity. An empty name
 * with savedIndex -1 means no display was chosen (pick one automatically).
 */
struct MonitorIdentity {
    std::string name;               // GLFW monitor name; empty = unknown
    IVec2 position = IVec2(0, 0);   // origin on the virtual desktop (valid with a name)
    int savedIndex = -1;            // monitor index when chosen; -1 = none
};

/**
 * OutputWindow represents a single output window for projection mapping.
 * Each output window has its own GLFW window with a shared OpenGL context
 * for efficient texture sharing with the main application window.
 */
class OutputWindow {
public:
    OutputWindow();
    ~OutputWindow();

    // Neither copyable nor movable: the GLFW window's user pointer is this object's
    // address (the key callback finds it there). Hold outputs by unique_ptr.
    OutputWindow(const OutputWindow&) = delete;
    OutputWindow& operator=(const OutputWindow&) = delete;
    OutputWindow(OutputWindow&&) = delete;
    OutputWindow& operator=(OutputWindow&&) = delete;

    // Identity
    const UUID& getId() const { return id_; }
    const std::string& getName() const { return name_; }
    void setName(const std::string& n) { name_ = n; }

    // Resolution and display settings
    IVec2 getResolution() const { return resolution_; }
    void setResolution(IVec2 r) { resolution_ = r; }
    Rotation getRotation() const { return rotation_; }
    void setRotation(Rotation r) { rotation_ = r; }
    // The connected monitor this output resolved to (-1 = none connected).
    // OutputManager sets it; a user's choice goes through OutputManager::assignMonitor.
    int getMonitor() const { return monitor_; }
    void setMonitor(int m) { monitor_ = m; }
    const MonitorIdentity& getDesiredMonitor() const { return desiredMonitor_; }
    void setDesiredMonitor(const MonitorIdentity& identity) { desiredMonitor_ = identity; }
    // No display was chosen: the output takes the first one that is not the editor's.
    bool isMonitorAutomatic() const {
        return desiredMonitor_.name.empty() && desiredMonitor_.savedIndex < 0;
    }

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

    // Routing (ADR-047 §2): the one scene this output shows (null = none, black).
    // Several outputs may show the same scene. An id no scene has counts as none.
    const UUID& getSceneId() const { return sceneId_; }
    void setSceneId(const UUID& sceneId) { sceneId_ = sceneId; }

    // The slices of the scene's canvas this output shows, drawn in order. An output
    // starts with none; whoever routes a scene to it gives it one (makeFullCanvasSlice).
    std::vector<Slice>& getSlices() { return slices_; }
    const std::vector<Slice>& getSlices() const { return slices_; }

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

    /**
     * Set this window's swap interval (0 = don't wait for vblank, 1 = vsync).
     * Call with this window's context current; a no-op when unchanged.
     */
    void setSwapInterval(int interval);

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

    // Copies the output's frame onto this window (ADR-054). Framebuffer objects are
    // not shared between contexts, so the window owns its copier: made on first use
    // (call with this window's context current), freed by destroy() in this context,
    // so a window created again never reuses a dead context's object.
    FrameCopy& frameCopy();

    // Keyboard callback support for perform mode
    using KeyCallback = std::function<void(int key, int scancode, int action, int mods)>;

    /**
     * Set a callback for key events on this output window.
     * Used primarily for perform mode escape handling. The callback may replace or
     * clear itself while it runs: the window invokes a copy.
     */
    void setKeyCallback(KeyCallback callback);

    /**
     * Get the current key callback.
     */
    const KeyCallback& getKeyCallback() const { return keyCallback_; }

    // The physical projector behind this output, modelled as a camera (ADR-052): its
    // lens and pose place 3D objects, and the point pairs it was calibrated from.
    ProjectorCamera& getProjector() { return projector_; }
    const ProjectorCamera& getProjector() const { return projector_; }
    std::vector<CalibrationPoint>& getCalibrationPoints() { return calibrationPoints_; }
    const std::vector<CalibrationPoint>& getCalibrationPoints() const { return calibrationPoints_; }

private:
    // Static GLFW key callback handler
    static void glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    // Identity
    UUID id_;
    std::string name_;

    // Projector model for 3D objects (ADR-052)
    ProjectorCamera projector_;
    std::vector<CalibrationPoint> calibrationPoints_;

    // Display settings
    IVec2 resolution_ = IVec2(1920, 1080);
    Rotation rotation_ = Rotation::R0;
    int monitor_ = -1;
    MonitorIdentity desiredMonitor_;

    // State flags
    bool fullscreen_ = false;
    bool borderless_ = false;
    bool enabled_ = true;

    // Scene assignments
    UUID sceneId_;
    std::vector<Slice> slices_;

    // GLFW window
    GLFWwindow* window_ = nullptr;
    GLFWwindow* shareContext_ = nullptr;
    int swapInterval_ = -1;  // -1 = never set (driver default)
    std::unique_ptr<FrameCopy> frameCopy_;  // lives in window_'s context

    // Window position (saved for fullscreen toggle)
    IVec2 position_ = IVec2(100, 100);
    IVec2 savedWindowedPos_ = IVec2(100, 100);
    IVec2 savedWindowedSize_ = IVec2(1920, 1080);

    // Keyboard callback for perform mode
    KeyCallback keyCallback_;
};

} // namespace quadmapper
