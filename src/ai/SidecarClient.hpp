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

// ---- Gray-code scan (ADR-053 §3) -------------------------------------------------------

// POST /graycode/patterns: how many frames the sidecar wrote (pattern_000.png ...), from
// OpenCV's generator. 0 with stub == true: the sidecar has no OpenCV contrib.
struct GrayPatternsResult {
    int count = 0;
    bool stub = false;
};
// POST /graycode: the decoded camera -> projector map beside the captures.
struct GrayCodeResult {
    std::string mapPath;                // empty on a stub
    float coverage = 0.0f;              // share of the projector's area the camera saw
    std::optional<float> decodeErrorPx; // nullopt: nothing decoded
    bool stub = false;
};
// POST /graymap: normalized photo points -> normalized projector points, with whether
// each fell inside the decoded area.
struct GrayMapResult {
    std::vector<std::array<float, 2>> points;
    std::vector<bool> valid;
};

// What a Gray-code request hands back: a result, or why there is none.
template <typename T>
struct GrayReply {
    std::optional<T> result;
    bool endpointMissing = false;  // HTTP 404: the sidecar predates the endpoint
    std::string error;             // a 400's detail (a missing capture, a stale map)
};

// The most points one /graymap call carries (the sidecar's limit).
constexpr size_t MAX_GRAYMAP_POINTS = 64;

// Parse the replies; nullopt on a version mismatch or a malformed body. /graymap must
// carry exactly `expectedPoints` points.
std::optional<GrayPatternsResult> parseGrayPatternsResponse(const std::string& body,
                                                            int expectedApiVersion);
std::optional<GrayCodeResult> parseGrayCodeResponse(const std::string& body,
                                                    int expectedApiVersion);
std::optional<GrayMapResult> parseGrayMapResponse(const std::string& body,
                                                  int expectedApiVersion,
                                                  size_t expectedPoints);

// The capabilities a /health body advertises. A sidecar that predates the field
// (ADR-053 §1) can do what it always could: {"surface", "oneflash"}.
std::vector<std::string> parseCapabilities(const std::string& healthBody);

// The service name a real sidecar reports at /health (sidecar.py SERVICE_NAME).
constexpr const char* SIDECAR_SERVICE_NAME = "quadmapper-ai-sidecar";
// The most response body the client reads; a larger answer is an error.
constexpr size_t MAX_RESPONSE_BYTES = 8u * 1024u * 1024u;

struct SidecarConfig {
    std::string host = "127.0.0.1";   // loopback-only (ADR-035)
    int port = 8765;
    // Absolute paths, found next to the executable (AiAssist::locateSidecar). Either
    // empty or relative: the AI features are not installed and nothing is launched
    // (never a bare "python", which Windows looks up in the current folder first).
    std::string pythonExe;
    std::string scriptPath;
    std::string modelsDir;            // non-empty: QM_MODELS_DIR for the child, unless the app has one
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
    Attached,  // a sidecar we did not spawn is answering (developer token only)
    Launched,  // we spawned it (and will kill it on stop()/destruction)
    Failed     // start() found no usable sidecar, or a request lost the connection
};

// Why the last start() failed (None while it has not failed).
enum class SidecarFailure {
    None,
    NotInstalled,  // no interpreter or script was found
    PortInUse,     // something the app did not launch answers on the port
    NoResponse     // the launch failed, or the sidecar never became healthy
};

// The environment variable that carries the token to the sidecar (ADR-035, amendment
// of 2026-11-14). Set in the app's own environment, it is a developer's: the app uses
// it and may attach to a sidecar already running with it.
constexpr const char* SIDECAR_TOKEN_ENV = "QM_SIDECAR_TOKEN";

// Not copyable: owns a process + job handle. Move is not needed (held by value
// or unique_ptr in the app).
class SidecarClient {
public:
    explicit SidecarClient(SidecarConfig config);
    ~SidecarClient();

    SidecarClient(const SidecarClient&) = delete;
    SidecarClient& operator=(const SidecarClient&) = delete;

    // Launch the sidecar from pythonExe + scriptPath with a fresh random token, and
    // wait for it to answer /health as the sidecar (service name, api_version, token
    // accepted) from a listener inside its job. Never attaches to a server it did not
    // launch, unless QM_SIDECAR_TOKEN is set in the app's environment (a developer's
    // hand-started sidecar). Returns true iff it is usable afterwards; failure()
    // says why not. Safe to call again after a failure. Blocks the caller for up to
    // startupTimeoutMs while launching/polling — prefer startAsync() from the UI thread.
    bool start();

    // Non-blocking start (TD-10): runs the attach/launch/health-poll on a worker
    // thread so the UI never freezes on enable. state() reads Starting until it
    // resolves to Attached/Launched/Failed. No-op if already connecting or
    // connected; safe to call again after a failure.
    void startAsync();

    // Terminate a sidecar we launched (no-op when Attached/Stopped). Idempotent;
    // also invoked by the destructor so no child outlives the app.
    void stop();

    // GET /health with the token, validating status, service and api_version. Cheap;
    // safe any time.
    bool isHealthy() const;

    // A request that loses the connection (refused, reset, or a timeout once the
    // process has died) sets Failed and ends a launched process, so Enable starts it
    // again. An HTTP error answer does not.
    SidecarState state() const;
    SidecarFailure failure() const;
    int port() const;

    // What the sidecar advertised at its last successful health check ("surface",
    // "oneflash", "segment", "depth", "graycode"); empty until one succeeds. Cached: no I/O.
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

    // The Gray-code scan (ADR-053 §3), same best-effort contract. `folder` is the scan
    // folder; width x height the output's resolution.
    //   requestGrayPatterns: write the pattern PNGs into `folder`.
    //   requestGrayCode:     decode folder's captures into a map.
    //   requestGrayMap:      map up to MAX_GRAYMAP_POINTS normalized photo points.
    GrayReply<GrayPatternsResult> requestGrayPatterns(const std::string& folder, int width,
                                                      int height);
    GrayReply<GrayCodeResult> requestGrayCode(const std::string& folder, int width, int height);
    GrayReply<GrayMapResult> requestGrayMap(const std::string& mapPath,
                                            const std::vector<std::array<float, 2>>& points);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quadmapper::ai
