// The perform lock in the UI (ADR-051 §4) against real headless ImGui: the PanelHost
// draws the show-editing panels disabled, and, with the real EditorUi in a hidden GL 3.3
// window (skips without GL), Tab and the arrows do nothing on the canvas while locked
// and work again after the unlock, and the menu's Perform Lock asks before unlocking.
// The editor-level rules are in editor/PerformLockTest.cpp.

#include "ui/EditorUi.hpp"
#include "ui/PanelHost.hpp"
#include "editor/ActionRegistry.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/PerformLock.hpp"
#include "output/OutputManager.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"
#include "ui/panels/MainMenuBar.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>

#include <array>
#include <cfloat>
#include <initializer_list>
#include <memory>
#include <string>

using namespace quadmapper;

namespace {

// Records whether the host drew it disabled.
class ProbePanel : public Panel {
public:
    ProbePanel(const char* id, PanelKind kind, bool editsShow)
        : id_(id), kind_(kind), editsShow_(editsShow) {}
    const char* id() const override { return id_; }
    PanelKind kind() const override { return kind_; }
    bool editsShow() const override { return editsShow_; }
    void drawContent(EditorContext&) override {
        drawnDisabled = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
        ++draws;
    }

    bool drawnDisabled = false;
    int draws = 0;

private:
    const char* id_;
    PanelKind kind_;
    bool editsShow_;
};

void setUpImGui(ImGuiIO& io, float width, float height) {
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
}

class PerformLockPanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        imgui_ = ImGui::CreateContext();
        setUpImGui(ImGui::GetIO(), 1280.0f, 800.0f);
        for (ProbePanel* panel : {&editor, &viewer, &overlay}) host.add(*panel);
    }
    void TearDown() override { ImGui::DestroyContext(imgui_); }

    void frame() {
        ImGui::NewFrame();
        host.drawWindows(ctx);
        host.drawOverlays(ctx);
        ImGui::Render();
    }

    ImGuiContext* imgui_ = nullptr;
    EditorState es;
    EditorContext ctx{es, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    PanelHost host;
    ProbePanel editor{"Editor", PanelKind::Window, /*editsShow=*/true};
    ProbePanel viewer{"Viewer", PanelKind::Window, /*editsShow=*/false};
    ProbePanel overlay{"Overlay", PanelKind::Overlay, /*editsShow=*/true};
};

}  // namespace

TEST_F(PerformLockPanelTest, ShowEditingPanelsAreDrawnDisabledOnlyWhileLocked) {
    frame();
    EXPECT_FALSE(editor.drawnDisabled);
    EXPECT_FALSE(overlay.drawnDisabled);

    lockPerform(es);
    frame();
    EXPECT_EQ(editor.draws, 2) << "still drawn, for viewing";
    EXPECT_TRUE(editor.drawnDisabled);
    EXPECT_TRUE(overlay.drawnDisabled) << "overlays such as Calibration too";
    EXPECT_FALSE(viewer.drawnDisabled) << "a show control (editsShow false) stays live";

    requestPerformUnlock(es);
    ASSERT_TRUE(confirmPerformUnlock(es));
    frame();
    EXPECT_FALSE(editor.drawnDisabled);
    EXPECT_FALSE(overlay.drawnDisabled);
}

namespace {

// The real EditorUi in a hidden GL window, as EditorUiFrameTest sets it up.
class PerformLockUiTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!glfwInit()) GTEST_SKIP() << "no GLFW";
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        window_ = glfwCreateWindow(800, 600, "perform-lock", nullptr, nullptr);
        glfwDefaultWindowHints();
        if (!window_) {
            glfwTerminate();
            GTEST_SKIP() << "no GL 3.3 window";
        }
        glfwMakeContextCurrent(window_);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
            glfwTerminate();
            GTEST_SKIP() << "no GL";
        }

        imgui_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        setUpImGui(io, 800.0f, 600.0f);
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        registerEditActions(registry);
        std::optional<Action> lockAction = makeAppAction(action_id::PERFORM_LOCK, togglePerformLock);
        ASSERT_TRUE(lockAction);
        ASSERT_EQ(registry.add(std::move(*lockAction)), AddResult::Added);
        installPerformLockPolicy(registry, es);

        ctx.window = window_;
        ctx.actions = &registry;
        ui = std::make_unique<EditorUi>(prefs);
        for (Panel* panel : ui->panels().panels()) panel->visible = false;

        quad = es.quadManager().createQuad("A");
        es.quadManager().createQuad("B");
        es.quadManager().selectQuad(quad->getId());
    }

    void TearDown() override {
        ui.reset();
        if (imgui_) ImGui::DestroyContext(imgui_);
        if (window_) {
            glfwDestroyWindow(window_);
            glfwTerminate();
        }
    }

    void frame() {
        ImGui::NewFrame();
        ui->frame(ctx, {});
        ImGui::Render();
    }

    // Press `keys` together for one frame, then release them.
    void press(std::initializer_list<ImGuiKey> keys) {
        ImGuiIO& io = ImGui::GetIO();
        for (ImGuiKey key : keys) io.AddKeyEvent(key, true);
        frame();
        for (ImGuiKey key : keys) io.AddKeyEvent(key, false);
        frame();
    }

    // Tab to a corner, then an arrow: whether that corner moved.
    bool tabAndNudge() {
        frame();  // the canvas claims its keys from the next frame
        const std::array<Vec2, 4> before = quad->getGeometry().getCorners();
        press({ImGuiKey_Tab});
        press({ImGuiKey_RightArrow});
        return quad->getGeometry().getCorners() != before;
    }

    // The Output > Perform Lock item, drawn in a plain window and clicked.
    void clickPerformLockItem() {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 center;
        const auto menuFrame = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
            ImGui::SetNextWindowSize(ImVec2(300.0f, 100.0f));
            ImGui::Begin("Menu");
            actionMenuItem(ctx, action_id::PERFORM_LOCK, es.performLock.isOn());
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            center = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
            ImGui::End();
            ImGui::Render();
        };
        menuFrame();
        io.AddMousePosEvent(center.x, center.y);
        menuFrame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        menuFrame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        menuFrame();
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        menuFrame();
    }

    GLFWwindow* window_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    EditorState es;
    OutputManager om;
    EditorContext ctx{es, nullptr, nullptr, nullptr, &om, nullptr, nullptr};
    ActionRegistry registry;
    PreferencesDialog prefs;
    std::unique_ptr<EditorUi> ui;
    Quad* quad = nullptr;
};

}  // namespace

// The success metric while locked: Tab and the arrows do nothing, and no editing key
// (Delete, Ctrl+A) acts. After the unlock the Tab + arrow nudge moves the vertex again.
TEST_F(PerformLockUiTest, TabAndArrowsDoNothingWhileLockedAndWorkAfterUnlock) {
    clickPerformLockItem();
    ASSERT_TRUE(es.performLock.isOn()) << "locking is immediate";

    EXPECT_FALSE(tabAndNudge());
    EXPECT_EQ(es.cornerSelectedVertex, -1);
    press({ImGuiKey_Delete});
    press({ImGuiMod_Ctrl, ImGuiKey_A});
    EXPECT_EQ(es.quadManager().getQuadCount(), 2u);
    EXPECT_EQ(es.quadManager().getSelectionCount(), 1u);

    requestPerformUnlock(es);
    ASSERT_TRUE(confirmPerformUnlock(es));
    EXPECT_TRUE(tabAndNudge()) << "the success-metric nudge works again";
    EXPECT_EQ(es.cornerSelectedVertex, 0);
}

// The menu item on a locked show only asks: the "Unlock the Show?" modal opens, and
// the show stays locked until it is answered.
TEST_F(PerformLockUiTest, UnlockingFromTheMenuAsksFirst) {
    clickPerformLockItem();
    ASSERT_TRUE(es.performLock.isOn());

    clickPerformLockItem();
    EXPECT_TRUE(es.performLock.isOn());
    EXPECT_TRUE(es.performLock.isConfirmingUnlock());
    frame();  // the session dialogs open the modal
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_NE(std::string(modal->Name).find("Unlock the Show?"), std::string::npos) << modal->Name;
    frame();
    EXPECT_TRUE(es.performLock.isOn()) << "nothing unlocks until the modal is answered";

    press({ImGuiKey_Escape});  // Esc answers "Stay Locked"
    EXPECT_TRUE(es.performLock.isOn());
    EXPECT_FALSE(es.performLock.isConfirmingUnlock());
}
