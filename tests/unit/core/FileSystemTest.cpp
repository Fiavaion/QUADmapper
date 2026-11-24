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

// NTFS file identity: the same for a file rewritten in place, new for a file that
// replaced it by rename.
static ULONGLONG fileIndex(const fs::path& p) {
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info{};
    GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    return (static_cast<ULONGLONG>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

TEST_F(FileSystemAtomicTest, ReplacesTargetByRenameNeverInPlace) {
    // A writer that truncates the target and rewrites it (even after copying a .bak)
    // leaves a window where a crash destroys the show. The atomic write must instead
    // swap a fully written file in, which gives the target a new file identity.
    const fs::path target = dir_ / "show.qmap";
    spit(target, "previous save");
    const ULONGLONG before = fileIndex(target);
    ASSERT_NE(before, 0u);

    ASSERT_FALSE(FileSystem::writeFileAtomic(target, "new save").isError());

    EXPECT_NE(fileIndex(target), before) << "target was rewritten in place";
    EXPECT_EQ(slurp(target), "new save");
}

// Path strings are UTF-8 (GLFW drops, file dialogs, .qmap, preferences). These names
// are outside every ANSI code page, where the narrow conversions garble or throw.
TEST(Utf8PathTest, ConvertsNonAsciiNamesExactlyBothWays) {
    const std::string utf8 = u8"C:/Show/\u00fc/\u65e5\u672c/\u0416.mp4";
    const fs::path path = pathFromUtf8(utf8);
    EXPECT_EQ(path.wstring(), L"C:/Show/\u00fc/\u65e5\u672c/\u0416.mp4");
    EXPECT_EQ(utf8FromPath(path), utf8);
    EXPECT_EQ(utf8FromPath(path.filename()), u8"\u0416.mp4");
    EXPECT_EQ(utf8FromPath(path.extension()), ".mp4");
    EXPECT_EQ(utf8FromPath(path.parent_path().filename()), u8"\u65e5\u672c");
    EXPECT_TRUE(pathFromUtf8("").empty());
    EXPECT_EQ(utf8FromPath(fs::path()), "");
}

TEST_F(FileSystemAtomicTest, ReadsAndWritesInsideNonAsciiFolder) {
    const fs::path folder = dir_ / pathFromUtf8(u8"\u00fc \u65e5\u672c \u0416");
    ASSERT_FALSE(FileSystem::createDirectory(folder).isError());
    ASSERT_TRUE(FileSystem::isDirectory(folder));
    const fs::path target = folder / pathFromUtf8(u8"\u0416.qmap");

    ASSERT_FALSE(FileSystem::writeFileAtomic(target, "saved").isError());
    ASSERT_FALSE(FileSystem::writeFileAtomic(target, "saved again").isError());

    EXPECT_TRUE(FileSystem::exists(target));
    auto text = FileSystem::readFile(target);
    ASSERT_TRUE(text.isSuccess()) << text.error().message;
    EXPECT_EQ(text.value(), "saved again");
    auto bytes = FileSystem::readBinaryFile(fs::path(target) += ".bak");
    ASSERT_TRUE(bytes.isSuccess()) << bytes.error().message;
    EXPECT_EQ(std::string(bytes.value().begin(), bytes.value().end()), "saved");
}

TEST_F(FileSystemAtomicTest, UnwritableDirectoryReturnsErrAndCreatesNothing) {
    const fs::path target = dir_ / "missing-subdir" / "show.qmap";

    auto result = FileSystem::writeFileAtomic(target, "content");

    EXPECT_TRUE(result.isError());
    EXPECT_FALSE(fs::exists(target));
}

// The app's own folders must not depend on the folder it was started from.
TEST(AppFoldersTest, ExecutableDirHoldsTheRunningProgramWhateverTheCurrentFolder) {
    const fs::path before = executableDir();
    const fs::path cwd = fs::current_path();
    fs::current_path(fs::temp_directory_path());
    const fs::path after = executableDir();
    fs::current_path(cwd);

    EXPECT_EQ(before, after);
    EXPECT_TRUE(fs::exists(before / "test_core.exe"));
    EXPECT_EQ(resourcePath("fonts/a.ttf"), before / "resources" / "fonts" / "a.ttf");
}

TEST(AppFoldersTest, UserFoldersExistAndAreNamedForTheApp) {
    for (const fs::path& dir : {userConfigDir(), userLocalDir()}) {
        EXPECT_EQ(dir.filename(), "QUADmapper");
        EXPECT_TRUE(fs::is_directory(dir));
    }
    EXPECT_NE(userConfigDir(), userLocalDir());
}

TEST_F(FileSystemAtomicTest, ReadBinaryFileReturnsExactBytesAndErrOnMissingFile) {
    const fs::path file = dir_ / "bytes.bin";
    spit(file, std::string("\x01\x00\x02", 3));

    auto ok = FileSystem::readBinaryFile(file);
    ASSERT_TRUE(ok.isSuccess());
    EXPECT_EQ(ok.value(), (std::vector<uint8_t>{1, 0, 2}));
    EXPECT_TRUE(FileSystem::readBinaryFile(dir_ / "absent.bin").isError());
}

TEST_F(FileSystemAtomicTest, AFileOverTheLimitIsNotRead) {
    const fs::path file = dir_ / "five.bin";
    spit(file, "12345");

    EXPECT_TRUE(FileSystem::readFile(file, 5).isSuccess());
    EXPECT_TRUE(FileSystem::readFile(file, 4).isError());
    EXPECT_TRUE(FileSystem::readBinaryFile(file, 5).isSuccess());
    EXPECT_TRUE(FileSystem::readBinaryFile(file, 4).isError());
}

}  // namespace test
}  // namespace quadmapper
