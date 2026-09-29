// The PanelHost (ui/PanelHost, ADR-051 §2) against real headless ImGui: which panels it
// draws and where, the Window menu it generates, and the open requests it drains.

#include "ui/PanelHost.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "ui/dialogs/MissingMediaWindow.hpp"
#include "ui/panels/ResourcesPanel.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>

#include <string>

using namespace quadmapper;

namespace {

class FakePanel : public Panel {
public:
    FakePanel(const char* id, PanelKind kind, const char* menuLabel = "", bool shown = true)
        : id_(id), kind_(kind), menuLabel_(menuLabel) {
        visible = shown;
    }
    const char* id() const override { return id_; }
    // "" = listed under its id; nullptr = unlisted.
    const char* menuLabel() const override {
        return menuLabel_ && !*menuLabel_ ? id_ : menuLabel_;
    }
    PanelKind kind() const override { return kind_; }
    bool available(const EditorContext&) const override { return available_; }
    void onAttach(EditorContext&) override { ++attached; }
    void onProjectReplaced(EditorContext&) override { ++replaced; }
    void tick(EditorContext&) override { ++ticks; }
    void drawContent(EditorContext&) override {
        ++draws;
        drawnIn = ImGui::GetCurrentWindow()->Name;
    }

    bool available_ = true;
    int attached = 0, replaced = 0, ticks = 0, draws = 0;
    std::string drawnIn;

private:
    const char* id_;
    PanelKind kind_;
    const char* menuLabel_;
};

class PanelHostTest : public ::testing::Test {
protected:
    void SetUp() override {
        imgui_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280.0f, 800.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }
    void TearDown() override { ImGui::DestroyContext(imgui_); }

    // One frame: the Window panels, then the overlays.
    void frame() {
        ImGui::NewFrame();
        host.drawWindows(ctx);
        host.drawOverlays(ctx);
        ImGui::Render();
    }

    static bool windowActive(const char* name) {
        const ImGuiWindow* window = ImGui::FindWindowByName(name);
        return window && window->Active;
    }

    // The Window menu's items as rendered text, drawn into a plain window (its second
    // frame, once the window has sized itself).
    std::string windowMenuText() {
        std::string text;
        for (int i = 0; i < 2; ++i) {
            ImGui::NewFrame();
            ImGui::Begin("Menu");
            ImGui::LogToBuffer();
            host.drawWindowMenuItems();
            text = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            ImGui::End();
            ImGui::Render();
        }
        return text;
    }

    ImGuiContext* imgui_ = nullptr;
    EditorState es;
    EditorContext ctx{es, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    PanelHost host;
};

}  // namespace

TEST_F(PanelHostTest, HiddenPanelsAreNotDrawn) {
    FakePanel shown("Shown", PanelKind::Window);
    FakePanel hidden("Hidden", PanelKind::Window, "", /*shown=*/false);
    host.add(shown);
    host.add(hidden);

    frame();
    EXPECT_EQ(shown.draws, 1);
    EXPECT_EQ(shown.drawnIn, "Shown") << "the host opens the panel's own window";
    EXPECT_TRUE(windowActive("Shown"));
    EXPECT_EQ(hidden.draws, 0);
    EXPECT_FALSE(windowActive("Hidden"));
    EXPECT_EQ(hidden.ticks, 1) << "a hidden panel still ticks";

    shown.visible = false;
    frame();
    EXPECT_EQ(shown.draws, 1);
    EXPECT_FALSE(windowActive("Shown"));
}

TEST_F(PanelHostTest, UnavailablePanelsAreNotDrawn) {
    FakePanel window("Window", PanelKind::Window);
    window.available_ = false;
    host.add(window);
    frame();
    EXPECT_EQ(window.draws, 0);
    EXPECT_FALSE(windowActive("Window"));
}

TEST_F(PanelHostTest, WindowMenuListsEveryWindowPanelWithItsLabel) {
    FakePanel quadList("Quad List", PanelKind::Window);
    FakePanel control("Control", PanelKind::Window, "Control (OSC / MIDI)", /*shown=*/false);
    FakePanel unlisted("Preferences", PanelKind::Window, nullptr, /*shown=*/false);
    FakePanel tab("Media", PanelKind::Tab);
    FakePanel overlay("Banners", PanelKind::Overlay, nullptr);
    for (FakePanel* panel : {&quadList, &control, &unlisted, &tab, &overlay}) host.add(*panel);

    const std::string menu = windowMenuText();
    EXPECT_NE(menu.find("Quad List"), std::string::npos) << menu;
    EXPECT_NE(menu.find("Control (OSC / MIDI)"), std::string::npos) << menu;
    EXPECT_NE(menu.find("Reset Layout"), std::string::npos) << menu;
    EXPECT_EQ(menu.find("Preferences"), std::string::npos) << "unlisted: " << menu;
    EXPECT_EQ(menu.find("Media"), std::string::npos) << "a tab: " << menu;
    EXPECT_EQ(menu.find("Banners"), std::string::npos) << "an overlay: " << menu;
}

TEST_F(PanelHostTest, TabPanelDrawsInsideResources) {
    ResourcesPanel resources(host);
    FakePanel media("Media", PanelKind::Tab);
    host.add(resources);
    host.add(media);

    frame();
    frame();  // a tab bar settles its selection on its first frame
    EXPECT_GE(media.draws, 1);
    EXPECT_EQ(media.drawnIn, "Resources");
    EXPECT_FALSE(windowActive("Media")) << "a tab has no window of its own";

    resources.visible = false;
    const int before = media.draws;
    frame();
    EXPECT_EQ(media.draws, before) << "hidden with the Resources window";
}

TEST_F(PanelHostTest, UnavailableTabIsLeftOut) {
    ResourcesPanel resources(host);
    FakePanel project("Project", PanelKind::Tab);
    project.available_ = false;  // Simple mode hides the Advanced tabs
    host.add(resources);
    host.add(project);
    frame();
    frame();
    EXPECT_EQ(project.draws, 0);
}

TEST_F(PanelHostTest, RequestOpenOpensAPanel) {
    FakePanel missing("Missing", PanelKind::Window, nullptr, /*shown=*/false);
    host.add(missing);
    frame();
    EXPECT_EQ(missing.draws, 0);

    es.requestPanelOpen("Missing");
    es.requestPanelOpen("No such panel");  // logged and dropped
    frame();
    EXPECT_TRUE(missing.visible);
    EXPECT_EQ(missing.draws, 1) << "opened and drawn in the same frame";
    EXPECT_TRUE(es.panelOpenRequests.empty());
}

TEST_F(PanelHostTest, OverlaysDrawAfterTheWindows) {
    FakePanel overlay("Overlay", PanelKind::Overlay, nullptr);
    host.add(overlay);
    ImGui::NewFrame();
    host.drawWindows(ctx);
    EXPECT_EQ(overlay.draws, 0);
    EXPECT_EQ(overlay.ticks, 0);
    host.drawOverlays(ctx);
    EXPECT_EQ(overlay.draws, 1);
    EXPECT_EQ(overlay.ticks, 1);
    ImGui::Render();
}

TEST_F(PanelHostTest, AttachAndProjectReplacedReachEveryPanel) {
    FakePanel window("Window", PanelKind::Window);
    FakePanel hidden("Hidden", PanelKind::Window, "", /*shown=*/false);
    FakePanel tab("Tab", PanelKind::Tab);
    FakePanel overlay("Overlay", PanelKind::Overlay, nullptr);
    for (FakePanel* panel : {&window, &hidden, &tab, &overlay}) host.add(*panel);

    host.attach(ctx);
    host.projectReplaced(ctx);
    for (FakePanel* panel : {&window, &hidden, &tab, &overlay}) {
        EXPECT_EQ(panel->attached, 1) << panel->id();
        EXPECT_EQ(panel->replaced, 1) << panel->id();
    }
}

// The Missing Media window closes itself once nothing is missing, so a request made for
// a project that was then replaced never opens an empty window.
TEST_F(PanelHostTest, MissingMediaWindowClosesWhenNothingIsMissing) {
    MissingMediaWindow window;
    host.add(window);
    es.requestPanelOpen(EditorState::MISSING_MEDIA_PANEL_ID);
    frame();
    EXPECT_FALSE(window.visible);
    EXPECT_FALSE(windowActive(EditorState::MISSING_MEDIA_PANEL_ID));
}
