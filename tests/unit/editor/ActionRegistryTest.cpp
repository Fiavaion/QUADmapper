// The action registry (ADR-051 §3): what add() refuses, exact chord lookup, and the
// Outcome of each invoke, including the perform lock's hook (E7).

#include "editor/ActionRegistry.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"

#include <imgui.h>

#include <gtest/gtest.h>

using namespace quadmapper;

namespace {

Action counting(const char* id, std::optional<Chord> chord, int* runs,
                ActionClass cls = ActionClass::Edit) {
    Action action;
    action.id = id;
    action.label = id;
    action.chord = chord;
    action.cls = cls;
    action.run = [runs](EditorContext&) { ++*runs; };
    return action;
}

struct Rig {
    EditorState es;
    EditorContext ctx{es, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    ActionRegistry registry;
    int runs = 0;
};

}  // namespace

TEST(ActionRegistryTest, RefusesDuplicateIdsAndClashingChords) {
    Rig rig;
    EXPECT_EQ(rig.registry.add(counting("a", Chord{ImGuiKey_S, MOD_CTRL}, &rig.runs)),
              AddResult::Added);
    EXPECT_EQ(rig.registry.add(counting("a", std::nullopt, &rig.runs)), AddResult::DuplicateId);
    EXPECT_EQ(rig.registry.add(counting("b", Chord{ImGuiKey_S, MOD_CTRL}, &rig.runs)),
              AddResult::ChordClash);
    // Same key, other modifiers: a different chord.
    EXPECT_EQ(rig.registry.add(counting("c", Chord{ImGuiKey_S, MOD_CTRL | MOD_SHIFT}, &rig.runs)),
              AddResult::Added);
    EXPECT_EQ(rig.registry.actions().size(), 2u);
}

TEST(ActionRegistryTest, RefusesInvalidActions) {
    Rig rig;
    EXPECT_EQ(rig.registry.add(counting("", std::nullopt, &rig.runs)), AddResult::Invalid);
    Action noRun;
    noRun.id = "x";
    EXPECT_EQ(rig.registry.add(noRun), AddResult::Invalid);
    EXPECT_EQ(rig.registry.add(counting("y", Chord{ImGuiKey_None, MOD_CTRL}, &rig.runs)),
              AddResult::Invalid);
    EXPECT_TRUE(rig.registry.actions().empty());
}

TEST(ActionRegistryTest, RefusesTheCanvasKeysWithAnyModifiers) {
    Rig rig;
    const int reserved[] = {ImGuiKey_Tab, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                            ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_H};
    int n = 0;
    for (int key : reserved) {
        for (uint8_t mods : {uint8_t(MOD_NONE), uint8_t(MOD_CTRL), uint8_t(MOD_SHIFT),
                             uint8_t(MOD_ALT), uint8_t(MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_SUPER)}) {
            const std::string id = "canvas." + std::to_string(n++);
            EXPECT_EQ(rig.registry.add(counting(id.c_str(), Chord{key, mods}, &rig.runs)),
                      AddResult::ReservedKey)
                << "key " << key << " mods " << int(mods);
        }
    }
    EXPECT_TRUE(rig.registry.actions().empty());
    EXPECT_FALSE(isReservedCanvasKey(ImGuiKey_Delete));
}

TEST(ActionRegistryTest, ChordLookupMatchesModifiersExactly) {
    Rig rig;
    rig.registry.add(counting("file.save", Chord{ImGuiKey_S, MOD_CTRL}, &rig.runs));
    const Action* save = rig.registry.findByChord(Chord{ImGuiKey_S, MOD_CTRL});
    ASSERT_NE(save, nullptr);
    EXPECT_EQ(save->id, "file.save");
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_S, MOD_CTRL | MOD_SHIFT}), nullptr)
        << "Ctrl+Shift+S is not Save";
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_S, MOD_CTRL | MOD_ALT}), nullptr);
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_S, MOD_NONE}), nullptr);
    EXPECT_EQ(rig.registry.find("file.save"), save);
    EXPECT_EQ(rig.registry.find("nope"), nullptr);
}

TEST(ActionRegistryTest, InvokeReportsEachOutcome) {
    Rig rig;
    bool enabled = false;
    Action gated = counting("gated", std::nullopt, &rig.runs);
    gated.enabled = [&enabled](const EditorContext&) { return enabled; };
    rig.registry.add(gated);

    EXPECT_EQ(rig.registry.invoke("missing", rig.ctx, Source::Key), Outcome::Unknown);
    EXPECT_EQ(rig.registry.invoke("gated", rig.ctx, Source::Menu), Outcome::Disabled);
    EXPECT_EQ(rig.runs, 0);
    enabled = true;
    EXPECT_EQ(rig.registry.invoke("gated", rig.ctx, Source::Menu), Outcome::Ran);
    EXPECT_EQ(rig.registry.invoke("gated", rig.ctx, Source::Key), Outcome::Ran);
    EXPECT_EQ(rig.runs, 2) << "the menu and the key run the same action";
}

TEST(ActionRegistryTest, ExternalControlRunsOnlyShowActions) {
    Rig rig;
    rig.registry.add(counting("edit.thing", std::nullopt, &rig.runs, ActionClass::Edit));
    rig.registry.add(counting("output.blackout", std::nullopt, &rig.runs, ActionClass::Show));
    EXPECT_EQ(rig.registry.invoke("edit.thing", rig.ctx, Source::Control), Outcome::Disabled);
    EXPECT_EQ(rig.registry.invoke("output.blackout", rig.ctx, Source::Control), Outcome::Ran);
    EXPECT_EQ(rig.runs, 1);
}

TEST(ActionRegistryTest, LockPolicyRefusesWithLocked) {
    Rig rig;
    rig.registry.add(counting("edit.delete", std::nullopt, &rig.runs, ActionClass::Edit));
    rig.registry.add(counting("file.open", std::nullopt, &rig.runs, ActionClass::File));
    rig.registry.add(counting("output.blackout", std::nullopt, &rig.runs, ActionClass::Show));
    rig.registry.setLockPolicy([](const Action& action, Source) {
        return action.cls == ActionClass::Edit || action.cls == ActionClass::File;
    });
    EXPECT_EQ(rig.registry.invoke("edit.delete", rig.ctx, Source::Key), Outcome::Locked);
    EXPECT_EQ(rig.registry.invoke("file.open", rig.ctx, Source::Menu), Outcome::Locked);
    EXPECT_EQ(rig.registry.invoke("output.blackout", rig.ctx, Source::Key), Outcome::Ran);
    EXPECT_EQ(rig.runs, 1);
    rig.registry.setLockPolicy({});
    EXPECT_EQ(rig.registry.invoke("edit.delete", rig.ctx, Source::Key), Outcome::Ran);
}

// The shipped chords (edit actions plus the app actions' definitions) all register
// together: no clash, no canvas key.
TEST(ActionRegistryTest, EveryShippedChordRegisters) {
    Rig rig;
    registerEditActions(rig.registry);
    const size_t edits = rig.registry.actions().size();
    EXPECT_EQ(edits, 13u);
    const auto noop = [](EditorContext&) {};
    for (const char* id : {action_id::SAVE, action_id::PREFERENCES, action_id::BLACKOUT,
                           action_id::PERFORM_TOGGLE, action_id::PERFORM_ESCAPE}) {
        auto action = makeAppAction(id, noop);
        ASSERT_TRUE(action.has_value()) << id;
        EXPECT_EQ(rig.registry.add(*action), AddResult::Added) << id;
    }
    for (int n = 1; n <= 9; ++n) {
        auto take = makeAppAction(sceneTakeId(n), noop);
        ASSERT_TRUE(take.has_value()) << n;
        EXPECT_FALSE(take->chord.has_value()) << "takes are captured from the physical key";
        EXPECT_EQ(take->cls, ActionClass::Show);
        EXPECT_EQ(rig.registry.add(*take), AddResult::Added) << n;
    }
    EXPECT_FALSE(makeAppAction("scene.take.10", noop).has_value());
    EXPECT_FALSE(makeAppAction("edit.undo", noop).has_value());
    EXPECT_EQ(rig.registry.actions().size(), edits + 5 + 9);

    // The looser Show guard and no rebinding: F11, Esc and Ctrl+B only.
    for (const Action& action : rig.registry.actions()) {
        const bool panic = action.id == action_id::BLACKOUT ||
                           action.id == action_id::PERFORM_TOGGLE ||
                           action.id == action_id::PERFORM_ESCAPE;
        EXPECT_EQ(action.guard == KeyGuard::Show, panic) << action.id;
        EXPECT_EQ(action.fixed, panic) << action.id;
    }
    ASSERT_NE(rig.registry.findByChord(Chord{ImGuiKey_S, MOD_CTRL}), nullptr);
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_S, MOD_CTRL})->id, action_id::SAVE);
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_M, MOD_CTRL | MOD_ALT}), nullptr)
        << "Ctrl+Alt+M no longer toggles mesh edit (ADR-051 consequences)";
}
