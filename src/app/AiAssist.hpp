#pragma once

#include "../core/UUID.hpp"
#include "../editor/EditorServices.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace quadmapper {

struct EditorState;
class OutputManager;
class Renderer;
class Quad;
class Texture;
namespace ai { class SidecarClient; struct SidecarConfig; enum class SidecarState; }

// The optional AI pre-show assist (ADR-035, ADR-051 §1): the sidecar's lifecycle,
// "Detect Surface" and the One-Flash pattern, capture and solve. Owned by
// Application; results land on the selected quad and in the editor's status line.
// Geometry only: nothing here changes the .qmap format.
class AiAssist {
public:
    // `sidecar` may be null: every action then reports that AI is unavailable.
    AiAssist(EditorState& editor, std::unique_ptr<ai::SidecarClient> sidecar);
    ~AiAssist();

    AiAssist(const AiAssist&) = delete;
    AiAssist& operator=(const AiAssist&) = delete;

    // The sidecar script next to the working dir or the repo root. When it is
    // missing, scriptPath stays empty and start() fails cleanly (AI menu shows disabled).
    static ai::SidecarConfig locateSidecar();

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

    // What a request made off the main thread hands back: a status line and, for a
    // confident warp, the target quad's new corners (TL,TR,BR,BL, normalized).
    struct Outcome {
        std::string status;
        float seconds = 5.0f;
        std::optional<std::array<std::array<float, 2>, 4>> corners;
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
    void detectSurface();

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

    // When the One-Flash pattern is armed, draws it fullscreen into every output's
    // frame in place of the scene and returns true; otherwise draws nothing.
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

    // The request in flight (at most one). The worker owns a share of Request, so a
    // worker still stuck at shutdown can be detached safely.
    struct Request;
    std::shared_ptr<Request> request_;
    std::thread requestThread_;
    // On the main thread: post a finished request's outcome (see startRequest).
    void applyOutcome(const Request& request);
};

} // namespace quadmapper
