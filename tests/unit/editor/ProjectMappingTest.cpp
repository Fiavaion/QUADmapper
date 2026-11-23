// The live<->DTO mapping (editor/ProjectMapping.cpp, ADR-051 E4): an editor's show goes
// through syncEditorToProject and comes back through syncProjectToEditor with
// everything the .qmap carries. No GL: sources are either absent or files that cannot
// be opened, which fail before any texture exists.

#include "editor/ProjectMapping.hpp"
#include "editor/ControlBindings.hpp"
#include "editor/EditorState.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "project/Project.hpp"
#include "source/SourceManager.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace quadmapper;

namespace {

// Saves `from` into a project and loads that project into `to`.
Project roundTrip(EditorState& from, EditorState& to, SourceManager* sources = nullptr,
                  OutputManager* fromOutputs = nullptr, OutputManager* toOutputs = nullptr) {
    Project project;
    syncEditorToProject(from, fromOutputs, sources, project);
    syncProjectToEditor(to, toOutputs, project, sources, nullptr);
    return project;
}

const Scene* sceneNamed(const EditorState& es, const std::string& name) {
    for (const auto& scene : es.scenes) {
        if (scene && scene->getName() == name) return scene.get();
    }
    return nullptr;
}

const QuadGroup* groupNamed(const Scene& scene, const std::string& name) {
    for (const auto& group : scene.getGroups()) {
        if (group && group->getName() == name) return group.get();
    }
    return nullptr;
}

std::string quadTarget(const UUID& id) { return "/qm/quad/" + id.toString() + "/opacity"; }
std::string sourceTarget(const UUID& id) { return "/qm/source/" + id.toString() + "/play"; }

}  // namespace

TEST(ProjectMappingTest, ScenesKeepTheirIdsNamesCanvasAndView) {
    EditorState from;
    Scene* first = from.addScene("Stage");
    first->setCanvasSize(IVec2(3840, 1080));
    first->setCanvasLocked(true);
    first->getViewState().panOffset = Vec2(12.0f, -40.0f);
    first->getViewState().zoomLevel = 0.5f;
    first->getViewState().gridVisible = false;
    first->getViewState().snapToGrid = true;
    Scene* second = from.addScene("Encore");

    EditorState to;
    to.addScene("Replaced");  // loading replaces what was open
    roundTrip(from, to);

    ASSERT_EQ(to.scenes.size(), 2u);
    EXPECT_EQ(sceneNamed(to, "Replaced"), nullptr);
    const Scene* stage = sceneNamed(to, "Stage");
    ASSERT_NE(stage, nullptr);
    EXPECT_EQ(stage->getId(), first->getId());
    EXPECT_EQ(stage->getCanvasSize(), IVec2(3840, 1080));
    EXPECT_TRUE(stage->isCanvasLocked());
    EXPECT_EQ(stage->getViewState().panOffset, Vec2(12.0f, -40.0f));
    EXPECT_FLOAT_EQ(stage->getViewState().zoomLevel, 0.5f);
    EXPECT_FALSE(stage->getViewState().gridVisible);
    EXPECT_TRUE(stage->getViewState().snapToGrid);
    const Scene* encore = sceneNamed(to, "Encore");
    ASSERT_NE(encore, nullptr);
    EXPECT_EQ(encore->getId(), second->getId());
    EXPECT_EQ(to.activeSceneId, first->getId()) << "the first scene is active after a load";
}

TEST(ProjectMappingTest, QuadsKeepGeometryMeshBezierCropAndColour) {
    EditorState from;
    Scene* scene = from.addScene("Stage");
    Quad* quad = scene->getQuadManager().createQuad("Wall");
    quad->setVisible(false);
    quad->setLocked(true);
    quad->setOpacity(0.25f);
    quad->setZOrder(7);

    QuadGeometry& geometry = quad->getGeometry();
    geometry.setCorners({Vec2(10.0f, 20.0f), Vec2(900.0f, 15.0f), Vec2(880.0f, 700.0f), Vec2(30.0f, 650.0f)});
    geometry.setGridCoords({0.0f, 0.3f, 1.0f}, {0.0f, 0.6f, 1.0f});
    geometry.setMeshMode(MeshMode::MESH_EDIT);
    geometry.setMeshVertex(1, 1, Vec2(4.0f, -3.0f));
    geometry.setBezierEnabled(true);
    geometry.setBezierControl(0, 0, Vec2(300.0f, 0.0f));

    TextureBinding& texture = quad->getTexture();
    texture.uvOffset = Vec2(0.1f, 0.2f);
    texture.uvScale = Vec2(1.5f, 0.75f);
    texture.uvRotation = 0.3f;
    texture.cropRect = NormalizedRect{0.1f, 0.2f, 0.5f, 0.4f};
    texture.flipH = true;
    texture.flipV = true;

    ColorCorrection colour;
    colour.brightness = 0.2f;
    colour.contrast = 1.4f;
    colour.gamma = 2.2f;
    colour.rgbGain = Vec3(1.1f, 0.9f, 0.8f);
    quad->setColorCorrection(colour);
    SoftEdge edge;
    edge.left = 0.1f;
    edge.bottom = 0.2f;
    edge.gamma = 1.8f;
    quad->setSoftEdge(edge);

    EditorState to;
    roundTrip(from, to);

    const Scene* stage = sceneNamed(to, "Stage");
    ASSERT_NE(stage, nullptr);
    ASSERT_EQ(stage->getQuadManager().getQuadCount(), 1u);
    const Quad& back = *stage->getQuadManager().getQuads()[0];
    EXPECT_EQ(back.getId(), quad->getId()) << "a quad keeps its id, so OSC and MIDI still find it";
    EXPECT_EQ(back.getName(), "Wall");
    EXPECT_FALSE(back.isVisible());
    EXPECT_TRUE(back.isLocked());
    EXPECT_FLOAT_EQ(back.getOpacity(), 0.25f);
    EXPECT_EQ(back.getZOrder(), 7);

    const QuadGeometry& g = back.getGeometry();
    EXPECT_EQ(g.getCorners(), geometry.getCorners());
    EXPECT_EQ(g.getUCoords(), geometry.getUCoords());
    EXPECT_EQ(g.getVCoords(), geometry.getVCoords());
    EXPECT_EQ(g.getMeshMode(), MeshMode::MESH_EDIT);
    EXPECT_EQ(g.getMeshVertices(), geometry.getMeshVertices());
    EXPECT_EQ(g.getMeshVertex(1, 1), Vec2(4.0f, -3.0f));
    EXPECT_TRUE(g.isBezierEnabled());
    EXPECT_EQ(g.getBezierOffsets(), geometry.getBezierOffsets());
    EXPECT_TRUE(g.hasBezierEdges());

    const TextureBinding& t = back.getTexture();
    EXPECT_TRUE(t.mediaId.isNull()) << "no source was bound";
    EXPECT_EQ(t.uvOffset, texture.uvOffset);
    EXPECT_EQ(t.uvScale, texture.uvScale);
    EXPECT_FLOAT_EQ(t.uvRotation, 0.3f);
    EXPECT_FLOAT_EQ(t.cropRect.x, 0.1f);
    EXPECT_FLOAT_EQ(t.cropRect.y, 0.2f);
    EXPECT_FLOAT_EQ(t.cropRect.width, 0.5f);
    EXPECT_FLOAT_EQ(t.cropRect.height, 0.4f);
    EXPECT_TRUE(t.flipH);
    EXPECT_TRUE(t.flipV);

    const ColorCorrection& c = back.getColorCorrection();
    EXPECT_FLOAT_EQ(c.brightness, 0.2f);
    EXPECT_FLOAT_EQ(c.contrast, 1.4f);
    EXPECT_FLOAT_EQ(c.gamma, 2.2f);
    EXPECT_EQ(c.rgbGain, Vec3(1.1f, 0.9f, 0.8f));
    const SoftEdge& s = back.getSoftEdge();
    EXPECT_FLOAT_EQ(s.left, 0.1f);
    EXPECT_FLOAT_EQ(s.right, 0.0f);
    EXPECT_FLOAT_EQ(s.bottom, 0.2f);
    EXPECT_FLOAT_EQ(s.gamma, 1.8f);

    EXPECT_TRUE(stage->getQuadManager().isSelected(quad->getId()))
        << "the first quad of the active scene is selected after a load";
}

TEST(ProjectMappingTest, GroupsKeepMembersFlagsAndNesting) {
    EditorState from;
    Scene* scene = from.addScene("Stage");
    Quad* a = scene->getQuadManager().createQuad("A");
    Quad* b = scene->getQuadManager().createQuad("B");
    Quad* c = scene->getQuadManager().createQuad("C");
    QuadGroup* outer = scene->createGroup("Outer");
    outer->addMember(a->getId());
    outer->setLocked(true);
    outer->setCollapsed(true);
    QuadGroup* inner = scene->createGroup("Inner");
    inner->addMember(b->getId());
    inner->addMember(c->getId());
    inner->setVisible(false);
    inner->setParentGroup(outer->getId());

    EditorState to;
    roundTrip(from, to);

    const Scene* stage = sceneNamed(to, "Stage");
    ASSERT_NE(stage, nullptr);
    ASSERT_EQ(stage->getGroups().size(), 2u);
    const QuadGroup* outerBack = groupNamed(*stage, "Outer");
    const QuadGroup* innerBack = groupNamed(*stage, "Inner");
    ASSERT_NE(outerBack, nullptr);
    ASSERT_NE(innerBack, nullptr);
    EXPECT_EQ(outerBack->getMemberQuadIds(), std::vector<UUID>{a->getId()});
    EXPECT_TRUE(outerBack->isLocked());
    EXPECT_TRUE(outerBack->isCollapsed());
    EXPECT_TRUE(outerBack->isVisible());
    EXPECT_FALSE(outerBack->getParentGroup().has_value());
    EXPECT_EQ(innerBack->getMemberQuadIds(), (std::vector<UUID>{b->getId(), c->getId()}));
    EXPECT_FALSE(innerBack->isVisible());
    ASSERT_TRUE(innerBack->getParentGroup().has_value());
    EXPECT_EQ(*innerBack->getParentGroup(), outerBack->getId()) << "nesting follows the live group ids";
}

// A missing file is written back exactly as it was loaded, and a file that cannot be
// opened on load stays referenced for the user to relocate (PRD §4.1.8).
TEST(ProjectMappingTest, MissingSourcesAreKeptAndWrittenBackAsLoaded) {
    SourceManager sources;
    EditorState from;
    Scene* scene = from.addScene("Stage");
    Quad* clipQuad = scene->getQuadManager().createQuad("Clip");
    Quad* stillQuad = scene->getQuadManager().createQuad("Still");

    Project::SourceData clip;
    clip.id = UUID::generate();
    clip.kind = "video";
    clip.path = "C:/qm_absent_show/clip.mp4";
    clip.missing = true;
    Project::TransportData transport;
    transport.playing = true;
    transport.loopMode = LoopMode::PING_PONG;
    transport.speed = 0.5f;
    transport.volume = 0.7f;
    transport.muted = true;
    clip.transport = transport;
    from.missingSources.emplace(clip.id, clip);
    clipQuad->getTexture().mediaId = clip.id;
    from.quadTexturePaths[clipQuad->getId()] = clip.path;

    Project::SourceData still;  // not flagged missing yet: the load finds it gone
    still.id = UUID::generate();
    still.kind = "image";
    still.path = "C:/qm_absent_show/still.png";
    from.missingSources.emplace(still.id, still);
    stillQuad->getTexture().mediaId = still.id;

    EditorState to;
    const Project saved = roundTrip(from, to, &sources);

    ASSERT_EQ(saved.getSources().size(), 2u);
    const Project::SourceData& savedClip = saved.getSources()[0];
    EXPECT_EQ(savedClip.id, clip.id);
    EXPECT_EQ(savedClip.path, clip.path);
    EXPECT_TRUE(savedClip.missing);
    ASSERT_TRUE(savedClip.transport.has_value());
    EXPECT_EQ(savedClip.transport->loopMode, LoopMode::PING_PONG);

    const Scene* stage = sceneNamed(to, "Stage");
    ASSERT_NE(stage, nullptr);
    const Quad* clipBack = stage->getQuadManager().getQuad(clipQuad->getId());
    const Quad* stillBack = stage->getQuadManager().getQuad(stillQuad->getId());
    ASSERT_NE(clipBack, nullptr);
    ASSERT_NE(stillBack, nullptr);
    EXPECT_EQ(clipBack->getTexture().mediaId, clip.id) << "the saved id is kept when nothing has it";
    EXPECT_EQ(stillBack->getTexture().mediaId, still.id);
    EXPECT_EQ(to.quadTexturePaths[clipQuad->getId()], clip.path);
    EXPECT_EQ(to.quadTexturePaths[stillQuad->getId()], still.path);
    EXPECT_EQ(sources.getSourceCount(), 0u) << "nothing opened";

    ASSERT_EQ(to.missingSources.size(), 2u);
    const Project::SourceData& clipMissing = to.missingSources.at(clip.id);
    EXPECT_TRUE(clipMissing.missing);
    EXPECT_EQ(clipMissing.kind, "video");
    ASSERT_TRUE(clipMissing.transport.has_value());
    EXPECT_TRUE(clipMissing.transport->playing);
    EXPECT_FLOAT_EQ(clipMissing.transport->speed, 0.5f);
    EXPECT_FLOAT_EQ(clipMissing.transport->volume, 0.7f);
    EXPECT_TRUE(clipMissing.transport->muted);
    const Project::SourceData& stillMissing = to.missingSources.at(still.id);
    EXPECT_TRUE(stillMissing.missing) << "a file that could not be opened is now listed as missing";
    EXPECT_EQ(stillMissing.path, still.path);

    // Saved again, the show is the same.
    Project again;
    syncEditorToProject(to, nullptr, &sources, again);
    ASSERT_EQ(again.getSources().size(), 2u);
    for (const auto& source : again.getSources()) {
        EXPECT_TRUE(source.missing);
        EXPECT_TRUE(source.id == clip.id || source.id == still.id);
    }
}

TEST(ProjectMappingTest, MidiMappingsFollowQuadsAndMissingSources) {
    SourceManager sources;
    EditorState from;
    Scene* scene = from.addScene("Stage");
    Quad* quad = scene->getQuadManager().createQuad("Wall");
    Project::SourceData clip;
    clip.id = UUID::generate();
    clip.kind = "video";
    clip.path = "C:/qm_absent_show/clip.mp4";
    clip.missing = true;
    from.missingSources.emplace(clip.id, clip);
    quad->getTexture().mediaId = clip.id;

    control::MidiMapping opacity;
    opacity.device = "APC mini";
    opacity.type = control::MidiControlType::CC;
    opacity.channel = 3;
    opacity.number = 7;
    opacity.bits = 14;
    opacity.mode = control::MidiMode::Value;
    opacity.target = quadTarget(quad->getId());
    control::MidiMapping play;
    play.type = control::MidiControlType::Note;
    play.number = 60;
    play.mode = control::MidiMode::Toggle;
    play.target = sourceTarget(clip.id);
    control::MidiMapping take;
    take.type = control::MidiControlType::Program;
    take.number = 2;
    take.target = "/qm/scene/take";
    from.midiMappings = {opacity, play, take};

    EditorState to;
    const Project saved = roundTrip(from, to, &sources);

    ASSERT_EQ(saved.getMidiMappings().size(), 3u);
    ASSERT_EQ(to.midiMappings.size(), 3u);
    EXPECT_TRUE(sameBinding(to.midiMappings[0], opacity));
    EXPECT_TRUE(to.midiMappings[0].is14Bit());
    EXPECT_TRUE(sameBinding(to.midiMappings[1], play));
    EXPECT_TRUE(sameBinding(to.midiMappings[2], take));
}

// Two scenes holding a quad with the same id (a file edited by hand, or an old
// duplicate): the second is renumbered, and a mapping saved on the id stays on the
// quad that kept it.
TEST(ProjectMappingTest, AClashingQuadIdIsRenumberedAndTheMappingStaysOnTheFirst) {
    EditorState from;
    Scene* first = from.addScene("First");
    Quad* original = first->getQuadManager().createQuad("Original");
    Scene* second = from.addScene("Second");
    second->getQuadManager().insertQuad(std::make_unique<Quad>(original->getId()), SIZE_MAX);
    control::MidiMapping mapping;
    mapping.target = quadTarget(original->getId());
    from.midiMappings = {mapping};

    EditorState to;
    roundTrip(from, to);

    const Scene* firstBack = sceneNamed(to, "First");
    const Scene* secondBack = sceneNamed(to, "Second");
    ASSERT_NE(firstBack, nullptr);
    ASSERT_NE(secondBack, nullptr);
    ASSERT_EQ(secondBack->getQuadManager().getQuadCount(), 1u);
    EXPECT_EQ(firstBack->getQuadManager().getQuads()[0]->getId(), original->getId());
    EXPECT_NE(secondBack->getQuadManager().getQuads()[0]->getId(), original->getId());
    ASSERT_EQ(to.midiMappings.size(), 1u);
    EXPECT_EQ(to.midiMappings[0].target, quadTarget(original->getId()));
}

// Outputs are rebuilt from the file with the scene each shows and its settings. No
// window is created: the managers are not initialised.
TEST(ProjectMappingTest, OutputsKeepTheSceneTheyShowAndTheirSettings) {
    EditorState from;
    Scene* stage = from.addScene("Stage");
    from.addScene("Unshown");
    OutputManager fromOutputs;
    OutputWindow* projector = fromOutputs.createOutput("Projector A");
    ASSERT_NE(projector, nullptr);
    projector->setSceneId(stage->getId());
    projector->setResolution(IVec2(1280, 800));
    projector->setRotationDegrees(180);
    projector->setEnabled(false);

    EditorState to;
    OutputManager toOutputs;
    toOutputs.createOutput("Startup default");  // replaced by the file's outputs
    const Project saved = roundTrip(from, to, nullptr, &fromOutputs, &toOutputs);

    ASSERT_EQ(saved.getOutputs().size(), 1u);
    ASSERT_EQ(toOutputs.getOutputs().size(), 1u);
    const OutputWindow& back = *toOutputs.getOutputs()[0];
    EXPECT_EQ(back.getName(), "Projector A");
    EXPECT_EQ(back.getSceneId(), stage->getId());
    EXPECT_EQ(back.getResolution(), IVec2(1280, 800));
    EXPECT_EQ(back.getRotationDegrees(), 180);
    EXPECT_FALSE(back.isEnabled());
}

// The test grid on new quads (on by default) never lands on a loaded show's quads:
// loading marks every one of them seen, in every scene (owner, 2026-11-23).
TEST(ProjectMappingTest, LoadedQuadsAreSeenSoTheyGetNoTestGrid) {
    EditorState from;
    Quad* wall = from.addScene("Stage")->getQuadManager().createQuad("Wall");
    Quad* floor = from.addScene("Floor")->getQuadManager().createQuad("Floor");
    ASSERT_TRUE(wall && floor);
    EditorState to;
    EXPECT_TRUE(to.uvGridOnNewQuads) << "on by default";
    roundTrip(from, to);
    int quads = 0;
    for (const auto& scene : to.scenes) {
        for (const auto& quad : scene->getQuadManager().getQuads()) {
            EXPECT_TRUE(to.seenQuadIds.count(quad->getId())) << quad->getName();
            ++quads;
        }
    }
    EXPECT_GE(quads, 2);
}

TEST(ProjectMappingTest, AnEmptyProjectLoadsAsOneActiveScene) {
    EditorState to;
    to.addScene("Old");
    const Project empty;
    syncProjectToEditor(to, nullptr, empty, nullptr, nullptr);
    ASSERT_EQ(to.scenes.size(), 1u);
    EXPECT_NE(to.scenes[0]->getName(), "Old");
    EXPECT_EQ(to.activeSceneId, to.scenes[0]->getId());
    EXPECT_TRUE(to.midiMappings.empty());
}

// One scene per projector (owner, 2026-11-23): the first output takes the scene no output
// shows, each further one a new scene with one quad, each canvas at its projector's size.
TEST(ProjectMappingTest, EachOutputGetsItsOwnSceneSizedToIt) {
    EditorState es;
    OutputManager om;
    OutputWindow* one = om.createOutput("Projector 1");
    OutputWindow* two = om.createOutput("Projector 2");
    ASSERT_TRUE(one && two);
    one->setResolution(IVec2(1280, 800));
    two->setResolution(IVec2(3840, 2160));
    ASSERT_TRUE(es.scenes.empty()) << "a fresh editor, as at startup";

    EXPECT_EQ(giveOutputsTheirOwnScenes(es, om, {one, two}), 2);
    const UUID active = es.activeSceneId;

    ASSERT_EQ(es.scenes.size(), 2u);
    const Scene* s1 = OutputManager::sceneForOutput(es.scenes, *one);
    const Scene* s2 = OutputManager::sceneForOutput(es.scenes, *two);
    ASSERT_TRUE(s1 && s2);
    EXPECT_EQ(s1->getId(), active) << "the default scene goes to the first projector";
    EXPECT_EQ(s1->getCanvasSize(), IVec2(1280, 800));
    EXPECT_EQ(s2->getName(), "Scene 2");
    EXPECT_EQ(s2->getCanvasSize(), IVec2(3840, 2160));
    EXPECT_EQ(s2->getQuadManager().getQuadCount(), 1u);

    // Outputs that show a scene are left alone; a shown scene is never handed out again.
    EXPECT_EQ(giveOutputsTheirOwnScenes(es, om, {one, two}), 0);
    OutputWindow* three = om.createOutput("Projector 3");
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(giveOutputsTheirOwnScenes(es, om, {three}), 1);
    EXPECT_EQ(es.scenes.size(), 3u);
    EXPECT_EQ(OutputManager::sceneForOutput(es.scenes, *three), es.scenes.back().get());
    EXPECT_EQ(es.activeSceneId, active) << "a hotplug never switches the scene being edited";
}
