#pragma once

#include "GrayCodeScan.hpp"
#include "../core/UUID.hpp"
#include "../editor/EditorServices.hpp"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace quadmapper {

struct EditorState;
class OutputManager;
class OutputWindow;
struct MonitorInfo;
class Renderer;
class Quad;
class Texture;
namespace ai {
class SidecarClient;
struct SidecarConfig;
struct SegmentReply;
struct SurfaceResult;
struct GrayMapResult;
template <typename T>
struct GrayReply;
enum class SidecarState;
}

// The optional AI pre-show assist (ADR-035, ADR-051 §1, ADR-053): the sidecar's
// lifecycle, "Detect Surface", the One-Flash pattern, capture and solve, Pick Surface,
// and the Gray-code Scan Output with its session-only maps. Owned by Application;
// results land on the selected quad (or calibration point) and in the editor's status
// line. Geometry only: nothing here changes the .qmap format.
class AiAssist {
public:
    // `sidecar` may be null: every action then reports that AI is unavailable.
    AiAssist(EditorState& editor, std::unique_ptr<ai::SidecarClient> sidecar);
    ~AiAssist();

    AiAssist(const AiAssist&) = delete;
    AiAssist& operator=(const AiAssist&) = delete;

    // The sidecar script next to the working dir or the repo root. When it is
    // missing, scriptPath stays empty and start() fails cleanly (AI menu shows disabled).
    // The interpreter is the sidecar's own environment (tools/ai-sidecar/.venv, ADR-053
    // §1) when it exists beside the script, else `python` on PATH.
    static ai::SidecarConfig locateSidecar();
    // The same, searching from `workingDir` instead of the current directory.
    static ai::SidecarConfig locateSidecarFrom(const std::filesystem::path& workingDir);

    // AI menu: the sidecar is connected (reads cached state, no per-frame I/O).
    bool isActive() const;
    // AI menu: a camera capture or sidecar request is running; the items that start
    // one are disabled until tick() has applied its outcome.
    bool isBusy() const;
    // AI menu: connect off the UI thread (TD-10) so the menu never freezes 1-3s;
    // tick() reports the outcome.
    void enable();

    // An AI menu item. The perform lock (ADR-051 §4) refuses every one but Clear
    // Pattern, which gives the show back: the rest warp quads or put patterns on the
    // projectors. `outputs` may be null (Emit Pattern then does nothing).
    void run(EditorServices::AiAction action, OutputManager* outputs);

    // Once per interactive frame, after the frame is presented: posts a one-shot
    // status when an async connect resolves (TD-10), applies a finished request's
    // outcome, and runs the QM_AI_SELFTEST hook on the third frame.
    void tick(OutputManager* outputs);

    // A Gray-code map of one output (ADR-053 §3), held for the session only: never in
    // the .qmap. Made at `output`'s resolution and display; a change to either makes it
    // stale (grayMapCurrent).
    struct GrayMap {
        UUID outputId;
        ScanOutput output;
        std::string path;  // the sidecar's .npz, beside the captures
        float coverage = 0.0f;
        std::optional<float> decodeErrorPx;
    };
    static bool grayMapCurrent(const GrayMap& map, const std::optional<ScanOutput>& now);
    // What `output` shows in perform mode, which a map is valid for: its display and
    // that display's mode (`monitors`, OutputManager::getMonitors). Without a resolved
    // display, the output's own resolution.
    static ScanOutput performedOutput(const OutputWindow& output, const std::vector<MonitorInfo>& monitors);

    // Assisted calibration: where a click on the photo put a calibration point, in the
    // output's normalized frame. `world` names the point, so a list edited meanwhile
    // never moves another one.
    struct CalibrationPixel {
        UUID outputId;
        int index = -1;
        Vec3 world{0.0f};
        std::array<float, 2> projector{};
    };

    // What a request made off the main thread hands back: a status line and, for a
    // confident warp, the target quad's new corners (TL,TR,BR,BL, normalized). The
    // AI Photo requests hand back a proposal (Pick Surface) or a new photo; a One-Flash
    // capture hands back its photo, which Pick Surface's Apply maps through. A scan's
    // requests hand back its frame count, then its map; an assisted calibration click
    // its point's pixel.
    struct Outcome {
        std::string status;
        float seconds = 5.0f;
        std::optional<std::array<std::array<float, 2>, 4>> corners;
        std::optional<AiSurfaceProposal> proposal;
        std::optional<std::string> photo;
        std::optional<std::string> oneFlashPhoto;
        // The remembered One-Flash photo showed no usable pattern: forget it, so the
        // next Apply does not try it again.
        bool forgetOneFlashPhoto = false;
        // /graycode/patterns: frames written (0: failed, `status` says why).
        std::optional<int> scanFrames;
        std::optional<GrayMap> grayMap;
        std::optional<CalibrationPixel> calibrationPixel;
    };
    // Runs `work` on a worker thread so the camera and the sidecar's HTTP call never
    // stall a frame; tick() applies its outcome on the main thread. Corners land on
    // the quad `target` names, looked up when the outcome arrives: a deleted quad or
    // an engaged perform lock drops them with a status, changing nothing. `who`
    // prefixes those statuses ("AI", "One-Flash"). `work` must not touch this object
    // or the editor: it may outlive both at shutdown. False, and nothing started,
    // while another request is running. The detect, capture and solve actions use
    // it; a test drives the async path with it, without a camera or a sidecar.
    bool startRequest(const std::string& busyStatus, const std::string& who,
                      const UUID& target, std::function<Outcome()> work);

    // "Detect Surface" (QM-058): capture a frame (live camera, or the
    // QM_AI_TEST_IMAGE override) -> sidecar /surface -> apply the proposed quad to
    // the selected quad when confident, else prompt "tap the surface". The applied
    // corners are an ordinary warp the user refines with Tab-cycle + arrow-nudge.
    // Its corners map into projector space as Pick Surface's do (mapPhotoCorners), with
    // `outputs` finding the current scan; null: no scan is used.
    void detectSurface(const OutputManager* outputs = nullptr);

    // One-Flash capture flow (QM-059): flash a known ChArUco pattern fullscreen on
    // the projector outputs, then capture a single photo (surface + pattern) to
    // disk for the sidecar's homography solve (QM-060).
    void emitPattern(const OutputManager& outputs);
    void capturePhoto(const OutputManager* outputs);
    void clearPattern();
    // Capture a photo of the projected pattern, solve via POST /oneflash, and
    // apply the resulting corners to the selected quad.
    //   solveAndApply (QM-060): target = the detected surface.
    //   anamorphic    (QM-062): target = a centered audience rectangle ->
    //     content looks frontal from the captured viewpoint (anamorphosis).
    void solveAndApply(const OutputManager* outputs);
    void anamorphic(const OutputManager* outputs);

    // ---- Pick Surface, the AI Photo panel (ADR-053 §2) ----------------------------
    // The sidecar advertises "segment" (cached at its health check, no I/O).
    bool canSegment() const;
    const AiPhoto& photo() const { return photo_; }
    // Take the panel's photo (live camera, or the QM_AI_TEST_IMAGE override); a new
    // photo clears the proposal.
    void takePhoto();
    // Ask the sidecar for the surface under a click on the photo (normalized, top-left
    // origin). The proposal is only shown: nothing changes until applyProposal().
    void segmentAt(float x, float y);
    // Warp the quad selected now onto the proposal, mapped into projector space with
    // the best mapping available (pickMapping). `outputs` finds the output showing the
    // quad, whose Gray-code map is used when it is current; it may be null.
    void applyProposal(const OutputManager* outputs = nullptr);
    void clearProposal();

    // How Pick Surface maps photo corners into projector space, best first.
    enum class PickMapping {
        GrayCode,           // through the scanned output's current Gray-code map (/graymap)
        OneFlash,           // through /oneflash with the One-Flash photo taken this session
        CameraAtProjector,  // photo space as-is
    };
    static PickMapping pickMapping(bool grayMapCurrent, const std::string& oneFlashPhoto);

    // The mapping chain for photo corners (Pick Surface's Apply and Detect Surface), run
    // by the worker: through `grayMap` when given, else (or when a corner falls outside
    // the scanned area, or the map fails) through `oneFlash` when given, else photo
    // space as-is. The status starts with `lead` and says which and why.
    using Corners = std::array<std::array<float, 2>, 4>;
    using GrayMapFn = std::function<ai::GrayReply<ai::GrayMapResult>(
        const std::vector<std::array<float, 2>>&)>;
    using OneFlashFn = std::function<std::optional<ai::SurfaceResult>(const Corners&)>;
    static Outcome mapPhotoCorners(const std::string& lead, const Corners& photoCorners,
                                   const GrayMapFn& grayMap, const OneFlashFn& oneFlash);
    // A finished /segment call as the panel shows it: the proposal (also a stub or a
    // low-confidence one, which Apply refuses) and a status line.
    static Outcome segmentOutcome(const ai::SegmentReply& reply);

    // ---- Scan Output (ADR-053 §3) ---------------------------------------------------
    // The sidecar advertises "graycode" (cached at its health check, no I/O).
    bool canScan() const;
    // A scan is preparing or showing its frames (its decode afterwards is an ordinary
    // request: isBusy).
    bool isScanning() const { return scan_ != nullptr; }
    // AI > Scan Output: scan the selected output, else the first enabled one. Refused
    // under the perform lock, under blackout, while calibrating or editing a slice
    // warp, while another request runs, and without the "graycode" capability. Ticked
    // by tick(): it goes fullscreen, then asks the sidecar for the frames at the size
    // the output shows there. The status line shows its progress, and Cancel Scan, Esc
    // (EditorState::aiScan) or anything that leaves perform mode cancels it.
    void scanOutput(OutputManager* outputs);
    void cancelScan();
    // The output's map when this session has one and it is current; else null.
    const GrayMap* currentGrayMap(const UUID& outputId, const OutputManager* outputs) const;
    // Assisted calibration (ADR-052 §6): place the selected calibration point's pixel
    // where a click on the AI photo (normalized) maps through the calibrated output's
    // current scan. One calibration undo step; refused under the perform lock.
    void calibrationPixelAt(float x, float y, const OutputManager* outputs);

    // Draws, in place of the scene, the scan's frame fullscreen on the scanned output
    // (the others black) while a scan shows one, else the armed One-Flash pattern on
    // every output, and returns true; otherwise draws nothing.
    bool renderPatternFrames(OutputManager& outputs, Renderer& renderer);

    // Shutdown: stop and kill any launched sidecar (no orphan). Application calls it
    // before the outputs go, and destroys this object (the pattern's GL texture)
    // later, while the editor's context is still current.
    void stopSidecar();

private:
    void setStatus(const std::string& msg, float seconds);
    bool sidecarReady() const;
    void runSelfTest(OutputManager* outputs);
    void runOneFlash(const OutputManager* outputs,
                     const std::array<std::array<float, 2>, 4>* audienceTarget,
                     const char* label);
    // Open the first camera, grab one frame, and write it to pngPath. Returns
    // false with a human-readable reason in `error`.
    static bool captureCameraFrameToPng(const std::string& pngPath, std::string& error);

    // ScanHost over the outputs and the editor; showFrame loads the scan's frame image.
    class ScanHostImpl;
    // Every frame is captured: let the scan go and ask the sidecar for its map.
    void startScanDecode();
    // The mappings photo corners can take now (the current scan of an output showing the
    // active scene, the One-Flash photo), as the worker's calls; returns the best.
    PickMapping photoMappings(const OutputManager* outputs, GrayMapFn& grayMap,
                              OneFlashFn& oneFlash) const;
    // The fullscreen identity quad the One-Flash pattern and the scan's frames ride on.
    void ensurePatternQuad();

    EditorState& editor_;
    // Shared with a running request, which may still be inside its HTTP call when
    // stopSidecar() lets go of it.
    std::shared_ptr<ai::SidecarClient> sidecar_;
    // Last observed sidecar state, so tick() can report an async connect (TD-10).
    ai::SidecarState lastSidecarState_;

    // QM-058 self-test hook (QM_AI_SELFTEST). Off in normal runs.
    bool selfTestPending_ = false;
    int frameCount_ = 0;

    // One-Flash pattern (QM-059): while armed, the outputs show calibQuad_
    // (identity, fullscreen) instead of the scene. Built on the first emit.
    bool emitPattern_ = false;
    std::unique_ptr<Quad> calibQuad_;
    std::unique_ptr<Texture> calibTexture_;

    // The AI Photo panel's photo and proposal, and the last One-Flash > Capture Photo
    // this session (Pick Surface maps through it).
    AiPhoto photo_;
    std::string oneFlashPhoto_;

    // The scan in progress (at most one), the frame it shows, and the session's maps.
    std::unique_ptr<ScanHostImpl> scanHost_;
    std::unique_ptr<GrayCodeScan> scan_;
    std::unique_ptr<Texture> scanTexture_;
    bool scanFrameShown_ = false;
    UUID scanOutputId_;
    std::string scanProgress_;  // last progress posted, so it is posted on change only
    std::unordered_map<UUID, GrayMap> grayMaps_;

    // The request in flight (at most one). The worker owns a share of Request, so a
    // worker still stuck at shutdown can be detached safely.
    struct Request;
    std::shared_ptr<Request> request_;
    std::thread requestThread_;
    // On the main thread: post a finished request's outcome (see startRequest).
    void applyOutcome(const Request& request, OutputManager* outputs);
    // False, with the status saying why, when the point or its calibration is gone.
    bool applyCalibrationPixel(const CalibrationPixel& pixel, OutputManager* outputs);
};

} // namespace quadmapper
