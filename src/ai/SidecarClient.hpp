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

struct SidecarConfig {
    std::string host = "127.0.0.1";   // loopback-only (ADR-035)
    int port = 8765;
    std::string pythonExe = "python"; // resolved from PATH if not absolute
    std::string scriptPath;           // absolute path to tools/ai-sidecar/sidecar.py
    int expectedApiVersion = 1;       // refuse to attach to an unknown version
    int startupTimeoutMs = 8000;      // budget for a freshly launched sidecar to answer /health
    int requestTimeoutMs = 30000;     // per-request ceiling (pre-show, latency-tolerant)
};

enum class SidecarState {
    Stopped,   // not started, or stopped
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
    // reachable afterwards. Safe to call again after a failure.
    bool start();

    // Terminate a sidecar we launched (no-op when Attached/Stopped). Idempotent;
    // also invoked by the destructor so no child outlives the app.
    void stop();

    // GET /health, validating status and api_version. Cheap; safe any time.
    bool isHealthy() const;

    SidecarState state() const;

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quadmapper::ai
