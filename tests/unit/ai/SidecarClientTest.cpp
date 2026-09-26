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

#include <process.h>  // _getpid

#include <chrono>
#include <string>
#include <thread>

#ifndef QM_SIDECAR_SCRIPT
#define QM_SIDECAR_SCRIPT ""
#endif

using quadmapper::ai::SidecarClient;
using quadmapper::ai::SidecarConfig;
using quadmapper::ai::SidecarState;

namespace {

// A port unlikely to collide with a developer's running sidecar (default 8765) or
// another test process's (TD-23): suites running at once, e.g. in several
// worktrees, each launch a live sidecar, and on one fixed port a client could
// attach to, or still see, another process's sidecar.
int liveTestPort() {
    return 40000 + (_getpid() % 9000);
}

SidecarConfig liveConfig() {
    SidecarConfig cfg;
    cfg.scriptPath = QM_SIDECAR_SCRIPT;
    cfg.port = liveTestPort();
    cfg.startupTimeoutMs = 10000;  // first Python start can be slow on Windows
    return cfg;
}

}  // namespace

// --- Graceful absence (always runs, no Python needed) -----------------------

TEST(SidecarClientTest, NoScriptAndNoServerFailsCleanly) {
    SidecarConfig cfg;
    cfg.scriptPath.clear();   // nothing to launch
    cfg.port = 8793;          // nothing listening
    SidecarClient client(cfg);

    EXPECT_FALSE(client.start());
    EXPECT_EQ(client.state(), SidecarState::Failed);
    EXPECT_FALSE(client.isHealthy());
}

TEST(SidecarClientTest, RequestBeforeStartReturnsNullopt) {
    SidecarConfig cfg;
    cfg.port = 8794;
    SidecarClient client(cfg);

    // No start() -> Stopped -> AI calls are best-effort no-ops.
    EXPECT_FALSE(client.requestSurface("anything.png").has_value());
}

TEST(SidecarClientTest, BogusPythonExeFailsButDoesNotThrow) {
    SidecarConfig cfg = liveConfig();
    cfg.pythonExe = "definitely-not-a-real-python-exe-qm056";
    cfg.startupTimeoutMs = 1500;
    // Its own port (TD-23): on a live round-trip's port, a real sidecar answering
    // /health under ctest -j let start() attach to it.
    cfg.port = 8792;
    SidecarClient client(cfg);

    EXPECT_FALSE(client.start());
    EXPECT_NE(client.state(), SidecarState::Launched);
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

    client.stop();
    EXPECT_EQ(client.state(), SidecarState::Stopped);
    // Process killed -> no longer reachable.
    EXPECT_FALSE(client.isHealthy());
}
