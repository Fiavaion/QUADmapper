#pragma once

#include "OutputWindow.hpp"
#include "core/Result.hpp"
#include "core/UUID.hpp"
#include "scene/QuadManager.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneFade.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Texture.hpp"
#include "renderer/Framebuffer.hpp"
#include <array>
#include <chrono>
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

/** How an output's monitor was resolved, strongest first (QM-090). */
enum class MonitorMatch {
    Exact,        // desired name + position
    Position,     // desired position only
    UniqueName,   // desired name, unique among the connected monitors
    SavedIndex,   // the saved index (fallback)
    FirstFree,    // first monitor that is neither the editor's nor taken (fallback)
    Unresolved    // nothing suitable is connected
};

struct MonitorResolution {
    int index = -1;
    MonitorMatch match = MonitorMatch::Unresolved;
};

/**
 * OutputManager manages multiple output windows for projection mapping.
 * It handles window creation with shared OpenGL contexts, monitor enumeration,
 * and perform mode transitions.
 */
// The blend test pattern's quads (ADR-047 §3), in normalized canvas space: white
// grid lines and two solid white bands, drawn over opaque black.
std::vector<Quad> makeBlendTestPattern();

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

    /**
     * Resolve an output's desired monitor against the connected monitors (QM-090).
     * Pure: no GLFW calls, so every step is unit-testable. Tried in order:
     *   Exact      - same name and position (always honoured, even the editor's
     *                monitor: only an explicit choice records that identity);
     *   Position   - same position (same desk layout, display swapped or renamed);
     *   UniqueName - same name, only when no other connected monitor shares it;
     *   SavedIndex - the saved index;
     *   FirstFree  - the first connected monitor;
     *   Unresolved - index -1.
     * Every step after Exact skips `editorMonitor` and the `taken` monitors, so an
     * automatic choice never covers the operator's screen or another output.
     */
    static MonitorResolution resolveMonitor(const std::vector<MonitorInfo>& monitors,
                                            const MonitorIdentity& desired,
                                            int editorMonitor,
                                            const std::vector<int>& taken);

    /**
     * Resolve several outputs together: stronger matches claim their monitor
     * first (every Exact, then Position/UniqueName, then the fallbacks), so one
     * output's fallback cannot take the display another output really belongs on.
     * Pure. Results are in the order of `desired`.
     */
    static std::vector<MonitorResolution> resolveMonitors(
        const std::vector<MonitorInfo>& monitors,
        const std::vector<MonitorIdentity>& desired,
        int editorMonitor);

    /**
     * Index of the monitor whose rectangle contains a virtual-desktop point, or -1.
     */
    static int monitorContaining(const std::vector<MonitorInfo>& monitors, IVec2 point);

    /**
     * Re-resolve every output's monitor from its desired identity against the
     * monitors found by the last refreshMonitors(). Enabled outputs claim
     * displays; disabled ones resolve without taking any.
     */
    void resolveOutputs();

    /**
     * What a monitor picker shows for `output` (the Output panel and the Inspector):
     * the display it resolved to, marked "(automatic)" when none was chosen; else the
     * chosen display marked "(not connected)"; else "Not connected". Never names a
     * display the output is not on. Pure.
     */
    static std::string describeMonitorChoice(const std::vector<MonitorInfo>& monitors,
                                             const OutputWindow& output);

    /**
     * The user explicitly chose monitor `monitorIndex` for `output` (-1 =
     * automatic). Records that monitor's current identity as the desired one,
     * which is honoured as-is even when it is the editor's own screen. In
     * perform mode the output moves there at once.
     */
    void assignMonitor(OutputWindow& output, int monitorIndex);

    /**
     * Swap-interval policy (QM-090, TD-12): the index of the output that waits for
     * vblank; -1 if none, in which case the editor is the vsync'd window. A minimised
     * window's swap returns at once, so it must never be the one pacing the loop.
     * Among the others: the one with the lowest known refresh rate, the first in list
     * order on a tie. The loop then runs at the slowest display's rate, so no display
     * is sent more frames than it can show (outputs at a higher rate repeat frames),
     * and outputs sharing that rate get one new frame per refresh. `refreshHz` gives
     * each output's display rate (0 or missing = unknown: such an output holds vsync
     * only when no rate is known). Pure.
     */
    static int vsyncHolder(const std::vector<bool>& iconified, const std::vector<int>& refreshHz = {});

    // Operator-facing messages (e.g. a projector that is not connected at F11).
    // Everything passed here is also logged.
    using NoticeCallback = std::function<void(const std::string&)>;
    void setNoticeCallback(NoticeCallback callback);

    // Scene take keys (US-008): in perform mode an output window can hold the OS
    // focus, so its scene-take presses (see sceneTakeNumber) are handed to this
    // callback with the scene number.
    using SceneKeyCallback = std::function<void(int sceneNumber)>;
    void setSceneKeyCallback(SceneKeyCallback callback) { sceneKeyCallback_ = std::move(callback); }

    // The scene number (1-9) a GLFW key event takes, or 0: a press of top-row digit
    // 1-9 or numpad 1-9 with no Shift/Ctrl/Alt/Super. GLFW key codes name the physical
    // key (its US-layout position), so the top-row keys take their scene on every
    // layout, e.g. unshifted "&" on AZERTY takes scene 1. Pure, so the key rule is
    // testable without GLFW; the editor and output windows both use it.
    static int sceneTakeNumber(int key, int action, int mods);

    // Master blackout (ADR-048 §3): while on, every output and every preview renders
    // black, whatever it would show (scene or One-Flash pattern). Runtime state, never
    // saved. Toggled from the Output menu, Ctrl+B (editor and, in perform mode, a
    // focused output window) and the /qm/blackout control target.
    void setBlackout(bool on);
    void toggleBlackout() { setBlackout(!blackout_); }
    bool isBlackout() const { return blackout_; }

    // True for the blackout shortcut: a Ctrl+B press, no Shift/Alt/Super. Pure, like
    // sceneTakeNumber, so the editor and output windows share one testable rule.
    static bool isBlackoutKey(int key, int action, int mods);

    // Called with the id of every output deleteOutput removes, after it is gone, so
    // the owner can drop references to it and mark the show changed.
    using OutputRemovedCallback = std::function<void(const UUID& outputId)>;
    void setOutputRemovedCallback(OutputRemovedCallback callback) {
        outputRemovedCallback_ = std::move(callback);
    }

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
     * Enter perform mode - re-resolve every output's monitor, then show each
     * enabled output fullscreen on it. An output whose display is not connected
     * stays hidden and a notice is posted; the others still go fullscreen.
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

    /**
     * A key on an output window in perform mode (its GLFW key callback). Esc and F11
     * only request the exit, which applyPendingPerformExit() carries out from the main
     * loop: leaving perform mode inside the callback would clear the callback that is
     * running. Ctrl+B toggles blackout; 1-9 are scene takes (setSceneKeyCallback).
     */
    void handleOutputWindowKey(int key, int action, int mods);

    /** True between an output window's Esc/F11 and the next applyPendingPerformExit(). */
    bool isPerformExitPending() const { return performExitPending_; }

    /**
     * Main loop, outside any GLFW callback: leave perform mode if an output window
     * asked to. Clears the request either way.
     */
    void applyPendingPerformExit();

    // Rendering (ADR-054): each output's frame is drawn once per frame, in the main
    // GL context, into a colour texture that context owns. The output window copies
    // that texture to the screen and the preview panel shows the same texture, so a
    // preview is exactly what the projector shows.

    /**
     * Draw every output's frame into its render target, in the CURRENT (main) GL
     * context with the main context's `renderer` (ADR-047 §4, ADR-054).
     *  1. Each scene that an output shows is drawn once into its canvas texture, at
     *     the scene's canvas size: its quads in z-order, clean (no selection or
     *     editor handles), colour blended over black with coverage in alpha.
     *  2. Each output's frame: the scene's 3D objects through the output's projector
     *     camera (ADR-052), then the output's slices of the canvas composited over
     *     them, all turned by the output's rotation (QM-087).
     * With no scene, or during blackout, the frame is black (no canvas is drawn in
     * blackout). The target is the output window's framebuffer size. The frame is
     * opaque (alpha 1 everywhere), so the preview can draw it blended. Restores the
     * framebuffer binding, viewport and blend function; leaves the renderer's
     * rotation at R0 for the editor canvas.
     * @param scenes    The project's scenes (read-only)
     * @param textures  Map of quad/object UUID -> Texture* (pointers owned by EditorState)
     */
    void renderOutputFrames(const std::vector<std::unique_ptr<Scene>>& scenes,
                            const std::unordered_map<UUID, Texture*>& textures,
                            Renderer& renderer);

    /**
     * Pure routing decision: the scene the output shows (its sceneId), or nullptr
     * when it shows none or no scene has that id. Static and side-effect-free so the
     * routing rule is unit-testable without GL.
     */
    static const Scene* sceneForOutput(const std::vector<std::unique_ptr<Scene>>& scenes,
                                       const OutputWindow& output);

    /**
     * Route `scene` to `output` (null = show nothing; the slices are kept). An output
     * without slices gets one slice of the whole canvas. Slices it has are rescaled
     * from the canvas they were made for (OutputWindow::getSlicesCanvas) to the new
     * one, so they keep their place on the picture whatever the output showed in
     * between; slices of an unknown canvas are replaced by one whole-canvas slice.
     */
    static void showScene(OutputWindow& output, const Scene* scene,
                          const std::vector<std::unique_ptr<Scene>>& scenes);

    /**
     * An operator's routing (Inspector, Scenes panel, scene takes): showScene, then,
     * when `output` is the only output showing the scene and shows it through its
     * default slicing (isDefaultSlicing), the canvas takes the output's effective
     * (rotated) resolution, so the picture is not resampled and the editor has the
     * projector's aspect. Quads are normalized, so the projector shows the same.
     * A canvas the operator set (Scene::isCanvasLocked) is never adopted.
     * Returns true when the canvas size changed.
     */
    bool routeScene(OutputWindow& output, Scene* scene,
                    const std::vector<std::unique_ptr<Scene>>& scenes);

    /**
     * Once per frame: a scene shown by one output alone, through its default slice
     * (the whole canvas, no warp, no blend), whose canvas is that output's effective
     * resolution, keeps following that resolution when it changes (a resolution or
     * rotation edit), as the editor canvas did before canvases (ADR-047 part A).
     * Every other scene, and every scene whose canvas the operator set
     * (Scene::isCanvasLocked), keeps its canvas size. Returns true when a canvas changed in
     * a way the show should be saved with. A change that comes with an output going
     * fullscreen or back (F11 enter/exit sets the monitor's mode, then restores the
     * windowed size) is followed but not reported: it is undone on exit.
     */
    bool followOutputSizes(std::vector<std::unique_ptr<Scene>>& scenes);

    // ---- Scene crossfades (ADR-047 §5) ------------------------------------------
    // The frame clock: seconds on a steady wall clock, set once per frame before the
    // frame's takes and render (Application::presentFrame; tests step it by hand).
    // Fades start and are mixed at this time, so they follow wall time, not frames.
    void setFrameTime(double seconds) { frameTime_ = seconds; }
    double frameTime() const { return frameTime_; }

    /**
     * A scene take onto `output`: routeScene, then a fade over `fadeSeconds` (0 = a
     * cut) from what the output showed until now, starting at frameTime(). A take
     * during a fade follows SceneFades::begin. Returns routeScene's result.
     */
    bool takeScene(OutputWindow& output, Scene* scene,
                   const std::vector<std::unique_ptr<Scene>>& scenes, double fadeSeconds);

    /**
     * Once per frame, after setFrameTime and before the frame's takes: end the fades
     * that have finished, and those whose output is gone, now shows another scene
     * than the one it was fading to, or whose scenes are gone. True when one ended.
     */
    bool retireSceneFades(const std::vector<std::unique_ptr<Scene>>& scenes);

    /** The fade `output` is in at frameTime() (mix < 1, to the scene it shows), or nullptr. */
    const SceneFade* sceneFade(const OutputWindow& output) const;
    const SceneFades& sceneFades() const { return sceneFades_; }

    /** A scene is being deleted: fades from or to it end at once. */
    void cancelSceneFades(const UUID& sceneId) { sceneFades_.cancelScene(sceneId); }

    /** The show is being replaced (New, Open, recovery): every fade ends at once. */
    void clearSceneFades() { sceneFades_.clear(); }

    /** The first output (list order) that shows this scene, or nullptr. */
    OutputWindow* firstOutputShowing(const UUID& sceneId) const;

    /**
     * A scene's canvas as drawn by the last renderOutputFrames, or nullptr when no
     * output showed it. Bottom-up colour texture, like outputFrame, turned by the
     * rotation all outputs showing the scene share (unturned when they differ).
     */
    const Framebuffer* sceneCanvas(const UUID& sceneId) const;

    /** The rotation sceneCanvas(sceneId) was drawn in (R0 when it has none). */
    Rotation sceneCanvasRotation(const UUID& sceneId) const;

    /**
     * What `output` shows of `scene` with no crossfade, drawn over what the bound
     * target holds: the scene's 3D objects from the output's projector, then the
     * scene's canvas (as the last renderOutputFrames drew it) through the output's
     * slices, blend zones and black level. `effectiveSize` is the output's unturned
     * frame. The output frames are drawn with it, and so is the editor's calibration
     * view (TD-29), which is why a point picked on that view is a projector pixel
     * whatever the slicing. Changes the blend function; a caller that needs its own
     * restores it.
     */
    void renderSceneFrame(const OutputWindow& output, const Scene& scene, IVec2 effectiveSize,
                          const std::unordered_map<UUID, Texture*>& textures, Renderer& renderer);

    /**
     * Blend test pattern (ADR-047 §3): while on, every canvas is drawn as a grid of
     * white lines on black with solid white bands across the middle instead of its
     * scene's quads, so the outputs show it through their slices and the overlaps can
     * be judged by eye. Session state: never saved with the show.
     */
    void setBlendTestPattern(bool on) { blendTestPattern_ = on; }
    bool isBlendTestPattern() const { return blendTestPattern_; }

    /**
     * Like renderOutputFrames, but every output's frame is one fullscreen calibration
     * quad (One-Flash pattern, QM-059) instead of its scene, so one photo can solve
     * the projector->surface homography (QM-060).
     */
    void renderPatternFrames(const Quad& patternQuad, const Texture* texture,
                             Renderer& renderer);

    /**
     * Show the frames drawn this frame: copy each enabled, created, VISIBLE output's
     * frame to its window and swap. Only the first presented, non-minimised output
     * waits for vblank (vsyncHolder); the rest, and then the editor, swap with
     * interval 0, so N outputs never divide the frame rate. With no such output the
     * editor is the vsync'd window again. Call after the editor's swap, never inside
     * an ImGui frame.
     */
    void presentOutputs();

    // When each output the last presentOutputs showed finished its swap, and the swap
    // interval it swapped with (TD-12 measurement, --bench=present). Reused per call.
    struct PresentStamp {
        UUID outputId;
        std::chrono::steady_clock::time_point swapped;
        int swapInterval = 0;
    };
    const std::vector<PresentStamp>& lastPresents() const { return lastPresents_; }

    /**
     * An output's current frame, or nullptr before its first renderOutputFrames.
     * The colour texture is bottom-up (GL origin): display it with flipped V.
     */
    const Framebuffer* outputFrame(const UUID& outputId) const;

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

    // Draw every output's frame (see renderOutputFrames); `draw` issues the content
    // draws after the black clear, with the output's rotation set on `renderer`. It
    // gets the frame's effective (unrotated) size.
    void renderFrames(Renderer& renderer,
                      const std::function<void(const OutputWindow&, IVec2 effectiveSize)>& draw);

    // Step 1 of renderOutputFrames: draw the canvas of every scene an output shows,
    // and release the canvases of scenes no output shows.
    void renderCanvases(const std::vector<std::unique_ptr<Scene>>& scenes,
                        const std::unordered_map<UUID, Texture*>& textures, Renderer& renderer);

    // Release the canvases of scenes no output shows (after an output is removed).
    void releaseUnshownCanvases();

    // True when an output shows the scene, or is fading out of it (its canvas is
    // still drawn then).
    bool sceneOnAnOutput(const UUID& sceneId) const;

    // The size an output's frame is drawn at: its window's framebuffer, or its
    // resolution while it has no window or the window is minimised.
    static IVec2 frameSize(const OutputWindow& output);

    // The monitor the editor window's centre is on; the primary monitor if the
    // editor is off every monitor (e.g. minimised), -1 if unknown.
    int editorMonitorIndex() const;

    // Show an enabled output fullscreen on its resolved monitor (perform mode).
    // Returns false, leaving the output hidden, if it has no monitor or the switch failed.
    bool showOnResolvedMonitor(OutputWindow& output);

    // Re-resolve every output (after a hotplug, or a user's choice), and in
    // perform mode move outputs whose display changed and hide those left
    // without one.
    void reresolveOutputs(bool refreshMonitorList);

    void notify(const std::string& message);

    // Tell the operator once (notify) when the presented outputs' displays run at
    // different refresh rates, and which one paces the show (TD-12).
    void noticeMixedRefresh(const std::vector<OutputWindow*>& presented, int holder);

    // Set the editor (main) window's swap interval; a no-op when unchanged.
    void setEditorSwapInterval(int interval);

    // Perform mode keeps keyboard focus in the editor so Tab/arrow nudging works
    // while watching the projector. If an output took focus anyway (the user
    // clicked it), give focus back to the editor.
    void returnFocusToEditor();

    // Main window for context sharing
    GLFWwindow* mainWindow_ = nullptr;
    int editorSwapInterval_ = 1;  // Application enables vsync on the editor at startup

    // Output windows
    std::vector<std::unique_ptr<OutputWindow>> outputs_;

    // Each output's frame (ADR-054), keyed by output UUID. These render targets live
    // in the MAIN GL context and are destroyed in shutdown()/deleteOutput() with it
    // current; their colour textures are shared with the output contexts.
    std::unordered_map<UUID, std::unique_ptr<Framebuffer>> frames_;

    // Each shown scene's canvas (ADR-047), keyed by scene UUID; main context, like
    // frames_. It is drawn turned by the rotation every output showing the scene
    // shares (R0 when they differ), so one rotated projector's canvas rasterizes
    // exactly as its frame did before canvases. canvasScenes_ is per-frame scratch
    // (reused, so no allocation).
    struct SceneCanvas {
        std::unique_ptr<Framebuffer> target;
        Rotation rotation = Rotation::R0;
        IVec2 failedSize = IVec2(0, 0);  // texture size that could not be allocated
    };
    std::unordered_map<UUID, SceneCanvas> canvases_;
    bool blendTestPattern_ = false;
    std::vector<Quad> testPattern_;  // built on first use; stable ids for the renderer cache
    std::vector<UUID> canvasScenes_;
    GLint maxTextureSize_ = 0;  // GL_MAX_TEXTURE_SIZE, queried on the first canvas

    // Scene crossfades in progress, by output (ADR-047 §5), and the frame clock.
    SceneFades sceneFades_;
    double frameTime_ = 0.0;

    // During a fade whose scenes have 3D objects, each scene's whole frame (its
    // objects, then its canvas through the output's slices) renders into its own
    // frame-sized colour + depth layer (outgoing, its frozen second scene, incoming),
    // and the layers mix into the frame by weight through the slice pass, whole-frame
    // and texel for texel (fadeLayerSlice_). Main context; kept while the output
    // fades, released at the next frame after it ends, so a fade allocates once and a
    // show without fades never does.
    struct FadeLayers {
        std::array<std::unique_ptr<Framebuffer>, 3> legs;
    };
    std::unordered_map<UUID, FadeLayers> fadeLayers_;
    Slice fadeLayerSlice_ = makeFullCanvasSlice(IVec2(1, 1));  // own id: its mesh is cached

    // Draw `scene`'s canvas through the output's slices over what the bound target
    // holds; `fade` mixes the outgoing canvases in (null: no fade).
    void renderSceneSlices(const OutputWindow& output, const Scene& scene, IVec2 effectiveSize,
                           const Renderer::SliceFade* fade, Renderer& renderer);

    // Draw a fading output as the exact dissolve of its scenes' frames (see
    // fadeLayers_). `legs` are the outgoing, frozen second and incoming scenes (null:
    // none, black); the incoming one is never null. False, drawing nothing, when no
    // scene has objects (the slices mix the canvases exactly) or a layer could not
    // be allocated.
    bool renderFadeLayers(const OutputWindow& output, IVec2 effectiveSize,
                          const std::array<const Scene*, 3>& legs, const Renderer::SliceFade& weights,
                          const std::unordered_map<UUID, Texture*>& textures, Renderer& renderer);

    // Each output's effective resolution, and whether it was fullscreen (or
    // borderless), when followOutputSizes last saw it.
    struct KnownOutputSize {
        IVec2 size;
        bool presenting = false;
    };
    std::unordered_map<UUID, KnownOutputSize> knownSizes_;

    // Signalled when the frames drawn by renderFrames are complete; each output
    // context waits on it on the GPU before copying (a change made in one context is
    // only guaranteed visible in another once complete). Consumed by presentOutputs.
    GLsync framesDrawn_ = nullptr;

    std::vector<PresentStamp> lastPresents_;  // see lastPresents()
    std::vector<int> presentedRefresh_;       // per-frame scratch for vsyncHolder
    std::string mixedRefreshNotice_;          // the last mixed-rate notice, sent once

    // Available monitors
    std::vector<MonitorInfo> monitors_;

    // Perform mode state
    bool performMode_ = false;
    bool useBorderlessFullscreen_ = true;
    bool blackout_ = false;
    bool performExitPending_ = false;  // Esc/F11 on an output window (handleOutputWindowKey)

    // Counter for auto-naming outputs
    int outputCounter_ = 0;

    // Monitor change detection
    MonitorChangeCallback monitorChangeCallback_;
    NoticeCallback noticeCallback_;
    SceneKeyCallback sceneKeyCallback_;
    OutputRemovedCallback outputRemovedCallback_;
    bool monitorChangeDetectionEnabled_ = false;

    // Singleton-like pointer for static callback (GLFW limitation)
    static OutputManager* instance_;
};

} // namespace quadmapper
