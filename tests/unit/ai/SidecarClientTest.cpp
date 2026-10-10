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

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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
