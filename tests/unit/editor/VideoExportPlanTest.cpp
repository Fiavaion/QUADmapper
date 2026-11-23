// Video export rules (ADR-060): the length when one video is mapped, the file names,
// the frame count and the audio sample ranges per video frame.

#include "editor/VideoExportPlan.hpp"

#include <gtest/gtest.h>

#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>

using namespace quadmapper;

namespace {

UUID bindNewQuad(Scene& scene, const UUID& mediaId) {
    Quad* quad = scene.createQuad();
    quad->getTexture().mediaId = mediaId;
    return quad->getId();
}

}  // namespace

TEST(VideoExportPlanTest, MediaUsedOnlyByTheChosenScenes) {
    std::vector<std::unique_ptr<Scene>> scenes;
    scenes.push_back(std::make_unique<Scene>());
    scenes.push_back(std::make_unique<Scene>());
    const UUID a = UUID::generate(), b = UUID::generate();
    bindNewQuad(*scenes[0], a);
    bindNewQuad(*scenes[0], a);  // twice: counted once
    scenes[0]->createQuad();     // no media
    bindNewQuad(*scenes[1], b);

    const auto first = mediaUsedByScenes(scenes, {scenes[0]->getId()});
    EXPECT_EQ(first.size(), 1u);
    EXPECT_EQ(first.count(a), 1u);
    const auto both = mediaUsedByScenes(scenes, {scenes[0]->getId(), scenes[1]->getId()});
    EXPECT_EQ(both.size(), 2u);
    EXPECT_TRUE(mediaUsedByScenes(scenes, {}).empty());
}

TEST(VideoExportPlanTest, OneVideoMappedGivesItsLengthElseTheUserSetsIt) {
    const UUID video = UUID::generate(), image = UUID::generate(), other = UUID::generate();
    std::unordered_map<UUID, std::optional<double>> lengths = {{video, 12.5}, {image, std::nullopt}, {other, 4.0}};
    auto lengthOf = [&](const UUID& id) { return lengths.at(id); };

    EXPECT_EQ(singleVideoSeconds({video}, lengthOf), std::optional<double>(12.5));
    // Images and generators do not count.
    EXPECT_EQ(singleVideoSeconds({video, image}, lengthOf), std::optional<double>(12.5));
    // Two videos, or none: no automatic length.
    EXPECT_FALSE(singleVideoSeconds({video, other}, lengthOf).has_value());
    EXPECT_FALSE(singleVideoSeconds({image}, lengthOf).has_value());
    EXPECT_FALSE(singleVideoSeconds({}, lengthOf).has_value());
}

TEST(VideoExportPlanTest, PlayingLengthFollowsSpeed) {
    EXPECT_DOUBLE_EQ(*playingSeconds(10.0, 1.0), 10.0);
    EXPECT_DOUBLE_EQ(*playingSeconds(10.0, 2.0), 5.0);
    EXPECT_DOUBLE_EQ(*playingSeconds(10.0, -0.5), 20.0);  // reverse plays the same frames
    EXPECT_FALSE(playingSeconds(10.0, 0.0).has_value());
    EXPECT_FALSE(playingSeconds(0.0, 1.0).has_value());
}

TEST(VideoExportPlanTest, FrameCountRoundsToTheNearestFrame) {
    EXPECT_EQ(exportFrameCount(10.0, 30), 300);
    EXPECT_EQ(exportFrameCount(12.51, 25), 313);
    EXPECT_EQ(exportFrameCount(0.001, 30), 1);  // never an empty file
    EXPECT_EQ(exportFrameCount(0.0, 30), 0);
    EXPECT_EQ(exportFrameCount(5.0, 0), 0);
}

TEST(VideoExportPlanTest, AudioRangesTileTheTimelineWithoutDrift) {
    // 48000 / 30 = 1600 exactly; 48000 / 60 = 800; at a non-integer rate the counts
    // vary but the ranges stay contiguous and add up to the clock.
    for (int fps : {25, 30, 50, 60, 24}) {
        int64_t next = 0;
        for (int64_t frame = 0; frame < 1000; ++frame) {
            const SampleRange r = audioSamplesForFrame(frame, fps, 44100);
            EXPECT_EQ(r.first, next);
            EXPECT_GT(r.count, 0);
            next = r.first + r.count;
        }
        EXPECT_EQ(next, static_cast<int64_t>(std::llround(1000.0 * 44100 / fps)));
    }
    EXPECT_EQ(audioSamplesForFrame(3, 30, 48000).first, 4800);
    EXPECT_EQ(audioSamplesForFrame(3, 30, 48000).count, 1600);
}

TEST(VideoExportPlanTest, FileNamesAreSafeOnWindows) {
    EXPECT_EQ(exportFileName("Show", "Projector 1"), "Show - Projector 1.mp4");
    EXPECT_EQ(exportFileName("a:b/c", "Out*?"), "a_b_c - Out__.mp4");
    EXPECT_EQ(exportFileName("", "P"), "Export - P.mp4");
    EXPECT_EQ(exportFileName("Show. ", " "), "Show - _.mp4");
}

TEST(VideoExportPlanTest, TwoOutputsNeverShareAFile) {
    const auto names = exportFileNames("Show", {"Left", "left", "Right", "Left:", "Left?"});
    ASSERT_EQ(names.size(), 5u);
    EXPECT_EQ(names[0], "Show - Left.mp4");
    EXPECT_EQ(names[1], "Show - left (2).mp4");  // Windows ignores case
    EXPECT_EQ(names[2], "Show - Right.mp4");
    EXPECT_EQ(names[3], "Show - Left_.mp4");
    EXPECT_EQ(names[4], "Show - Left_ (2).mp4");
}
