// Missing-media app logic (editor/MissingMedia.hpp) and the saved-transport helper it
// shares with load and source release (editor/MediaAssign.hpp), plus the project-replace
// order (editor/ProjectReplace.hpp) and pause-while-unseen, on a real EditorState.
// Opening a real file needs GL (its texture), so relocation is tested from the point
// the new source exists: the rebind and the transport it is given. Also the app's
// off-thread mesh loading (editor/MeshLoads.hpp) and undo routing (editor/UndoRouting.hpp).

#include "editor/MissingMedia.hpp"
#include "editor/PerformLock.hpp"
#include "editor/ProjectMapping.hpp"
#include "editor/ProjectReplace.hpp"
#include "editor/TextureResolve.hpp"
#include "editor/UndoRouting.hpp"
#include "project/ProjectSerializer.hpp"
#include "renderer/Framebuffer.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Texture.hpp"
#include "FakePlayable.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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

// TD-16: an object only the undo history holds (deleted), and an object media step,
// come back on the relocated source, not on the missing id nothing resolves any more.
TEST(MissingMediaTest, RebindMovesObjectsTheUndoHistoryHolds) {
    MissingFixture fx;
    Scene& scene = *fx.es.scenes[0];
    MeshObject* held = scene.adoptObject(std::make_unique<MeshObject>());
    held->getTexture().mediaId = fx.missingId;
    const UUID heldId = held->getId();
    fx.es.commandHistory.execute(
        std::make_unique<DeleteObjectCommand>(ObjectRef{&fx.es.quadResolver, scene.getId(), heldId}));
    // A media step on the fixture's object: None, so undo would put the missing id back.
    TextureBinding none = fx.object->getTexture();
    none.mediaId = UUID();
    fx.es.commandHistory.execute(makeSetObjectTextureCommand(
        ObjectRef{&fx.es.quadResolver, scene.getId(), fx.object->getId()}, none));

    const UUID newId = UUID::generate();
    rebindMissingSource(fx.es, fx.missingId, fx.oldPath, newId, "D:/found/clip.mp4", true);

    fx.es.commandHistory.undo();  // the media step
    EXPECT_EQ(fx.object->getTexture().mediaId, newId);
    fx.es.commandHistory.undo();  // the delete
    ASSERT_NE(scene.getObject(heldId), nullptr);
    EXPECT_EQ(scene.getObject(heldId)->getTexture().mediaId, newId);
    const auto referenced = referencedMediaIds(fx.es.scenes, fx.es.commandHistory);
    EXPECT_EQ(referenced.count(fx.missingId), 0u);
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

namespace {

namespace fs = std::filesystem;

// A file named like a video that holds bytes no container format reads.
fs::path writeJunkVideo(const std::string& name) {
    const fs::path path = fs::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (int i = 0; i < 4096; ++i) out.put(static_cast<char>((i * 131) & 0xff));
    return path;
}

// A WAV file named .mp4: a container FFmpeg reads, with sound and no video.
fs::path writeSoundOnlyVideo(const std::string& name) {
    const fs::path path = fs::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto u32 = [&out](uint32_t v) { for (int i = 0; i < 4; ++i) out.put(static_cast<char>((v >> (8 * i)) & 0xff)); };
    const auto u16 = [&out](uint16_t v) { out.put(static_cast<char>(v & 0xff)); out.put(static_cast<char>(v >> 8)); };
    const uint32_t samples = 8000, rate = 8000;
    out.write("RIFF", 4);
    u32(36 + samples * 2);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);  // PCM
    u16(1);  // mono
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(samples * 2);
    for (uint32_t i = 0; i < samples; ++i) u16(static_cast<uint16_t>((i * 97) & 0x3fff));
    return path;
}

}  // namespace

// A file that exists but is not a video (TD-35 review): relocation refuses it, as
// "accepted only if it loads" promises, rather than binding the quads to a source that
// will never show anything.
TEST(MissingMediaTest, RelocateRefusesAFileThatIsNotAVideo) {
    MissingFixture fx;
    SourceManager sm;
    const fs::path junk = writeJunkVideo("qm_relocate_not_a_video.mp4");
    const fs::path sound = writeSoundOnlyVideo("qm_relocate_sound_only.mp4");
    EXPECT_EQ(relocateMissingSource(fx.es, sm, fx.missingId, junk), "The file could not be loaded.");
    EXPECT_EQ(relocateMissingSource(fx.es, sm, fx.missingId, sound), "The file could not be loaded.");
    EXPECT_EQ(fx.es.missingSources.count(fx.missingId), 1u) << "still missing after a refusal";
    EXPECT_EQ(fx.a->getTexture().mediaId, fx.missingId);
    EXPECT_EQ(sm.getSourceCount(), 0u);
    std::error_code ec;
    fs::remove(junk, ec);
    fs::remove(sound, ec);
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

    releaseUnusedSources(es, sm, nullptr);  // nothing uses it

    EXPECT_FALSE(sm.hasSource(id));
    ASSERT_EQ(es.releasedTransports.count(id), 1u);
    const Project::TransportData& t = es.releasedTransports[id];
    EXPECT_FALSE(t.playing);
    EXPECT_TRUE(t.timelineLocked);
    EXPECT_FLOAT_EQ(t.speed, 0.5f);

    clearProjectContent(es, &sm, nullptr, nullptr);  // a new project forgets them
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
    releaseUnusedSources(fx.es, fx.sm, nullptr);  // on screen: untouched
    EXPECT_EQ(fx.clip->state, PlaybackState::PLAYING);
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());

    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm, nullptr);
    ASSERT_TRUE(fx.sm.hasSource(fx.clipId));  // undo may still need it
    EXPECT_EQ(fx.clip->state, PlaybackState::PAUSED);
    EXPECT_EQ(fx.es.sourcesPausedWhileUnseen.count(fx.clipId), 1u);

    releaseUnusedSources(fx.es, fx.sm, nullptr);  // a later pass changes nothing
    EXPECT_EQ(fx.clip->pauses, 1);

    ASSERT_TRUE(fx.es.commandHistory.undo());
    releaseUnusedSources(fx.es, fx.sm, nullptr);
    EXPECT_EQ(fx.clip->state, PlaybackState::PLAYING);
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());
}

TEST(PauseWhileUnseenTest, AClipTheUserPausedStaysPausedThroughDeleteAndUndo) {
    UnseenFixture fx;
    fx.clip->pause();
    const int plays = fx.clip->plays;
    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm, nullptr);
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());

    ASSERT_TRUE(fx.es.commandHistory.undo());
    releaseUnusedSources(fx.es, fx.sm, nullptr);
    EXPECT_EQ(fx.clip->state, PlaybackState::PAUSED);
    EXPECT_EQ(fx.clip->plays, plays);  // never resumed behind the user's back
}

TEST(PauseWhileUnseenTest, TheVideoPlayersClipKeepsPlayingWithItsQuadDeleted) {
    UnseenFixture fx;
    fx.es.activeVideoSourceId = fx.clipId;
    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm, nullptr);
    EXPECT_EQ(fx.clip->state, PlaybackState::PLAYING);
    EXPECT_TRUE(fx.es.sourcesPausedWhileUnseen.empty());
}

TEST(PauseWhileUnseenTest, AClipNoUndoCanRestoreIsReleasedAndForgotten) {
    UnseenFixture fx;
    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm, nullptr);
    ASSERT_EQ(fx.es.sourcesPausedWhileUnseen.count(fx.clipId), 1u);

    fx.es.commandHistory.clear();
    releaseUnusedSources(fx.es, fx.sm, nullptr);
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

    const UUID playerId = fx.sm.registerSource(std::make_unique<FakePlayable>("player.mp4"));
    fx.es.activeVideoSourceId = playerId;  // the Video Player's clip is project media
    fx.es.quadTexturePaths[fx.quad->getId()] = "C:/show/clip.mp4";
    fx.es.deleteSelectedQuads();
    releaseUnusedSources(fx.es, fx.sm, &library);  // the clip is paused, held by undo only
    ASSERT_EQ(fx.es.sourcesPausedWhileUnseen.count(fx.clipId), 1u);
    fx.es.releasedTransports[UUID::generate()] = Project::TransportData{};
    Project::SourceData missing;
    missing.id = UUID::generate();
    missing.path = "C:/show/gone.mp4";
    fx.es.missingSources.emplace(missing.id, missing);

    clearProjectContent(fx.es, &fx.sm, nullptr, &library);

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
}

// The media bin's hover preview is an open clip no quad is bound to: it survives the
// frame boundary while hovered; once its hover ends it is removed unless something
// bound it meanwhile, and a bound preview is handed over (forgotten, not removed).
TEST(MediaPreviewTest, AnEndedPreviewIsRemovedUnlessBoundAndABoundOneIsHandedOver) {
    UnseenFixture fx;
    const UUID previewId = fx.sm.registerSource(std::make_unique<FakePlayable>("preview.mp4"));
    fx.es.previewSourceId = previewId;
    fx.es.previewSourcePath = "C:/bin/preview.mp4";
    releaseUnusedSources(fx.es, fx.sm, nullptr);
    EXPECT_TRUE(fx.sm.hasSource(previewId)) << "a hovered preview stays";

    fx.es.endedPreviews.push_back(fx.es.previewSourceId);  // the hover moved away
    fx.es.previewSourceId = UUID();
    fx.es.previewSourcePath.clear();
    releaseUnusedSources(fx.es, fx.sm, nullptr);
    EXPECT_FALSE(fx.sm.hasSource(previewId));
    EXPECT_TRUE(fx.es.endedPreviews.empty());

    // A preview an assignment bound (the quad now shows it) belongs to the quad.
    fx.es.previewSourceId = fx.clipId;
    fx.es.previewSourcePath = "C:/bin/clip.mp4";
    releaseUnusedSources(fx.es, fx.sm, nullptr);
    EXPECT_TRUE(fx.sm.hasSource(fx.clipId));
    EXPECT_TRUE(fx.es.previewSourceId.isNull());
    EXPECT_TRUE(fx.es.previewSourcePath.empty());
}

// ---- Undo while calibrating (TD-17, editor/UndoRouting.hpp) ------------------------------

namespace {

// The editor with one or two headless outputs (no windows: the manager is not
// initialised) and a quad edit made before calibration starts.
struct RoutingFixture {
    EditorState es;
    OutputManager om;
    OutputWindow* a = nullptr;
    OutputWindow* b = nullptr;
    Quad* quad = nullptr;

    RoutingFixture() {
        es.outputResolver.setOutputManager(&om);
        a = om.createOutput("A");
        b = om.createOutput("B");
        for (OutputWindow* o : {a, b}) {
            CalibrationPoint p;
            p.world = Vec3(0.0f);
            p.pixel = Vec2(100.0f, 100.0f);
            o->getCalibrationPoints() = {p};
        }
        quad = es.activeScene().createQuad();
        es.commandHistory.execute(std::make_unique<SetQuadOpacityCommand>(es.quadRef(*quad), 0.5f));
    }

    // Calibrate `output`, move its point 1 by `dx` and record the step as the panel does.
    void calibrationStep(OutputWindow& output, float dx) {
        if (es.calibratingOutputId != output.getId()) {
            es.beginCalibration(output);
        }
        output.getCalibrationPoints()[0].pixel.x += dx;
        es.calibration.noteEdit("Move calibration point 1", CalibrationSession::moveKey(0));
        es.calibration.markChanged();
        es.calibration.update(output.getCalibrationPoints(), output.getProjector());
        if (auto edit = es.calibration.finishFrame(output.getCalibrationPoints(), output.getProjector())) {
            es.commandHistory.execute(
                std::make_unique<CalibrationEditCommand>(es.outputRef(output.getId()), std::move(*edit)));
        }
    }
};

}  // namespace

TEST(UndoRoutingTest, CalibratingUndoesOnlyThatOutputsCalibrationSteps) {
    RoutingFixture fx;
    fx.calibrationStep(*fx.a, 10.0f);
    ASSERT_EQ(fx.es.commandHistory.getUndoCount(), 2u);

    EXPECT_TRUE(canUndoStep(fx.es));
    ASSERT_TRUE(undoStep(fx.es));
    EXPECT_FLOAT_EQ(fx.a->getCalibrationPoints()[0].pixel.x, 100.0f);

    // Next is the quad edit from before calibration: held back, and said so.
    EXPECT_FALSE(canUndoStep(fx.es));
    std::string heldBack;
    EXPECT_FALSE(undoStep(fx.es, &heldBack));
    EXPECT_EQ(heldBack, "Change opacity");
    EXPECT_FLOAT_EQ(fx.quad->getOpacity(), 0.5f);
    EXPECT_EQ(fx.es.commandHistory.getUndoCount(), 1u);  // not consumed

    // Leaving calibration: the history works in order again.
    fx.es.endCalibration();
    ASSERT_TRUE(undoStep(fx.es));
    EXPECT_FLOAT_EQ(fx.quad->getOpacity(), 1.0f);

    // Redo: the quad step first, which calibration holds back.
    fx.es.beginCalibration(*fx.a);
    EXPECT_FALSE(canRedoStep(fx.es));
    EXPECT_FALSE(redoStep(fx.es, &heldBack));
    fx.es.endCalibration();
    ASSERT_TRUE(redoStep(fx.es));
    fx.es.beginCalibration(*fx.a);
    EXPECT_TRUE(canRedoStep(fx.es));
    ASSERT_TRUE(redoStep(fx.es));
    EXPECT_FLOAT_EQ(fx.a->getCalibrationPoints()[0].pixel.x, 110.0f);
}

TEST(UndoRoutingTest, AnotherOutputsCalibrationStepIsHeldBack) {
    RoutingFixture fx;
    fx.calibrationStep(*fx.a, 10.0f);
    fx.es.endCalibration();
    fx.es.beginCalibration(*fx.b);
    std::string heldBack;
    EXPECT_FALSE(undoStep(fx.es, &heldBack));
    EXPECT_EQ(heldBack, "Move calibration point 1");
    EXPECT_FLOAT_EQ(fx.a->getCalibrationPoints()[0].pixel.x, 110.0f);
}

TEST(UndoRoutingTest, OutsideCalibrationAStepAppliesToItsOutputById) {
    RoutingFixture fx;
    fx.calibrationStep(*fx.a, 10.0f);
    fx.es.endCalibration();
    ASSERT_TRUE(undoStep(fx.es));
    EXPECT_FLOAT_EQ(fx.a->getCalibrationPoints()[0].pixel.x, 100.0f);
    EXPECT_FLOAT_EQ(fx.b->getCalibrationPoints()[0].pixel.x, 100.0f);
    ASSERT_TRUE(redoStep(fx.es));
    EXPECT_FLOAT_EQ(fx.a->getCalibrationPoints()[0].pixel.x, 110.0f);
}

TEST(UndoRoutingTest, UndoAfterTheOutputWasDeletedAndReCreatedIsANoOp) {
    RoutingFixture fx;
    fx.calibrationStep(*fx.a, 10.0f);
    fx.es.endCalibration();
    const UUID gone = fx.a->getId();
    fx.om.deleteOutput(gone);
    OutputWindow* again = fx.om.createOutput("A");  // same name, new id
    CalibrationPoint p;
    p.pixel = Vec2(5.0f, 5.0f);
    again->getCalibrationPoints() = {p};

    EXPECT_TRUE(canUndoStep(fx.es));
    EXPECT_FALSE(undoStep(fx.es));  // resolved to nothing; consumed, no crash
    EXPECT_FLOAT_EQ(again->getCalibrationPoints()[0].pixel.x, 5.0f);
    ASSERT_TRUE(undoStep(fx.es));   // the quad step still works
    EXPECT_FLOAT_EQ(fx.quad->getOpacity(), 1.0f);
    ASSERT_TRUE(redoStep(fx.es));   // the quad step
    EXPECT_FALSE(redoStep(fx.es));  // the gone output's step: a no-op again
    EXPECT_FLOAT_EQ(again->getCalibrationPoints()[0].pixel.x, 5.0f);
    EXPECT_FALSE(fx.es.commandHistory.canRedo());
}

// ---- Meshes parsed off the render thread (TD-16a, editor/MeshLoads.hpp) -----------------

namespace {

namespace fs = std::filesystem;

struct MeshLoadFixture {
    EditorState es;
    fs::path dir = fs::temp_directory_path() / ("qm_meshloads_" + UUID::generate().toString());

    MeshLoadFixture() { fs::create_directories(dir); }
    ~MeshLoadFixture() {
        es.objLoads.cancelAll();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::string writeObj(const std::string& name, int triangles = 1) {
        const fs::path file = dir / name;
        std::ofstream out(file, std::ios::binary);
        for (int i = 0; i < triangles; ++i) {
            out << "v " << i << " 0 0\nv " << i + 1 << " 0 0\nv " << i << " 1 0\n";
        }
        for (int i = 0; i < triangles; ++i) {
            out << "f " << 3 * i + 1 << ' ' << 3 * i + 2 << ' ' << 3 * i + 3 << '\n';
        }
        return utf8FromPath(file);
    }

    // Run the frame-boundary step until nothing is being parsed (30 s at most).
    std::vector<MeshImportOutcome> adoptAll() {
        std::vector<MeshImportOutcome> outcomes;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& o : adoptLoadedMeshes(es)) outcomes.push_back(std::move(o));
            if (es.meshLoads.tickets.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return outcomes;
    }

    void waitIdle() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (es.objLoads.pendingCount() > 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // An object as a project load makes it: its file named, its mesh not loaded yet.
    MeshObject* savedObject(Scene& scene, const std::string& path) {
        MeshObject* object = scene.createObject("Saved");
        object->setSourcePath(path);
        return object;
    }
};

}  // namespace

TEST(MeshLoadsTest, ImportShowsLoadingThenBecomesOneUndoStep) {
    MeshLoadFixture fx;
    const std::string path = fx.writeObj("prop.obj", 3);
    MeshObject* placeholder = beginObjectImport(fx.es, "prop", path);
    const UUID id = placeholder->getId();
    Scene& scene = fx.es.activeScene();

    EXPECT_TRUE(isMeshLoading(fx.es, *placeholder));
    EXPECT_FALSE(isMeshMissing(fx.es, *placeholder));  // not offered for relocation
    EXPECT_TRUE(collectMissingMedia(fx.es).empty());
    EXPECT_FALSE(fx.es.commandHistory.canUndo());  // not an undo step yet

    const auto outcomes = fx.adoptAll();
    ASSERT_EQ(outcomes.size(), 1u);
    EXPECT_TRUE(outcomes[0].import);
    EXPECT_TRUE(outcomes[0].ok);
    EXPECT_EQ(outcomes[0].objectId, id);
    MeshObject* object = scene.getObject(id);  // same id: selection and edits carry over
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->getMesh().triangleCount(), 3u);
    EXPECT_FALSE(isMeshLoading(fx.es, *object));
    EXPECT_EQ(fx.es.commandHistory.getUndoDescription(), "Import 3D object");

    ASSERT_TRUE(fx.es.commandHistory.undo());
    EXPECT_EQ(scene.getObject(id), nullptr);
    ASSERT_TRUE(fx.es.commandHistory.redo());
    ASSERT_NE(scene.getObject(id), nullptr);
    EXPECT_EQ(scene.getObject(id)->getMesh().triangleCount(), 3u);
}

// The perform lock (ADR-051 §4) taken while an import parses: the history is read-only,
// so the object stays in the scene with its mesh but without an undo step.
TEST(MeshLoadsTest, ImportFinishingUnderThePerformLockKeepsItsObject) {
    MeshLoadFixture fx;
    const UUID id = beginObjectImport(fx.es, "prop", fx.writeObj("prop.obj", 2))->getId();
    lockPerform(fx.es);

    const auto outcomes = fx.adoptAll();
    ASSERT_EQ(outcomes.size(), 1u);
    EXPECT_TRUE(outcomes[0].ok);
    const MeshObject* object = fx.es.activeScene().getObject(id);
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->getMesh().triangleCount(), 2u);
    EXPECT_FALSE(isMeshLoading(fx.es, *object));
    EXPECT_FALSE(fx.es.commandHistory.canUndo());
}

// A finished import frames the uncalibrated projectors on it, except under the perform
// lock: the object imported before locking stays, and nothing on air moves.
TEST(MeshLoadsTest, AnImportFinishingUnderThePerformLockDoesNotReAimProjectors) {
    MeshLoadFixture fx;
    OutputManager om;
    OutputWindow* output = om.createOutput("Projector");
    OutputManager::showScene(*output, &fx.es.activeScene(), fx.es.scenes);
    const auto adopt = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            adoptObjectMeshes(fx.es, &om);
            if (fx.es.meshLoads.tickets.empty()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (std::chrono::steady_clock::now() < deadline);
    };

    const Vec3 aimed = output->getProjector().position();
    const UUID id = beginObjectImport(fx.es, "prop", fx.writeObj("prop.obj", 2))->getId();
    lockPerform(fx.es);
    adopt();
    ASSERT_NE(fx.es.activeScene().getObject(id), nullptr) << "the object stays";
    EXPECT_EQ(output->getProjector().position(), aimed) << "the projector did not move";
    EXPECT_NE(std::string(fx.es.statusMessage).find("not re-aimed"), std::string::npos)
        << fx.es.statusMessage;

    // Unlocked, the same import frames the projector on it.
    requestPerformUnlock(fx.es);
    ASSERT_TRUE(confirmPerformUnlock(fx.es));
    beginObjectImport(fx.es, "prop2", fx.writeObj("prop2.obj", 2));
    adopt();
    EXPECT_NE(output->getProjector().position(), aimed);
}

TEST(MeshLoadsTest, ImportKeepsItsPlaceAndEditsMadeWhileLoading) {
    MeshLoadFixture fx;
    Scene& scene = fx.es.activeScene();
    scene.createObject("Before");
    MeshObject* placeholder = beginObjectImport(fx.es, "prop", fx.writeObj("prop.obj"));
    scene.createObject("After");
    const UUID id = placeholder->getId();
    placeholder->setOpacity(0.3f);  // an edit while it loads

    fx.adoptAll();
    ASSERT_EQ(scene.getObjects().size(), 3u);
    EXPECT_EQ(scene.getObjects()[1]->getId(), id);
    EXPECT_FLOAT_EQ(scene.getObjects()[1]->getOpacity(), 0.3f);
}

TEST(MeshLoadsTest, FailedImportRemovesThePlaceholderAndReportsWhy) {
    MeshLoadFixture fx;
    const std::string path = utf8FromPath(fx.dir / "absent.obj");
    MeshObject* placeholder = beginObjectImport(fx.es, "absent", path);
    const UUID id = placeholder->getId();

    const auto outcomes = fx.adoptAll();
    ASSERT_EQ(outcomes.size(), 1u);
    EXPECT_FALSE(outcomes[0].ok);
    EXPECT_FALSE(outcomes[0].error.empty());
    EXPECT_EQ(fx.es.activeScene().getObject(id), nullptr);
    EXPECT_FALSE(fx.es.commandHistory.canUndo());
    EXPECT_TRUE(collectMissingMedia(fx.es).empty());

    // Importing it again tries again (the earlier failure is not remembered for it).
    fx.writeObj("absent.obj");
    MeshObject* again = beginObjectImport(fx.es, "absent", path);
    const UUID againId = again->getId();
    const auto retry = fx.adoptAll();
    ASSERT_EQ(retry.size(), 1u);
    EXPECT_TRUE(retry[0].ok);
    EXPECT_NE(fx.es.activeScene().getObject(againId), nullptr);
}

TEST(MeshLoadsTest, SavedObjectsLoadInPlaceAndShareOneParsePerFile) {
    MeshLoadFixture fx;
    const std::string shared = fx.writeObj("column.obj", 2);
    Scene& scene = fx.es.activeScene();
    MeshObject* a = fx.savedObject(scene, shared);
    MeshObject* b = fx.savedObject(scene, shared);
    auto other = std::make_unique<Scene>();
    MeshObject* c = fx.savedObject(*other, shared);
    fx.es.scenes.push_back(std::move(other));

    requestMeshLoads(fx.es);
    EXPECT_EQ(fx.es.meshLoads.tickets.size(), 1u);
    EXPECT_TRUE(isMeshLoading(fx.es, *a));
    EXPECT_TRUE(isMeshLoading(fx.es, *c));

    const auto outcomes = fx.adoptAll();
    EXPECT_TRUE(outcomes.empty());  // not imports
    for (MeshObject* o : {a, b, c}) EXPECT_EQ(o->getMesh().triangleCount(), 2u);
    EXPECT_FALSE(fx.es.commandHistory.canUndo());  // loading is not an edit
}

TEST(MeshLoadsTest, ABadSavedMeshIsMissingAndNotRetriedEveryFrame) {
    MeshLoadFixture fx;
    const fs::path bad = fx.dir / "bad.obj";
    std::ofstream(bad, std::ios::binary) << "v 0 0\n";
    MeshObject* object = fx.savedObject(fx.es.activeScene(), utf8FromPath(bad));

    requestMeshLoads(fx.es);
    const auto outcomes = fx.adoptAll();
    ASSERT_EQ(outcomes.size(), 1u);  // reported, as a show's object, not an import
    EXPECT_FALSE(outcomes[0].import);
    EXPECT_FALSE(outcomes[0].ok);
    EXPECT_EQ(outcomes[0].objectId, object->getId());
    EXPECT_FALSE(outcomes[0].error.empty());
    EXPECT_TRUE(object->getMesh().empty());
    EXPECT_TRUE(isMeshMissing(fx.es, *object));
    ASSERT_EQ(collectMissingMedia(fx.es).size(), 1u);
    EXPECT_EQ(fx.es.meshLoads.errors.count(utf8FromPath(bad)), 1u);

    requestMeshLoads(fx.es);  // the next frame
    EXPECT_TRUE(fx.es.meshLoads.tickets.empty());
}

TEST(MeshLoadsTest, AnObjectRestoredAfterItsLoadFinishedLoadsAgain) {
    MeshLoadFixture fx;
    const std::string path = fx.writeObj("prop.obj");
    Scene& scene = fx.es.activeScene();
    MeshObject* object = fx.savedObject(scene, path);
    const UUID id = object->getId();
    requestMeshLoads(fx.es);

    // Deleted while loading; the parse finishes with nobody waiting for it.
    fx.es.commandHistory.execute(
        std::make_unique<DeleteObjectCommand>(ObjectRef{&fx.es.quadResolver, scene.getId(), id}));
    fx.adoptAll();
    // Undo brings back the meshless object; the next frame boundary loads it.
    ASSERT_TRUE(fx.es.commandHistory.undo());
    ASSERT_NE(scene.getObject(id), nullptr);
    EXPECT_FALSE(isMeshLoading(fx.es, *scene.getObject(id)));
    requestMeshLoads(fx.es);
    EXPECT_TRUE(isMeshLoading(fx.es, *scene.getObject(id)));
    fx.adoptAll();
    EXPECT_EQ(scene.getObject(id)->getMesh().triangleCount(), 1u);
}

TEST(MeshLoadsTest, APlaceholderDeletedAndRestoredWhileLoadingStillImports) {
    MeshLoadFixture fx;
    Scene& scene = fx.es.activeScene();
    MeshObject* placeholder = beginObjectImport(fx.es, "prop", fx.writeObj("prop.obj"));
    const UUID id = placeholder->getId();
    fx.es.commandHistory.execute(
        std::make_unique<DeleteObjectCommand>(ObjectRef{&fx.es.quadResolver, scene.getId(), id}));
    fx.es.commandHistory.undo();

    const auto outcomes = fx.adoptAll();
    ASSERT_EQ(outcomes.size(), 1u);
    EXPECT_TRUE(outcomes[0].ok);
    ASSERT_NE(scene.getObject(id), nullptr);
    EXPECT_EQ(fx.es.commandHistory.getUndoDescription(), "Import 3D object");
}

TEST(MeshLoadsTest, NewOrLoadCancelsEveryParse) {
    MeshLoadFixture fx;
    beginObjectImport(fx.es, "prop", fx.writeObj("prop.obj", 50));
    clearProjectContent(fx.es, nullptr, nullptr, nullptr);  // what New, Load and recovery run

    EXPECT_TRUE(fx.es.meshLoads.tickets.empty());
    EXPECT_TRUE(fx.es.meshLoads.imports.empty());
    fx.waitIdle();
    EXPECT_TRUE(adoptLoadedMeshes(fx.es).empty());
    EXPECT_TRUE(fx.es.objLoads.takeCompleted().empty());
}

TEST(MeshLoadsTest, AStaleResultForAPathLoadedAgainIsIgnored) {
    MeshLoadFixture fx;
    const std::string path = fx.writeObj("prop.obj");
    MeshObject* object = fx.savedObject(fx.es.activeScene(), path);
    requestMeshLoads(fx.es);
    const uint64_t first = fx.es.meshLoads.tickets.at(path);
    // As if the path had been requested again since: the first ticket's result must
    // not be adopted as the second's.
    fx.es.meshLoads.tickets[path] = first + 1000;
    fx.waitIdle();
    adoptLoadedMeshes(fx.es);
    EXPECT_TRUE(object->getMesh().empty());
    EXPECT_EQ(fx.es.meshLoads.tickets.count(path), 1u);
}

TEST(MeshLoadsTest, RelocatingAMissingMeshLoadsOnTheWorkerAndMovesItsObjects) {
    MeshLoadFixture fx;
    const std::string gone = utf8FromPath(fx.dir / "gone" / "prop.obj");
    Scene& scene = fx.es.activeScene();
    MeshObject* a = fx.savedObject(scene, gone);
    MeshObject* b = fx.savedObject(scene, gone);
    requestMeshLoads(fx.es);
    fx.adoptAll();  // the file is not there
    ASSERT_TRUE(isMeshMissing(fx.es, *a));
    ASSERT_EQ(collectMissingMedia(fx.es).size(), 1u);
    const MissingMediaItem item = collectMissingMedia(fx.es)[0];

    const std::string found = fx.writeObj("prop.obj", 4);
    EXPECT_EQ(locateMissingMedia(fx.es, nullptr, item, pathFromUtf8(found)), 1);
    // Accepted, not loaded yet: loading, not missing, and not parsed on this thread.
    EXPECT_TRUE(a->getMesh().empty());
    EXPECT_TRUE(isMeshLoading(fx.es, *a));
    EXPECT_TRUE(collectMissingMedia(fx.es).empty());

    fx.adoptAll();
    for (MeshObject* o : {a, b}) {
        EXPECT_EQ(o->getMesh().triangleCount(), 4u);
        EXPECT_EQ(pathFromUtf8(o->getSourcePath()), pathFromUtf8(found));
    }
    EXPECT_TRUE(fx.es.meshLoads.relocations.empty());
    EXPECT_FALSE(fx.es.commandHistory.canUndo());
}

TEST(MeshLoadsTest, ARelocationToABadFileLeavesTheMeshMissingWithTheReason) {
    MeshLoadFixture fx;
    const std::string gone = utf8FromPath(fx.dir / "gone" / "prop.obj");
    MeshObject* object = fx.savedObject(fx.es.activeScene(), gone);
    requestMeshLoads(fx.es);
    fx.adoptAll();
    const MissingMediaItem item = collectMissingMedia(fx.es).at(0);

    const fs::path bad = fx.dir / "bad.obj";
    std::ofstream(bad, std::ios::binary) << "f 1 2 3\n";
    EXPECT_EQ(relocateMissingItem(fx.es, nullptr, item, bad), "");
    const auto outcomes = fx.adoptAll();
    ASSERT_EQ(outcomes.size(), 1u);
    EXPECT_FALSE(outcomes[0].ok);
    EXPECT_TRUE(outcomes[0].objectId.isNull());
    EXPECT_EQ(object->getSourcePath(), gone);  // still where it was, still missing
    EXPECT_TRUE(isMeshMissing(fx.es, *object));
    ASSERT_EQ(fx.es.missingMediaErrors.count(gone), 1u);
    EXPECT_FALSE(fx.es.missingMediaErrors[gone].empty());

    // Nothing missing at a path: refused at once.
    EXPECT_EQ(relocateMissingMesh(fx.es, "C:/not/missing.obj", bad), "It is no longer missing.");
}

TEST(MeshLoadsTest, NewOrLoadDropsARelocationInProgress) {
    MeshLoadFixture fx;
    const std::string gone = utf8FromPath(fx.dir / "gone" / "prop.obj");
    fx.savedObject(fx.es.activeScene(), gone);
    requestMeshLoads(fx.es);
    fx.adoptAll();
    const MissingMediaItem item = collectMissingMedia(fx.es).at(0);
    ASSERT_EQ(relocateMissingItem(fx.es, nullptr, item, pathFromUtf8(fx.writeObj("prop.obj", 50))), "");
    clearProjectContent(fx.es, nullptr, nullptr, nullptr);
    EXPECT_TRUE(fx.es.meshLoads.relocations.empty());
    fx.waitIdle();
    EXPECT_TRUE(adoptLoadedMeshes(fx.es).empty());
}

// ---- Delete key on a selected 3D object (EditorState::deleteSelectedObject) ----------

TEST(DeleteKeyTest, DeletesTheSelectedObjectAsAnUndoStep) {
    EditorState es;
    Scene& scene = es.activeScene();
    MeshObject* object = scene.createObject("Prop");
    const UUID id = object->getId();
    es.selectedObjectId = id;
    es.currentSelectionType = EditorState::SelectionType::Object;

    ASSERT_TRUE(es.deleteSelectedObject());
    EXPECT_EQ(scene.getObject(id), nullptr);
    EXPECT_TRUE(es.selectedObjectId.isNull());
    EXPECT_EQ(es.commandHistory.getUndoDescription(), "Delete 3D object");
    EXPECT_FALSE(es.deleteSelectedObject());  // nothing selected any more

    ASSERT_TRUE(es.commandHistory.undo());
    EXPECT_NE(scene.getObject(id), nullptr);
}

TEST(DeleteKeyTest, LeavesObjectsAloneWhenTheyAreNotTheSelection) {
    EditorState es;
    Scene& scene = es.activeScene();
    MeshObject* object = scene.createObject("Prop");
    const UUID id = object->getId();
    es.selectedObjectId = id;

    es.currentSelectionType = EditorState::SelectionType::Quad;  // e.g. a quad picked since
    EXPECT_FALSE(es.deleteSelectedObject());
    es.currentSelectionType = EditorState::SelectionType::Output;
    EXPECT_FALSE(es.deleteSelectedObject());

    // Object selection, but a quad selected on the canvas after it: the quad is newer.
    es.currentSelectionType = EditorState::SelectionType::Object;
    Quad* quad = scene.createQuad();
    scene.getQuadManager().selectQuad(quad->getId());
    EXPECT_FALSE(es.deleteSelectedObject());
    scene.getQuadManager().deselectAll();

    // A stale selection (the object is gone, or in another scene) deletes nothing.
    scene.deleteObject(id);
    EXPECT_FALSE(es.deleteSelectedObject());
    EXPECT_FALSE(es.commandHistory.canUndo());
}

// ---- Clips opening off the main thread (TD-35 review) --------------------------------

namespace {

// Hidden-window GL 3.3 context, RAII; ok == false means no GL (the tests skip).
struct HiddenGl {
    GLFWwindow* win = nullptr;
    bool ok = false;
    HiddenGl() {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        win = glfwCreateWindow(64, 64, "missing-media-test", nullptr, nullptr);
        glfwDefaultWindowHints();
        if (!win) { glfwTerminate(); return; }
        glfwMakeContextCurrent(win);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(win); win = nullptr; glfwTerminate(); return;
        }
        ok = true;
    }
    ~HiddenGl() { if (win) { glfwDestroyWindow(win); glfwTerminate(); } }
    HiddenGl(const HiddenGl&) = delete;
    HiddenGl& operator=(const HiddenGl&) = delete;
};

// A real H.264 clip (a checked-in fixture), copied so the test owns its file.
fs::path copyFixtureClip(const std::string& name) {
    const fs::path from = fs::path(QM_PROJECT_FIXTURES_DIR) / ".." / "videos" / "h264_closed_gop.mp4";
    const fs::path to = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return ec ? fs::path() : to;
}

// One scene with one quad over the whole canvas.
Quad* addFullQuad(EditorState& es) {
    auto scene = std::make_unique<Scene>();
    Quad* quad = scene->getQuadManager().createQuad("Q");
    quad->getGeometry().setCorners({Vec2(0.0f, 0.0f), Vec2(1.0f, 0.0f), Vec2(1.0f, 1.0f), Vec2(0.0f, 1.0f)});
    es.scenes.push_back(std::move(scene));
    return quad;
}

// What the renderer draws in the middle of a 16x16 frame for `quad` with `texture`
// (null: none resolved), on a grey background.
std::array<int, 3> renderedCentre(Renderer& renderer, const Quad& quad, const Texture* texture) {
    Framebuffer frame;
    EXPECT_TRUE(frame.resize(16, 16));
    frame.bind();
    glViewport(0, 0, 16, 16);
    glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderer.renderQuad(quad, texture);
    unsigned char px[4] = {0, 0, 0, 0};
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    Framebuffer::unbind();
    return {px[0], px[1], px[2]};
}

}  // namespace

// A clip still opening has no picture yet (0x0, no texture). Its quads drew the
// renderer's white default texture, so every video surface glowed white on the
// projectors for as long as the opens took (seconds at a project load). They are black.
TEST(OpeningMediaTest, AQuadOnAClipStillOpeningRendersBlackNeverWhite) {
    HiddenGl gl;
    if (!gl.ok) GTEST_SKIP() << "no GL 3.3 context";
    const fs::path clip = copyFixtureClip("qm_opening_clip.mp4");
    ASSERT_FALSE(clip.empty());
    EditorState es;
    Quad* quad = addFullQuad(es);
    SourceManager sm;
    Renderer renderer;
    ASSERT_TRUE(renderer.initialize());

    const UUID id = sm.createVideoSource(clip);
    ASSERT_FALSE(id.isNull());
    quad->getTexture().mediaId = id;
    ASSERT_FALSE(sm.getSource(id)->isReady()) << "opening until updateAll() adopts its decoder";
    for (SurfaceView view : {SurfaceView::Output, SurfaceView::Editor}) {
        const auto map = buildQuadTextureMap(es, &sm, view);
        const auto it = map.find(quad->getId());
        const std::array<int, 3> rgb = renderedCentre(renderer, *quad, it != map.end() ? it->second : nullptr);
        const char* where = view == SurfaceView::Output ? "output" : "editor";
        EXPECT_LE(rgb[0] + rgb[1] + rgb[2], 6) << where << ": " << rgb[0] << "," << rgb[1] << "," << rgb[2];
    }
    sm.finishBackgroundWork();
    std::error_code ec;
    fs::remove(clip, ec);
}

// A clip that passed the header probe but whose decoder failed off the main thread (an
// exception here, standing in for a codec FFmpeg cannot open) is missing media: black
// on outputs, listed in Missing Media, and saved -> loaded -> saved with the transport
// it was given (it was written back stopped: the failed open reset it).
TEST(UndecodableMediaTest, AClipThatFailsToOpenBecomesMissingMediaAndKeepsItsTransport) {
    HiddenGl gl;
    if (!gl.ok) GTEST_SKIP() << "no GL 3.3 context";
    const fs::path clip = copyFixtureClip("qm_undecodable_clip.mp4");
    ASSERT_FALSE(clip.empty());
    EditorState es;
    Quad* quad = addFullQuad(es);
    SourceManager sm;
    sm.setOpenHookForTesting([](const fs::path&) { throw std::runtime_error("no decoder for this stream"); });

    const UUID id = sm.getOrCreateVideoSource(clip);
    ASSERT_FALSE(id.isNull());
    quad->getTexture().mediaId = id;
    es.quadTexturePaths[quad->getId()] = utf8FromPath(clip);
    Project::TransportData saved;
    saved.playing = true;
    saved.loopMode = LoopMode::PING_PONG;
    saved.speed = 1.5f;
    applySavedTransport(sm, id, saved);
    sm.finishBackgroundWork();
    adoptFailedOpens(es, sm);

    EXPECT_FALSE(sm.hasSource(id)) << "its empty source is removed";
    ASSERT_EQ(es.missingSources.count(id), 1u);
    const std::vector<MissingMediaItem> items = collectMissingMedia(es);
    ASSERT_EQ(items.size(), 1u) << "the Missing Media window lists it";
    EXPECT_EQ(items.front().sourceId, id);
    EXPECT_EQ(items.front().quadUses, 1);
    const auto map = buildQuadTextureMap(es, &sm, SurfaceView::Output);
    ASSERT_EQ(map.count(quad->getId()), 1u);
    EXPECT_EQ(map.at(quad->getId()), es.missingBlackTexture.get()) << "black on the projectors";
    EXPECT_TRUE(sm.takeFailedOpens().empty());

    const auto expectSaved = [&saved](const Project& project, const char* when) {
        ASSERT_EQ(project.getSources().size(), 1u) << when;
        const Project::SourceData& source = project.getSources().front();
        EXPECT_EQ(source.kind, "video") << when;
        ASSERT_TRUE(source.transport.has_value()) << when;
        EXPECT_EQ(source.transport->playing, saved.playing) << when;
        EXPECT_EQ(source.transport->loopMode, saved.loopMode) << when;
        EXPECT_FLOAT_EQ(source.transport->speed, saved.speed) << when;
    };
    Project first;
    syncEditorToProject(es, nullptr, &sm, first);
    expectSaved(first, "first save");
    const fs::path file = fs::temp_directory_path() / "qm_undecodable_roundtrip.qmap";
    ASSERT_TRUE(ProjectSerializer::save(first, file));
    auto reloaded = ProjectSerializer::load(file);
    ASSERT_TRUE(reloaded);
    EditorState loaded;
    SourceManager loadedSources;  // the file exists, so it is tried again: still undecodable
    loadedSources.setOpenHookForTesting([](const fs::path&) { throw std::runtime_error("no decoder for this stream"); });
    syncProjectToEditor(loaded, nullptr, reloaded.value(), &loadedSources, nullptr);
    loadedSources.finishBackgroundWork();
    adoptFailedOpens(loaded, loadedSources);
    EXPECT_EQ(collectMissingMedia(loaded).size(), 1u);
    Project second;
    syncEditorToProject(loaded, nullptr, &loadedSources, second);
    expectSaved(second, "save after load");

    std::error_code ec;
    fs::remove(file, ec);
    fs::remove(clip, ec);
}
