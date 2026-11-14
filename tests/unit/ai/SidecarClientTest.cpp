// Contract test for the AI sidecar client (QM-056 AC3, app side).
//
// Two halves:
//  * Graceful-absence cases run everywhere and are deterministic — they prove the
//    app degrades cleanly when no sidecar is reachable (the ADR-035 guarantee).
//  * The live round-trip launches the real Python sidecar and is skipped when
//    Python/the script is unavailable (mirrors the QM-055 skip-if-absent webcam
//    test). The sidecar skeleton is stdlib-only, so this stays GPU-free.

#include "ai/SidecarClient.hpp"

#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>  // _getpid

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

#ifndef QM_SIDECAR_SCRIPT
#define QM_SIDECAR_SCRIPT ""
#endif

using quadmapper::ai::SidecarClient;
using quadmapper::ai::SidecarConfig;
using quadmapper::ai::SidecarFailure;
using quadmapper::ai::SidecarState;

namespace {

// A port unlikely to collide with a developer's running sidecar (default 8765) or
// another test process's (TD-23): suites running at once, e.g. in several
// worktrees, each launch a live sidecar, and on one fixed port a client could
// attach to, or still see, another process's sidecar.
int liveTestPort() {
    return 40000 + (_getpid() % 9000);
}

// The live tests run the sidecar's own environment (setup.ps1) when it exists beside
// the script; without it the client launches nothing and they skip.
SidecarConfig liveConfig() {
    SidecarConfig cfg;
    cfg.scriptPath = QM_SIDECAR_SCRIPT;
    const std::filesystem::path venvPython =
        std::filesystem::path(cfg.scriptPath).parent_path() / ".venv" / "Scripts" / "python.exe";
    std::error_code ec;
    if (!cfg.scriptPath.empty() && std::filesystem::exists(venvPython, ec)) {
        cfg.pythonExe = venvPython.string();
    }
    cfg.port = liveTestPort();
    cfg.startupTimeoutMs = 10000;  // first Python start can be slow on Windows
    return cfg;
}

// Sets (or, with "", removes) an environment variable for one test and puts back what
// was there.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        char* old = nullptr;
        size_t length = 0;
        if (_dupenv_s(&old, &length, name) == 0 && old != nullptr) {
            had_ = true;
            old_ = old;
            free(old);
        }
        _putenv_s(name, value);
    }
    ~ScopedEnv() { _putenv_s(name_, had_ ? old_.c_str() : ""); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    bool had_ = false;
    std::string old_;
};

// Paths that pass the client's "installed" check without anything there to run.
SidecarConfig configFor(int port) {
    SidecarConfig cfg;
    cfg.port = port;
    cfg.pythonExe = "C:/qm-test/no/such/python.exe";
    cfg.scriptPath = "C:/qm-test/no/such/sidecar.py";
    cfg.startupTimeoutMs = 1000;
    cfg.requestTimeoutMs = 5000;
    return cfg;
}

std::string httpReply(int status, const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) +
           " X\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

const std::string kHealth = R"({"status": "ok", "api_version": 1,
    "service": "quadmapper-ai-sidecar", "capabilities": ["segment"]})";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The value of header `name` in a raw request, or "(none)".
std::string header(const std::string& request, const std::string& name) {
    const std::string wanted = "\r\n" + lower(name) + ":";
    const size_t at = lower(request).find(wanted);
    if (at == std::string::npos) return "(none)";
    const size_t start = request.find_first_not_of(' ', at + wanted.size());
    return request.substr(start, request.find("\r\n", start) - start);
}

// A loopback HTTP server in this process: each connection's request goes to `respond`,
// whose answer is sent back; an empty answer resets the connection instead.
class FakeServer {
public:
    using Respond = std::function<std::string(const std::string& request)>;

    explicit FakeServer(Respond respond) : respond_(std::move(respond)) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(listen_, 8);
        int length = sizeof(addr);
        getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &length);
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this, listening = listen_] { serve(listening); });
    }
    ~FakeServer() {
        close();
        WSACleanup();
    }
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;

    // Stop listening: the port then refuses connections.
    void close() {
        if (listen_ != INVALID_SOCKET) closesocket(listen_);
        listen_ = INVALID_SOCKET;
        if (thread_.joinable()) thread_.join();
    }
    int port() const { return port_; }
    std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void serve(SOCKET listening) {
        for (;;) {
            SOCKET client = accept(listening, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            DWORD timeout = 5000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                       sizeof(timeout));
            const std::string request = readRequest(client);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(request);
            }
            const std::string reply = respond_(request);
            if (reply.empty()) {
                linger abort{1, 0};  // RST, as a process that died mid-request
                setsockopt(client, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&abort),
                           sizeof(abort));
            } else {
                for (size_t sent = 0; sent < reply.size();) {
                    const int n = send(client, reply.data() + sent,
                                       static_cast<int>(std::min<size_t>(reply.size() - sent, 65536)), 0);
                    if (n <= 0) break;  // the client stopped reading
                    sent += static_cast<size_t>(n);
                }
                shutdown(client, SD_SEND);
            }
            closesocket(client);
        }
    }

    static std::string readRequest(SOCKET client) {
        std::string data;
        char buffer[4096];
        size_t headerEnd = std::string::npos;
        while ((headerEnd = data.find("\r\n\r\n")) == std::string::npos) {
            const int n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) return data;
            data.append(buffer, static_cast<size_t>(n));
        }
        const std::string length = header(data, "Content-Length");
        const size_t bodyLength = length == "(none)" ? 0 : std::stoul(length);
        while (data.size() < headerEnd + 4 + bodyLength) {
            const int n = recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            data.append(buffer, static_cast<size_t>(n));
        }
        return data;
    }

    Respond respond_;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
};

// Answers /health as the sidecar does and every POST with `post`.
FakeServer::Respond sidecarLike(std::function<std::string()> post) {
    return [post](const std::string& request) {
        return request.rfind("GET /health ", 0) == 0 ? httpReply(200, kHealth) : post();
    };
}

const char* kDevToken = "dev-token_0123";

}  // namespace

// --- Graceful absence (always runs, no Python needed) -----------------------

TEST(SidecarClientTest, NoScriptAndNoServerFailsCleanly) {
    ScopedEnv noToken(quadmapper::ai::SIDECAR_TOKEN_ENV, "");
    SidecarConfig cfg;
    cfg.scriptPath.clear();   // nothing to launch
    cfg.port = 8793;          // nothing listening
    SidecarClient client(cfg);

    EXPECT_FALSE(client.start());
    EXPECT_EQ(client.state(), SidecarState::Failed);
    EXPECT_EQ(client.failure(), SidecarFailure::NotInstalled);
    EXPECT_FALSE(client.isHealthy());
}

// Never a bare "python" (Windows looks in the current folder first): an interpreter
// that is not an absolute path is not installed, and nothing is launched.
TEST(SidecarClientTest, ARelativeInterpreterIsNotInstalledAndNothingIsLaunched) {
    ScopedEnv noToken(quadmapper::ai::SIDECAR_TOKEN_ENV, "");
    for (const char* python : {"python", "python.exe", ".venv/Scripts/python.exe", ""}) {
        SidecarConfig cfg;
        cfg.pythonExe = python;
        cfg.scriptPath = "C:/qm-test/sidecar.py";
        cfg.port = 8793;
        SidecarClient client(cfg);
        EXPECT_FALSE(client.start()) << python;
        EXPECT_EQ(client.failure(), SidecarFailure::NotInstalled) << python;
    }
    SidecarConfig cfg;
    cfg.pythonExe = "C:/qm-test/python.exe";
    cfg.scriptPath = "sidecar.py";
    cfg.port = 8793;
    SidecarClient client(cfg);
    EXPECT_FALSE(client.start());
    EXPECT_EQ(client.failure(), SidecarFailure::NotInstalled);
}

// --- Who the client talks to (ADR-035, amendment of 2026-11-14) -------------------

// Without the developer's token the client never attaches, even to a perfect sidecar
// reply: the port is in use, and nothing but /health was sent to it.
TEST(SidecarClientTest, AListenerTheAppDidNotLaunchIsNeverAttached) {
    ScopedEnv noToken(quadmapper::ai::SIDECAR_TOKEN_ENV, "");
    FakeServer server(sidecarLike([] { return httpReply(200, "{}"); }));
    SidecarClient client(configFor(server.port()));

    EXPECT_FALSE(client.start());
    EXPECT_EQ(client.state(), SidecarState::Failed);
    EXPECT_EQ(client.failure(), SidecarFailure::PortInUse);
    EXPECT_EQ(client.port(), server.port());
    EXPECT_FALSE(client.requestSurface("x.png").has_value());

    // Each start makes a fresh 128-bit token, sent as hex.
    EXPECT_FALSE(client.start());
    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    for (const std::string& request : requests) {
        EXPECT_EQ(request.rfind("GET /health ", 0), 0u);
        const std::string token = header(request, "X-QM-Token");
        EXPECT_EQ(token.size(), 32u);
        EXPECT_TRUE(std::all_of(token.begin(), token.end(),
                                [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }));
    }
    EXPECT_NE(header(requests[0], "X-QM-Token"), header(requests[1], "X-QM-Token"));
}

// With the developer's token the client attaches, but only to a reply that is the
// sidecar: its service name, the expected api_version and status ok.
TEST(SidecarClientTest, ADeveloperAttachNeedsTheSidecarsOwnHealthReply) {
    ScopedEnv devToken(quadmapper::ai::SIDECAR_TOKEN_ENV, kDevToken);
    for (const std::string& reply :
         {httpReply(200, R"({"status": "ok", "api_version": 1, "service": "something-else"})"),
          httpReply(200, R"({"status": "ok", "api_version": 1})"),
          httpReply(200, R"({"status": "ok", "api_version": 2, "service": "quadmapper-ai-sidecar"})"),
          httpReply(200, R"({"status": "busy", "api_version": 1, "service": "quadmapper-ai-sidecar"})"),
          httpReply(401, R"({"error": "missing or wrong X-QM-Token"})"),
          httpReply(200, "not json")}) {
        FakeServer server([&reply](const std::string&) { return reply; });
        SidecarClient client(configFor(server.port()));
        EXPECT_FALSE(client.start()) << reply;
        EXPECT_EQ(client.failure(), SidecarFailure::PortInUse) << reply;
    }

    FakeServer server(sidecarLike([] { return httpReply(200, "{}"); }));
    SidecarClient client(configFor(server.port()));
    ASSERT_TRUE(client.start());
    EXPECT_EQ(client.state(), SidecarState::Attached);
    EXPECT_EQ(client.capabilities(), std::vector<std::string>{"segment"});
    client.requestSurface("x.png");

    const std::vector<std::string> requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[1].rfind("POST /surface ", 0), 0u);
    for (const std::string& request : requests) {
        EXPECT_EQ(header(request, "X-QM-Token"), kDevToken);
        EXPECT_EQ(header(request, "Origin"), "(none)");
    }
    EXPECT_EQ(header(requests[1], "Content-Type"), "application/json");
}

// A token that could break the header is ignored: the client makes its own, and so
// does not attach.
TEST(SidecarClientTest, ADeveloperTokenThatIsNotPlainIsIgnored) {
    ScopedEnv devToken(quadmapper::ai::SIDECAR_TOKEN_ENV, "abc\r\nOrigin: x");
    FakeServer server(sidecarLike([] { return httpReply(200, "{}"); }));
    SidecarClient client(configFor(server.port()));
    EXPECT_FALSE(client.start());
    EXPECT_EQ(client.failure(), SidecarFailure::PortInUse);
    EXPECT_EQ(header(server.requests().at(0), "X-QM-Token").size(), 32u);
}

TEST(SidecarClientTest, AReplyOverTheCapIsAnErrorThatKeepsTheConnection) {
    ScopedEnv devToken(quadmapper::ai::SIDECAR_TOKEN_ENV, kDevToken);
    FakeServer server(sidecarLike([] {
        return httpReply(200, std::string(quadmapper::ai::MAX_RESPONSE_BYTES + 1, ' '));
    }));
    SidecarClient client(configFor(server.port()));
    ASSERT_TRUE(client.start());
    EXPECT_FALSE(client.requestSurface("x.png").has_value());
    EXPECT_EQ(client.state(), SidecarState::Attached);
}

// An HTTP error answer is the sidecar talking: it stays usable.
TEST(SidecarClientTest, AnHttpErrorKeepsTheConnection) {
    ScopedEnv devToken(quadmapper::ai::SIDECAR_TOKEN_ENV, kDevToken);
    FakeServer server(sidecarLike([] { return httpReply(500, R"({"error": "boom"})"); }));
    SidecarClient client(configFor(server.port()));
    ASSERT_TRUE(client.start());
    EXPECT_FALSE(client.requestSurface("x.png").has_value());
    EXPECT_FALSE(client.requestSegment("x.png", {0.5f, 0.5f}).result.has_value());
    EXPECT_FALSE(client.requestGrayCode("C:/tmp", 64, 32).result.has_value());
    EXPECT_EQ(client.state(), SidecarState::Attached);
    EXPECT_FALSE(client.capabilities().empty());
}

// A sidecar that has gone (the port refuses, or the connection is reset) leaves the
// client Failed, so AI > Enable AI Assist starts it again.
TEST(SidecarClientTest, ALostConnectionFailsSoEnableStartsItAgain) {
    ScopedEnv devToken(quadmapper::ai::SIDECAR_TOKEN_ENV, kDevToken);
    {
        FakeServer server(sidecarLike([] { return std::string(); }));  // reset mid-request
        SidecarClient client(configFor(server.port()));
        ASSERT_TRUE(client.start());
        EXPECT_FALSE(client.requestOneFlash("x.png").has_value());
        EXPECT_EQ(client.state(), SidecarState::Failed);
        EXPECT_EQ(client.failure(), SidecarFailure::NoResponse);
        EXPECT_TRUE(client.capabilities().empty());
        ASSERT_TRUE(client.start());  // what Enable does: it is answering again
        EXPECT_EQ(client.state(), SidecarState::Attached);
    }
    FakeServer server(sidecarLike([] { return httpReply(200, "{}"); }));
    SidecarClient client(configFor(server.port()));
    ASSERT_TRUE(client.start());
    server.close();  // gone: the port refuses
    EXPECT_FALSE(client.requestGrayMap("m.npz", {{0.5f, 0.5f}}).result.has_value());
    EXPECT_EQ(client.state(), SidecarState::Failed);
}

TEST(SidecarClientTest, RequestBeforeStartReturnsNullopt) {
    SidecarConfig cfg;
    cfg.port = 8794;
    SidecarClient client(cfg);

    // No start() -> Stopped -> AI calls are best-effort no-ops.
    EXPECT_FALSE(client.requestSurface("anything.png").has_value());
}

TEST(SidecarClientTest, BogusPythonExeFailsButDoesNotThrow) {
    ScopedEnv noToken(quadmapper::ai::SIDECAR_TOKEN_ENV, "");
    SidecarConfig cfg = liveConfig();
    cfg.pythonExe = "C:/qm-test/definitely-not-a-real-python-exe-qm056.exe";
    cfg.scriptPath = "C:/qm-test/sidecar.py";
    cfg.startupTimeoutMs = 1500;
    // Its own port (TD-23): on a live round-trip's port, a real sidecar answering
    // /health under ctest -j let start() attach to it.
    cfg.port = 8792;
    SidecarClient client(cfg);

    EXPECT_FALSE(client.start());
    EXPECT_NE(client.state(), SidecarState::Launched);
    EXPECT_EQ(client.failure(), SidecarFailure::NoResponse);
    EXPECT_FALSE(client.requestSurface("x.png").has_value());
}

// TD-10: startAsync() runs the connect off the caller's thread and resolves the
// state without blocking. With no script and nothing listening it must settle on
// Failed (and never leak/hang the worker — the destructor joins it).
TEST(SidecarClientTest, StartAsyncResolvesToFailedWithoutBlocking) {
    SidecarConfig cfg;
    cfg.scriptPath.clear();   // nothing to launch
    cfg.port = 8795;          // nothing listening
    cfg.startupTimeoutMs = 1000;
    SidecarClient client(cfg);

    client.startAsync();      // returns immediately; worker does the connect
    // The worker's only wait is one health probe bounded by WinHTTP timeouts; the
    // generous deadline only guards against a hang, so a loaded machine cannot
    // fail the test (TD-23).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (client.state() == SidecarState::Starting &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(client.state(), SidecarState::Failed);
    EXPECT_FALSE(client.requestSurface("x.png").has_value());
}

// --- Live round-trip (skipped when Python/script unavailable) ---------------

TEST(SidecarClientTest, LaunchHealthSurfaceTeardown) {
    if (std::string(QM_SIDECAR_SCRIPT).empty()) {
        GTEST_SKIP() << "sidecar script path not configured";
    }

    SidecarClient client(liveConfig());
    if (!client.start()) {
        GTEST_SKIP() << "Python sidecar could not be launched (Python absent?)";
    }

    EXPECT_EQ(client.state(), SidecarState::Launched);
    EXPECT_TRUE(client.isHealthy());

    auto surface = client.requestSurface("C:/tmp/frame.png");
    ASSERT_TRUE(surface.has_value());
    EXPECT_TRUE(surface->stub);
    EXPECT_GE(surface->confidence, 0.0f);
    // Stub returns a centered rectangle in normalized [0,1] space.
    for (const auto& corner : surface->corners) {
        EXPECT_GE(corner[0], 0.0f);
        EXPECT_LE(corner[0], 1.0f);
        EXPECT_GE(corner[1], 0.0f);
        EXPECT_LE(corner[1], 1.0f);
    }

    // Any sidecar advertises at least what the oldest one could do.
    EXPECT_FALSE(client.capabilities().empty());

    client.stop();
    EXPECT_EQ(client.state(), SidecarState::Stopped);
    // Process killed -> no longer reachable.
    EXPECT_FALSE(client.isHealthy());
    EXPECT_TRUE(client.capabilities().empty());
}

// --- /segment and /health parsing (ADR-053; no Python needed) ---------------

using quadmapper::ai::parseCapabilities;
using quadmapper::ai::parseSegmentResponse;

namespace {
const char* kSegmentReply = R"({"api_version": 1,
    "corners": [[0.1, 0.2], [0.9, 0.1], [0.8, 0.9], [0.2, 0.8]],
    "outline": [[0.1, 0.2], [0.5, 0.15], [0.9, 0.1], [0.8, 0.9]],
    "confidence": 0.82, "planarity": 0.91, "stub": false})";
}  // namespace

TEST(SidecarClientTest, SegmentReplyParses) {
    auto r = parseSegmentResponse(kSegmentReply, 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_FLOAT_EQ(r->corners[1][0], 0.9f);
    EXPECT_FLOAT_EQ(r->corners[3][1], 0.8f);
    ASSERT_EQ(r->outline.size(), 4u);
    EXPECT_FLOAT_EQ(r->outline[1][0], 0.5f);
    EXPECT_FLOAT_EQ(r->confidence, 0.82f);
    ASSERT_TRUE(r->planarity.has_value());
    EXPECT_FLOAT_EQ(*r->planarity, 0.91f);
    EXPECT_FALSE(r->stub);
}

TEST(SidecarClientTest, SegmentReplyWithoutPlanarityOrAsAStub) {
    auto r = parseSegmentResponse(R"({"api_version": 1,
        "corners": [[0,0],[1,0],[1,1],[0,1]], "outline": [], "confidence": 0.5,
        "planarity": null, "stub": true})", 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->planarity.has_value());
    EXPECT_TRUE(r->stub);
    EXPECT_TRUE(r->outline.empty());

    r = parseSegmentResponse(R"({"api_version": 1,
        "corners": [[0,0],[1,0],[1,1],[0,1]], "confidence": 0.5})", 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->planarity.has_value());
    EXPECT_TRUE(r->outline.empty());
    EXPECT_FALSE(r->stub);
}

TEST(SidecarClientTest, SegmentReplyWithABadOutlineOrVersionIsRefused) {
    const std::string corners = R"("corners": [[0,0],[1,0],[1,1],[0,1]], "confidence": 0.5)";
    for (const std::string& outline : {std::string(R"("outline": "nope")"),
                                       std::string(R"("outline": [[0.1]])"),
                                       std::string(R"("outline": [[0.1, "x"]])"),
                                       std::string(R"("outline": [0.1, 0.2])")}) {
        EXPECT_FALSE(parseSegmentResponse("{\"api_version\": 1, " + corners + ", " + outline + "}", 1))
            << outline;
    }
    std::string tooMany = "[";
    for (size_t i = 0; i <= quadmapper::ai::MAX_SEGMENT_OUTLINE_POINTS; ++i) {
        tooMany += (i ? ",[0.5,0.5]" : "[0.5,0.5]");
    }
    tooMany += "]";
    EXPECT_FALSE(parseSegmentResponse("{\"api_version\": 1, " + corners + ", \"outline\": " + tooMany + "}", 1));

    EXPECT_FALSE(parseSegmentResponse(kSegmentReply, 2));  // another API version
    EXPECT_FALSE(parseSegmentResponse(R"({"api_version": 1, "corners": [[0,0],[1,0]]})", 1));
    EXPECT_FALSE(parseSegmentResponse("not json", 1));
}

TEST(SidecarClientTest, CapabilitiesDefaultForAnOldSidecar) {
    EXPECT_EQ(parseCapabilities(R"({"status": "ok", "api_version": 1})"),
              (std::vector<std::string>{"surface", "oneflash"}));
    EXPECT_EQ(parseCapabilities(R"({"status": "ok", "api_version": 1,
                                    "capabilities": ["surface", "oneflash", "segment", "depth"]})"),
              (std::vector<std::string>{"surface", "oneflash", "segment", "depth"}));
    EXPECT_TRUE(parseCapabilities(R"({"capabilities": []})").empty());
    EXPECT_TRUE(parseCapabilities(R"({"capabilities": "segment"})").empty());
    EXPECT_TRUE(parseCapabilities("not json").empty());
}

TEST(SidecarClientTest, SegmentBeforeStartReturnsNothing) {
    SidecarConfig cfg;
    cfg.port = 8796;
    SidecarClient client(cfg);
    const auto reply = client.requestSegment("x.png", {0.5f, 0.5f});
    EXPECT_FALSE(reply.result.has_value());
    EXPECT_FALSE(reply.endpointMissing);
    EXPECT_TRUE(client.capabilities().empty());
}

// --- Gray-code scan (ADR-053 §3) ---------------------------------------------

using quadmapper::ai::parseGrayCodeResponse;
using quadmapper::ai::parseGrayMapResponse;
using quadmapper::ai::parseGrayPatternsResponse;

TEST(SidecarClientTest, GrayPatternsReplyParses) {
    auto r = parseGrayPatternsResponse(R"({"api_version": 1, "count": 46, "stub": false})", 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->count, 46);
    EXPECT_FALSE(r->stub);

    r = parseGrayPatternsResponse(R"({"api_version": 1, "count": 0, "stub": true})", 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->stub);

    EXPECT_FALSE(parseGrayPatternsResponse(R"({"api_version": 2, "count": 46})", 1));
    EXPECT_FALSE(parseGrayPatternsResponse(R"({"api_version": 1, "count": -1})", 1));
    EXPECT_FALSE(parseGrayPatternsResponse(R"({"api_version": 1, "count": 2.5})", 1));
    EXPECT_FALSE(parseGrayPatternsResponse(R"({"api_version": 1, "count": 2, "stub": false})", 1))
        << "fewer than one pattern plus white and black";
    EXPECT_FALSE(parseGrayPatternsResponse(R"({"api_version": 1})", 1));
}

TEST(SidecarClientTest, GrayCodeReplyParses) {
    auto r = parseGrayCodeResponse(R"({"api_version": 1, "map_path": "C:/s/graymap.npz",
        "coverage": 0.93, "decode_error_px": 0.21, "stub": false})", 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mapPath, "C:/s/graymap.npz");
    EXPECT_FLOAT_EQ(r->coverage, 0.93f);
    ASSERT_TRUE(r->decodeErrorPx.has_value());
    EXPECT_FLOAT_EQ(*r->decodeErrorPx, 0.21f);

    r = parseGrayCodeResponse(R"({"api_version": 1, "map_path": "m.npz", "coverage": 0.0,
        "decode_error_px": null, "stub": false})", 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->decodeErrorPx.has_value());

    r = parseGrayCodeResponse(R"({"api_version": 1, "map_path": null, "coverage": 0.0,
        "decode_error_px": null, "stub": true})", 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->stub);
    EXPECT_TRUE(r->mapPath.empty());

    EXPECT_FALSE(parseGrayCodeResponse(R"({"api_version": 1, "map_path": "", "coverage": 0.5})", 1));
    EXPECT_FALSE(parseGrayCodeResponse(R"({"api_version": 1, "map_path": "m.npz"})", 1));
    EXPECT_FALSE(parseGrayCodeResponse(R"({"api_version": 3, "map_path": "m.npz", "coverage": 1})", 1));
}

TEST(SidecarClientTest, GrayMapReplyParsesOnlyWithOneAnswerPerPoint) {
    const char* body = R"({"api_version": 1, "points": [[0.25, 0.5], [-1, -1]], "valid": [true, false]})";
    auto r = parseGrayMapResponse(body, 1, 2);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->points.size(), 2u);
    EXPECT_FLOAT_EQ(r->points[0][0], 0.25f);
    EXPECT_EQ(r->valid, (std::vector<bool>{true, false}));

    EXPECT_FALSE(parseGrayMapResponse(body, 1, 3));
    EXPECT_FALSE(parseGrayMapResponse(R"({"api_version": 1, "points": [[0.5, 0.5]], "valid": [1]})", 1, 1));
    EXPECT_FALSE(parseGrayMapResponse(R"({"api_version": 1, "points": [[0.5]], "valid": [true]})", 1, 1));
    EXPECT_FALSE(parseGrayMapResponse(R"({"api_version": 1, "points": [[0.5, 0.5]]})", 1, 1));
}

TEST(SidecarClientTest, GrayRequestsBeforeStartReturnNothing) {
    SidecarConfig cfg;
    cfg.port = 8796;
    SidecarClient client(cfg);
    EXPECT_FALSE(client.requestGrayPatterns("C:/tmp", 64, 32).result.has_value());
    EXPECT_FALSE(client.requestGrayCode("C:/tmp", 64, 32).result.has_value());
    const auto map = client.requestGrayMap("m.npz", {{0.5f, 0.5f}});
    EXPECT_FALSE(map.result.has_value());
    EXPECT_FALSE(map.endpointMissing);
    EXPECT_TRUE(map.error.empty());
}

// The 400s carry their detail to the app; a sidecar with OpenCV contrib writes patterns.
TEST(SidecarClientTest, LiveGrayCodeRequestsReportBadRequestsAndWritePatterns) {
    if (std::string(QM_SIDECAR_SCRIPT).empty()) {
        GTEST_SKIP() << "sidecar script path not configured";
    }
    SidecarClient client(liveConfig());
    if (!client.start()) {
        GTEST_SKIP() << "Python sidecar could not be launched (Python absent?)";
    }
    const auto missing = client.requestGrayCode("Z:/no/such/qm/folder", 64, 32);
    EXPECT_FALSE(missing.result.has_value());
    EXPECT_EQ(missing.error, "folder does not exist");
    const auto badMap = client.requestGrayMap("Z:/no/such/graymap.npz", {{0.5f, 0.5f}});
    EXPECT_EQ(badMap.error, "map_path does not exist");

    const auto caps = client.capabilities();
    if (std::find(caps.begin(), caps.end(), "graycode") != caps.end()) {
        namespace fs = std::filesystem;
        const fs::path folder =
            fs::temp_directory_path() / ("qm_graycode_" + std::to_string(_getpid()));
        fs::create_directories(folder);
        const auto patterns = client.requestGrayPatterns(folder.string(), 64, 32);
        ASSERT_TRUE(patterns.result.has_value());
        EXPECT_FALSE(patterns.result->stub);
        EXPECT_GT(patterns.result->count, 2);
        EXPECT_TRUE(fs::exists(folder / "pattern_000.png"));
        std::error_code ec;
        fs::remove_all(folder, ec);
    }
    client.stop();
}
