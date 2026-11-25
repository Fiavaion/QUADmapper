#include <gtest/gtest.h>
#include "scene/ObjLoader.hpp"
#include "core/FileSystem.hpp"
#include "core/UUID.hpp"

#include <glm/glm.hpp>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace quadmapper {
namespace test {

namespace fs = std::filesystem;

class ObjLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("qm_obj_" + UUID::generate().toString());
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    fs::path write(const std::string& text) {
        const fs::path file = dir_ / "mesh.obj";
        std::ofstream(file, std::ios::binary) << text;
        return file;
    }

    fs::path dir_;
};

TEST_F(ObjLoaderTest, SingleQuadIsFanTriangulatedWithComputedNormals) {
    auto result = loadObj(write(
        "# a unit quad\n"
        "o Quad\n"
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "s off\n"
        "f 1 2 3 4\n"));

    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const MeshData& mesh = result.value();
    EXPECT_EQ(mesh.positions.size(), 4u);
    EXPECT_EQ(mesh.indices, (std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
    EXPECT_TRUE(mesh.uvs.empty());  // none in the file: the renderer falls back
    ASSERT_EQ(mesh.normals.size(), 4u);
    for (const Vec3& n : mesh.normals) {
        EXPECT_NEAR(n.z, 1.0f, 1e-6f);  // counter-clockwise in the XY plane faces +Z
    }
}

TEST_F(ObjLoaderTest, CubeWithUvsAndNormalsDeduplicatesByTriple) {
    std::string obj = "mtllib cube.mtl\nusemtl Mat\n";
    for (int i = 0; i < 8; ++i) {
        obj += "v " + std::to_string(i & 1) + " " + std::to_string((i >> 1) & 1) + " " +
               std::to_string((i >> 2) & 1) + "\n";
    }
    obj += "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n";
    obj += "vn 0 0 -1\nvn 0 0 1\nvn 0 -1 0\nvn 0 1 0\nvn -1 0 0\nvn 1 0 0\n";
    // Six faces, each with its own normal: 24 distinct (v, vt, vn) corners.
    obj += "f 1/1/1 3/4/1 4/3/1 2/2/1\n"
           "f 5/1/2 6/2/2 8/3/2 7/4/2\n"
           "f 1/1/3 2/2/3 6/3/3 5/4/3\n"
           "f 3/1/4 7/2/4 8/3/4 4/4/4\n"
           "f 1/1/5 5/2/5 7/3/5 3/4/5\n"
           "f 2/1/6 4/2/6 8/3/6 6/4/6\n";

    auto result = loadObj(write(obj));

    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const MeshData& mesh = result.value();
    EXPECT_EQ(mesh.positions.size(), 24u);
    EXPECT_EQ(mesh.uvs.size(), 24u);
    EXPECT_EQ(mesh.normals.size(), 24u);
    EXPECT_EQ(mesh.triangleCount(), 12u);
    EXPECT_EQ(mesh.normals[0], Vec3(0, 0, -1));  // file normals kept as given
    EXPECT_EQ(mesh.uvs[1], Vec2(0, 0));  // vt 0 1, v flipped
}

TEST_F(ObjLoaderTest, TextureCoordinateVIsFlippedToTopRowFirst) {
    // OBJ puts v = 0 at the image bottom, but every frame (still, video, camera) is
    // stored top row first with v = 0 at the image top, so the loader stores 1 - v.
    // Keeping v as authored left video upside down on objects (GUI QA, 2026-09-26).
    auto result = loadObj(write(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "f 1/1 2/2 3/3\n"));

    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const MeshData& mesh = result.value();
    ASSERT_EQ(mesh.uvs.size(), 3u);
    EXPECT_EQ(mesh.uvs[0], Vec2(0, 1));
    EXPECT_EQ(mesh.uvs[1], Vec2(1, 1));
    EXPECT_EQ(mesh.uvs[2], Vec2(0, 0));
}

TEST_F(ObjLoaderTest, SharedCornersAreReused) {
    auto result = loadObj(write(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "f 1 2 3\nf 1 3 4\n"));

    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    EXPECT_EQ(result.value().positions.size(), 4u);
    EXPECT_EQ(result.value().triangleCount(), 2u);
}

TEST_F(ObjLoaderTest, NegativeIndicesCountBackFromTheLatestElement) {
    auto result = loadObj(write(
        "v 9 9 9\n"  // not used by the face
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "f -3/-3 -2/-2 -1/-1\n"));

    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const MeshData& mesh = result.value();
    ASSERT_EQ(mesh.positions.size(), 3u);
    EXPECT_EQ(mesh.positions[0], Vec3(0, 0, 0));
    EXPECT_EQ(mesh.positions[2], Vec3(0, 1, 0));
    EXPECT_EQ(mesh.uvs[1], Vec2(1, 1));  // vt 1 0, v flipped
}

TEST_F(ObjLoaderTest, MixedFaceFormatsLoad) {
    auto result = loadObj(write(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 1\n"
        "vn 0 0 1\n"
        "f 1 2/1 3//1 4/2/1\n"));

    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const MeshData& mesh = result.value();
    EXPECT_EQ(mesh.triangleCount(), 2u);
    ASSERT_EQ(mesh.uvs.size(), 4u);  // some corners have uvs: missing ones are (0, 0)
    EXPECT_EQ(mesh.uvs[3], Vec2(1, 0));  // vt 1 1, v flipped
    ASSERT_EQ(mesh.normals.size(), 4u);  // not every corner has one: all computed
    EXPECT_NEAR(mesh.normals[0].z, 1.0f, 1e-6f);
}

TEST_F(ObjLoaderTest, MalformedInputReturnsErr) {
    const char* bad[] = {
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4\n",     // index past the end
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 0\n",     // zero index
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 -4\n",    // relative index before the start
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/5 2 3\n",   // missing uv
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 x\n",     // non-numeric index
        "v 0 zero 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",  // non-numeric coordinate
        "v 0 0\n",                                  // short vertex
        "v 0 0 0\nv 1 0 0\nf 1 2\n",                // face with two vertices
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1/1/1 2 3\n",  // too many slashes
    };
    for (const char* text : bad) {
        auto result = loadObj(write(text));
        EXPECT_TRUE(result.isError()) << text;
    }

    auto result = loadObj(write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 4\n"));
    ASSERT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("line 4"), std::string::npos) << result.error().message;
}

// A line is read into a fixed buffer: one that does not fit fails the import with its
// line number, and one just inside the limit (here a comment) is read as before.
TEST_F(ObjLoaderTest, ALineTooLongToBeRealFailsTheImport) {
    const std::string triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

    auto fits = loadObj(write("# " + std::string(1000 * 1000, 'x') + "\n" + triangle));
    ASSERT_TRUE(fits.isSuccess()) << fits.error().message;
    EXPECT_EQ(fits.value().indices.size(), 3u);

    auto tooLong = loadObj(write(triangle + "# " + std::string(3 * 1024 * 1024, 'x') + "\n"));
    ASSERT_TRUE(tooLong.isError());
    EXPECT_NE(tooLong.error().message.find("line 5"), std::string::npos) << tooLong.error().message;
    EXPECT_NE(tooLong.error().message.find("too long"), std::string::npos) << tooLong.error().message;

    // The last line needs no line break.
    auto unterminated = loadObj(write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3"));
    EXPECT_TRUE(unterminated.isSuccess());
}

TEST_F(ObjLoaderTest, ACoordinateTooLargeForAFloatToPlaceFailsTheImport) {
    auto result = loadObj(write("v 0 0 0\nv 1e10 0 0\nv 0 1 0\nf 1 2 3\n"));
    ASSERT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("line 2"), std::string::npos) << result.error().message;

    EXPECT_TRUE(loadObj(write("v 0 0 0\nv -9e8 0 0\nv 0 1 0\nf 1 2 3\n")).isSuccess());
}

#ifdef __APPLE__
// macOS reads numbers with strtof_l, not std::from_chars: the forms from_chars refuses
// are refused there too.
TEST_F(ObjLoaderTest, NumbersOnlyStrtofWouldTakeAreRefused) {
    for (const char* number : {"+1", "0x10", "inf", "nan", "1e-50", "-1e-50"}) {
        const std::string text = std::string("v 0 0 0\nv ") + number + " 0 0\nv 0 1 0\nf 1 2 3\n";
        EXPECT_TRUE(loadObj(write(text)).isError()) << number;
    }
    // A number a float holds only as a subnormal still loads.
    EXPECT_TRUE(loadObj(write("v 0 0 0\nv 1e-40 0 0\nv 0 1 0\nf 1 2 3\n")).isSuccess());
}
#endif

TEST_F(ObjLoaderTest, EmptyOrFacelessFileReturnsErr) {
    EXPECT_TRUE(loadObj(write("")).isError());
    EXPECT_TRUE(loadObj(write("# only a comment\nv 0 0 0\n")).isError());
}

TEST_F(ObjLoaderTest, MissingFileReturnsErr) {
    EXPECT_TRUE(loadObj(dir_ / "absent.obj").isError());
}

TEST_F(ObjLoaderTest, LoadsFromNonAsciiFolder) {
    const fs::path folder = dir_ / pathFromUtf8(u8"\u00fc \u65e5\u672c \u0416");
    fs::create_directories(folder);
    const fs::path file = folder / pathFromUtf8(u8"\u0416.obj");
    std::ofstream(file, std::ios::binary) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

    auto result = loadObj(file);
    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    EXPECT_EQ(result.value().indices.size(), 3u);

    // Errors name the file in UTF-8 rather than throwing on the conversion.
    auto missing = loadObj(folder / pathFromUtf8(u8"\u65e5\u672c.obj"));
    ASSERT_TRUE(missing.isError());
    EXPECT_NE(missing.error().message.find(u8"\u65e5\u672c.obj"), std::string::npos);
}

// ---- Parsing on the worker (TD-16a) ------------------------------------------------

namespace {

// An n x n grid of quads: (n+1)^2 vertices, 2 n^2 triangles.
void writeGrid(const fs::path& file, int n) {
    std::ofstream out(file, std::ios::binary);
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) out << "v " << x << ' ' << y << " 0\n";
    }
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int a = y * (n + 1) + x + 1;
            out << "f " << a << ' ' << a + 1 << ' ' << a + n + 2 << ' ' << a + n + 1 << '\n';
        }
    }
}

// Take results until `count` have arrived or 30 s pass.
std::vector<ObjLoadQueue::Completed> waitFor(ObjLoadQueue& queue, size_t count) {
    std::vector<ObjLoadQueue::Completed> all;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (all.size() < count && std::chrono::steady_clock::now() < deadline) {
        for (auto& done : queue.takeCompleted()) all.push_back(std::move(done));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return all;
}

}  // namespace

TEST_F(ObjLoaderTest, ASetCancelFlagStopsTheParse) {
    const fs::path file = dir_ / "grid.obj";
    writeGrid(file, 100);  // more lines than one cancel-check interval
    std::atomic<bool> cancel{true};
    auto result = loadObj(file, &cancel);
    ASSERT_TRUE(result.isError());
    EXPECT_NE(result.error().message.find("cancelled"), std::string::npos) << result.error().message;
    cancel = false;
    EXPECT_TRUE(loadObj(file, &cancel).isSuccess());
}

TEST_F(ObjLoaderTest, QueueParsesInOrderOffTheCallingThread) {
    const fs::path good = write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const fs::path bad = dir_ / "absent.obj";
    const fs::path grid = dir_ / "grid.obj";
    writeGrid(grid, 20);

    ObjLoadQueue queue;
    const uint64_t t1 = queue.request(good);
    const uint64_t t2 = queue.request(bad);
    const uint64_t t3 = queue.request(grid);
    EXPECT_NE(t1, 0u);
    EXPECT_NE(t1, t2);
    EXPECT_NE(t2, t3);

    auto done = waitFor(queue, 3);
    ASSERT_EQ(done.size(), 3u);
    EXPECT_EQ(done[0].ticket, t1);
    EXPECT_EQ(done[0].path, good);
    ASSERT_TRUE(done[0].mesh.isSuccess());
    EXPECT_EQ(done[0].mesh.value().triangleCount(), 1u);
    EXPECT_EQ(done[1].ticket, t2);
    EXPECT_TRUE(done[1].mesh.isError());  // errors come back as results
    EXPECT_EQ(done[2].ticket, t3);
    ASSERT_TRUE(done[2].mesh.isSuccess());
    EXPECT_EQ(done[2].mesh.value().triangleCount(), 2u * 20 * 20);
    EXPECT_EQ(queue.pendingCount(), 0u);
    EXPECT_TRUE(queue.takeCompleted().empty());
}

TEST_F(ObjLoaderTest, CancelledRequestsNeverArriveAndTheWorkerCarriesOn) {
    const fs::path big = dir_ / "big.obj";
    writeGrid(big, 300);
    const fs::path small = write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    ObjLoadQueue queue;
    const uint64_t running = queue.request(big);
    const uint64_t queued = queue.request(big);
    queue.cancel(queued);   // still queued
    queue.cancel(running);  // queued or already being parsed: stops either way
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (queue.pendingCount() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(queue.pendingCount(), 0u);

    const uint64_t after = queue.request(small);
    auto done = waitFor(queue, 1);
    ASSERT_EQ(done.size(), 1u);
    EXPECT_EQ(done[0].ticket, after);  // nothing from the cancelled ones
    EXPECT_TRUE(done[0].mesh.isSuccess());

    // cancelAll drops what is queued, being parsed, and finished but not yet taken.
    const auto untilIdle = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto waitIdle = [&] {
        while (queue.pendingCount() > 0 && std::chrono::steady_clock::now() < untilIdle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    queue.request(small);
    waitIdle();  // parsed; its result waits, untaken
    queue.request(big);
    queue.cancelAll();
    waitIdle();
    EXPECT_EQ(queue.pendingCount(), 0u);
    EXPECT_TRUE(queue.takeCompleted().empty());
}

TEST_F(ObjLoaderTest, DestroyingTheQueueStopsAParseInProgress) {
    const fs::path big = dir_ / "big.obj";
    writeGrid(big, 400);
    // Time a full parse, then destroy a queue mid-parse: it must not wait for the end.
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(loadObj(big).isSuccess());
    const auto fullParse = std::chrono::steady_clock::now() - t0;

    auto queue = std::make_unique<ObjLoadQueue>();
    queue->request(big);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto t1 = std::chrono::steady_clock::now();
    queue.reset();
    const auto teardown = std::chrono::steady_clock::now() - t1;
    EXPECT_LT(teardown, fullParse) << "full parse "
        << std::chrono::duration_cast<std::chrono::milliseconds>(fullParse).count() << " ms, teardown "
        << std::chrono::duration_cast<std::chrono::milliseconds>(teardown).count() << " ms";
}

}  // namespace test
}  // namespace quadmapper
