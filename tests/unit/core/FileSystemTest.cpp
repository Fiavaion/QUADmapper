#include <gtest/gtest.h>
#include "core/FileSystem.hpp"
#include "core/UUID.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace quadmapper {
namespace test {

namespace fs = std::filesystem;

// Atomic save (ADR-046 §1): each test works in its own temp directory.
class FileSystemAtomicTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() / ("qm_fs_" + UUID::generate().toString());
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    static std::string slurp(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    static void spit(const fs::path& p, const std::string& s) {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << s;
    }

    fs::path dir_;
};

TEST_F(FileSystemAtomicTest, CreatesNewFile) {
    const fs::path target = dir_ / "show.qmap";

    auto result = FileSystem::writeFileAtomic(target, "{\"v\":1}");

    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    EXPECT_EQ(slurp(target), "{\"v\":1}");
    EXPECT_FALSE(fs::exists(dir_ / "show.qmap.tmp"));
    EXPECT_FALSE(fs::exists(dir_ / "show.qmap.bak"));  // nothing to back up on first save
}

TEST_F(FileSystemAtomicTest, ReplaceKeepsPreviousContentInBak) {
    const fs::path target = dir_ / "show.qmap";
    spit(target, "old content");

    auto result = FileSystem::writeFileAtomic(target, "new content");

    ASSERT_TRUE(result.isSuccess()) << result.error().message;
    EXPECT_EQ(slurp(target), "new content");
    EXPECT_EQ(slurp(dir_ / "show.qmap.bak"), "old content");
    EXPECT_FALSE(fs::exists(dir_ / "show.qmap.tmp"));
}

TEST_F(FileSystemAtomicTest, BakRollsToThePreviousSave) {
    const fs::path target = dir_ / "show.qmap";
    ASSERT_TRUE(FileSystem::writeFileAtomic(target, "one").isSuccess());
    ASSERT_TRUE(FileSystem::writeFileAtomic(target, "two").isSuccess());
    ASSERT_TRUE(FileSystem::writeFileAtomic(target, "three").isSuccess());

    EXPECT_EQ(slurp(target), "three");
    EXPECT_EQ(slurp(dir_ / "show.qmap.bak"), "two");
}

TEST_F(FileSystemAtomicTest, FailedReplaceLeavesOriginalByteIdentical) {
    const fs::path target = dir_ / "show.qmap";
    const std::string original = "calibrated mapping\r\n\x01\x02 bytes";
    spit(target, original);
    // Another process holding the file without FILE_SHARE_DELETE (a sync client, an
    // editor) makes the final replace fail after the temp has been written.
    HANDLE lock = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);

    auto result = FileSystem::writeFileAtomic(target, "half-written garbage");
    CloseHandle(lock);

    EXPECT_TRUE(result.isError());
    EXPECT_EQ(slurp(target), original);
    EXPECT_FALSE(fs::exists(dir_ / "show.qmap.tmp"));
}

TEST_F(FileSystemAtomicTest, UnwritableDirectoryReturnsErrAndCreatesNothing) {
    const fs::path target = dir_ / "missing-subdir" / "show.qmap";

    auto result = FileSystem::writeFileAtomic(target, "content");

    EXPECT_TRUE(result.isError());
    EXPECT_FALSE(fs::exists(target));
}

}  // namespace test
}  // namespace quadmapper
