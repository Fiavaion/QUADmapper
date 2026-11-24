#include "SidecarClient.hpp"

#include "core/Logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

// Windows networking + process control. Kept entirely in this TU (pimpl).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>  // the listener tables' types; before windows.h
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")    // BCryptGenRandom: the token
#pragma comment(lib, "iphlpapi.lib")  // GetExtendedTcpTable: who owns the port

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
    bool ok = false;        // transport succeeded and the whole body was read
    long status = 0;        // HTTP status code
    std::string body;       // response body (may be empty)
    DWORD error = 0;        // the Windows error when !ok and !tooLarge
    bool tooLarge = false;  // the body passed MAX_RESPONSE_BYTES (it is dropped)
};

// Synchronous loopback HTTP request via WinHTTP, carrying the token header. method/path
// are ASCII; body is sent as application/json when non-empty. Never throws.
HttpResponse httpRequest(const std::string& host, int port,
                         const std::string& method, const std::string& path,
                         const std::string& body, const std::string& token, int timeoutMs) {
    HttpResponse result;
    const auto failed = [&result] {
        result.error = GetLastError();
        return result;
    };

    WinHttpHandle session(WinHttpOpen(L"QUADmapper-Sidecar/1.0",
                                      WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return failed();

    // resolve / connect / send / receive all share the same ceiling.
    WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    // The token goes to this host:port only, never on to wherever a reply points.
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));

    WinHttpHandle connect(WinHttpConnect(session.h, toWide(host).c_str(),
                                         static_cast<INTERNET_PORT>(port), 0));
    if (!connect) return failed();

    WinHttpHandle request(WinHttpOpenRequest(
        connect.h, toWide(method).c_str(), toWide(path).c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request) return failed();

    std::wstring headers = L"X-QM-Token: " + toWide(token) + L"\r\n";
    if (!body.empty()) headers += L"Content-Type: application/json\r\n";

    BOOL sent = WinHttpSendRequest(
        request.h, headers.c_str(), static_cast<DWORD>(-1L),  // null-terminated
        body.empty() ? WINHTTP_NO_REQUEST_DATA
                     : static_cast<LPVOID>(const_cast<char*>(body.data())),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent) return failed();

    if (!WinHttpReceiveResponse(request.h, nullptr)) return failed();

    DWORD statusCode = 0;
    DWORD statusLen = sizeof(statusCode);
    if (WinHttpQueryHeaders(request.h,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusLen,
                            WINHTTP_NO_HEADER_INDEX)) {
        result.status = static_cast<long>(statusCode);
    }

    // Drain the body, up to the cap.
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return failed();
        if (available == 0) break;
        if (available > MAX_RESPONSE_BYTES - result.body.size()) {
            result.body.clear();
            result.tooLarge = true;
            return result;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.h, chunk.data(), available, &read)) return failed();
        chunk.resize(read);
        result.body += chunk;
        if (read == 0) break;
    }

    result.ok = true;
    return result;
}

// A fresh 128-bit token from the system's cryptographic generator, as hex.
std::optional<std::string> randomToken() {
    unsigned char bytes[16];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return std::nullopt;
    }
    static const char HEX[] = "0123456789abcdef";
    std::string token;
    for (unsigned char b : bytes) {
        token += HEX[b >> 4];
        token += HEX[b & 0x0F];
    }
    return token;
}

// A variable of the app's own environment; empty when unset.
std::string environmentVariable(const char* name) {
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
    std::string out(value);
    free(value);
    return out;
}

// QM_SIDECAR_TOKEN from the app's own environment (a developer's), or empty. Only
// letters, digits, '-' and '_' (it goes into a header).
std::string developerToken() {
    const std::string token = environmentVariable(SIDECAR_TOKEN_ENV);
    if (token.empty()) return {};
    const bool clean = token.size() <= 256 &&
                       std::all_of(token.begin(), token.end(), [](unsigned char c) {
                           return std::isalnum(c) != 0 || c == '-' || c == '_';
                       });
    if (!clean) {
        Logger::warning(std::string("AI sidecar: ") + SIDECAR_TOKEN_ENV +
                        " ignored (letters, digits, - and _ only, at most 256)");
        return {};
    }
    return token;
}

// The app's environment without the variables named in `drop`, and with `set` added or
// replacing variables of the same name, as a sorted CREATE_UNICODE_ENVIRONMENT block.
std::wstring childEnvironment(const std::vector<std::pair<std::wstring, std::wstring>>& set,
                              const std::vector<std::wstring>& drop) {
    std::vector<std::wstring> vars;
    if (LPWCH block = GetEnvironmentStringsW()) {
        for (LPWCH p = block; *p != L'\0'; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            const auto isNamed = [&entry](const std::wstring& name) {
                return entry.size() > name.size() && entry[name.size()] == L'=' &&
                       _wcsnicmp(entry.c_str(), name.c_str(), name.size()) == 0;
            };
            const bool replaced = std::any_of(set.begin(), set.end(),
                                              [&](const auto& var) { return isNamed(var.first); });
            const bool dropped = std::any_of(drop.begin(), drop.end(), isNamed);
            if (!replaced && !dropped) vars.push_back(entry);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : set) vars.push_back(name + L"=" + value);
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::wstring out;
    for (const std::wstring& var : vars) {
        out += var;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

// A port as GetExtendedTcpTable stores it (network byte order in the low 16 bits).
int portFromTable(DWORD value) {
    return static_cast<int>(((value & 0xFFu) << 8) | ((value >> 8) & 0xFFu));
}

bool processInJob(DWORD pid, HANDLE job) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return false;
    BOOL inJob = FALSE;
    const bool ok = IsProcessInJob(process, job, &inJob) && inJob;
    CloseHandle(process);
    return ok;
}

// Every TCP listener on `port` (IPv4 and IPv6, any address) belongs to a process in
// `job`, and there is at least one.
bool listenersInJob(int port, HANDLE job) {
    bool found = false;
    for (ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
        std::vector<unsigned char> buffer;
        DWORD size = 0;
        DWORD rc = ERROR_INSUFFICIENT_BUFFER;
        for (int attempt = 0; attempt < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
            buffer.resize(size);
            rc = GetExtendedTcpTable(buffer.empty() ? nullptr : buffer.data(), &size, FALSE,
                                     family, TCP_TABLE_OWNER_PID_LISTENER, 0);
        }
        if (rc != NO_ERROR) return false;
        const auto ours = [&](DWORD localPort, DWORD pid) {
            if (portFromTable(localPort) != port) return true;
            found = true;
            return processInJob(pid, job);
        };
        if (family == AF_INET) {
            const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                if (!ours(table->table[i].dwLocalPort, table->table[i].dwOwningPid)) return false;
            }
        } else {
            const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
            for (DWORD i = 0; i < table->dwNumEntries; ++i) {
                if (!ours(table->table[i].dwLocalPort, table->table[i].dwOwningPid)) return false;
            }
        }
    }
    return found;
}

// A number from a reply as a float: nullopt unless it is finite and a float can hold it.
std::optional<float> finiteFloat(const json& value) {
    if (!value.is_number()) return std::nullopt;
    const double d = value.get<double>();
    if (!std::isfinite(d) || std::abs(d) > std::numeric_limits<float>::max()) return std::nullopt;
    return static_cast<float>(d);
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
            for (size_t axis = 0; axis < 2; ++axis) {
                const std::optional<float> value = finiteFloat(corners[i].at(axis));
                if (!value) return std::nullopt;
                out.corners[i][axis] = *value;
            }
        }
        if (j.contains("plane") && j["plane"].is_object()) {
            const auto& plane = j.at("plane");
            const auto& n = plane.value("normal", json::array({0, 0, 1}));
            for (size_t i = 0; i < 3 && i < n.size(); ++i) {
                const std::optional<float> value = finiteFloat(n[i]);
                if (!value) return std::nullopt;
                out.planeNormal[i] = *value;
            }
            if (plane.contains("d")) {
                const std::optional<float> d = finiteFloat(plane["d"]);
                if (!d) return std::nullopt;
                out.planeD = *d;
            }
        }
        if (j.contains("confidence")) {
            const std::optional<float> confidence = finiteFloat(j["confidence"]);
            if (!confidence) return std::nullopt;
            out.confidence = std::clamp(*confidence, 0.0f, 1.0f);
        }
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
                if (!point.is_array() || point.size() != 2) return std::nullopt;
                const std::optional<float> x = finiteFloat(point[0]);
                const std::optional<float> y = finiteFloat(point[1]);
                if (!x || !y) return std::nullopt;
                out.outline.push_back({*x, *y});
            }
        }
        if (j.contains("planarity")) out.planarity = finiteFloat(j.at("planarity"));
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
        const std::optional<float> covered = finiteFloat(coverage);
        if (!path.is_string() || path.get<std::string>().empty() || !covered) {
            return std::nullopt;
        }
        out.mapPath = path.get<std::string>();
        out.coverage = std::clamp(*covered, 0.0f, 1.0f);
        if (j.contains("decode_error_px")) {
            if (const std::optional<float> error = finiteFloat(j.at("decode_error_px"))) {
                out.decodeErrorPx = *error;
            }
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
            if (!p.is_array() || p.size() != 2 || !valid[i].is_boolean()) return std::nullopt;
            const std::optional<float> x = finiteFloat(p[0]);
            const std::optional<float> y = finiteFloat(p[1]);
            if (!x || !y) return std::nullopt;
            out.points.push_back({*x, *y});
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

    std::atomic<SidecarFailure> failure{SidecarFailure::None};

    // The token every request carries: fresh for each start(), or the developer's.
    mutable std::mutex tokenMutex;
    std::string token;

    // Set only when we launched the process ourselves. processMutex guards them: a
    // request thread that loses the connection ends the process (lostConnection).
    std::mutex processMutex;
    HANDLE job = nullptr;          // job with KILL_ON_JOB_CLOSE
    HANDLE process = nullptr;      // child process handle
    DWORD processId = 0;

    explicit Impl(SidecarConfig c) : config(std::move(c)) {}

    ~Impl() {
        if (worker.joinable()) worker.join();
        killLaunched();
    }

    std::string currentToken() const {
        std::lock_guard<std::mutex> lock(tokenMutex);
        return token;
    }

    bool usable() const {
        const SidecarState s = state.load();
        return s == SidecarState::Attached || s == SidecarState::Launched;
    }

    bool fail(SidecarFailure why) {
        failure = why;
        state = SidecarState::Failed;
        return false;
    }

    // What answers GET /health with `token`.
    enum class Probe {
        Healthy,  // the sidecar: our service name and api_version, the token accepted
        Foreign,  // something listens, but not that
        Free      // nothing listens
    };
    Probe probe(const std::string& probeToken, int timeoutMs) const {
        const HttpResponse r = httpRequest(config.host, config.port, "GET", "/health", {},
                                           probeToken, timeoutMs);
        if (!r.ok) {
            return !r.tooLarge && r.error == ERROR_WINHTTP_CANNOT_CONNECT ? Probe::Free
                                                                          : Probe::Foreign;
        }
        if (r.status != 200) return Probe::Foreign;
        try {
            const json j = json::parse(r.body);
            const bool healthy = j.value("status", "") == "ok" &&
                                 j.value("service", "") == SIDECAR_SERVICE_NAME &&
                                 j.value("api_version", -1) == config.expectedApiVersion;
            if (!healthy) return Probe::Foreign;
            std::lock_guard<std::mutex> lock(capabilitiesMutex);
            capabilities = parseCapabilities(r.body);
            return Probe::Healthy;
        } catch (const json::exception&) {
            return Probe::Foreign;
        }
    }

    // A request to a usable sidecar. One that lost the connection ends it.
    HttpResponse post(const char* path, const json& body) {
        // The process we launched has exited: whatever holds the port now is not it,
        // and must not be sent the token.
        if (launchedExited()) {
            HttpResponse gone;
            gone.error = ERROR_WINHTTP_CANNOT_CONNECT;
            lostConnection(gone);
            return gone;
        }
        const HttpResponse r = httpRequest(config.host, config.port, "POST", path, body.dump(),
                                           currentToken(), config.requestTimeoutMs);
        lostConnection(r);
        return r;
    }

    // The sidecar is gone (refused, reset, or any failure, a timeout too, from a
    // launched process that has exited): Failed, and a launched process is ended, so
    // Enable starts it again. A reply, even an HTTP error, keeps it.
    void lostConnection(const HttpResponse& r) {
        if (r.ok || r.tooLarge) return;
        const bool gone = r.error == ERROR_WINHTTP_CANNOT_CONNECT ||
                          r.error == ERROR_WINHTTP_CONNECTION_ERROR || launchedExited();
        if (!gone) return;
        std::lock_guard<std::mutex> lock(processMutex);
        if (!usable()) return;  // stopped or already failed meanwhile
        killLaunchedLocked();
        fail(SidecarFailure::NoResponse);
        Logger::warning("AI sidecar: lost the connection (error " + std::to_string(r.error) +
                        "); AI disabled until it is enabled again");
    }

    // Launch the sidecar in a job object so it cannot outlive us, with the token in
    // its environment. Returns true if the process started (not that it is healthy yet).
    bool launchProcess(const std::string& launchToken) {
        std::lock_guard<std::mutex> lock(processMutex);
        job = CreateJobObjectW(nullptr, nullptr);
        if (!job) return false;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                     sizeof(limits))) {
            // Without kill-on-close a crash would leave the sidecar running.
            Logger::warning("AI sidecar: job setup failed (error " +
                            std::to_string(GetLastError()) + ")");
            CloseHandle(job);
            job = nullptr;
            return false;
        }

        // "<python>" "<script>" --host <host> --port <port>, the interpreter also named
        // in full as the application, so nothing is looked up on a search path.
        const std::wstring python = toWide(config.pythonExe);
        const std::wstring script = toWide(config.scriptPath);
        std::wstring cmd = L"\"" + python + L"\" \"" + script + L"\" --host " +
                           toWide(config.host) + L" --port " + std::to_wstring(config.port);

        // The token by environment, never the command line (other programs read that).
        std::vector<std::pair<std::wstring, std::wstring>> vars{
            {toWide(SIDECAR_TOKEN_ENV), toWide(launchToken)}};
        if (!config.modelsDir.empty() && environmentVariable("QM_MODELS_DIR").empty()) {
            vars.emplace_back(L"QM_MODELS_DIR", toWide(config.modelsDir));
        }
        // The engine runs on its own packages only: the user's Python settings could
        // put other code on its import path or run a start-up script.
        vars.emplace_back(L"PYTHONNOUSERSITE", L"1");
        std::wstring environment = childEnvironment(
            vars, {L"PYTHONPATH", L"PYTHONHOME", L"PYTHONSTARTUP", L"PYTHONUSERBASE"});
        // It starts in its own folder, not wherever the app was started from.
        const std::wstring workDir = std::filesystem::path(script).parent_path().wstring();

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        // Suspended so we can assign to the job before any work runs.
        BOOL created = CreateProcessW(
            python.c_str(), cmd.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
            environment.data(), workDir.c_str(), &si, &pi);
        if (!created) {
            CloseHandle(job);
            job = nullptr;
            return false;
        }

        // Outside the job it could outlive us and the listener check could not vouch
        // for it: it is ended before it ran a single instruction.
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(job);
            job = nullptr;
            return false;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        process = pi.hProcess;
        processId = pi.dwProcessId;
        return true;
    }

    bool launchedExited() {
        std::lock_guard<std::mutex> lock(processMutex);
        return process != nullptr && WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    }

    // The listener on the port is the process we launched (or its child: a venv's
    // python.exe starts the real interpreter), not one that took the port first.
    bool listenerIsOurs() {
        std::lock_guard<std::mutex> lock(processMutex);
        return job != nullptr && listenersInJob(config.port, job);
    }

    void killLaunched() {
        std::lock_guard<std::mutex> lock(processMutex);
        killLaunchedLocked();
    }

    void killLaunchedLocked() {
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

namespace {

bool installed(const SidecarConfig& config) {
    return !config.pythonExe.empty() && !config.scriptPath.empty() &&
           std::filesystem::path(config.pythonExe).is_absolute() &&
           std::filesystem::path(config.scriptPath).is_absolute();
}

// A Gray-code endpoint's reply: a 200 parsed with `parse`; a 404 or a 400's detail is
// reported, never thrown.
template <typename T, typename Parse>
GrayReply<T> grayReply(const HttpResponse& r, Parse parse) {
    GrayReply<T> reply;
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

// ---- public surface --------------------------------------------------------

SidecarClient::SidecarClient(SidecarConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

SidecarClient::~SidecarClient() = default;

bool SidecarClient::start() {
    auto& im = *impl_;
    if (im.usable()) return true;  // already running: a new token would lock us out
    im.failure = SidecarFailure::None;

    const std::string devToken = developerToken();
    std::string token = devToken;
    if (token.empty()) {
        const std::optional<std::string> fresh = randomToken();
        if (!fresh) {
            Logger::warning("AI sidecar: no random source for the token; AI disabled");
            return im.fail(SidecarFailure::NoResponse);
        }
        token = *fresh;
    }
    {
        std::lock_guard<std::mutex> lock(im.tokenMutex);
        im.token = token;
    }
    const std::string where = im.config.host + ":" + std::to_string(im.config.port);

    // 1) Developers only: a sidecar started by hand with their QM_SIDECAR_TOKEN.
    std::optional<Impl::Probe> first;
    if (!devToken.empty()) {
        first = im.probe(token, 1000);
        if (*first == Impl::Probe::Healthy) {
            im.state = SidecarState::Attached;
            Logger::info("AI sidecar: attached to the instance on " + where + " (" +
                         SIDECAR_TOKEN_ENV + " is set)");
            return true;
        }
    }

    // 2) Otherwise launch one, onto a free port only.
    if (!installed(im.config)) {
        Logger::warning("AI sidecar: no interpreter or script found next to the executable; "
                        "AI disabled");
        return im.fail(SidecarFailure::NotInstalled);
    }
    if (!first) first = im.probe(token, 1000);
    if (*first != Impl::Probe::Free) {
        Logger::warning("AI sidecar: " + where + " is in use by a program the app did not "
                        "launch; AI disabled");
        return im.fail(SidecarFailure::PortInUse);
    }
    if (!im.launchProcess(token)) {
        Logger::warning("AI sidecar: failed to launch " + im.config.pythonExe +
                        " (error " + std::to_string(GetLastError()) + "); AI disabled");
        return im.fail(SidecarFailure::NoResponse);
    }

    // 3) Poll for readiness within the startup budget.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(im.config.startupTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (im.probe(token, 500) == Impl::Probe::Healthy) {
            if (!im.listenerIsOurs()) {
                Logger::warning("AI sidecar: " + where + " is answered by a process the app "
                                "did not launch; AI disabled");
                im.killLaunched();
                return im.fail(SidecarFailure::PortInUse);
            }
            im.state = SidecarState::Launched;
            Logger::info("AI sidecar: launched (pid " +
                               std::to_string(im.processId) + ")");
            return true;
        }
        if (im.launchedExited()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    Logger::warning("AI sidecar: launched process did not become healthy; "
                          "AI disabled");
    im.killLaunched();
    return im.fail(SidecarFailure::NoResponse);
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
    return impl_->probe(impl_->currentToken(), 1000) == Impl::Probe::Healthy;
}

SidecarState SidecarClient::state() const {
    return impl_->state.load();
}

SidecarFailure SidecarClient::failure() const {
    return impl_->failure.load();
}

int SidecarClient::port() const {
    return impl_->config.port;
}

std::vector<std::string> SidecarClient::capabilities() const {
    if (!impl_->usable()) return {};
    std::lock_guard<std::mutex> lock(impl_->capabilitiesMutex);
    return impl_->capabilities;
}

SegmentReply SidecarClient::requestSegment(const std::string& imagePath,
                                           const std::array<float, 2>& point) {
    auto& im = *impl_;
    if (!im.usable()) return {};

    json req = {{"image_path", imagePath}, {"point", {point[0], point[1]}}};
    const HttpResponse r = im.post("/segment", req);
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

GrayReply<GrayPatternsResult> SidecarClient::requestGrayPatterns(const std::string& folder,
                                                                 int width, int height) {
    auto& im = *impl_;
    if (!im.usable()) return {};
    const int version = im.config.expectedApiVersion;
    return grayReply<GrayPatternsResult>(
        im.post("/graycode/patterns", json{{"folder", folder}, {"projector", {width, height}}}),
        [version](const std::string& body) { return parseGrayPatternsResponse(body, version); });
}

GrayReply<GrayCodeResult> SidecarClient::requestGrayCode(const std::string& folder, int width,
                                                         int height) {
    auto& im = *impl_;
    if (!im.usable()) return {};
    const int version = im.config.expectedApiVersion;
    return grayReply<GrayCodeResult>(
        im.post("/graycode", json{{"folder", folder}, {"projector", {width, height}}}),
        [version](const std::string& body) { return parseGrayCodeResponse(body, version); });
}

GrayReply<GrayMapResult> SidecarClient::requestGrayMap(
    const std::string& mapPath, const std::vector<std::array<float, 2>>& points) {
    auto& im = *impl_;
    if (points.empty() || points.size() > MAX_GRAYMAP_POINTS || !im.usable()) return {};
    json list = json::array();
    for (const auto& p : points) list.push_back({p[0], p[1]});
    const int version = im.config.expectedApiVersion;
    const size_t count = points.size();
    return grayReply<GrayMapResult>(
        im.post("/graymap", json{{"map_path", mapPath}, {"points", list}}),
        [version, count](const std::string& body) {
            return parseGrayMapResponse(body, version, count);
        });
}

std::optional<SurfaceResult> SidecarClient::requestSurface(
    const std::string& imagePath) {
    auto& im = *impl_;
    if (!im.usable()) return std::nullopt;

    json req = {{"image_path", imagePath}};
    const HttpResponse r = im.post("/surface", req);
    if (!r.ok || r.status != 200) return std::nullopt;
    return parseCornersResponse(r.body, im.config.expectedApiVersion);
}

std::optional<SurfaceResult> SidecarClient::requestOneFlash(
    const std::string& patternImagePath,
    const std::array<std::array<float, 2>, 4>* targetCorners) {
    auto& im = *impl_;
    if (!im.usable()) return std::nullopt;

    json req = {{"pattern_image", patternImagePath}};
    if (targetCorners != nullptr) {
        req["target_corners"] = json::array();
        for (const auto& c : *targetCorners) {
            req["target_corners"].push_back({c[0], c[1]});
        }
    }
    const HttpResponse r = im.post("/oneflash", req);
    if (!r.ok || r.status != 200) return std::nullopt;
    return parseCornersResponse(r.body, im.config.expectedApiVersion);
}

}  // namespace quadmapper::ai
