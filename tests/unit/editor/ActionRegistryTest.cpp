// The action registry (ADR-051 §3): what add() refuses, exact chord lookup, and the
// Outcome of each invoke, including the perform lock's hook (E7).

#include "editor/ActionRegistry.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/KeyLabels.hpp"

#include <imgui.h>

#include <gtest/gtest.h>

using namespace quadmapper;

namespace {

Action counting(const char* id, std::optional<Chord> chord, int* runs,
                ActionClass cls = ActionClass::Edit) {
    Action action;
    action.id = id;
    action.label = id;
    if (chord) action.chords = {*chord};
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
    registerFileActions(rig.registry);
    const size_t edits = rig.registry.actions().size();
    EXPECT_EQ(edits, 17u);
    const auto noop = [](EditorContext&) {};
    for (const char* id : {action_id::SAVE, action_id::PREFERENCES, action_id::BLACKOUT,
                           action_id::PERFORM_TOGGLE, action_id::PERFORM_ESCAPE,
                           action_id::PERFORM_LOCK}) {
        auto action = makeAppAction(id, noop);
        ASSERT_TRUE(action.has_value()) << id;
        EXPECT_EQ(rig.registry.add(*action), AddResult::Added) << id;
    }
    for (int n = 1; n <= 9; ++n) {
        auto take = makeAppAction(sceneTakeId(n), noop);
        ASSERT_TRUE(take.has_value()) << n;
        EXPECT_TRUE(take->chords.empty()) << "takes are captured from the physical key";
        EXPECT_EQ(take->cls, ActionClass::Show);
        EXPECT_EQ(rig.registry.add(*take), AddResult::Added) << n;
    }
    EXPECT_FALSE(makeAppAction("scene.take.10", noop).has_value());
    EXPECT_FALSE(makeAppAction("edit.undo", noop).has_value());
    EXPECT_EQ(rig.registry.actions().size(), edits + 6 + 9);

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

    // The owner's chords (2026-09-30).
    const auto on = [&rig](int key, uint8_t mods) {
        const Action* action = rig.registry.findByChord(Chord{key, mods});
        return action ? action->id : std::string("(none)");
    };
    EXPECT_EQ(on(ImGuiKey_L, MOD_CTRL | MOD_SHIFT), action_id::PERFORM_LOCK);
    EXPECT_EQ(on(ImGuiKey_N, MOD_CTRL), action_id::NEW_PROJECT);
    EXPECT_EQ(on(ImGuiKey_O, MOD_CTRL), action_id::OPEN_PROJECT);
    EXPECT_EQ(on(ImGuiKey_S, MOD_CTRL | MOD_SHIFT), action_id::SAVE_AS);
    EXPECT_EQ(on(ImGuiKey_Y, MOD_CTRL), action_id::REDO);
    EXPECT_EQ(on(ImGuiKey_Z, MOD_CTRL | MOD_SHIFT), action_id::REDO);
    for (const char* id : {action_id::NEW_PROJECT, action_id::OPEN_PROJECT, action_id::SAVE_AS}) {
        EXPECT_EQ(rig.registry.find(id)->cls, ActionClass::File) << id;
    }
}

// ---- More than one chord per action --------------------------------------------------

TEST(ActionRegistryTest, AnActionAnswersToEachOfItsChords) {
    Rig rig;
    Action redo = counting("redo", Chord{ImGuiKey_Y, MOD_CTRL}, &rig.runs);
    redo.chords.push_back(Chord{ImGuiKey_Z, MOD_CTRL | MOD_SHIFT});
    ASSERT_EQ(rig.registry.add(redo), AddResult::Added);
    ASSERT_NE(rig.registry.findByChord(Chord{ImGuiKey_Y, MOD_CTRL}), nullptr);
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_Y, MOD_CTRL})->id, "redo");
    ASSERT_NE(rig.registry.findByChord(Chord{ImGuiKey_Z, MOD_CTRL | MOD_SHIFT}), nullptr);
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_Z, MOD_CTRL | MOD_SHIFT})->id, "redo");
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_Z, MOD_CTRL}), nullptr);
    EXPECT_EQ(rig.registry.find("redo")->defaultChords, rig.registry.find("redo")->chords);
}

TEST(ActionRegistryTest, AddChecksEveryChord) {
    Rig rig;
    ASSERT_EQ(rig.registry.add(counting("undo", Chord{ImGuiKey_Z, MOD_CTRL}, &rig.runs)),
              AddResult::Added);

    Action second = counting("clash", Chord{ImGuiKey_Q, MOD_CTRL}, &rig.runs);
    second.chords.push_back(Chord{ImGuiKey_Z, MOD_CTRL});
    EXPECT_EQ(rig.registry.add(second), AddResult::ChordClash) << "its second chord is Undo's";

    Action reserved = counting("reserved", Chord{ImGuiKey_Q, MOD_CTRL}, &rig.runs);
    reserved.chords.push_back(Chord{ImGuiKey_Tab, MOD_CTRL});
    EXPECT_EQ(rig.registry.add(reserved), AddResult::ReservedKey);

    Action twice = counting("twice", Chord{ImGuiKey_Q, MOD_CTRL}, &rig.runs);
    twice.chords.push_back(Chord{ImGuiKey_Q, MOD_CTRL});
    EXPECT_EQ(rig.registry.add(twice), AddResult::ChordClash) << "the same chord twice";

    Action none = counting("none", Chord{ImGuiKey_Q, MOD_CTRL}, &rig.runs);
    none.chords.push_back(Chord{ImGuiKey_None, MOD_CTRL});
    EXPECT_EQ(rig.registry.add(none), AddResult::Invalid);
    EXPECT_EQ(rig.registry.actions().size(), 1u);
}

// ---- Chord text: the menu's and the preferences file's form ----------------------------

TEST(ActionRegistryTest, ChordTextRoundTripsThroughParseChord) {
    EXPECT_EQ(chordText(Chord{ImGuiKey_L, MOD_CTRL | MOD_SHIFT}), "Ctrl+Shift+L");
    EXPECT_EQ(chordText(Chord{ImGuiKey_F5, MOD_NONE}), "F5");
    EXPECT_EQ(chordText(Chord{ImGuiKey_Keypad3, MOD_ALT | MOD_SUPER}), "Alt+Super+Keypad3");
    EXPECT_EQ(chordsText({Chord{ImGuiKey_Y, MOD_CTRL}, Chord{ImGuiKey_Z, MOD_CTRL | MOD_SHIFT}}),
              "Ctrl+Y, Ctrl+Shift+Z");
    EXPECT_EQ(chordsText({}), "");

    int keys = 0;
    for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
        if (!isChordKey(key)) continue;
        ++keys;
        for (uint8_t mods : {uint8_t(MOD_NONE), uint8_t(MOD_CTRL), uint8_t(MOD_CTRL | MOD_SHIFT),
                             uint8_t(MOD_CTRL | MOD_SHIFT | MOD_ALT | MOD_SUPER)}) {
            const Chord chord{key, mods};
            const std::optional<Chord> parsed = parseChord(chordText(chord));
            ASSERT_TRUE(parsed) << chordText(chord);
            EXPECT_EQ(*parsed, chord) << chordText(chord);
        }
    }
    EXPECT_GT(keys, 26 + 10 + 24);
    EXPECT_FALSE(isChordKey(ImGuiKey_LeftCtrl));
    EXPECT_FALSE(isChordKey(ImGuiKey_MouseLeft));
}

TEST(ActionRegistryTest, ParseChordAcceptsAnyOrderAndCaseAndRefusesTheRest) {
    EXPECT_EQ(parseChord("shift+ctrl+l"), (Chord{ImGuiKey_L, MOD_CTRL | MOD_SHIFT}));
    EXPECT_EQ(parseChord("Ctrl+="), (Chord{ImGuiKey_Equal, MOD_CTRL}));
    EXPECT_EQ(parseChord("Alt+-"), (Chord{ImGuiKey_Minus, MOD_ALT}));
    EXPECT_EQ(parseChord("Delete"), (Chord{ImGuiKey_Delete, MOD_NONE}));
    for (const char* bad : {"", "Ctrl+", "Ctrl+Nope", "Hyper+L", "Ctrl++L", "LeftCtrl", "Ctrl+Shift"}) {
        EXPECT_FALSE(parseChord(bad).has_value()) << bad;
    }
}

// ---- Rebinding ---------------------------------------------------------------------

namespace {

// The shipped registry, as the app has it (the app actions' runs do nothing here).
struct ShippedRig : Rig {
    ShippedRig() {
        registerEditActions(registry);
        registerFileActions(registry);
        const auto noop = [](EditorContext&) {};
        for (const char* id : {action_id::SAVE, action_id::PREFERENCES, action_id::BLACKOUT,
                               action_id::PERFORM_TOGGLE, action_id::PERFORM_ESCAPE,
                               action_id::PERFORM_LOCK}) {
            registry.add(*makeAppAction(id, noop));
        }
        for (int n = 1; n <= 9; ++n) registry.add(*makeAppAction(sceneTakeId(n), noop));
    }
    std::vector<Chord> chordsOf(const char* id) const { return registry.find(id)->chords; }
};

}  // namespace

TEST(ActionRegistryTest, RebindingChangesTheChordsAndResetRestoresThem) {
    ShippedRig rig;
    const Chord ctrlK{ImGuiKey_K, MOD_CTRL | MOD_SHIFT};
    EXPECT_EQ(rig.registry.setChords(action_id::PERFORM_LOCK, {ctrlK}).result, RebindResult::Changed);
    EXPECT_EQ(rig.chordsOf(action_id::PERFORM_LOCK), std::vector<Chord>{ctrlK});
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_L, MOD_CTRL | MOD_SHIFT}), nullptr);
    EXPECT_EQ(rig.registry.findByChord(ctrlK)->id, action_id::PERFORM_LOCK);

    // A second chord, then an action with none (menu only), then a bare F-key and Delete.
    EXPECT_EQ(rig.registry.setChords(action_id::SELECT_ALL,
                                     {Chord{ImGuiKey_A, MOD_CTRL}, Chord{ImGuiKey_A, MOD_ALT}})
                  .result,
              RebindResult::Changed);
    EXPECT_EQ(rig.registry.setChords(action_id::DELETE_SELECTION, {}).result, RebindResult::Changed);
    EXPECT_EQ(rig.registry.setChords(action_id::DESELECT_ALL, {Chord{ImGuiKey_Delete, MOD_NONE}}).result,
              RebindResult::Changed)
        << "Delete, freed above, works alone";
    EXPECT_EQ(rig.registry.setChords(sceneTakeId(1), {Chord{ImGuiKey_F5, MOD_NONE}}).result,
              RebindResult::Changed);

    EXPECT_TRUE(rig.registry.resetChords(action_id::PERFORM_LOCK));
    EXPECT_EQ(rig.chordsOf(action_id::PERFORM_LOCK),
              std::vector<Chord>{(Chord{ImGuiKey_L, MOD_CTRL | MOD_SHIFT})});
    rig.registry.resetAllChords();
    for (const Action& action : rig.registry.actions()) {
        EXPECT_EQ(action.chords, action.defaultChords) << action.id;
    }
    EXPECT_FALSE(rig.registry.resetChords(action_id::BLACKOUT)) << "fixed";
    EXPECT_FALSE(rig.registry.resetChords("nope"));
}

TEST(ActionRegistryTest, RebindingRefusesReservedClashingBareAndFixed) {
    ShippedRig rig;
    const auto refusal = [&rig](const char* id, std::vector<Chord> chords) {
        return rig.registry.setChords(id, std::move(chords));
    };

    // The canvas keys, with any modifiers.
    for (int key : {int(ImGuiKey_Tab), int(ImGuiKey_UpArrow), int(ImGuiKey_LeftArrow), int(ImGuiKey_H)}) {
        const RebindCheck check = refusal(action_id::SELECT_ALL, {Chord{key, MOD_CTRL | MOD_ALT}});
        EXPECT_EQ(check.result, RebindResult::ReservedKey) << key;
        EXPECT_NE(rebindRefusal(check).find("canvas"), std::string::npos) << rebindRefusal(check);
    }

#ifdef __APPLE__
    // The chords the macOS application menu takes first: Cmd+Q, Cmd+M, Ctrl+Cmd+F.
    for (const Chord& chord : {Chord{ImGuiKey_Q, MOD_CTRL}, Chord{ImGuiKey_M, MOD_CTRL},
                               Chord{ImGuiKey_F, MOD_CTRL | MOD_SUPER}}) {
        const RebindCheck systemCheck = refusal(action_id::SELECT_ALL, {chord});
        EXPECT_EQ(systemCheck.result, RebindResult::SystemKey) << chordLabel(chord);
        EXPECT_NE(rebindRefusal(systemCheck).find("macOS"), std::string::npos) << rebindRefusal(systemCheck);
    }
#endif

    // Another action's chord, named; also on a second chord.
    RebindCheck check = refusal(action_id::PERFORM_LOCK, {Chord{ImGuiKey_S, MOD_CTRL}});
    EXPECT_EQ(check.result, RebindResult::ChordClash);
    EXPECT_EQ(check.clashWith, "Save Project");
    EXPECT_EQ(rebindRefusal(check), QM_KEY_CTRL "+S is already used by Save Project.");
    check = refusal(action_id::PERFORM_LOCK,
                    {Chord{ImGuiKey_L, MOD_CTRL | MOD_SHIFT}, Chord{ImGuiKey_B, MOD_CTRL}});
    EXPECT_EQ(check.result, RebindResult::ChordClash);
    EXPECT_EQ(check.clashWith, "Blackout");
    check = refusal(action_id::PERFORM_LOCK, {Chord{ImGuiKey_K, MOD_CTRL}, Chord{ImGuiKey_K, MOD_CTRL}});
    EXPECT_EQ(check.result, RebindResult::ChordClash) << "listed twice";

    // Bare keys: letters, digits (the scene takes), Space, Enter; F-keys and Delete pass.
    for (int key : {int(ImGuiKey_Q), int(ImGuiKey_1), int(ImGuiKey_Keypad5), int(ImGuiKey_Space),
                    int(ImGuiKey_Enter), int(ImGuiKey_Comma)}) {
        check = refusal(action_id::PERFORM_LOCK, {Chord{key, MOD_NONE}});
        EXPECT_EQ(check.result, RebindResult::NeedsModifier) << key;
    }
    EXPECT_NE(rebindRefusal(check).find("needs " QM_KEY_CTRL), std::string::npos) << rebindRefusal(check);
    EXPECT_EQ(refusal(action_id::PERFORM_LOCK, {Chord{ImGuiKey_1, MOD_SHIFT}}).result,
              RebindResult::Changed)
        << "a modifier makes a digit a chord (the takes are bare 1-9)";
    EXPECT_EQ(refusal(action_id::PERFORM_LOCK, {Chord{ImGuiKey_F12, MOD_NONE}}).result,
              RebindResult::Changed);

    // Fixed: F11, Esc, Ctrl+B.
    for (const char* id : {action_id::BLACKOUT, action_id::PERFORM_TOGGLE, action_id::PERFORM_ESCAPE}) {
        EXPECT_EQ(refusal(id, {Chord{ImGuiKey_F9, MOD_NONE}}).result, RebindResult::Fixed) << id;
    }
    EXPECT_EQ(refusal("nope", {}).result, RebindResult::UnknownId);

    // A refusal changes nothing.
    EXPECT_EQ(rig.chordsOf(action_id::BLACKOUT), std::vector<Chord>{(Chord{ImGuiKey_B, MOD_CTRL})});
    EXPECT_EQ(rig.chordsOf(action_id::SELECT_ALL), std::vector<Chord>{(Chord{ImGuiKey_A, MOD_CTRL})});
}

TEST(ActionRegistryTest, KeyBindingsListOnlyTheReboundActions) {
    ShippedRig rig;
    EXPECT_TRUE(keyBindingsOf(rig.registry).empty());
    rig.registry.setChords(action_id::PERFORM_LOCK, {Chord{ImGuiKey_K, MOD_CTRL | MOD_SHIFT}});
    rig.registry.setChords(action_id::REDO, {Chord{ImGuiKey_Y, MOD_CTRL}});
    EXPECT_EQ(keyBindingsOf(rig.registry),
              (KeyBindings{{action_id::PERFORM_LOCK, {"Ctrl+Shift+K"}}, {action_id::REDO, {"Ctrl+Y"}}}));
}

TEST(ActionRegistryTest, ApplyingKeyBindingsIgnoresWhatItCannotUse) {
    ShippedRig rig;
    rig.registry.setChords(action_id::SELECT_ALL, {Chord{ImGuiKey_A, MOD_ALT}});  // undone by apply
    const KeyBindings bindings{
        {action_id::PERFORM_LOCK, {"Ctrl+Shift+K"}},
        {"edit.noSuchAction", {"Ctrl+J"}},
        {action_id::TOGGLE_MESH_EDIT, {"Ctrl+Nope"}},          // unparseable
        {action_id::RESET_MESH, {"Ctrl+S"}},                   // Save's
        {action_id::BRING_FORWARD, {"Tab"}},                   // the canvas's
        {action_id::SEND_BACKWARD, {"Q"}},                     // bare
        {action_id::BLACKOUT, {"Ctrl+Shift+B"}},               // fixed
    };
    const std::vector<std::string> problems = applyKeyBindings(rig.registry, bindings);
    EXPECT_EQ(problems.size(), 6u);
    EXPECT_EQ(rig.chordsOf(action_id::PERFORM_LOCK),
              std::vector<Chord>{(Chord{ImGuiKey_K, MOD_CTRL | MOD_SHIFT})});
    for (const char* id : {action_id::SELECT_ALL, action_id::TOGGLE_MESH_EDIT, action_id::RESET_MESH,
                           action_id::BRING_FORWARD, action_id::SEND_BACKWARD, action_id::BLACKOUT}) {
        EXPECT_EQ(rig.registry.find(id)->chords, rig.registry.find(id)->defaultChords) << id;
    }
}

// Two actions can swap chords: every overridden action lets go of its chords first.
TEST(ActionRegistryTest, ApplyingKeyBindingsCanSwapTwoChords) {
    ShippedRig rig;
    applyKeyBindings(rig.registry, {{action_id::UNDO, {"Ctrl+Y"}}, {action_id::REDO, {"Ctrl+Z"}}});
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_Y, MOD_CTRL})->id, action_id::UNDO);
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_Z, MOD_CTRL})->id, action_id::REDO);
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_Z, MOD_CTRL | MOD_SHIFT}), nullptr);
    applyKeyBindings(rig.registry, {});
    EXPECT_EQ(rig.registry.findByChord(Chord{ImGuiKey_Z, MOD_CTRL})->id, action_id::UNDO)
        << "no bindings: the defaults";
}
