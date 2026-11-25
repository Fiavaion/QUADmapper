// ThumbnailCache's failure memory: a file that cannot be thumbnailed is not read and
// decoded again every frame, only after it changes. These paths never reach GL, so no
// context is needed.

#include "media/ThumbnailCache.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace quadmapper;

namespace {

class ThumbnailCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "quadmapper_thumbnail_cache_test";
        std::filesystem::create_directories(dir_);
        broken_ = dir_ / "not_really_an_image.png";
        write("these bytes are not a png");
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    void write(const char* bytes) {
        std::ofstream out(broken_, std::ios::binary | std::ios::trunc);
        out << bytes;
    }

    std::filesystem::path dir_;
    std::filesystem::path broken_;
};

}  // namespace

TEST_F(ThumbnailCacheTest, PruningRemovesOnlyOldThumbnails) {
    namespace fs = std::filesystem;
    const auto touch = [this](const char* name, std::chrono::hours age) {
        const fs::path file = dir_ / name;
        std::ofstream(file) << "x";
        fs::last_write_time(file, fs::file_time_type::clock::now() - age);
        return file;
    };
    const fs::path oldThumb = touch("1.v2.thumb", std::chrono::hours(24 * 100));
    const fs::path newThumb = touch("2.v2.thumb", std::chrono::hours(1));
    const fs::path oldOther = touch("notes.txt", std::chrono::hours(24 * 100));

    ThumbnailCache::pruneDiskCache(dir_, std::chrono::hours(24 * 90));

    EXPECT_FALSE(fs::exists(oldThumb));
    EXPECT_TRUE(fs::exists(newThumb));
    EXPECT_TRUE(fs::exists(oldOther));
}

TEST_F(ThumbnailCacheTest, AFileNotSeenYetNeedsGeneration) {
    ThumbnailCache cache;
    EXPECT_TRUE(cache.needsGeneration(broken_));
}

TEST_F(ThumbnailCacheTest, AFailedFileIsNotRetriedUntilItChanges) {
    ThumbnailCache cache;
    EXPECT_EQ(cache.getThumbnail(broken_, 64), 0u);
    EXPECT_FALSE(cache.needsGeneration(broken_));
    EXPECT_EQ(cache.getThumbnail(broken_, 64), 0u);

    // Replaced on disk: its write time moves, so it is tried again.
    write("still not a png, but a different file");
    std::filesystem::last_write_time(
        broken_, std::filesystem::last_write_time(broken_) + std::chrono::seconds(10));
    EXPECT_TRUE(cache.needsGeneration(broken_));
}

TEST_F(ThumbnailCacheTest, AMissingFileFailsOnceThenWaitsForItToAppear) {
    ThumbnailCache cache;
    const auto missing = dir_ / "later.png";
    EXPECT_EQ(cache.getThumbnail(missing, 64), 0u);
    EXPECT_FALSE(cache.needsGeneration(missing));

    std::ofstream(missing, std::ios::binary) << "now it exists";
    EXPECT_TRUE(cache.needsGeneration(missing));
}

TEST_F(ThumbnailCacheTest, InvalidateForgetsAFailure) {
    ThumbnailCache cache;
    EXPECT_EQ(cache.getThumbnail(broken_, 64), 0u);
    cache.invalidate(broken_);
    EXPECT_TRUE(cache.needsGeneration(broken_));
}
