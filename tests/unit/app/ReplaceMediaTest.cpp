// Replace in show (app/ReplaceMedia.hpp, QM-082) on a real EditorState: what shows the
// original clip moves to the converted one as one undo step, and undo/redo touch only
// what that step moved. Opening a real file needs GL, so the command is driven with ids.

#include "app/ReplaceMedia.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace quadmapper;

namespace {

std::string sourceTarget(const UUID& id) { return "/qm/source/" + id.toString() + "/restart"; }

struct ReplaceFixture {
    EditorState es;
    UUID oldId = UUID::generate();
    UUID otherId = UUID::generate();
    UUID newId = UUID::generate();
    const std::string oldPath = "C:/show/clip.mp4";
    const std::string newPath = "C:/show/clip_hap.mov";
    Quad* a = nullptr;
    Quad* b = nullptr;
    Quad* c = nullptr;
    MeshObject* object = nullptr;

    ReplaceFixture() {
        auto scene = std::make_unique<Scene>();
        a = scene->getQuadManager().createQuad("A");
        b = scene->getQuadManager().createQuad("B");
        c = scene->getQuadManager().createQuad("C");
        a->getTexture().mediaId = oldId;
        b->getTexture().mediaId = oldId;
        c->getTexture().mediaId = otherId;
        object = scene->adoptObject(std::make_unique<MeshObject>());
        object->getTexture().mediaId = oldId;
        es.scenes.push_back(std::move(scene));
        es.quadTexturePaths[a->getId()] = oldPath;
        es.quadTexturePaths[b->getId()] = oldPath;
        es.quadTexturePaths[c->getId()] = "C:/show/other.png";
        es.activeVideoSourceId = oldId;
        control::MidiMapping toOld;
        toOld.target = sourceTarget(oldId);
        control::MidiMapping toOther;
        toOther.target = sourceTarget(otherId);
        es.midiMappings = {toOld, toOther};
    }

    void replace() {
        es.commandHistory.execute(std::make_unique<ReplaceSourceCommand>(
            es, std::unordered_set<UUID>{oldId}, newId, newPath));
    }
};

}  // namespace

TEST(ReplaceMediaTest, MovesEverythingShowingTheClipAsOneUndoStep) {
    ReplaceFixture fx;
    fx.replace();
    ASSERT_EQ(fx.es.commandHistory.getUndoCount(), 1u);
    EXPECT_EQ(fx.a->getTexture().mediaId, fx.newId);
    EXPECT_EQ(fx.b->getTexture().mediaId, fx.newId);
    EXPECT_EQ(fx.c->getTexture().mediaId, fx.otherId);
    EXPECT_EQ(fx.object->getTexture().mediaId, fx.newId);
    EXPECT_EQ(fx.es.quadTexturePaths[fx.a->getId()], fx.newPath) << "saved with the project";
    EXPECT_EQ(fx.es.quadTexturePaths[fx.c->getId()], "C:/show/other.png");
    EXPECT_EQ(fx.es.midiMappings[0].target, sourceTarget(fx.newId));
    EXPECT_EQ(fx.es.midiMappings[1].target, sourceTarget(fx.otherId));
    EXPECT_EQ(fx.es.activeVideoSourceId, fx.newId);

    // The history keeps the original clip loaded for undo.
    const auto referenced = referencedMediaIds(fx.es.scenes, fx.es.commandHistory);
    EXPECT_TRUE(referenced.count(fx.oldId));
    EXPECT_TRUE(referenced.count(fx.newId));

    ASSERT_TRUE(fx.es.commandHistory.undo());
    EXPECT_EQ(fx.a->getTexture().mediaId, fx.oldId);
    EXPECT_EQ(fx.b->getTexture().mediaId, fx.oldId);
    EXPECT_EQ(fx.object->getTexture().mediaId, fx.oldId);
    EXPECT_EQ(fx.es.quadTexturePaths[fx.a->getId()], fx.oldPath);
    EXPECT_EQ(fx.es.midiMappings[0].target, sourceTarget(fx.oldId));
    EXPECT_EQ(fx.es.activeVideoSourceId, fx.oldId);

    ASSERT_TRUE(fx.es.commandHistory.redo());
    EXPECT_EQ(fx.a->getTexture().mediaId, fx.newId);
    EXPECT_EQ(fx.object->getTexture().mediaId, fx.newId);
}

TEST(ReplaceMediaTest, UndoAndRedoLeaveLaterBindingsAlone) {
    ReplaceFixture fx;
    fx.replace();
    fx.c->getTexture().mediaId = fx.newId;  // the user gives C the HAP clip too
    ASSERT_TRUE(fx.es.commandHistory.undo());
    EXPECT_EQ(fx.c->getTexture().mediaId, fx.newId) << "C was not moved by the replace";

    fx.a->getTexture().mediaId = fx.otherId;  // and A something else
    ASSERT_TRUE(fx.es.commandHistory.redo());
    EXPECT_EQ(fx.a->getTexture().mediaId, fx.otherId);
    EXPECT_EQ(fx.b->getTexture().mediaId, fx.newId);
}

TEST(ReplaceMediaTest, NothingToMoveRecordsNothing) {
    ReplaceFixture fx;
    fx.es.commandHistory.execute(std::make_unique<ReplaceSourceCommand>(
        fx.es, std::unordered_set<UUID>{UUID::generate()}, fx.newId, fx.newPath));
    EXPECT_EQ(fx.es.commandHistory.getUndoCount(), 0u);
}

TEST(ReplaceMediaTest, ReplaceRefusesAClipTheShowDoesNotUse) {
    ReplaceFixture fx;
    SourceManager sm;
    EXPECT_EQ(replaceClipInShow(fx.es, sm, "C:/nowhere/unused.mp4", "C:/nowhere/unused_hap.mov"),
              "Nothing in the show uses unused.mp4.");
    EXPECT_EQ(fx.es.commandHistory.getUndoCount(), 0u);
}
