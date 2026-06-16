#include "SidecarClient.hpp"

#include "core/Logger.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
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

// ---- Impl ------------------------------------------------------------------

struct SidecarClient::Impl {
    SidecarConfig config;
    SidecarState state = SidecarState::Stopped;

    // Set only when we launched the process ourselves.
    HANDLE job = nullptr;          // job with KILL_ON_JOB_CLOSE
    HANDLE process = nullptr;      // child process handle
    DWORD processId = 0;

    explicit Impl(SidecarConfig c) : config(std::move(c)) {}

    ~Impl() { killLaunched(); }

    bool healthOnce(int timeoutMs) const {
        HttpResponse r = httpRequest(config.host, config.port, "GET", "/health",
                                     {}, timeoutMs);
        if (!r.ok || r.status != 200) return false;
        try {
            json j = json::parse(r.body);
            return j.value("status", "") == "ok" &&
                   j.value("api_version", -1) == config.expectedApiVersion;
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
        // Closing the job handle terminates the child (KILL_ON_JOB_CLOSE).
        if (job) {
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

void SidecarClient::stop() {
    auto& im = *impl_;
    if (im.state == SidecarState::Launched) {
        im.killLaunched();
    }
    im.state = SidecarState::Stopped;
}

bool SidecarClient::isHealthy() const {
    return impl_->healthOnce(1000);
}

SidecarState SidecarClient::state() const {
    return impl_->state;
}

std::optional<SurfaceResult> SidecarClient::requestSurface(
    const std::string& imagePath) {
    auto& im = *impl_;
    if (im.state != SidecarState::Attached && im.state != SidecarState::Launched) {
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
    if (im.state != SidecarState::Attached && im.state != SidecarState::Launched) {
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
