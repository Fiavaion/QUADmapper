// Unused-source release (QM-088): file-backed and procedural sources nothing
// references are shut down and removed; referenced ones and cameras stay. Uses
// stand-in sources with zero size so no GL context is needed.

#include <gtest/gtest.h>

#include "source/SourceManager.hpp"
#include "source/TextureSource.hpp"

#include <memory>
#include <string>

using namespace quadmapper;

namespace {

class FakeSource : public TextureSource {
public:
    FakeSource(SourceType type, int* shutdowns) : type_(type), shutdowns_(shutdowns) {}

    const UUID& getId() const override { return id_; }
    std::string getName() const override { return "fake"; }
    SourceType getType() const override { return type_; }
    std::string getTypeName() const override { return "Fake"; }
    bool initialize() override { return true; }
    void shutdown() override { ++*shutdowns_; }
    bool isReady() const override { return true; }
    bool update(double) override { return false; }
    bool hasNewFrame() const override { return false; }
    const FrameData* getCurrentFrame() const override { return nullptr; }
    void markFrameConsumed() override {}
    SourceCapabilities getCapabilities() const override { return {}; }
    int getWidth() const override { return 0; }   // no GPU texture allocated
    int getHeight() const override { return 0; }

private:
    UUID id_ = UUID::generate();
    SourceType type_;
    int* shutdowns_;
};

UUID add(SourceManager& sm, SourceType type, int* shutdowns) {
    return sm.registerSource(std::make_unique<FakeSource>(type, shutdowns));
}

}  // namespace

TEST(SourceRelease, ReleasesOnlyUnreferencedMediaSources) {
    int shutdowns = 0;
    SourceManager sm;
    const UUID boundVideo = add(sm, SourceType::VIDEO_FILE, &shutdowns);
    const UUID orphanVideo = add(sm, SourceType::VIDEO_FILE, &shutdowns);
    const UUID orphanImage = add(sm, SourceType::STATIC_IMAGE, &shutdowns);
    const UUID binProcedural = add(sm, SourceType::PROCEDURAL_PLASMA, &shutdowns);
    const UUID orphanProcedural = add(sm, SourceType::PROCEDURAL_NOISE, &shutdowns);
    const UUID camera = add(sm, SourceType::WEBCAM, &shutdowns);
    ASSERT_EQ(sm.getSourceCount(), 6u);

    // binProcedural stands for a generator the media bin still offers.
    const size_t released = sm.releaseUnreferenced({boundVideo, binProcedural});

    EXPECT_EQ(released, 3u);
    EXPECT_EQ(shutdowns, 3);  // each released source was shut down (decode thread joined)
    EXPECT_TRUE(sm.hasSource(boundVideo));
    EXPECT_FALSE(sm.hasSource(orphanVideo));
    EXPECT_FALSE(sm.hasSource(orphanImage));
    EXPECT_TRUE(sm.hasSource(binProcedural));
    // A generator from a previous project's sources table no longer runs every frame.
    EXPECT_FALSE(sm.hasSource(orphanProcedural));
    EXPECT_TRUE(sm.hasSource(camera));
}

TEST(SourceRelease, ReferencedSourcesSurviveRepeatedPasses) {
    int shutdowns = 0;
    SourceManager sm;
    const UUID video = add(sm, SourceType::VIDEO_FILE, &shutdowns);
    const UUID image = add(sm, SourceType::STATIC_IMAGE, &shutdowns);

    for (int frame = 0; frame < 3; ++frame) {
        EXPECT_EQ(sm.releaseUnreferenced({video, image}), 0u);
    }
    EXPECT_EQ(shutdowns, 0);

    // Unbinding the image lets the next pass release it.
    EXPECT_EQ(sm.releaseUnreferenced({video}), 1u);
    EXPECT_TRUE(sm.hasSource(video));
    EXPECT_FALSE(sm.hasSource(image));
}

TEST(SourceRelease, RemovalIsReportedThroughTheCallback) {
    int shutdowns = 0;
    SourceManager sm;
    UUID removed;
    sm.setSourceRemovedCallback([&](const UUID& id) { removed = id; });
    const UUID orphan = add(sm, SourceType::VIDEO_FILE, &shutdowns);

    sm.releaseUnreferenced({});
    EXPECT_EQ(removed, orphan);
}
