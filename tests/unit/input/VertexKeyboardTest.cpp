#include <gtest/gtest.h>
#include "input/VertexKeyboard.hpp"

#include <imgui.h>
#include <imgui_internal.h>

namespace quadmapper {
namespace test {

// --- Nudge / cycle maths (success metric: Shift = 10x, Alt = 0.1x) ---------------

TEST(VertexKeyboardTest, NudgeStepAppliesModifiers) {
    EXPECT_FLOAT_EQ(nudgeStep(false, false), 0.001f);
    EXPECT_FLOAT_EQ(nudgeStep(true, false), 0.01f);
    EXPECT_FLOAT_EQ(nudgeStep(false, true), 0.0001f);
    EXPECT_FLOAT_EQ(nudgeStep(true, true), 0.001f);
}

TEST(VertexKeyboardTest, CycleVertexWrapsBothWays) {
    EXPECT_EQ(cycleVertex(-1, 4, false), 0);   // Tab from nothing selects TL
    EXPECT_EQ(cycleVertex(0, 4, false), 1);
    EXPECT_EQ(cycleVertex(3, 4, false), 0);
    EXPECT_EQ(cycleVertex(-1, 4, true), 3);    // Shift+Tab from nothing selects BL
    EXPECT_EQ(cycleVertex(0, 4, true), 3);
    EXPECT_EQ(cycleVertex(2, 4, true), 1);
}

TEST(VertexKeyboardTest, CycleVertexRecoversFromStaleIndex) {
    // A mesh grid can shrink under a selected vertex.
    EXPECT_EQ(cycleVertex(10, 4, false), 0);
    EXPECT_EQ(cycleVertex(10, 4, true), 3);
    EXPECT_EQ(cycleVertex(0, 0, false), -1);
}

TEST(VertexKeyboardTest, CycleHandleVisitsOnlyAvailableHandlesThenTheVertex) {
    const unsigned all = 0xF;
    EXPECT_EQ(cycleHandle(-1, all, false), 0);
    EXPECT_EQ(cycleHandle(3, all, false), -1);   // after the last: back to the vertex
    EXPECT_EQ(cycleHandle(-1, all, true), 3);    // Shift+H from the vertex: the last
    EXPECT_EQ(cycleHandle(0, all, true), -1);
    const unsigned topLeft = (1u << 1) | (1u << 3);  // U after, V after
    EXPECT_EQ(cycleHandle(-1, topLeft, false), 1);
    EXPECT_EQ(cycleHandle(1, topLeft, false), 3);
    EXPECT_EQ(cycleHandle(3, topLeft, false), -1);
    EXPECT_EQ(cycleHandle(-1, topLeft, true), 3);
    EXPECT_EQ(cycleHandle(-1, 0u, false), -1);   // no handles: stays on the vertex
    EXPECT_EQ(cycleHandle(7, all, false), 0);    // a stale index restarts
}

TEST(VertexKeyboardTest, NudgeDeltaFollowsArrowKeys) {
    const float s = 0.01f;
    EXPECT_EQ(nudgeDelta(true, false, false, false, s), Vec2(0.0f, -s));
    EXPECT_EQ(nudgeDelta(false, true, false, true, s), Vec2(s, s));
    EXPECT_EQ(nudgeDelta(true, true, true, true, s), Vec2(0.0f, 0.0f));
}

// --- Canvas keyboard ownership against real (headless) ImGui ---------------------
//
// The success-metric failure this guards: after clicking "+ Add Quad" (or anything in
// a panel) the panel keeps focus, and ImGui's tabbing / keyboard navigation consumed
// Tab and the arrows, so the vertex did not move until the user clicked empty canvas.

class CanvasKeyOwnershipTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // as Application does
        io.ConfigNavCaptureKeyboard = false;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    }

    void TearDown() override { ImGui::DestroyContext(ctx_); }

    // One app-shaped frame: claim (if enabled) right after NewFrame, then a panel
    // with a button and a text field, like the Quad List / Inspector.
    void frame(bool claim, bool quadSelected = true, bool focusPanel = false) {
        ImGui::NewFrame();
        if (claim) claimCanvasEditingKeys(quadSelected);
        sawTab_ = sawTab_ || ImGui::IsKeyPressed(ImGuiKey_Tab, false);  // ViewportEditor's query
        if (canvasOwnsEditingKeys(quadSelected)) {  // the mesh-edit path's read
            const CanvasKeys keys = readCanvasKeys();
            sawHandleKey_ = sawHandleKey_ || keys.handle;
            sawShiftHandleKey_ = sawShiftHandleKey_ || (keys.handle && keys.shift);
        }
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
        if (focusPanel) ImGui::SetNextWindowFocus();
        ImGui::Begin("Panel");
        ImGui::Button("+ Add Quad");
        ImGui::InputText("Name", name_, sizeof(name_));
        ImGui::End();
        ImGui::Render();
    }

    void press(ImGuiKey key, bool claim, bool quadSelected = true) {
        ImGui::GetIO().AddKeyEvent(key, true);
        frame(claim, quadSelected);
        ImGui::GetIO().AddKeyEvent(key, false);
        frame(claim, quadSelected);
    }

    ImGuiContext* ctx_ = nullptr;
    char name_[32] = "Quad 1";
    bool sawTab_ = false;
    bool sawHandleKey_ = false;
    bool sawShiftHandleKey_ = false;
};

TEST_F(CanvasKeyOwnershipTest, HandleKeyReachesTheCanvasThroughAFocusedPanel) {
    frame(true, true, /*focusPanel=*/true);
    frame(true);
    EXPECT_FALSE(ImGui::TestKeyOwner(GRID_HANDLE_KEY, ImGuiKeyOwner_NoOwner))
        << "H is claimed with the other canvas keys";

    press(GRID_HANDLE_KEY, /*claim=*/true);
    EXPECT_TRUE(sawHandleKey_);
    EXPECT_FALSE(sawShiftHandleKey_);
    EXPECT_FALSE(ImGui::GetIO().WantTextInput);

    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    press(GRID_HANDLE_KEY, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, false);
    EXPECT_TRUE(sawShiftHandleKey_) << "Shift+H cycles backwards";
}

TEST_F(CanvasKeyOwnershipTest, CtrlHIsNotTheHandleKey) {
    frame(true, true, /*focusPanel=*/true);
    frame(true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true);
    press(GRID_HANDLE_KEY, true);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, false);
    EXPECT_FALSE(sawHandleKey_);
}

TEST_F(CanvasKeyOwnershipTest, FocusedPanelDoesNotCaptureKeyboard) {
    frame(true, true, /*focusPanel=*/true);
    frame(true);
    ASSERT_NE(GImGui->NavWindow, nullptr) << "the panel should hold focus";
    EXPECT_FALSE(ImGui::GetIO().WantCaptureKeyboard);
    EXPECT_TRUE(canvasOwnsEditingKeys(true));
}

TEST_F(CanvasKeyOwnershipTest, ClaimedTabReachesCanvasAndNotPanelTabbing) {
    frame(true, true, /*focusPanel=*/true);
    frame(true);
    const ImGuiID navBefore = GImGui->NavId;

    press(ImGuiKey_Tab, /*claim=*/true);
    press(ImGuiKey_Tab, /*claim=*/true);

    EXPECT_TRUE(sawTab_) << "the canvas must see Tab to cycle vertices";
    EXPECT_EQ(GImGui->NavId, navBefore) << "panel tabbing must not also move focus";
    EXPECT_FALSE(ImGui::GetIO().WantTextInput) << "tabbing must not activate the text field";
}

TEST_F(CanvasKeyOwnershipTest, WithoutClaimPanelTabbingStealsTab) {
    // Control: shows the failure the claim prevents, so the test above is meaningful.
    frame(false, true, /*focusPanel=*/true);
    frame(false);
    const ImGuiID navBefore = GImGui->NavId;

    press(ImGuiKey_Tab, /*claim=*/false);
    press(ImGuiKey_Tab, /*claim=*/false);

    EXPECT_NE(GImGui->NavId, navBefore);
}

TEST_F(CanvasKeyOwnershipTest, ArrowKeysAreOwnedWhileClaimed) {
    frame(true, true, /*focusPanel=*/true);
    frame(true);  // ownership set last frame applies from this NewFrame
    for (ImGuiKey key : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow,
                         ImGuiKey_RightArrow, ImGuiKey_Tab}) {
        EXPECT_FALSE(ImGui::TestKeyOwner(key, ImGuiKeyOwner_NoOwner))
            << "navigation only reads unowned keys; key " << static_cast<int>(key);
    }
}

TEST_F(CanvasKeyOwnershipTest, NoClaimWithoutSelectedQuad) {
    frame(true, /*quadSelected=*/false, /*focusPanel=*/true);
    frame(true, false);
    frame(true, false);
    EXPECT_TRUE(ImGui::TestKeyOwner(ImGuiKey_Tab, ImGuiKeyOwner_NoOwner))
        << "with nothing to edit, panel keyboard navigation keeps working";
}

TEST_F(CanvasKeyOwnershipTest, TypingInATextFieldKeepsTheKeys) {
    frame(true, true, /*focusPanel=*/true);
    // Activate the text field as a click would.
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
    ImGui::Begin("Panel");
    ImGui::Button("+ Add Quad");
    ImGui::SetKeyboardFocusHere();
    ImGui::InputText("Name", name_, sizeof(name_));
    ImGui::End();
    ImGui::Render();
    frame(true);  // the focus request activates the field here
    frame(true);  // ...and WantTextInput reflects it from the next NewFrame

    ASSERT_TRUE(ImGui::GetIO().WantTextInput);
    EXPECT_FALSE(canvasOwnsEditingKeys(true)) << "arrows/Tab must edit the text, not a vertex";
}

TEST_F(CanvasKeyOwnershipTest, OpenPopupKeepsTheKeys) {
    ImGui::NewFrame();
    ImGui::OpenPopup("Unsaved changes");
    ImGui::BeginPopupModal("Unsaved changes");
    EXPECT_FALSE(canvasOwnsEditingKeys(true));
    ImGui::EndPopup();
    ImGui::Render();
}

TEST(CanvasMouseTest, PassthroughCanvasDoesNotCaptureMouseButPanelsDo) {
    // ViewportEditor starts clicks and drags only when !io.WantCaptureMouse. That is
    // correct only if hovering the dockspace's passthrough central node (the canvas)
    // leaves WantCaptureMouse false. Mirrors Application's dockspace host exactly.
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

    auto frame = [] {
        ImGui::NewFrame();
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        const ImGuiWindowFlags hostFlags =
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("DockSpace", nullptr, hostFlags);
        ImGui::PopStyleVar();
        ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0.0f, 0.0f),
                         ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
        ImGui::Begin("Panel");
        ImGui::Button("+ Add Quad");
        ImGui::End();
        ImGui::Render();
    };

    io.AddMousePosEvent(600.0f, 400.0f);  // over the canvas
    frame();
    frame();
    EXPECT_FALSE(io.WantCaptureMouse) << "the passthrough canvas must not capture the mouse";

    io.AddMouseButtonEvent(0, true);  // a press that starts on the canvas
    frame();
    frame();
    EXPECT_FALSE(io.WantCaptureMouse) << "a canvas press stays with the canvas";
    io.AddMouseButtonEvent(0, false);
    frame();

    io.AddMousePosEvent(100.0f, 100.0f);  // over a panel
    frame();
    frame();
    EXPECT_TRUE(io.WantCaptureMouse);

    ImGui::DestroyContext(ctx);
}

}  // namespace test
}  // namespace quadmapper
