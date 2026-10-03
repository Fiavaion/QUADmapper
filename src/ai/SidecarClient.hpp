#pragma once

// SidecarClient — C++ side of the AI pre-show integration boundary (ADR-035,
// QM-056). Owns the lifecycle of the optional out-of-process Python sidecar and
// talks to it over loopback HTTP+JSON. The app is fully usable without it: every
// method degrades gracefully (start() returns false, requestSurface() returns
// nullopt) so the AI UI can disable cleanly when the sidecar is absent or errors.
//
// All Windows/WinHTTP headers stay behind the pimpl, matching the project's
// FFmpeg/miniaudio isolation pattern — this header pulls in std types only.

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quadmapper::ai {

// One detected surface, in normalized [0,1] image coordinates. In the QM-056
// skeleton the sidecar returns a centered placeholder with stub == true.
struct SurfaceResult {
    std::array<std::array<float, 2>, 4> corners{};  // TL, TR, BR, BL
    std::array<float, 3> planeNormal{};
    float planeD = 0.0f;
    float confidence = 0.0f;
    bool stub = false;
};

// A click-to-segment surface proposal (ADR-053 §2, POST /segment), in normalized [0,1]
// photo coordinates with a top-left origin.
struct SegmentResult {
    std::array<std::array<float, 2>, 4> corners{};  // TL, TR, BR, BL
    std::vector<std::array<float, 2>> outline;      // the mask's outline
    float confidence = 0.0f;                        // planarity already folded in
    std::optional<float> planarity;                 // nullopt: no depth model
    bool stub = false;
};

// What requestSegment() hands back: a result, or why there is none.
struct SegmentReply {
    std::optional<SegmentResult> result;
    bool endpointMissing = false;  // HTTP 404: the sidecar predates /segment
};

// The most outline points a /segment reply may carry; more is a malformed reply.
constexpr size_t MAX_SEGMENT_OUTLINE_POINTS = 256;

// Parse a /segment reply body. nullopt on a version mismatch, malformed corners or a
// malformed outline (not a list of [x, y] numbers, or too many points). A missing
// outline reads as empty, a missing or null planarity as nullopt.
std::optional<SegmentResult> parseSegmentResponse(const std::string& body,
                                                  int expectedApiVersion);

// The capabilities a /health body advertises. A sidecar that predates the field
// (ADR-053 §1) can do what it always could: {"surface", "oneflash"}.
std::vector<std::string> parseCapabilities(const std::string& healthBody);

struct SidecarConfig {
    std::string host = "127.0.0.1";   // loopback-only (ADR-035)
    int port = 8765;
    std::string pythonExe = "python"; // resolved from PATH if not absolute
    std::string scriptPath;           // absolute path to tools/ai-sidecar/sidecar.py
    int expectedApiVersion = 1;       // refuse to attach to an unknown version
    int startupTimeoutMs = 8000;      // budget for a freshly launched sidecar to answer /health
    // Per-request ceiling. Requests run off the main thread (AiAssist::startRequest), so
    // this only bounds a wait: a first request that loads the models can take ~40 s from
    // a cold disk, and /surface on a CPU-only machine ~30 s (ADR-053).
    int requestTimeoutMs = 120000;
};

enum class SidecarState {
    Stopped,   // not started, or stopped
    Starting,  // startAsync() is connecting on a worker thread (not yet usable)
    Attached,  // a sidecar we did not spawn is answering (we do not own its process)
    Launched,  // we spawned it (and will kill it on stop()/destruction)
    Failed     // start() ran but no healthy sidecar is reachable
};

// Not copyable: owns a process + job handle. Move is not needed (held by value
// or unique_ptr in the app).
class SidecarClient {
public:
    explicit SidecarClient(SidecarConfig config);
    ~SidecarClient();

    SidecarClient(const SidecarClient&) = delete;
    SidecarClient& operator=(const SidecarClient&) = delete;

    // Attach to an already-running sidecar on host:port, or launch one from
    // scriptPath. Returns true iff a healthy sidecar (matching api_version) is
    // reachable afterwards. Safe to call again after a failure. Blocks the
    // caller for up to startupTimeoutMs while launching/polling — prefer
    // startAsync() from the UI thread.
    bool start();

    // Non-blocking start (TD-10): runs the attach/launch/health-poll on a worker
    // thread so the UI never freezes on enable. state() reads Starting until it
    // resolves to Attached/Launched/Failed. No-op if already connecting or
    // connected; safe to call again after a failure.
    void startAsync();

    // Terminate a sidecar we launched (no-op when Attached/Stopped). Idempotent;
    // also invoked by the destructor so no child outlives the app.
    void stop();

    // GET /health, validating status and api_version. Cheap; safe any time.
    bool isHealthy() const;

    SidecarState state() const;

    // What the sidecar advertised at its last successful health check ("surface",
    // "oneflash", "segment", "depth"); empty until one succeeds. Cached: no I/O.
    std::vector<std::string> capabilities() const;

    // POST /surface {image_path}. Returns nullopt on any transport, parse, or
    // version error — never throws — so callers treat AI as best-effort.
    std::optional<SurfaceResult> requestSurface(const std::string& imagePath);

    // POST /oneflash {pattern_image, target_corners?} — One-Flash homography
    // solve. Returns projector-normalized [0,1] quad corners. Same best-effort
    // contract as requestSurface.
    //   targetCorners == nullptr (QM-060): the sidecar maps the *detected surface*
    //     back through the homography -> content lands on the surface.
    //   targetCorners != nullptr (QM-062): the given audience-frame rectangle
    //     (photo-normalized TL,TR,BR,BL) is mapped back -> content looks frontal
    //     from the captured viewpoint (anamorphosis).
    std::optional<SurfaceResult> requestOneFlash(
        const std::string& patternImagePath,
        const std::array<std::array<float, 2>, 4>* targetCorners = nullptr);

    // POST /segment {image_path, point} (ADR-053 §2): the surface under `point`
    // (normalized photo coordinates, top-left origin). Same best-effort contract;
    // an old sidecar's 404 sets endpointMissing.
    SegmentReply requestSegment(const std::string& imagePath,
                                const std::array<float, 2>& point);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quadmapper::ai
