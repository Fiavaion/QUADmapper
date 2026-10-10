#include "SidecarClient.hpp"

#include "core/Logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

// Windows networking + process control. Kept entirely in this TU (pimpl).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace quadmapper::ai {

namespace {

using nlohmann::json;

// ---- small RAII helpers ----------------------------------------------------

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                  static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), len);
    return out;
}

struct HttpResponse {
    bool ok = false;       // transport succeeded (got an HTTP status back)
    long status = 0;       // HTTP status code
    std::string body;      // response body (may be empty)
};

// Synchronous loopback HTTP request via WinHTTP. method/path are ASCII; body is
// sent as application/json when non-empty. Never throws.
HttpResponse httpRequest(const std::string& host, int port,
                         const std::string& method, const std::string& path,
                         const std::string& body, int timeoutMs) {
    HttpResponse result;

    WinHttpHandle session(WinHttpOpen(L"QUADmapper-Sidecar/1.0",
                                      WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return result;

    // resolve / connect / send / receive all share the same ceiling.
    WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    WinHttpHandle connect(WinHttpConnect(session.h, toWide(host).c_str(),
                                         static_cast<INTERNET_PORT>(port), 0));
    if (!connect) return result;

    WinHttpHandle request(WinHttpOpenRequest(
        connect.h, toWide(method).c_str(), toWide(path).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request) return result;

    LPCWSTR headers = WINHTTP_NO_ADDITIONAL_HEADERS;
    DWORD headersLen = 0;
    if (!body.empty()) {
        headers = L"Content-Type: application/json\r\n";
        headersLen = static_cast<DWORD>(-1);  // header is null-terminated
    }

    BOOL sent = WinHttpSendRequest(
        request.h, headers, headersLen,
        body.empty() ? WINHTTP_NO_REQUEST_DATA
                     : static_cast<LPVOID>(const_cast<char*>(body.data())),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent) return result;

    if (!WinHttpReceiveResponse(request.h, nullptr)) return result;

    DWORD statusCode = 0;
    DWORD statusLen = sizeof(statusCode);
    if (WinHttpQueryHeaders(request.h,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusLen,
                            WINHTTP_NO_HEADER_INDEX)) {
        result.status = static_cast<long>(statusCode);
    }

    // Drain the body.
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) break;
        if (available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), available, &read)) break;
        chunk.resize(read);
        result.body += chunk;
        if (read == 0) break;
    }

    result.ok = true;
    return result;
}

// Parse a corners response (shared by /surface and /oneflash): validates the
// api_version, reads corners[4], and the optional plane/confidence/stub. The
// homography in a /oneflash reply is ignored on the C++ side — only the corners
// drive the quad.
std::optional<SurfaceResult> parseCornersResponse(const std::string& body,
                                                  int expectedApiVersion) {
    try {
        json j = json::parse(body);
        if (j.value("api_version", -1) != expectedApiVersion) {
            return std::nullopt;
        }
        SurfaceResult out;
        const auto& corners = j.at("corners");
        if (!corners.is_array() || corners.size() != 4) return std::nullopt;
        for (size_t i = 0; i < 4; ++i) {
            out.corners[i][0] = corners[i].at(0).get<float>();
            out.corners[i][1] = corners[i].at(1).get<float>();
        }
        if (j.contains("plane") && j["plane"].is_object()) {
            const auto& plane = j.at("plane");
            const auto& n = plane.value("normal", json::array({0, 0, 1}));
            for (size_t i = 0; i < 3 && i < n.size(); ++i) {
                out.planeNormal[i] = n[i].get<float>();
            }
            out.planeD = plane.value("d", 0.0f);
        }
        out.confidence = j.value("confidence", 0.0f);
        out.stub = j.value("stub", false);
        return out;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

}  // namespace

std::optional<SegmentResult> parseSegmentResponse(const std::string& body,
                                                  int expectedApiVersion) {
    // The corners, confidence and stub read exactly as /surface's do.
    const std::optional<SurfaceResult> corners = parseCornersResponse(body, expectedApiVersion);
    if (!corners) return std::nullopt;
    try {
        const json j = json::parse(body);
        SegmentResult out;
        out.corners = corners->corners;
        out.confidence = corners->confidence;
        out.stub = corners->stub;
        if (j.contains("outline")) {
            const json& outline = j.at("outline");
            if (!outline.is_array() || outline.size() > MAX_SEGMENT_OUTLINE_POINTS) {
                return std::nullopt;
            }
            out.outline.reserve(outline.size());
            for (const json& point : outline) {
                if (!point.is_array() || point.size() != 2 || !point[0].is_number() ||
                    !point[1].is_number()) {
                    return std::nullopt;
                }
                out.outline.push_back({point[0].get<float>(), point[1].get<float>()});
            }
        }
        if (j.contains("planarity") && j.at("planarity").is_number()) {
            out.planarity = j.at("planarity").get<float>();
        }
        return out;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::optional<GrayPatternsResult> parseGrayPatternsResponse(const std::string& body,
                                                            int expectedApiVersion) {
    try {
        const json j = json::parse(body);
        if (j.value("api_version", -1) != expectedApiVersion) return std::nullopt;
        const json& count = j.at("count");
        if (!count.is_number_integer() || count.get<long long>() < 0 ||
            count.get<long long>() > 10000) {
            return std::nullopt;
        }
        GrayPatternsResult out;
        out.count = count.get<int>();
        out.stub = j.value("stub", false);
        if (!out.stub && out.count < 3) return std::nullopt;  // at least one pattern + white/black
        return out;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::optional<GrayCodeResult> parseGrayCodeResponse(const std::string& body,
                                                    int expectedApiVersion) {
    try {
        const json j = json::parse(body);
        if (j.value("api_version", -1) != expectedApiVersion) return std::nullopt;
        GrayCodeResult out;
        out.stub = j.value("stub", false);
        if (out.stub) return out;
        const json& path = j.at("map_path");
        const json& coverage = j.at("coverage");
        if (!path.is_string() || path.get<std::string>().empty() || !coverage.is_number()) {
            return std::nullopt;
        }
        out.mapPath = path.get<std::string>();
        out.coverage = std::clamp(coverage.get<float>(), 0.0f, 1.0f);
        if (j.contains("decode_error_px") && j.at("decode_error_px").is_number()) {
            out.decodeErrorPx = j.at("decode_error_px").get<float>();
        }
        return out;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::optional<GrayMapResult> parseGrayMapResponse(const std::string& body,
                                                  int expectedApiVersion,
                                                  size_t expectedPoints) {
    try {
        const json j = json::parse(body);
        if (j.value("api_version", -1) != expectedApiVersion) return std::nullopt;
        const json& points = j.at("points");
        const json& valid = j.at("valid");
        if (!points.is_array() || !valid.is_array() || points.size() != expectedPoints ||
            valid.size() != expectedPoints) {
            return std::nullopt;
        }
        GrayMapResult out;
        for (size_t i = 0; i < expectedPoints; ++i) {
            const json& p = points[i];
            if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number() ||
                !valid[i].is_boolean()) {
                return std::nullopt;
            }
            out.points.push_back({p[0].get<float>(), p[1].get<float>()});
            out.valid.push_back(valid[i].get<bool>());
        }
        return out;
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

std::vector<std::string> parseCapabilities(const std::string& healthBody) {
    try {
        const json j = json::parse(healthBody);
        if (!j.contains("capabilities")) return {"surface", "oneflash"};
        if (!j.at("capabilities").is_array()) return {};
        std::vector<std::string> out;
        for (const json& c : j.at("capabilities")) {
            if (c.is_string()) out.push_back(c.get<std::string>());
        }
        return out;
    } catch (const json::exception&) {
        return {};
    }
}

// ---- Impl ------------------------------------------------------------------

struct SidecarClient::Impl {
    SidecarConfig config;
    // Atomic: written by start() (possibly on the startAsync worker) and read
    // per-frame by the UI thread via state() (TD-10).
    std::atomic<SidecarState> state{SidecarState::Stopped};

    // Background worker for startAsync(); joined before teardown so the process
    // handles below are only touched after it has finished.
    std::thread worker;

    // From the last successful health check, which may run on the startAsync worker
    // while the UI thread reads them.
    mutable std::mutex capabilitiesMutex;
    mutable std::vector<std::string> capabilities;

    // Set only when we launched the process ourselves.
    HANDLE job = nullptr;          // job with KILL_ON_JOB_CLOSE
    HANDLE process = nullptr;      // child process handle
    DWORD processId = 0;

    explicit Impl(SidecarConfig c) : config(std::move(c)) {}

    ~Impl() {
        if (worker.joinable()) worker.join();
        killLaunched();
    }

    bool healthOnce(int timeoutMs) const {
        HttpResponse r = httpRequest(config.host, config.port, "GET", "/health",
                                     {}, timeoutMs);
        if (!r.ok || r.status != 200) return false;
        try {
            json j = json::parse(r.body);
            const bool healthy = j.value("status", "") == "ok" &&
                                 j.value("api_version", -1) == config.expectedApiVersion;
            if (healthy) {
                std::lock_guard<std::mutex> lock(capabilitiesMutex);
                capabilities = parseCapabilities(r.body);
            }
            return healthy;
        } catch (const json::exception&) {
            return false;
        }
    }

    // Launch the sidecar in a job object so it cannot outlive us. Returns true
    // if the process started (not that it is healthy yet).
    bool launchProcess() {
        job = CreateJobObjectW(nullptr, nullptr);
        if (!job) return false;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits));

        // "<python>" "<script>" --host <host> --port <port>
        std::string cmd = "\"" + config.pythonExe + "\" \"" + config.scriptPath +
                          "\" --host " + config.host + " --port " +
                          std::to_string(config.port);
        std::wstring cmdW = toWide(cmd);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        // Suspended so we can assign to the job before any work runs.
        BOOL created = CreateProcessW(
            nullptr, cmdW.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
        if (!created) {
            CloseHandle(job);
            job = nullptr;
            return false;
        }

        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        process = pi.hProcess;
        processId = pi.dwProcessId;
        return true;
    }

    void killLaunched() {
        // Terminate the child and wait for it to exit, so its port is closed when
        // stop() returns (closing the job alone kills asynchronously; it would also
        // kill it, KILL_ON_JOB_CLOSE).
        if (job) {
            TerminateJobObject(job, 1);
            if (process) WaitForSingleObject(process, 5000);
            CloseHandle(job);
            job = nullptr;
        }
        if (process) {
            CloseHandle(process);
            process = nullptr;
        }
        processId = 0;
    }
};

// ---- public surface --------------------------------------------------------

SidecarClient::SidecarClient(SidecarConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

SidecarClient::~SidecarClient() = default;

bool SidecarClient::start() {
    auto& im = *impl_;

    // 1) Attach to an already-running sidecar if one answers.
    if (im.healthOnce(1000)) {
        im.state = SidecarState::Attached;
        Logger::info("AI sidecar: attached to running instance on " +
                           im.config.host + ":" + std::to_string(im.config.port));
        return true;
    }

    // 2) Otherwise launch one (needs a script path).
    if (im.config.scriptPath.empty()) {
        im.state = SidecarState::Failed;
        Logger::warning("AI sidecar: no script path configured; AI disabled");
        return false;
    }
    if (!im.launchProcess()) {
        im.state = SidecarState::Failed;
        Logger::warning("AI sidecar: failed to launch process; AI disabled");
        return false;
    }

    // 3) Poll for readiness within the startup budget.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(im.config.startupTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (im.healthOnce(500)) {
            im.state = SidecarState::Launched;
            Logger::info("AI sidecar: launched (pid " +
                               std::to_string(im.processId) + ")");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    Logger::warning("AI sidecar: launched process did not become healthy; "
                          "AI disabled");
    im.killLaunched();
    im.state = SidecarState::Failed;
    return false;
}

void SidecarClient::startAsync() {
    auto& im = *impl_;
    const SidecarState s = im.state.load();
    if (s == SidecarState::Attached || s == SidecarState::Launched ||
        s == SidecarState::Starting) {
        return;  // already connected, or a connect attempt is already in flight
    }
    // Reap a previous finished attempt before launching another.
    if (im.worker.joinable()) im.worker.join();
    im.state = SidecarState::Starting;
    im.worker = std::thread([this] { start(); });
}

void SidecarClient::stop() {
    auto& im = *impl_;
    if (im.worker.joinable()) im.worker.join();  // finish any in-flight startAsync
    if (im.state.load() == SidecarState::Launched) {
        im.killLaunched();
    }
    im.state = SidecarState::Stopped;
}

bool SidecarClient::isHealthy() const {
    return impl_->healthOnce(1000);
}

SidecarState SidecarClient::state() const {
    return impl_->state.load();
}

std::vector<std::string> SidecarClient::capabilities() const {
    const SidecarState s = impl_->state.load();
    if (s != SidecarState::Attached && s != SidecarState::Launched) return {};
    std::lock_guard<std::mutex> lock(impl_->capabilitiesMutex);
    return impl_->capabilities;
}

SegmentReply SidecarClient::requestSegment(const std::string& imagePath,
                                           const std::array<float, 2>& point) {
    auto& im = *impl_;
    const SidecarState s = im.state.load();
    if (s != SidecarState::Attached && s != SidecarState::Launched) return {};

    json req = {{"image_path", imagePath}, {"point", {point[0], point[1]}}};
    HttpResponse r = httpRequest(im.config.host, im.config.port, "POST",
                                 "/segment", req.dump(), im.config.requestTimeoutMs);
    SegmentReply reply;
    if (!r.ok) return reply;
    if (r.status == 404) {
        reply.endpointMissing = true;
        return reply;
    }
    if (r.status != 200) return reply;
    reply.result = parseSegmentResponse(r.body, im.config.expectedApiVersion);
    return reply;
}

namespace {

// POST `body` to a Gray-code endpoint and parse a 200 with `parse`; a 404 or a 400's
// detail is reported, never thrown.
template <typename T, typename Parse>
GrayReply<T> grayRequest(const SidecarConfig& config, SidecarState state, const char* path,
                         const json& body, Parse parse) {
    GrayReply<T> reply;
    if (state != SidecarState::Attached && state != SidecarState::Launched) return reply;
    const HttpResponse r =
        httpRequest(config.host, config.port, "POST", path, body.dump(), config.requestTimeoutMs);
    if (!r.ok) return reply;
    if (r.status == 404) {
        reply.endpointMissing = true;
    } else if (r.status == 400) {
        try {
            const json j = json::parse(r.body);
            if (j.contains("detail") && j.at("detail").is_string()) reply.error = j.at("detail").get<std::string>();
        } catch (const json::exception&) {
        }
        if (reply.error.empty()) reply.error = "bad request";
    } else if (r.status == 200) {
        reply.result = parse(r.body);
    }
    return reply;
}

}  // namespace

GrayReply<GrayPatternsResult> SidecarClient::requestGrayPatterns(const std::string& folder,
                                                                 int width, int height) {
    const int version = impl_->config.expectedApiVersion;
    return grayRequest<GrayPatternsResult>(
        impl_->config, impl_->state.load(), "/graycode/patterns",
        json{{"folder", folder}, {"projector", {width, height}}},
        [version](const std::string& body) { return parseGrayPatternsResponse(body, version); });
}

GrayReply<GrayCodeResult> SidecarClient::requestGrayCode(const std::string& folder, int width,
                                                         int height) {
    const int version = impl_->config.expectedApiVersion;
    return grayRequest<GrayCodeResult>(
        impl_->config, impl_->state.load(), "/graycode",
        json{{"folder", folder}, {"projector", {width, height}}},
        [version](const std::string& body) { return parseGrayCodeResponse(body, version); });
}

GrayReply<GrayMapResult> SidecarClient::requestGrayMap(
    const std::string& mapPath, const std::vector<std::array<float, 2>>& points) {
    if (points.empty() || points.size() > MAX_GRAYMAP_POINTS) return {};
    json list = json::array();
    for (const auto& p : points) list.push_back({p[0], p[1]});
    const int version = impl_->config.expectedApiVersion;
    const size_t count = points.size();
    return grayRequest<GrayMapResult>(
        impl_->config, impl_->state.load(), "/graymap",
        json{{"map_path", mapPath}, {"points", list}},
        [version, count](const std::string& body) {
            return parseGrayMapResponse(body, version, count);
        });
}

std::optional<SurfaceResult> SidecarClient::requestSurface(
    const std::string& imagePath) {
    auto& im = *impl_;
    const SidecarState s = im.state.load();
    if (s != SidecarState::Attached && s != SidecarState::Launched) {
        return std::nullopt;
    }

    json req = {{"image_path", imagePath}};
    HttpResponse r = httpRequest(im.config.host, im.config.port, "POST",
                                 "/surface", req.dump(), im.config.requestTimeoutMs);
    if (!r.ok || r.status != 200) return std::nullopt;
    return parseCornersResponse(r.body, im.config.expectedApiVersion);
}

std::optional<SurfaceResult> SidecarClient::requestOneFlash(
    const std::string& patternImagePath,
    const std::array<std::array<float, 2>, 4>* targetCorners) {
    auto& im = *impl_;
    const SidecarState s = im.state.load();
    if (s != SidecarState::Attached && s != SidecarState::Launched) {
        return std::nullopt;
    }

    json req = {{"pattern_image", patternImagePath}};
    if (targetCorners != nullptr) {
        req["target_corners"] = json::array();
        for (const auto& c : *targetCorners) {
            req["target_corners"].push_back({c[0], c[1]});
        }
    }
    HttpResponse r = httpRequest(im.config.host, im.config.port, "POST",
                                 "/oneflash", req.dump(), im.config.requestTimeoutMs);
    if (!r.ok || r.status != 200) return std::nullopt;
    return parseCornersResponse(r.body, im.config.expectedApiVersion);
}

}  // namespace quadmapper::ai
