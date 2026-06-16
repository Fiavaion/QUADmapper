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

#include <string>

#ifndef QM_SIDECAR_SCRIPT
#define QM_SIDECAR_SCRIPT ""
#endif

using quadmapper::ai::SidecarClient;
using quadmapper::ai::SidecarConfig;
using quadmapper::ai::SidecarState;

namespace {

// A port unlikely to collide with a developer's running sidecar (default 8765).
constexpr int kTestPort = 8791;

SidecarConfig liveConfig() {
    SidecarConfig cfg;
    cfg.scriptPath = QM_SIDECAR_SCRIPT;
    cfg.port = kTestPort;
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
    SidecarClient client(cfg);

    EXPECT_FALSE(client.start());
    EXPECT_NE(client.state(), SidecarState::Launched);
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
