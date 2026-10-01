// The Edit > Preferences window (ui/dialogs/PreferencesDialog) against real headless
// ImGui. The preferences file itself is tested in editor/PreferencesTest.cpp.

#include "ui/dialogs/PreferencesDialog.hpp"
#include "ui/PanelHost.hpp"
#include "core/UUID.hpp"
#include "editor/ActionRegistry.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/PerformLock.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <vector>

using namespace quadmapper;

namespace {

struct TempPrefsFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("qm_prefs_" + UUID::generate().toString() + ".json");
    ~TempPrefsFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    void write(const char* text) const {
        std::ofstream out(path, std::ios::binary);
        out << text;
    }
    nlohmann::json read() const {
        std::ifstream in(path, std::ios::binary);
        return nlohmann::json::parse(in);
    }
};

}  // namespace

// The window against real (headless) ImGui, drawn by a PanelHost: shown only while it is
// visible (Edit > Preferences... opens it),
// and its General tab edits the editor's UI mode directly (Application saves it).
TEST(PreferencesWindowTest, RendersOnlyWhenOpenAndShowsTheEditorsMode) {
    ImGuiContext* imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    EditorState es;
    EditorContext ctx{es, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    PreferencesDialog dialog;
    PanelHost host;
    host.add(dialog);
    auto frame = [&] {
        ImGui::NewFrame();
        host.drawWindows(ctx);
        const bool shown = ImGui::FindWindowByName("Preferences") != nullptr &&
                           ImGui::FindWindowByName("Preferences")->Active;
        ImGui::Render();
        return shown;
    };
    EXPECT_FALSE(frame()) << "closed by default";
    es.requestPanelOpen(PreferencesDialog::ID);
    es.uiMode = EditorState::UiMode::Advanced;
    EXPECT_TRUE(frame());
    EXPECT_TRUE(frame());
    EXPECT_EQ(es.uiMode, EditorState::UiMode::Advanced) << "rendering alone changes nothing";
    dialog.visible = false;
    EXPECT_FALSE(frame());
    ImGui::DestroyContext(imgui);
}

// Dragging "Every (minutes)" applies the value on every frame but writes the file once,
// on release: it used to rewrite preferences.json on every frame of the drag.
TEST(PreferencesWindowTest, DraggingTheAutosaveSliderSavesOnceOnRelease) {
    TempPrefsFile file;
    ImGuiContext* imgui = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    EditorState es;
    EditorContext ctx{es, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    PreferencesDialog dialog;
    dialog.visible = true;
    PanelHost host;
    host.add(dialog);
    dialog.setConfigPath(file.path);
    const int& interval = dialog.getPreferences().general.autoSaveIntervalMinutes;
    auto frame = [&] {
        ImGui::NewFrame();
        host.drawWindows(ctx);
        ImGui::Render();
    };
    frame();
    frame();

    // The slider's id: its label hashed in the General tab's id scope.
    ImGuiWindow* window = ImGui::FindWindowByName("Preferences");
    ASSERT_NE(window, nullptr);
    ImGuiTabBar* tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("PreferenceTabs"));
    ASSERT_NE(tabs, nullptr);
    const ImGuiID slider = ImHashStr("Every (minutes)", 0, tabs->VisibleTabId);

    // Find it by hovering down the left of the window (it is 200 px wide there).
    ImVec2 at;
    bool found = false;
    for (float y = window->Pos.y; y < window->Pos.y + window->Size.y && !found; y += 4.0f) {
        for (float x = window->Pos.x + 16.0f; x < window->Pos.x + 200.0f && !found; x += 24.0f) {
            io.AddMousePosEvent(x, y);
            frame();
            if (ImGui::GetCurrentContext()->HoveredId == slider) {
                at = ImVec2(x, y);
                found = true;
            }
        }
    }
    ASSERT_TRUE(found) << "the autosave slider was not found under the mouse";

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    const int pressed = interval;
    for (int i = 1; i <= 20; ++i) {
        io.AddMousePosEvent(at.x + 8.0f * static_cast<float>(i), at.y);
        frame();
    }
    EXPECT_GT(interval, pressed) << "the drag applies at once";
    EXPECT_FALSE(std::filesystem::exists(file.path)) << "written while still dragging";

    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    ASSERT_TRUE(std::filesystem::exists(file.path)) << "not written on release";
    EXPECT_EQ(file.read()["general"]["autoSaveIntervalMinutes"].get<int>(), interval);

    // Once: later frames do not write it again.
    std::filesystem::remove(file.path);
    for (int i = 0; i < 5; ++i) frame();
    EXPECT_FALSE(std::filesystem::exists(file.path));

    ImGui::DestroyContext(imgui);
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(file.path) += ".bak", ec);
}

// ---- Keyboard: rebinding (owner's decision, 2026-09-30) --------------------------------

namespace {

// The Preferences window in a PanelHost on headless ImGui, with a small registry, and
// the mouse and keys driven through io events.
class KeyboardTabTest : public ::testing::Test {
protected:
    void SetUp() override {
        imgui_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280.0f, 800.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

        const auto noop = [](EditorContext&) {};
        for (const char* id : {action_id::SAVE, action_id::BLACKOUT, action_id::PERFORM_LOCK}) {
            ASSERT_EQ(registry.add(*makeAppAction(id, noop)), AddResult::Added) << id;
        }
        ctx.actions = &registry;
        dialog.visible = true;
        dialog.setConfigPath(file.path);
        host.add(dialog);
        frame();
        frame();
        window = ImGui::FindWindowByName("Preferences");
        ASSERT_NE(window, nullptr);
        tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("PreferenceTabs"));
        ASSERT_NE(tabs, nullptr);
        keyboardTab = ImHashStr("Keyboard", 0, tabs->ID);
    }

    void TearDown() override {
        ImGui::DestroyContext(imgui_);
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(file.path) += ".bak", ec);
    }

    void frame() {
        ImGui::NewFrame();
        host.drawWindows(ctx);
        ImGui::Render();
    }

    // Hovers down the window until `id` is under the mouse. False if it never is.
    bool hover(ImGuiID id) {
        ImGuiIO& io = ImGui::GetIO();
        for (float y = window->Pos.y; y < window->Pos.y + window->Size.y; y += 4.0f) {
            for (float x = window->Pos.x + 4.0f; x < window->Pos.x + window->Size.x; x += 16.0f) {
                io.AddMousePosEvent(x, y);
                frame();
                if (ImGui::GetCurrentContext()->HoveredId == id) return true;
            }
        }
        return false;
    }

    bool click(ImGuiID id) {
        if (!hover(id)) return false;
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        return true;
    }

    // Press `keys` together for one frame, then release them.
    void press(std::initializer_list<ImGuiKey> keys) {
        ImGuiIO& io = ImGui::GetIO();
        for (ImGuiKey key : keys) io.AddKeyEvent(key, true);
        frame();
        for (ImGuiKey key : keys) io.AddKeyEvent(key, false);
        frame();
    }

    // A button in an action's row of the Keyboard table.
    ImGuiID rowButton(const char* actionId, const char* button) const {
        const ImGuiID table = ImHashStr("Shortcuts", 0, keyboardTab);
        return ImHashStr(button, 0, ImHashStr(actionId, 0, table));
    }

    std::vector<Chord> chordsOf(const char* id) const { return registry.find(id)->chords; }

    TempPrefsFile file;
    ImGuiContext* imgui_ = nullptr;
    EditorState es;
    EditorContext ctx{es, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    ActionRegistry registry;
    PreferencesDialog dialog;
    PanelHost host;
    ImGuiWindow* window = nullptr;
    ImGuiTabBar* tabs = nullptr;
    ImGuiID keyboardTab = 0;
};

const Chord CTRL_SHIFT_L{ImGuiKey_L, MOD_CTRL | MOD_SHIFT};
const Chord CTRL_SHIFT_K{ImGuiKey_K, MOD_CTRL | MOD_SHIFT};

}  // namespace

// Change captures the next chord: it is bound, saved in the preferences file as text,
// and Reset puts the default back (and takes it out of the file).
TEST_F(KeyboardTabTest, ChangeCapturesTheNextChordAndSavesIt) {
    ASSERT_TRUE(click(keyboardTab));
    ASSERT_EQ(tabs->VisibleTabId, keyboardTab);

    ASSERT_TRUE(click(rowButton(action_id::PERFORM_LOCK, "Change")));
    EXPECT_TRUE(es.capturingChord) << "the shortcuts leave the next key alone";
    press({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_K});
    EXPECT_FALSE(es.capturingChord);
    EXPECT_EQ(chordsOf(action_id::PERFORM_LOCK), std::vector<Chord>{CTRL_SHIFT_K});
    ASSERT_TRUE(std::filesystem::exists(file.path));
    EXPECT_EQ(file.read()["keyboard"][action_id::PERFORM_LOCK], nlohmann::json::array({"Ctrl+Shift+K"}));
    EXPECT_EQ(dialog.getPreferences().keyBindings,
              (KeyBindings{{action_id::PERFORM_LOCK, {"Ctrl+Shift+K"}}}));

    ASSERT_TRUE(click(rowButton(action_id::PERFORM_LOCK, "Reset")));
    EXPECT_EQ(chordsOf(action_id::PERFORM_LOCK), std::vector<Chord>{CTRL_SHIFT_L});
    EXPECT_TRUE(file.read()["keyboard"].empty());
}

// Add captures a second chord; Remove drops it again.
TEST_F(KeyboardTabTest, AddAndRemoveASecondChord) {
    ASSERT_TRUE(click(keyboardTab));
    ASSERT_TRUE(click(rowButton(action_id::PERFORM_LOCK, "Add")));
    press({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_K});
    EXPECT_EQ(chordsOf(action_id::PERFORM_LOCK), (std::vector<Chord>{CTRL_SHIFT_L, CTRL_SHIFT_K}));

    ASSERT_TRUE(click(rowButton(action_id::PERFORM_LOCK, "Remove")));
    EXPECT_EQ(chordsOf(action_id::PERFORM_LOCK), std::vector<Chord>{CTRL_SHIFT_L});
}

// Refused chords change nothing: another action's (Save's), a canvas key, a bare
// letter. Esc cancels a capture, and so does locking the show.
TEST_F(KeyboardTabTest, RefusedChordsAndEscChangeNothing) {
    ASSERT_TRUE(click(keyboardTab));
    for (std::initializer_list<ImGuiKey> keys :
         {std::initializer_list<ImGuiKey>{ImGuiMod_Ctrl, ImGuiKey_S},
          std::initializer_list<ImGuiKey>{ImGuiMod_Ctrl, ImGuiKey_Tab},
          std::initializer_list<ImGuiKey>{ImGuiKey_Q},
          std::initializer_list<ImGuiKey>{ImGuiKey_Escape}}) {
        ASSERT_TRUE(click(rowButton(action_id::PERFORM_LOCK, "Change")));
        ASSERT_TRUE(es.capturingChord);
        press(keys);
        EXPECT_FALSE(es.capturingChord);
        EXPECT_EQ(chordsOf(action_id::PERFORM_LOCK), std::vector<Chord>{CTRL_SHIFT_L});
    }
    EXPECT_EQ(chordsOf(action_id::SAVE), std::vector<Chord>{(Chord{ImGuiKey_S, MOD_CTRL})});
    EXPECT_FALSE(std::filesystem::exists(file.path)) << "nothing changed, nothing saved";

    ASSERT_TRUE(click(rowButton(action_id::PERFORM_LOCK, "Change")));
    ASSERT_TRUE(es.capturingChord);
    lockPerform(es);
    frame();
    EXPECT_FALSE(es.capturingChord) << "the lock ends the capture";
    press({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_K});
    EXPECT_EQ(chordsOf(action_id::PERFORM_LOCK), std::vector<Chord>{CTRL_SHIFT_L});
}

// The fixed keys (Ctrl+B here) are listed but have no Change.
TEST_F(KeyboardTabTest, FixedKeysCannotBeChanged) {
    ASSERT_TRUE(click(keyboardTab));
    EXPECT_TRUE(hover(rowButton(action_id::SAVE, "Change"))) << "a rebindable row";
    EXPECT_FALSE(hover(rowButton(action_id::BLACKOUT, "Change")));
    EXPECT_FALSE(es.capturingChord);
}

// TD-34: Browse's folder dialog has no owner, so it can be answered after the perform
// lock was taken. The answer is refused (ADR-051 §4 blocks Preferences edits), and
// nothing is saved; unlocked, the same answer is taken and saved.
TEST(PreferencesWindowTest, AMediaFolderPickedAfterLockingIsRefused) {
    TempPrefsFile file;
    PreferencesDialog dialog;
    dialog.setConfigPath(file.path);
    EditorState es;
    EditorContext ctx{es, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    const std::string folder = std::filesystem::temp_directory_path().string();

    lockPerform(es);
    dialog.mediaFolderChosen(ctx, folder);
    EXPECT_TRUE(dialog.getPreferences().general.defaultMediaFolder.empty());
    EXPECT_FALSE(std::filesystem::exists(file.path)) << "nothing saved";
    EXPECT_NE(std::string(es.statusMessage).find("the show is locked"), std::string::npos)
        << es.statusMessage;

    requestPerformUnlock(es);
    ASSERT_TRUE(confirmPerformUnlock(es));
    dialog.mediaFolderChosen(ctx, folder);
    EXPECT_EQ(dialog.getPreferences().general.defaultMediaFolder, folder);
    EXPECT_EQ(file.read()["general"]["defaultMediaFolder"], folder);
}
