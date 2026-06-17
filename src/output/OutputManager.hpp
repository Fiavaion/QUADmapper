#pragma once

#include "OutputWindow.hpp"
#include "core/Result.hpp"
#include "core/UUID.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Scene.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Texture.hpp"
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>

// Forward declare GLFW types
struct GLFWwindow;
struct GLFWmonitor;

namespace quadmapper {

/**
 * Information about a connected monitor/display.
 */
struct MonitorInfo {
    std::string name;                   // Monitor name from GLFW
    IVec2 position = IVec2(0, 0);       // Position in virtual screen space
    IVec2 resolution = IVec2(0, 0);     // Current resolution
    int refreshRate = 60;               // Refresh rate in Hz
    bool isPrimary = false;             // Is this the primary monitor?
    int index = -1;                     // Index in the monitor list

    /**
     * Get a display-friendly string for this monitor.
     * Format: "MonitorName (WxH @ RHz)"
     */
    std::string getDisplayString() const;
};

/**
 * Information about a supported video mode for a monitor.
 */
struct VideoModeInfo {
    int width = 0;
    int height = 0;
    int refreshRate = 60;

    std::string getDisplayString() const {
        return std::to_string(width) + " x " + std::to_string(height) + " @ " + std::to_string(refreshRate) + "Hz";
    }

    bool operator==(const VideoModeInfo& other) const {
        return width == other.width && height == other.height && refreshRate == other.refreshRate;
    }
};

/**
 * OutputManager manages multiple output windows for projection mapping.
 * It handles window creation with shared OpenGL contexts, monitor enumeration,
 * and perform mode transitions.
 */
class OutputManager {
public:
    OutputManager();
    ~OutputManager();

    // Non-copyable
    OutputManager(const OutputManager&) = delete;
    OutputManager& operator=(const OutputManager&) = delete;

    /**
     * Initialize the output manager with the main application window.
     * The main window's OpenGL context will be shared with all output windows.
     * @param mainWindow The main application GLFW window
     * @return Result indicating success or error
     */
    Result<void, Error> initialize(GLFWwindow* mainWindow);

    /**
     * Shutdown and clean up all output windows.
     */
    void shutdown();

    /**
     * Check if the manager has been initialized.
     */
    bool isInitialized() const { return mainWindow_ != nullptr; }

    // Output window management
    /**
     * Create a new output window.
     * @param name Optional name for the output window
     * @return Pointer to the created output window, or nullptr on failure
     */
    OutputWindow* createOutput(const std::string& name = "Output");

    /**
     * Delete an output window by ID.
     * @param id The UUID of the output to delete
     */
    void deleteOutput(const UUID& id);

    /**
     * Get an output window by ID.
     * @param id The UUID of the output
     * @return Pointer to the output window, or nullptr if not found
     */
    OutputWindow* getOutput(const UUID& id);

    /**
     * Get an output window by ID (const version).
     */
    const OutputWindow* getOutput(const UUID& id) const;

    /**
     * Get the number of output windows.
     */
    size_t getOutputCount() const { return outputs_.size(); }

    /**
     * Get all output windows.
     */
    const std::vector<std::unique_ptr<OutputWindow>>& getOutputs() const { return outputs_; }

    /**
     * Get all output windows (mutable access).
     */
    std::vector<std::unique_ptr<OutputWindow>>& getOutputs() { return outputs_; }

    // Monitor management
    /**
     * Refresh the list of available monitors.
     * Should be called when monitor configuration changes.
     */
    void refreshMonitors();

    /**
     * Get the list of available monitors.
     */
    const std::vector<MonitorInfo>& getMonitors() const { return monitors_; }

    /**
     * Get the number of available monitors.
     */
    size_t getMonitorCount() const { return monitors_.size(); }

    /**
     * Get a specific monitor by index.
     * @param index Monitor index
     * @return Pointer to MonitorInfo, or nullptr if index is invalid
     */
    const MonitorInfo* getMonitor(int index) const;

    /**
     * Find the primary monitor.
     * @return Pointer to the primary MonitorInfo, or nullptr if none found
     */
    const MonitorInfo* getPrimaryMonitor() const;

    /**
     * Get available video modes (resolutions) for a specific monitor.
     * @param monitorIndex Index of the monitor
     * @return Vector of available video modes, or empty vector if invalid index
     */
    std::vector<VideoModeInfo> getMonitorVideoModes(int monitorIndex) const;

    // Monitor change callback
    using MonitorChangeCallback = std::function<void()>;

    /**
     * Set a callback to be invoked when monitor configuration changes.
     * This allows the application to respond to monitors being connected/disconnected.
     */
    void setMonitorChangeCallback(MonitorChangeCallback callback);

    /**
     * Enable automatic monitor change detection.
     * When enabled, refreshMonitors() is called automatically when GLFW detects changes.
     */
    void enableMonitorChangeDetection();

    /**
     * Disable automatic monitor change detection.
     */
    void disableMonitorChangeDetection();

    /**
     * Check if monitor change detection is enabled.
     */
    bool isMonitorChangeDetectionEnabled() const { return monitorChangeDetectionEnabled_; }

    // Perform mode
    /**
     * Enter perform mode - show all enabled outputs fullscreen on their assigned monitors.
     * @param borderless If true, use borderless fullscreen mode (default: true)
     * @return Result indicating success or error
     */
    Result<void, Error> enterPerformMode(bool borderless = true);

    /**
     * Exit perform mode - return all outputs to windowed mode and hide them.
     * @return Result indicating success or error
     */
    Result<void, Error> exitPerformMode();

    /**
     * Toggle perform mode on/off.
     * @param borderless If true, use borderless fullscreen when entering (default: true)
     * @return Result indicating success or error
     */
    Result<void, Error> togglePerformMode(bool borderless = true);

    /**
     * Check if currently in perform mode.
     */
    bool isInPerformMode() const { return performMode_; }

    // Key event handlers for perform mode (prepared for Phase 7 input system)

    /**
     * Handle F11 key press - toggle perform mode.
     * This method can be called from the input system.
     */
    void handleF11Key();

    /**
     * Handle Escape key press - exit perform mode if active.
     * This method can be called from the input system.
     */
    void handleEscapeKey();

    // Rendering
    /**
     * Render each enabled output's ASSIGNED scene to that output window (ADR-039
     * per-output routing). An output shows the one scene whose assignedOutputId is
     * this output's id; outputs with no assigned scene clear to black. Each output
     * uses its own context-local Renderer; shared textures bind across contexts.
     * Selection highlighting and editor handles are suppressed — output is clean.
     * @param scenes    The project's scenes (read-only); each may target one output
     * @param textures  Map of quad UUID → Texture* (pointers owned by EditorState)
     */
    void renderToOutputs(const std::vector<std::unique_ptr<Scene>>& scenes,
                         const std::unordered_map<UUID, Texture*>& textures);

    /**
     * Pure routing decision: the scene assigned to a given output id, or nullptr if
     * none. Static + side-effect-free so the routing rule is unit-testable without GL.
     */
    static const Scene* sceneForOutput(const std::vector<std::unique_ptr<Scene>>& scenes,
                                       const UUID& outputId);

    /**
     * Render a single fullscreen calibration quad (One-Flash pattern, QM-059) to
     * all enabled outputs, replacing the scene. Flashes a known ChArUco pattern on
     * the chosen projector so one photo can solve the projector->surface
     * homography (QM-060). Same clean output path as renderToOutputs.
     */
    void renderPatternToOutputs(const Quad& patternQuad, const Texture* texture);

    /**
     * Get the main window used for context sharing.
     */
    GLFWwindow* getMainWindow() const { return mainWindow_; }

private:
    // Static GLFW monitor callback handler
    static void glfwMonitorCallback(GLFWmonitor* monitor, int event);

    // Setup key callbacks on output windows for perform mode
    void setupPerformModeKeyCallbacks();

    // Remove key callbacks from output windows
    void clearPerformModeKeyCallbacks();

    // Main window for context sharing
    GLFWwindow* mainWindow_ = nullptr;

    // Output windows
    std::vector<std::unique_ptr<OutputWindow>> outputs_;

    // Per-output Renderer instances keyed by OutputWindow UUID.
    // Each Renderer is created/destroyed in that output's GL context so its
    // VAOs (which are NOT shared) live in the right context.
    std::unordered_map<UUID, std::unique_ptr<Renderer>> outputRenderers_;

    // Available monitors
    std::vector<MonitorInfo> monitors_;

    // Perform mode state
    bool performMode_ = false;
    bool useBorderlessFullscreen_ = true;

    // Counter for auto-naming outputs
    int outputCounter_ = 0;

    // Monitor change detection
    MonitorChangeCallback monitorChangeCallback_;
    bool monitorChangeDetectionEnabled_ = false;

    // Singleton-like pointer for static callback (GLFW limitation)
    static OutputManager* instance_;
};

} // namespace quadmapper
