#include <gtest/gtest.h>
#include "scene/ObjLoader.hpp"
#include "core/UUID.hpp"

#include <glm/glm.hpp>

#include <filesystem>
#include <fstream>
#include <string>

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
    EXPECT_EQ(mesh.uvs[1], Vec2(0, 0));  // vt 0 1 (OBJ top-left) -> top row
}

TEST_F(ObjLoaderTest, TextureCoordinatesAreFlippedToTopRowOrigin) {
    // OBJ v = 0 is the image's bottom row; the renderer samples v = 0 at the top, so
    // an OBJ-authored texture must not come out upside down.
    auto result = loadObj(write(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "f 1/1 2/2 3/3\n"));

    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    const MeshData& mesh = result.value();
    ASSERT_EQ(mesh.uvs.size(), 3u);
    EXPECT_EQ(mesh.uvs[0], Vec2(0, 1));  // OBJ bottom-left -> bottom row
    EXPECT_EQ(mesh.uvs[2], Vec2(0, 0));  // OBJ top-left -> top row
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
    EXPECT_EQ(mesh.uvs[1], Vec2(1, 1));  // vt 1 0, flipped
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
    EXPECT_EQ(mesh.uvs[3], Vec2(1, 0));  // vt 1 1, flipped
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

TEST_F(ObjLoaderTest, EmptyOrFacelessFileReturnsErr) {
    EXPECT_TRUE(loadObj(write("")).isError());
    EXPECT_TRUE(loadObj(write("# only a comment\nv 0 0 0\n")).isError());
}

TEST_F(ObjLoaderTest, MissingFileReturnsErr) {
    EXPECT_TRUE(loadObj(dir_ / "absent.obj").isError());
}

}  // namespace test
}  // namespace quadmapper
