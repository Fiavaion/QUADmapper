#include <gtest/gtest.h>
#include "media/MediaLibrary.hpp"
#include "media/MediaItem.hpp"
#include <filesystem>

namespace quadmapper {
namespace test {

// ============ addItem Tests ============

TEST(MediaLibraryTest, AddItemCreatesNewItem) {
    MediaLibrary library;

    std::filesystem::path testPath = "test/image.png";
    MediaItem* item = library.addItem(testPath);

    EXPECT_NE(item, nullptr);
    EXPECT_EQ(library.getItemCount(), 1);
    EXPECT_EQ(item->getPath(), testPath);
}

TEST(MediaLibraryTest, AddItemSetsNameFromFilename) {
    MediaLibrary library;

    std::filesystem::path testPath = "folder/my_image.jpg";
    MediaItem* item = library.addItem(testPath);

    EXPECT_NE(item, nullptr);
    EXPECT_EQ(item->getName(), "my_image.jpg");
}

TEST(MediaLibraryTest, AddItemDetectsImageType) {
    MediaLibrary library;

    MediaItem* pngItem = library.addItem("test.png");
    MediaItem* jpgItem = library.addItem("test.jpg");
    MediaItem* bmpItem = library.addItem("test.bmp");

    EXPECT_EQ(pngItem->getType(), MediaType::IMAGE);
    EXPECT_EQ(jpgItem->getType(), MediaType::IMAGE);
    EXPECT_EQ(bmpItem->getType(), MediaType::IMAGE);
}

TEST(MediaLibraryTest, AddItemDetectsVideoType) {
    MediaLibrary library;

    MediaItem* mp4Item = library.addItem("test.mp4");
    MediaItem* movItem = library.addItem("test.mov");
    MediaItem* aviItem = library.addItem("test.avi");

    EXPECT_EQ(mp4Item->getType(), MediaType::VIDEO);
    EXPECT_EQ(movItem->getType(), MediaType::VIDEO);
    EXPECT_EQ(aviItem->getType(), MediaType::VIDEO);
}

TEST(MediaLibraryTest, AddItemPreventsDuplicatePaths) {
    MediaLibrary library;

    std::filesystem::path testPath = "test/image.png";
    MediaItem* item1 = library.addItem(testPath);
    MediaItem* item2 = library.addItem(testPath);

    EXPECT_NE(item1, nullptr);
    EXPECT_EQ(item2, nullptr);  // Duplicate path returns nullptr
    EXPECT_EQ(library.getItemCount(), 1);
}

// ============ removeItem Tests ============

TEST(MediaLibraryTest, RemoveItemByIdWorks) {
    MediaLibrary library;

    MediaItem* item = library.addItem("test.png");
    UUID id = item->getId();

    EXPECT_EQ(library.getItemCount(), 1);

    library.removeItem(id);

    EXPECT_EQ(library.getItemCount(), 0);
    EXPECT_EQ(library.getItem(id), nullptr);
}

TEST(MediaLibraryTest, RemoveItemWithInvalidIdDoesNothing) {
    MediaLibrary library;

    library.addItem("test.png");
    EXPECT_EQ(library.getItemCount(), 1);

    library.removeItem(UUID::generate());  // Invalid ID

    EXPECT_EQ(library.getItemCount(), 1);
}

// ============ getItem Tests ============

TEST(MediaLibraryTest, GetItemByIdReturnsCorrectItem) {
    MediaLibrary library;

    MediaItem* item1 = library.addItem("test1.png");
    MediaItem* item2 = library.addItem("test2.png");

    EXPECT_EQ(library.getItem(item1->getId()), item1);
    EXPECT_EQ(library.getItem(item2->getId()), item2);
}

TEST(MediaLibraryTest, GetItemWithInvalidIdReturnsNull) {
    MediaLibrary library;

    library.addItem("test.png");

    EXPECT_EQ(library.getItem(UUID::generate()), nullptr);
}

TEST(MediaLibraryTest, ConstGetItemWorks) {
    MediaLibrary library;
    MediaItem* item = library.addItem("test.png");

    const MediaLibrary& constLibrary = library;
    const MediaItem* constItem = constLibrary.getItem(item->getId());

    EXPECT_NE(constItem, nullptr);
    EXPECT_EQ(constItem->getName(), item->getName());
}

// ============ getItemCount and isEmpty Tests ============

TEST(MediaLibraryTest, IsEmptyInitially) {
    MediaLibrary library;

    EXPECT_TRUE(library.isEmpty());
    EXPECT_EQ(library.getItemCount(), 0);
}

TEST(MediaLibraryTest, IsNotEmptyAfterAddingItem) {
    MediaLibrary library;

    library.addItem("test.png");

    EXPECT_FALSE(library.isEmpty());
    EXPECT_EQ(library.getItemCount(), 1);
}

// ============ getItems Tests ============

TEST(MediaLibraryTest, GetItemsReturnsAllItems) {
    MediaLibrary library;

    library.addItem("test1.png");
    library.addItem("test2.png");
    library.addItem("test3.mp4");

    const auto& items = library.getItems();

    EXPECT_EQ(items.size(), 3);
}

// ============ clear Tests ============

TEST(MediaLibraryTest, ClearRemovesAllItems) {
    MediaLibrary library;

    library.addItem("test1.png");
    library.addItem("test2.png");

    EXPECT_EQ(library.getItemCount(), 2);

    library.clear();

    EXPECT_TRUE(library.isEmpty());
    EXPECT_EQ(library.getItemCount(), 0);
}

// ============ hasPath Tests ============

TEST(MediaLibraryTest, HasPathReturnsTrueForExistingPath) {
    MediaLibrary library;

    std::filesystem::path testPath = "test/image.png";
    library.addItem(testPath);

    EXPECT_TRUE(library.hasPath(testPath));
}

TEST(MediaLibraryTest, HasPathReturnsFalseForNonExistingPath) {
    MediaLibrary library;

    library.addItem("test.png");

    EXPECT_FALSE(library.hasPath("other.png"));
}

// ============ Static Extension Methods Tests ============

TEST(MediaLibraryTest, IsExtensionSupportedWorksForImages) {
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".png"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".jpg"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".jpeg"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".bmp"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".gif"));
}

TEST(MediaLibraryTest, IsExtensionSupportedWorksForVideos) {
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".mp4"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".mov"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".avi"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".mkv"));
}

TEST(MediaLibraryTest, IsExtensionSupportedIsCaseInsensitive) {
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".PNG"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".Jpg"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported(".MP4"));
}

TEST(MediaLibraryTest, IsExtensionSupportedWorksWithoutDot) {
    EXPECT_TRUE(MediaLibrary::isExtensionSupported("png"));
    EXPECT_TRUE(MediaLibrary::isExtensionSupported("mp4"));
}

TEST(MediaLibraryTest, IsExtensionSupportedReturnsFalseForUnknown) {
    EXPECT_FALSE(MediaLibrary::isExtensionSupported(".xyz"));
    EXPECT_FALSE(MediaLibrary::isExtensionSupported(".txt"));
    EXPECT_FALSE(MediaLibrary::isExtensionSupported(".doc"));
}

TEST(MediaLibraryTest, GetTypeFromExtensionReturnsCorrectTypes) {
    EXPECT_EQ(MediaLibrary::getTypeFromExtension(".png"), MediaType::IMAGE);
    EXPECT_EQ(MediaLibrary::getTypeFromExtension(".jpg"), MediaType::IMAGE);
    EXPECT_EQ(MediaLibrary::getTypeFromExtension(".mp4"), MediaType::VIDEO);
    EXPECT_EQ(MediaLibrary::getTypeFromExtension(".mov"), MediaType::VIDEO);
}

// TD-24: an extension that is neither image nor video has no type (it used to fall
// through to IMAGE).
TEST(MediaLibraryTest, GetTypeFromExtensionHasNoTypeForOtherFiles) {
    for (const char* ext : {".txt", ".xyz", ".doc", ".wav", "", "."}) {
        EXPECT_FALSE(MediaLibrary::getTypeFromExtension(ext).has_value()) << ext;
    }
}

TEST(MediaLibraryTest, GetTypeFromExtensionAgreesWithIsExtensionSupported) {
    for (const auto* list : {&MediaLibrary::getSupportedImageExtensions(),
                             &MediaLibrary::getSupportedVideoExtensions()}) {
        for (const std::string& ext : *list) {
            EXPECT_TRUE(MediaLibrary::getTypeFromExtension(ext).has_value()) << ext;
            EXPECT_TRUE(MediaLibrary::isExtensionSupported(ext)) << ext;
        }
    }
    for (const std::string& ext : MediaLibrary::getSupportedImageExtensions()) {
        EXPECT_EQ(MediaLibrary::getTypeFromExtension(ext), MediaType::IMAGE) << ext;
    }
    for (const std::string& ext : MediaLibrary::getSupportedVideoExtensions()) {
        EXPECT_EQ(MediaLibrary::getTypeFromExtension(ext), MediaType::VIDEO) << ext;
    }
}

TEST(MediaLibraryTest, AddItemRefusesAFileThatIsNeitherImageNorVideo) {
    MediaLibrary library;
    EXPECT_EQ(library.addItem("notes.txt"), nullptr);
    EXPECT_EQ(library.addItem("no-extension"), nullptr);
    EXPECT_EQ(library.getItemCount(), 0u);
}

TEST(MediaLibraryTest, GetTypeFromExtensionIsCaseInsensitive) {
    EXPECT_EQ(MediaLibrary::getTypeFromExtension(".PNG"), MediaType::IMAGE);
    EXPECT_EQ(MediaLibrary::getTypeFromExtension(".MP4"), MediaType::VIDEO);
}

TEST(MediaLibraryTest, GetSupportedExtensionsReturnsNonEmpty) {
    const auto& imageExts = MediaLibrary::getSupportedImageExtensions();
    const auto& videoExts = MediaLibrary::getSupportedVideoExtensions();

    EXPECT_FALSE(imageExts.empty());
    EXPECT_FALSE(videoExts.empty());
}

} // namespace test
} // namespace quadmapper
