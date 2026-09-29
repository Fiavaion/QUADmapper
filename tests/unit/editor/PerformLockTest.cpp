// The perform lock (editor/PerformLock, ADR-051 §4) on a real EditorState, outputs and
// action registry, with no ImGui frame or GL: what it refuses (the Edit actions, the
// canvas keys, commands, calibration and slice warp editing) and what it keeps (Save,
// scene takes, blackout, the OSC targets), and that unlocking needs its confirmation.
// The panels, the canvas under real key presses and the menu are in
// ui/PerformLockUiTest.cpp; New / Load in app/ProjectSessionTest.cpp.

#include "editor/PerformLock.hpp"
#include "editor/ActionRegistry.hpp"
#include "editor/ControlBindings.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/MeshEditKeys.hpp"
#include "editor/SliceEditing.hpp"
#include "control/ControlDispatcher.hpp"
#include "control/ExternalControl.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace quadmapper;

namespace {

// The editor as Application wires it: the edit actions, the app actions the lock must
// keep (Save, Blackout, the scene takes) and perform.lock, with the lock's policy.
class PerformLockTest : public ::testing::Test {
protected:
    void SetUp() override {
        output = om.createOutput("Projector");
        es.outputResolver.setOutputManager(&om);
        quad = es.quadManager().createQuad("Quad 1");
        es.quadManager().selectQuad(quad->getId());
        second = es.addScene("Second");
        es.setActiveScene(es.scenes.front()->getId());
        OutputManager::showScene(*output, es.scenes.front().get(), es.scenes);

        registerEditActions(registry);
        const auto app = [this](const char* id, std::function<void(EditorContext&)> run) {
            std::optional<Action> action = makeAppAction(id, std::move(run));
            ASSERT_TRUE(action);
            ASSERT_EQ(registry.add(std::move(*action)), AddResult::Added) << id;
        };
        app(action_id::SAVE, [this](EditorContext&) { ++saves; });
        app(action_id::BLACKOUT, [](EditorContext& c) { c.outputManager->toggleBlackout(); });
        app(action_id::PERFORM_LOCK, togglePerformLock);
        for (int n = 1; n <= 9; ++n) {
            app(sceneTakeId(n).c_str(), [n](EditorContext& c) { c.es.takeScene(n, *c.outputManager); });
        }
        installPerformLockPolicy(registry, es);
        ctx.actions = &registry;
    }

    Outcome invoke(const std::string& id, Source source = Source::Key) {
        return registry.invoke(id, ctx, source);
    }

    void lock() { ASSERT_EQ(invoke(action_id::PERFORM_LOCK, Source::Menu), Outcome::Ran); }

    // A Tab or arrow press on the canvas, dispatched as ViewportEditor does.
    bool pressCanvasKey(const CanvasKeys& keys) {
        if (canvasKeyOwner(es, &om) != CanvasKeyOwner::Quad) return false;
        return applyCornerKeys(keys, es.quadRef(*quad), quad->getGeometry(), es.commandHistory,
                               es.cornerSelectedVertex);
    }

    EditorState es;
    OutputManager om;
    OutputWindow* output = nullptr;
    Quad* quad = nullptr;
    Scene* second = nullptr;
    ActionRegistry registry;
    EditorContext ctx{es, nullptr, nullptr, nullptr, &om, nullptr, nullptr};
    int saves = 0;
};

CanvasKeys tab() {
    CanvasKeys keys;
    keys.tab = true;
    return keys;
}

CanvasKeys right() {
    CanvasKeys keys;
    keys.right = true;
    return keys;
}

}  // namespace

TEST_F(PerformLockTest, EditActionsAreLockedAndSayWhy) {
    lock();
    EXPECT_TRUE(es.performLock.isOn());
    es.statusMessage[0] = '\0';

    EXPECT_EQ(invoke(action_id::SELECT_ALL), Outcome::Locked);
    EXPECT_EQ(invoke(action_id::DELETE_SELECTION, Source::Menu), Outcome::Locked);
    EXPECT_EQ(invoke(action_id::UNDO), Outcome::Locked);
    EXPECT_EQ(invoke(action_id::TOGGLE_MESH_EDIT), Outcome::Locked);
    EXPECT_EQ(es.quadManager().getQuadCount(), 1u) << "Delete did nothing";
    EXPECT_EQ(quad->getGeometry().getMeshMode(), MeshMode::CORNERS_ONLY);
    EXPECT_NE(std::string(es.statusMessage).find("the show is locked"), std::string::npos)
        << es.statusMessage;
}

TEST_F(PerformLockTest, SaveBlackoutAndSceneTakesStillRun) {
    lock();
    EXPECT_EQ(invoke(action_id::SAVE), Outcome::Ran);
    EXPECT_EQ(saves, 1);

    EXPECT_EQ(invoke(action_id::BLACKOUT), Outcome::Ran);
    EXPECT_TRUE(om.isBlackout());
    EXPECT_EQ(invoke(action_id::BLACKOUT, Source::Menu), Outcome::Ran);
    EXPECT_FALSE(om.isBlackout());

    // A queued take (a 1-9 key) and an OSC / MIDI take.
    EXPECT_EQ(invoke(sceneTakeId(2)), Outcome::Ran);
    EXPECT_EQ(output->getSceneId(), second->getId());
    EXPECT_EQ(invoke(sceneTakeId(1), Source::Control), Outcome::Ran);
    EXPECT_EQ(output->getSceneId(), es.scenes.front()->getId());
}

TEST_F(PerformLockTest, AFadedTakeStillRuns) {
    lock();
    const SceneTake take = es.takeScene(2, om, /*fadeSeconds=*/0.5);
    EXPECT_EQ(take.kind, SceneTake::Kind::Route);
    EXPECT_EQ(output->getSceneId(), second->getId());
}

TEST_F(PerformLockTest, TheCanvasKeysAreNotClaimedAndNothingNudges) {
    ASSERT_EQ(canvasKeyOwner(es, &om), CanvasKeyOwner::Quad);
    lock();
    EXPECT_EQ(canvasKeyOwner(es, &om), CanvasKeyOwner::None);

    const std::array<Vec2, 4> before = quad->getGeometry().getCorners();
    EXPECT_FALSE(pressCanvasKey(tab()));
    EXPECT_FALSE(pressCanvasKey(right()));
    EXPECT_EQ(es.cornerSelectedVertex, -1);
    EXPECT_EQ(quad->getGeometry().getCorners(), before);
}

TEST_F(PerformLockTest, CommandHistoryRefusesCommands) {
    lock();
    EXPECT_TRUE(es.commandHistory.isReadOnly());
    const Vec2 corner = quad->getGeometry().getCorners()[0];
    es.commandHistory.execute(std::make_unique<MoveCornerCommand>(es.quadRef(*quad), 0, corner,
                                                                  corner + Vec2(0.1f, 0.0f)));
    EXPECT_EQ(quad->getGeometry().getCorners()[0], corner);
    EXPECT_EQ(es.commandHistory.getUndoCount(), 0u);
}

TEST_F(PerformLockTest, OscAndMidiTargetsStillApply) {
    ControlBindingHost host;
    host.scenes = &es.scenes;
    host.isBlackout = [this] { return om.isBlackout(); };
    host.setBlackout = [this](bool on) { om.setBlackout(on); };
    control::ControlRegistry targets;
    control::ControlDispatcher dispatcher{targets};
    registerControlTargets(targets, host);
    refreshControlEntities(targets, es.scenes, nullptr);
    lock();

    const auto osc = [&](const std::string& address, float value) {
        control::ControlEvent e;
        e.source = control::ControlEvent::Source::Osc;
        e.osc.address = address;
        control::OscArg arg;
        arg.type = control::OscArg::Type::Float;
        arg.f = value;
        e.osc.args.push_back(arg);
        std::vector<control::MidiMapping> mappings;
        dispatcher.dispatch(e, mappings, 0.0);
    };
    osc("/qm/quad/Quad 1/opacity", 0.25f);
    EXPECT_FLOAT_EQ(quad->getOpacity(), 0.25f);
    osc("/qm/blackout", 1.0f);
    EXPECT_TRUE(om.isBlackout());
}

TEST_F(PerformLockTest, LockingEndsCalibrationAndRefusesToStartIt) {
    es.beginCalibration(*output);
    ASSERT_FALSE(es.calibratingOutputId.isNull());
    lock();
    EXPECT_TRUE(es.calibratingOutputId.isNull()) << "an active calibration ends on lock";
    es.beginCalibration(*output);
    EXPECT_TRUE(es.calibratingOutputId.isNull()) << "and none starts while locked";
}

TEST_F(PerformLockTest, LockingEndsSliceWarpEditingAndRefusesToStartIt) {
    const UUID slice = output->getSlices()[0].id;
    es.beginSliceWarp(*output, slice);
    ASSERT_EQ(canvasKeyOwner(es, &om), CanvasKeyOwner::SliceWarp);
    lock();
    EXPECT_TRUE(es.sliceWarpOutputId.isNull());
    es.beginSliceWarp(*output, slice);
    EXPECT_TRUE(es.sliceWarpOutputId.isNull());
    EXPECT_EQ(canvasKeyOwner(es, &om), CanvasKeyOwner::None);
}

TEST_F(PerformLockTest, UnlockingNeedsTheConfirmation) {
    lock();
    EXPECT_FALSE(confirmPerformUnlock(es)) << "no unlock was asked for";
    EXPECT_TRUE(es.performLock.isOn());

    // The menu item again asks; it does not unlock.
    EXPECT_EQ(invoke(action_id::PERFORM_LOCK, Source::Menu), Outcome::Ran);
    EXPECT_TRUE(es.performLock.isOn());
    EXPECT_TRUE(es.performLock.isConfirmingUnlock());

    cancelPerformUnlock(es);  // "Stay Locked"
    EXPECT_TRUE(es.performLock.isOn());
    EXPECT_FALSE(confirmPerformUnlock(es));

    requestPerformUnlock(es);
    EXPECT_TRUE(confirmPerformUnlock(es));  // "Unlock"
    EXPECT_FALSE(es.performLock.isOn());
    EXPECT_FALSE(es.performLock.isConfirmingUnlock());
}

// After unlocking everything edits again: the edit actions, the canvas keys (the
// success metric), commands and calibration.
TEST_F(PerformLockTest, AfterUnlockEverythingWorksAgain) {
    lock();
    requestPerformUnlock(es);
    ASSERT_TRUE(confirmPerformUnlock(es));

    EXPECT_FALSE(es.commandHistory.isReadOnly());
    EXPECT_EQ(canvasKeyOwner(es, &om), CanvasKeyOwner::Quad);
    const Vec2 corner = quad->getGeometry().getCorners()[0];
    EXPECT_FALSE(pressCanvasKey(tab()));
    EXPECT_EQ(es.cornerSelectedVertex, 0);
    EXPECT_TRUE(pressCanvasKey(right()));
    EXPECT_GT(quad->getGeometry().getCorners()[0].x, corner.x);
    EXPECT_EQ(es.commandHistory.getUndoCount(), 1u) << "the nudge is an undo step";

    EXPECT_EQ(invoke(action_id::UNDO), Outcome::Ran);
    EXPECT_EQ(quad->getGeometry().getCorners()[0], corner);
    EXPECT_EQ(invoke(action_id::SELECT_ALL), Outcome::Ran);
    es.beginCalibration(*output);
    EXPECT_FALSE(es.calibratingOutputId.isNull());
}

// A MIDI learn waiting for its control would add a mapping to the show.
TEST_F(PerformLockTest, LockingCancelsAPendingMidiLearn) {
    control::ExternalControl control;
    control.dispatcher().beginLearn("/qm/blackout");
    ctx.control = &control;
    lock();
    EXPECT_FALSE(control.dispatcher().isLearning());
}
