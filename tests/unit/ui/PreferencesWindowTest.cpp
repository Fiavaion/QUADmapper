// The Edit > Preferences window (ui/dialogs/PreferencesDialog) against real headless
// ImGui. The preferences file itself is tested in editor/PreferencesTest.cpp.

#include "ui/dialogs/PreferencesDialog.hpp"
#include "ui/PanelHost.hpp"
#include "core/UUID.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

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
