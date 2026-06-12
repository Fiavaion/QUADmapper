#include <gtest/gtest.h>
#include "input/KeyboardShortcuts.hpp"

#include <GLFW/glfw3.h>

namespace quadmapper {
namespace test {

class KeyboardShortcutsTest : public ::testing::Test {
protected:
    void SetUp() override {
        shortcuts = std::make_unique<KeyboardShortcuts>();
    }

    std::unique_ptr<KeyboardShortcuts> shortcuts;
};

// KeyCombination tests

TEST(KeyCombinationTest, DefaultConstructorCreatesInvalidCombination) {
    KeyCombination kc;
    EXPECT_FALSE(kc.isValid());
    EXPECT_EQ(kc.key, GLFW_KEY_UNKNOWN);
    EXPECT_EQ(kc.modifiers, ModifierFlags::None);
}

TEST(KeyCombinationTest, SingleKeyConstructor) {
    KeyCombination kc(GLFW_KEY_A);
    EXPECT_TRUE(kc.isValid());
    EXPECT_EQ(kc.key, GLFW_KEY_A);
    EXPECT_EQ(kc.modifiers, ModifierFlags::None);
}

TEST(KeyCombinationTest, KeyWithModifiersConstructor) {
    KeyCombination kc(GLFW_KEY_S, ModifierFlags::Ctrl);
    EXPECT_TRUE(kc.isValid());
    EXPECT_EQ(kc.key, GLFW_KEY_S);
    EXPECT_EQ(kc.modifiers, ModifierFlags::Ctrl);
}

TEST(KeyCombinationTest, MultipleModifiers) {
    KeyCombination kc(GLFW_KEY_S, ModifierFlags::Ctrl | ModifierFlags::Shift);
    EXPECT_TRUE(kc.isValid());
    EXPECT_TRUE(hasFlag(kc.modifiers, ModifierFlags::Ctrl));
    EXPECT_TRUE(hasFlag(kc.modifiers, ModifierFlags::Shift));
    EXPECT_FALSE(hasFlag(kc.modifiers, ModifierFlags::Alt));
}

TEST(KeyCombinationTest, EqualityOperator) {
    KeyCombination kc1(GLFW_KEY_S, ModifierFlags::Ctrl);
    KeyCombination kc2(GLFW_KEY_S, ModifierFlags::Ctrl);
    KeyCombination kc3(GLFW_KEY_S, ModifierFlags::Shift);
    KeyCombination kc4(GLFW_KEY_A, ModifierFlags::Ctrl);

    EXPECT_EQ(kc1, kc2);
    EXPECT_NE(kc1, kc3);
    EXPECT_NE(kc1, kc4);
}

TEST(KeyCombinationTest, MatchesKeyEvent) {
    KeyCombination kc(GLFW_KEY_S, ModifierFlags::Ctrl);

    EXPECT_TRUE(kc.matches(GLFW_KEY_S, ModifierFlags::Ctrl));
    EXPECT_FALSE(kc.matches(GLFW_KEY_S, ModifierFlags::None));
    EXPECT_FALSE(kc.matches(GLFW_KEY_A, ModifierFlags::Ctrl));
}

TEST(KeyCombinationTest, ToStringSimpleKey) {
    KeyCombination kc(GLFW_KEY_A);
    EXPECT_EQ(kc.toString(), "A");
}

TEST(KeyCombinationTest, ToStringWithCtrl) {
    KeyCombination kc(GLFW_KEY_S, ModifierFlags::Ctrl);
    EXPECT_EQ(kc.toString(), "Ctrl+S");
}

TEST(KeyCombinationTest, ToStringWithMultipleModifiers) {
    KeyCombination kc(GLFW_KEY_S, ModifierFlags::Ctrl | ModifierFlags::Shift);
    EXPECT_EQ(kc.toString(), "Ctrl+Shift+S");
}

TEST(KeyCombinationTest, ToStringWithAllModifiers) {
    KeyCombination kc(GLFW_KEY_A, ModifierFlags::Ctrl | ModifierFlags::Shift |
                                   ModifierFlags::Alt | ModifierFlags::Super);
    EXPECT_EQ(kc.toString(), "Ctrl+Shift+Alt+Super+A");
}

TEST(KeyCombinationTest, ToStringInvalidReturnsNone) {
    KeyCombination kc;
    EXPECT_EQ(kc.toString(), "None");
}

TEST(KeyCombinationTest, FromStringSimpleKey) {
    KeyCombination kc = KeyCombination::fromString("A");
    EXPECT_TRUE(kc.isValid());
    EXPECT_EQ(kc.key, GLFW_KEY_A);
    EXPECT_EQ(kc.modifiers, ModifierFlags::None);
}

TEST(KeyCombinationTest, FromStringWithCtrl) {
    KeyCombination kc = KeyCombination::fromString("Ctrl+S");
    EXPECT_TRUE(kc.isValid());
    EXPECT_EQ(kc.key, GLFW_KEY_S);
    EXPECT_EQ(kc.modifiers, ModifierFlags::Ctrl);
}

TEST(KeyCombinationTest, FromStringWithMultipleModifiers) {
    KeyCombination kc = KeyCombination::fromString("Ctrl+Shift+S");
    EXPECT_TRUE(kc.isValid());
    EXPECT_EQ(kc.key, GLFW_KEY_S);
    EXPECT_TRUE(hasFlag(kc.modifiers, ModifierFlags::Ctrl));
    EXPECT_TRUE(hasFlag(kc.modifiers, ModifierFlags::Shift));
}

TEST(KeyCombinationTest, FromStringNone) {
    KeyCombination kc = KeyCombination::fromString("None");
    EXPECT_FALSE(kc.isValid());
}

TEST(KeyCombinationTest, FromStringEmpty) {
    KeyCombination kc = KeyCombination::fromString("");
    EXPECT_FALSE(kc.isValid());
}

TEST(KeyCombinationTest, RoundTripConversion) {
    KeyCombination original(GLFW_KEY_F5, ModifierFlags::Ctrl | ModifierFlags::Alt);
    std::string str = original.toString();
    KeyCombination parsed = KeyCombination::fromString(str);

    EXPECT_EQ(original, parsed);
}

// KeyboardShortcuts tests

TEST_F(KeyboardShortcutsTest, InitializesWithDefaults) {
    EXPECT_FALSE(shortcuts->getAllShortcuts().empty());
    EXPECT_GT(shortcuts->getAllShortcuts().size(), 40);  // Should have many shortcuts
}

TEST_F(KeyboardShortcutsTest, GetBindingReturnsCorrectDefault) {
    KeyCombination binding = shortcuts->getBinding(ShortcutAction::SaveProject);
    EXPECT_TRUE(binding.isValid());
    EXPECT_EQ(binding.key, GLFW_KEY_S);
    EXPECT_EQ(binding.modifiers, ModifierFlags::Ctrl);
}

TEST_F(KeyboardShortcutsTest, GetBindingForDeleteKey) {
    KeyCombination binding = shortcuts->getBinding(ShortcutAction::Delete);
    EXPECT_TRUE(binding.isValid());
    EXPECT_EQ(binding.key, GLFW_KEY_DELETE);
    EXPECT_EQ(binding.modifiers, ModifierFlags::None);
}

TEST_F(KeyboardShortcutsTest, SetBindingChangesBinding) {
    KeyCombination newKey(GLFW_KEY_F2, ModifierFlags::Ctrl);
    bool success = shortcuts->setBinding(ShortcutAction::SaveProject, newKey);

    EXPECT_TRUE(success);
    EXPECT_EQ(shortcuts->getBinding(ShortcutAction::SaveProject), newKey);
}

TEST_F(KeyboardShortcutsTest, SetBindingDetectsConflict) {
    // Try to set SaveProject to the same key as OpenProject (Ctrl+O)
    KeyCombination conflictKey(GLFW_KEY_O, ModifierFlags::Ctrl);
    bool success = shortcuts->setBinding(ShortcutAction::SaveProject, conflictKey);

    EXPECT_FALSE(success);
    // Original binding should be unchanged
    EXPECT_NE(shortcuts->getBinding(ShortcutAction::SaveProject), conflictKey);
}

TEST_F(KeyboardShortcutsTest, ResetBindingRestoresDefault) {
    // First change the binding
    KeyCombination newKey(GLFW_KEY_F2, ModifierFlags::Ctrl);
    shortcuts->setBinding(ShortcutAction::SaveProject, newKey);

    // Then reset it
    shortcuts->resetBinding(ShortcutAction::SaveProject);

    // Should be back to default
    KeyCombination binding = shortcuts->getBinding(ShortcutAction::SaveProject);
    EXPECT_EQ(binding.key, GLFW_KEY_S);
    EXPECT_EQ(binding.modifiers, ModifierFlags::Ctrl);
}

TEST_F(KeyboardShortcutsTest, ResetAllBindingsRestoresAllDefaults) {
    // Change multiple bindings
    shortcuts->setBinding(ShortcutAction::SaveProject,
                         KeyCombination(GLFW_KEY_F2, ModifierFlags::Ctrl));
    shortcuts->setBinding(ShortcutAction::Delete,
                         KeyCombination(GLFW_KEY_BACKSPACE));

    // Reset all
    shortcuts->resetAllBindings();

    // Check they're all back to defaults
    EXPECT_EQ(shortcuts->getBinding(ShortcutAction::SaveProject).key, GLFW_KEY_S);
    EXPECT_EQ(shortcuts->getBinding(ShortcutAction::Delete).key, GLFW_KEY_DELETE);
}

TEST_F(KeyboardShortcutsTest, FindConflictReturnsConflictingAction) {
    KeyCombination ctrlO(GLFW_KEY_O, ModifierFlags::Ctrl);

    auto conflict = shortcuts->findConflict(ctrlO, ShortcutAction::SaveProject);

    EXPECT_TRUE(conflict.has_value());
    EXPECT_EQ(*conflict, ShortcutAction::OpenProject);
}

TEST_F(KeyboardShortcutsTest, FindConflictReturnsNulloptForNoConflict) {
    KeyCombination unusedKey(GLFW_KEY_F15, ModifierFlags::Ctrl | ModifierFlags::Alt);

    auto conflict = shortcuts->findConflict(unusedKey, ShortcutAction::SaveProject);

    EXPECT_FALSE(conflict.has_value());
}

TEST_F(KeyboardShortcutsTest, FindConflictExcludesSpecifiedAction) {
    KeyCombination ctrlS(GLFW_KEY_S, ModifierFlags::Ctrl);

    // Should not report SaveProject as conflicting with itself
    auto conflict = shortcuts->findConflict(ctrlS, ShortcutAction::SaveProject);

    EXPECT_FALSE(conflict.has_value());
}

TEST_F(KeyboardShortcutsTest, GetActionNameReturnsCorrectName) {
    EXPECT_EQ(shortcuts->getActionName(ShortcutAction::SaveProject), "Save Project");
    EXPECT_EQ(shortcuts->getActionName(ShortcutAction::Undo), "Undo");
    EXPECT_EQ(shortcuts->getActionName(ShortcutAction::PlayPause), "Play/Pause");
}

TEST_F(KeyboardShortcutsTest, GetActionCategoryReturnsCorrectCategory) {
    EXPECT_EQ(shortcuts->getActionCategory(ShortcutAction::SaveProject), "File");
    EXPECT_EQ(shortcuts->getActionCategory(ShortcutAction::Undo), "Edit");
    EXPECT_EQ(shortcuts->getActionCategory(ShortcutAction::PlayPause), "Playback");
}

TEST_F(KeyboardShortcutsTest, GetCategoriesReturnsAllCategories) {
    auto categories = shortcuts->getCategories();

    EXPECT_FALSE(categories.empty());
    EXPECT_NE(std::find(categories.begin(), categories.end(), "File"), categories.end());
    EXPECT_NE(std::find(categories.begin(), categories.end(), "Edit"), categories.end());
    EXPECT_NE(std::find(categories.begin(), categories.end(), "Playback"), categories.end());
}

TEST_F(KeyboardShortcutsTest, GetShortcutsByCategoryFiltersCorrectly) {
    auto fileShortcuts = shortcuts->getShortcutsByCategory("File");

    EXPECT_FALSE(fileShortcuts.empty());
    for (const auto& info : fileShortcuts) {
        EXPECT_EQ(info.category, "File");
    }
}

TEST_F(KeyboardShortcutsTest, HasModifiedBindingsReturnsFalseInitially) {
    EXPECT_FALSE(shortcuts->hasModifiedBindings());
}

TEST_F(KeyboardShortcutsTest, HasModifiedBindingsReturnsTrueAfterChange) {
    shortcuts->setBinding(ShortcutAction::SaveProject,
                         KeyCombination(GLFW_KEY_F2, ModifierFlags::Ctrl));

    EXPECT_TRUE(shortcuts->hasModifiedBindings());
}

TEST_F(KeyboardShortcutsTest, HasModifiedBindingsReturnsFalseAfterReset) {
    shortcuts->setBinding(ShortcutAction::SaveProject,
                         KeyCombination(GLFW_KEY_F2, ModifierFlags::Ctrl));
    shortcuts->resetAllBindings();

    EXPECT_FALSE(shortcuts->hasModifiedBindings());
}

TEST_F(KeyboardShortcutsTest, CallbackIsInvokedOnKeyEvent) {
    bool callbackInvoked = false;
    shortcuts->registerCallback(ShortcutAction::SaveProject, [&callbackInvoked]() {
        callbackInvoked = true;
    });

    // Simulate Ctrl+S key press
    bool handled = shortcuts->processKeyEvent(GLFW_KEY_S, 0, GLFW_PRESS, GLFW_MOD_CONTROL);

    EXPECT_TRUE(handled);
    EXPECT_TRUE(callbackInvoked);
}

TEST_F(KeyboardShortcutsTest, CallbackNotInvokedOnKeyRelease) {
    bool callbackInvoked = false;
    shortcuts->registerCallback(ShortcutAction::SaveProject, [&callbackInvoked]() {
        callbackInvoked = true;
    });

    // Simulate key release (not press)
    bool handled = shortcuts->processKeyEvent(GLFW_KEY_S, 0, GLFW_RELEASE, GLFW_MOD_CONTROL);

    EXPECT_FALSE(handled);
    EXPECT_FALSE(callbackInvoked);
}

TEST_F(KeyboardShortcutsTest, CallbackNotInvokedForUnboundKey) {
    bool callbackInvoked = false;
    shortcuts->registerCallback(ShortcutAction::SaveProject, [&callbackInvoked]() {
        callbackInvoked = true;
    });

    // Simulate a key that isn't bound
    bool handled = shortcuts->processKeyEvent(GLFW_KEY_F15, 0, GLFW_PRESS, 0);

    EXPECT_FALSE(handled);
    EXPECT_FALSE(callbackInvoked);
}

TEST_F(KeyboardShortcutsTest, MultipleCallbacksCanBeRegistered) {
    int callCount = 0;
    shortcuts->registerCallback(ShortcutAction::SaveProject, [&callCount]() {
        callCount++;
    });
    shortcuts->registerCallback(ShortcutAction::SaveProject, [&callCount]() {
        callCount++;
    });

    shortcuts->processKeyEvent(GLFW_KEY_S, 0, GLFW_PRESS, GLFW_MOD_CONTROL);

    EXPECT_EQ(callCount, 2);
}

TEST_F(KeyboardShortcutsTest, ClearCallbacksRemovesAllCallbacks) {
    int callCount = 0;
    shortcuts->registerCallback(ShortcutAction::SaveProject, [&callCount]() {
        callCount++;
    });

    shortcuts->clearCallbacks(ShortcutAction::SaveProject);
    shortcuts->processKeyEvent(GLFW_KEY_S, 0, GLFW_PRESS, GLFW_MOD_CONTROL);

    EXPECT_EQ(callCount, 0);
}

TEST_F(KeyboardShortcutsTest, SetBindingToEmptyClearsBinding) {
    shortcuts->setBinding(ShortcutAction::SaveProject, KeyCombination());

    KeyCombination binding = shortcuts->getBinding(ShortcutAction::SaveProject);
    EXPECT_FALSE(binding.isValid());
}

// JSON serialization tests

TEST_F(KeyboardShortcutsTest, ToJsonIncludesOnlyModifiedBindings) {
    std::string json = shortcuts->toJson();

    // No bindings should be in the JSON since nothing is modified
    EXPECT_NE(json.find("\"bindings\": {}"), std::string::npos);
}

TEST_F(KeyboardShortcutsTest, ToJsonIncludesModifiedBindings) {
    shortcuts->setBinding(ShortcutAction::SaveProject,
                         KeyCombination(GLFW_KEY_F2, ModifierFlags::Ctrl));

    std::string json = shortcuts->toJson();

    EXPECT_NE(json.find("Save Project"), std::string::npos);
    EXPECT_NE(json.find("Ctrl+F2"), std::string::npos);
}

TEST_F(KeyboardShortcutsTest, FromJsonLoadsSavedBindings) {
    std::string json = R"({
        "version": "1.0",
        "bindings": {
            "Save Project": "Ctrl+F2"
        }
    })";

    auto result = shortcuts->fromJson(json);

    EXPECT_TRUE(result.isSuccess());
    KeyCombination binding = shortcuts->getBinding(ShortcutAction::SaveProject);
    EXPECT_EQ(binding.key, GLFW_KEY_F2);
    EXPECT_EQ(binding.modifiers, ModifierFlags::Ctrl);
}

TEST_F(KeyboardShortcutsTest, FromJsonFailsWithoutVersion) {
    std::string json = R"({
        "bindings": {
            "Save Project": "Ctrl+F2"
        }
    })";

    auto result = shortcuts->fromJson(json);

    EXPECT_TRUE(result.isError());
}

TEST_F(KeyboardShortcutsTest, FromJsonFailsWithInvalidJson) {
    std::string json = "not valid json";

    auto result = shortcuts->fromJson(json);

    EXPECT_TRUE(result.isError());
}

TEST_F(KeyboardShortcutsTest, RoundTripJsonSerialization) {
    // Modify some bindings
    shortcuts->setBinding(ShortcutAction::SaveProject,
                         KeyCombination(GLFW_KEY_F2, ModifierFlags::Ctrl));
    shortcuts->setBinding(ShortcutAction::Undo,
                         KeyCombination(GLFW_KEY_Z, ModifierFlags::Ctrl | ModifierFlags::Shift));

    // Serialize
    std::string json = shortcuts->toJson();

    // Create new instance and deserialize
    KeyboardShortcuts newShortcuts;
    auto result = newShortcuts.fromJson(json);

    EXPECT_TRUE(result.isSuccess());
    EXPECT_EQ(newShortcuts.getBinding(ShortcutAction::SaveProject),
              shortcuts->getBinding(ShortcutAction::SaveProject));
    EXPECT_EQ(newShortcuts.getBinding(ShortcutAction::Undo),
              shortcuts->getBinding(ShortcutAction::Undo));
}

// Modifier flag tests

TEST(ModifierFlagsTest, FromGLFWModsConvertsCorrectly) {
    EXPECT_EQ(KeyboardShortcuts::fromGLFWMods(0), ModifierFlags::None);
    EXPECT_EQ(KeyboardShortcuts::fromGLFWMods(GLFW_MOD_CONTROL), ModifierFlags::Ctrl);
    EXPECT_EQ(KeyboardShortcuts::fromGLFWMods(GLFW_MOD_SHIFT), ModifierFlags::Shift);
    EXPECT_EQ(KeyboardShortcuts::fromGLFWMods(GLFW_MOD_ALT), ModifierFlags::Alt);
    EXPECT_EQ(KeyboardShortcuts::fromGLFWMods(GLFW_MOD_SUPER), ModifierFlags::Super);
}

TEST(ModifierFlagsTest, FromGLFWModsCombinesMultiple) {
    ModifierFlags mods = KeyboardShortcuts::fromGLFWMods(GLFW_MOD_CONTROL | GLFW_MOD_SHIFT);

    EXPECT_TRUE(hasFlag(mods, ModifierFlags::Ctrl));
    EXPECT_TRUE(hasFlag(mods, ModifierFlags::Shift));
    EXPECT_FALSE(hasFlag(mods, ModifierFlags::Alt));
}

// Key name tests

TEST(KeyNameTest, GetKeyNameReturnsCorrectName) {
    EXPECT_EQ(KeyboardShortcuts::getKeyName(GLFW_KEY_A), "A");
    EXPECT_EQ(KeyboardShortcuts::getKeyName(GLFW_KEY_SPACE), "Space");
    EXPECT_EQ(KeyboardShortcuts::getKeyName(GLFW_KEY_ENTER), "Enter");
    EXPECT_EQ(KeyboardShortcuts::getKeyName(GLFW_KEY_F1), "F1");
    EXPECT_EQ(KeyboardShortcuts::getKeyName(GLFW_KEY_LEFT_BRACKET), "[");
    EXPECT_EQ(KeyboardShortcuts::getKeyName(GLFW_KEY_RIGHT_BRACKET), "]");
}

TEST(KeyNameTest, ParseKeyNameReturnsCorrectCode) {
    EXPECT_EQ(KeyboardShortcuts::parseKeyName("A"), GLFW_KEY_A);
    EXPECT_EQ(KeyboardShortcuts::parseKeyName("Space"), GLFW_KEY_SPACE);
    EXPECT_EQ(KeyboardShortcuts::parseKeyName("Enter"), GLFW_KEY_ENTER);
    EXPECT_EQ(KeyboardShortcuts::parseKeyName("F1"), GLFW_KEY_F1);
}

TEST(KeyNameTest, ParseKeyNameIsCaseInsensitive) {
    EXPECT_EQ(KeyboardShortcuts::parseKeyName("a"), GLFW_KEY_A);
    EXPECT_EQ(KeyboardShortcuts::parseKeyName("space"), GLFW_KEY_SPACE);
    EXPECT_EQ(KeyboardShortcuts::parseKeyName("ENTER"), GLFW_KEY_ENTER);
}

TEST(KeyNameTest, ParseUnknownKeyReturnsUnknown) {
    EXPECT_EQ(KeyboardShortcuts::parseKeyName("InvalidKey"), GLFW_KEY_UNKNOWN);
}

} // namespace test
} // namespace quadmapper
