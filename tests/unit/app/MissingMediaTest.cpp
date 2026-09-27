// Missing-media app logic (app/MissingMedia.hpp) and the saved-transport helper it
// shares with load and source release (app/MediaAssign.hpp), plus the project-replace
// order (app/ProjectReplace.hpp) and pause-while-unseen, on a real EditorState.
// Opening a real file needs GL (its texture), so relocation is tested from the point
// the new source exists: the rebind and the transport it is given.

#include "app/MissingMedia.hpp"
#include "app/ProjectReplace.hpp"
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
              "Not an image or video file QUADmapper can open.");
    EXPECT_EQ(relocateMissingSource(fx.es, sm, fx.missingId, "D:/found/no-extension"),
              "Not an image or video file QUADmapper can open.");
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

    clearProjectContent(es, &sm);  // a new project forgets them
    EXPECT_TRUE(es.releasedTransports.empty());
}

// ---- Pause-while-unseen and the project-replace order (TD-14a) ----------------------

namespace {

// One quad of the active scene showing a playing fake clip, selected so
// deleteSelectedQuads() takes it off screen while the undo history keeps it.
struct UnseenFixture {
    EditorState es;
    SourceManager sm;
    FakePlayable* clip = nullptr;
    UUID clipId;
    Quad* quad = nullptr;

    UnseenFixture() {
        auto fake = std::make_unique<FakePlayable>("clip.mp4");
        clip = fake.get();
        clipId = sm.registerSource(std::move(fake));
        clip->play();
        quad = es.quadManager().createQuad("Q");
        quad->getTexture().mediaId = clipId;
        es.quadManager().selectQuad(quad->getId());
    }
};

}  // namespace

TEST(PauseWhileUnseenTest, AClipOnlyUndoHoldsPausesAndResumesWhenUndoBringsItBack) {
    UnseenFixture fx;
    releaseUnusedSources(fx.es, fx.sm);  // on screen: untouched
    EXPECT_EQ(fx.clip->state, PlaybackState::PLAYING);
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());

    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm);
    ASSERT_TRUE(fx.sm.hasSource(fx.clipId));  // undo may still need it
    EXPECT_EQ(fx.clip->state, PlaybackState::PAUSED);
    EXPECT_EQ(fx.es.sourcesPausedWhileUnseen.count(fx.clipId), 1u);

    releaseUnusedSources(fx.es, fx.sm);  // a later pass changes nothing
    EXPECT_EQ(fx.clip->pauses, 1);

    ASSERT_TRUE(fx.es.commandHistory.undo());
    releaseUnusedSources(fx.es, fx.sm);
    EXPECT_EQ(fx.clip->state, PlaybackState::PLAYING);
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());
}

TEST(PauseWhileUnseenTest, AClipTheUserPausedStaysPausedThroughDeleteAndUndo) {
    UnseenFixture fx;
    fx.clip->pause();
    const int plays = fx.clip->plays;
    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm);
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());

    ASSERT_TRUE(fx.es.commandHistory.undo());
    releaseUnusedSources(fx.es, fx.sm);
    EXPECT_EQ(fx.clip->state, PlaybackState::PAUSED);
    EXPECT_EQ(fx.clip->plays, plays);  // never resumed behind the user's back
}

TEST(PauseWhileUnseenTest, TheVideoPlayersClipKeepsPlayingWithItsQuadDeleted) {
    UnseenFixture fx;
    fx.es.activeVideoSourceId = fx.clipId;
    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm);
    EXPECT_EQ(fx.clip->state, PlaybackState::PLAYING);
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());
}

TEST(PauseWhileUnseenTest, AClipNoUndoCanRestoreIsReleasedAndForgotten) {
    UnseenFixture fx;
    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm);
    ASSERT_EQ(fx.es.sourcesPausedWhileUnseen.count(fx.clipId), 1u);

    fx.es.commandHistory.clear();
    releaseUnusedSources(fx.es, fx.sm);
    EXPECT_FALSE(fx.sm.hasSource(fx.clipId));
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());
}

// New, Load and recovery all clear through clearProjectContent before the next project
// creates a source: the old project's clips are gone (a reloaded show opens its media
// fresh), with the undo history and every record naming them, while the media bin's
// own sources stay assignable.
TEST(ClearProjectContentTest, ReleasesTheOldProjectsMediaAndRecordsButKeepsTheBins) {
    UnseenFixture fx;
    const UUID binId = fx.sm.registerSource(std::make_unique<FakePlayable>("bin generator"));
    MediaLibrary library;
    auto binItem = std::make_unique<MediaItem>();
    binItem->setSourceId(binId);
    library.addItem(std::move(binItem));
    fx.es.mediaBinPanel.setMediaLibrary(&library);

    const UUID playerId = fx.sm.registerSource(std::make_unique<FakePlayable>("player.mp4"));
    fx.es.activeVideoSourceId = playerId;  // the Video Player's clip is project media
    fx.es.quadTexturePaths[fx.quad->getId()] = "C:/show/clip.mp4";
    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm);  // the clip is paused, held by undo only
    ASSERT_EQ(fx.es.sourcesPausedWhileUnseen.count(fx.clipId), 1u);
    fx.es.releasedTransports[UUID::generate()] = Project::TransportData{};
    Project::SourceData missing;
    missing.id = UUID::generate();
    missing.path = "C:/show/gone.mp4";
    fx.es.missingSources.emplace(missing.id, missing);
    fx.es.showMissingMedia = true;

    clearProjectContent(fx.es, &fx.sm);

    EXPECT_FALSE(fx.es.commandHistory.canUndo());
    EXPECT_TRUE(fx.es.scenes.empty());
    EXPECT_TRUE(fx.es.quadTexturePaths.empty());
    EXPECT_TRUE(fx.es.activeVideoSourceId.isNull());
    EXPECT_FALSE(fx.sm.hasSource(fx.clipId));
    EXPECT_FALSE(fx.sm.hasSource(playerId));
    EXPECT_TRUE(fx.sm.hasSource(binId));
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());
    EXPECT_TRUE(fx.es.releasedTransports.empty());
    EXPECT_TRUE(fx.es.missingSources.empty());
    EXPECT_FALSE(fx.es.showMissingMedia);

    fx.es.mediaBinPanel.setMediaLibrary(nullptr);  // `library` dies before `fx`
}
