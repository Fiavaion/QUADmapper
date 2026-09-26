// Missing-media app logic (app/MissingMedia.hpp) and the saved-transport helper it
// shares with load and source release (app/MediaAssign.hpp), on a real EditorState.
// Opening a real file needs GL (its texture), so relocation is tested from the point
// the new source exists: the rebind and the transport it is given.

#include "app/MissingMedia.hpp"
#include "../control/FakePlayable.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace quadmapper;

namespace {

std::string sourcePath(const UUID& id, const char* action) {
    return "/qm/source/" + id.toString() + "/" + action;
}

// One scene with two quads and an object on the missing clip M, a third quad on
// another source, and a MIDI mapping aimed at each.
struct MissingFixture {
    EditorState es;
    UUID missingId = UUID::generate();
    UUID otherId = UUID::generate();
    const std::string oldPath = "C:/show/gone/clip.mp4";
    Quad* a = nullptr;
    Quad* b = nullptr;
    Quad* c = nullptr;
    MeshObject* object = nullptr;

    MissingFixture() {
        auto scene = std::make_unique<Scene>();
        a = scene->getQuadManager().createQuad("A");
        b = scene->getQuadManager().createQuad("B");
        c = scene->getQuadManager().createQuad("C");
        a->getTexture().mediaId = missingId;
        b->getTexture().mediaId = missingId;
        c->getTexture().mediaId = otherId;
        object = scene->adoptObject(std::make_unique<MeshObject>());
        object->getTexture().mediaId = missingId;
        es.scenes.push_back(std::move(scene));

        es.quadTexturePaths[a->getId()] = oldPath;
        es.quadTexturePaths[b->getId()] = oldPath;
        es.quadTexturePaths[c->getId()] = "C:/show/other.png";

        Project::SourceData missing;
        missing.id = missingId;
        missing.kind = "video";
        missing.path = oldPath;
        missing.missing = true;
        Project::TransportData transport;
        transport.playing = false;
        transport.speed = 2.0f;
        transport.loopMode = LoopMode::PING_PONG;
        transport.timelineLocked = true;
        missing.transport = transport;
        es.missingSources.emplace(missingId, missing);

        control::MidiMapping toMissing;
        toMissing.number = 1;
        toMissing.target = sourcePath(missingId, "restart");
        control::MidiMapping toOther;
        toOther.number = 2;
        toOther.target = sourcePath(otherId, "play");
        es.midiMappings = {toMissing, toOther};
    }
};

}  // namespace

TEST(MissingMediaTest, CollectListsEachMissingFileWithItsUses) {
    MissingFixture fx;
    auto meshless = std::make_unique<MeshObject>();
    meshless->setSourcePath("C:/show/gone/stage.obj");  // path but no mesh: missing
    fx.es.scenes[0]->adoptObject(std::move(meshless));

    const auto items = collectMissingMedia(fx.es);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].kind, MissingMediaItem::Kind::Source);
    EXPECT_EQ(items[0].sourceId, fx.missingId);
    EXPECT_EQ(items[0].path, fx.oldPath);
    EXPECT_EQ(items[0].quadUses, 2);
    EXPECT_EQ(items[0].objectUses, 1);
    EXPECT_EQ(items[1].kind, MissingMediaItem::Kind::Mesh);
    EXPECT_EQ(items[1].path, "C:/show/gone/stage.obj");
    EXPECT_EQ(items[1].objectUses, 1);
}

TEST(MissingMediaTest, RebindMovesQuadsObjectsPathsAndMidiMappings) {
    MissingFixture fx;
    const UUID undoOnlyQuad = UUID::generate();  // a quad only an undo step holds
    fx.es.quadTexturePaths[undoOnlyQuad] = fx.oldPath;
    const UUID newId = UUID::generate();
    const std::string newPath = "D:/found/clip.mp4";

    rebindMissingSource(fx.es, fx.missingId, fx.oldPath, newId, newPath, /*isVideo=*/true);

    EXPECT_EQ(fx.a->getTexture().mediaId, newId);
    EXPECT_EQ(fx.b->getTexture().mediaId, newId);
    EXPECT_EQ(fx.c->getTexture().mediaId, fx.otherId);
    EXPECT_EQ(fx.object->getTexture().mediaId, newId);
    EXPECT_EQ(fx.es.quadTexturePaths[fx.a->getId()], newPath);
    EXPECT_EQ(fx.es.quadTexturePaths[undoOnlyQuad], newPath);
    EXPECT_EQ(fx.es.quadTexturePaths[fx.c->getId()], "C:/show/other.png");
    EXPECT_EQ(fx.es.midiMappings[0].target, sourcePath(newId, "restart"))
        << "a mapping aimed at the missing clip follows it to the relocated one";
    EXPECT_EQ(fx.es.midiMappings[1].target, sourcePath(fx.otherId, "play"));
    EXPECT_EQ(fx.es.missingSources.count(fx.missingId), 0u);
    EXPECT_EQ(fx.es.activeVideoSourceId, newId);
    EXPECT_TRUE(collectMissingMedia(fx.es).empty());
}

TEST(MissingMediaTest, RelocateRefusesWhatItCannotUse) {
    MissingFixture fx;
    SourceManager sm;
    EXPECT_EQ(relocateMissingSource(fx.es, sm, UUID::generate(), "D:/found/clip.mp4"),
              "It is no longer missing.");
    EXPECT_EQ(relocateMissingSource(fx.es, sm, fx.missingId, "D:/found/notes.txt"),
              "The file could not be loaded.");
    EXPECT_EQ(relocateMissingSource(fx.es, sm, fx.missingId, "D:/definitely/not/here.mp4"),
              "The file could not be loaded.");
    EXPECT_EQ(fx.es.missingSources.count(fx.missingId), 1u) << "still missing after a refusal";
    EXPECT_EQ(fx.a->getTexture().mediaId, fx.missingId);
}

TEST(SavedTransportTest, LockedClipSavedPausedComesBackLockedAndPaused) {
    SourceManager sm;
    auto fake = std::make_unique<FakePlayable>("clip.mp4");
    FakePlayable* clip = fake.get();
    const UUID id = sm.registerSource(std::move(fake));

    Project::TransportData saved;
    saved.playing = false;
    saved.speed = 2.0f;
    saved.loopMode = LoopMode::PING_PONG;
    saved.timelineLocked = true;
    applySavedTransport(sm, id, saved);

    EXPECT_TRUE(clip->isTimelineLocked());
    EXPECT_EQ(clip->getPlaybackState(), PlaybackState::PAUSED)
        << "locking sets it playing; the saved pause must still win";
    EXPECT_EQ(clip->getLoopMode(), LoopMode::PING_PONG);
    EXPECT_FLOAT_EQ(clip->getSpeed(), 2.0f);

    // What it would save now is what was loaded.
    const Project::TransportData now = transportOf(*clip);
    EXPECT_EQ(now.playing, saved.playing);
    EXPECT_EQ(now.timelineLocked, saved.timelineLocked);
    EXPECT_EQ(now.loopMode, saved.loopMode);
    EXPECT_FLOAT_EQ(now.speed, saved.speed);
}

TEST(SavedTransportTest, PlayingUnlockedAndNothingSaved) {
    SourceManager sm;
    auto first = std::make_unique<FakePlayable>("a.mp4");
    FakePlayable* a = first.get();
    const UUID aId = sm.registerSource(std::move(first));
    Project::TransportData saved;
    saved.playing = true;
    applySavedTransport(sm, aId, saved);
    EXPECT_EQ(a->getPlaybackState(), PlaybackState::PLAYING);
    EXPECT_FALSE(a->isTimelineLocked());

    auto second = std::make_unique<FakePlayable>("b.mp4");
    FakePlayable* b = second.get();
    const UUID bId = sm.registerSource(std::move(second));
    applySavedTransport(sm, bId, std::nullopt);  // a file older than v3.1: play, as on assign
    EXPECT_EQ(b->getPlaybackState(), PlaybackState::PLAYING);

    applySavedTransport(sm, UUID::generate(), saved);  // no such source: nothing to do
}

TEST(SavedTransportTest, ReleasingAClipRemembersItsTransport) {
    EditorState es;
    SourceManager sm;
    auto fake = std::make_unique<FakePlayable>("clip.mp4");
    FakePlayable* clip = fake.get();
    const UUID id = sm.registerSource(std::move(fake));
    clip->setSpeed(0.5f);
    sm.setTimelineLocked(id, true);
    clip->pause();

    releaseUnusedSources(es, sm);  // nothing uses it

    EXPECT_FALSE(sm.hasSource(id));
    ASSERT_EQ(es.releasedTransports.count(id), 1u);
    const Project::TransportData& t = es.releasedTransports[id];
    EXPECT_FALSE(t.playing);
    EXPECT_TRUE(t.timelineLocked);
    EXPECT_FLOAT_EQ(t.speed, 0.5f);

    releaseProjectSources(es, sm);  // a new project forgets them
    EXPECT_TRUE(es.releasedTransports.empty());
}
