// The editor's frame (EditorUi::frame, ADR-051 §2-3) and the menu's action items
// (actionMenuItem), against real headless ImGui with the real EditorUi in a hidden GL 3.3
// window (the canvas reads the pointer from it; no renderer). Skips without GL.
//
// The frame order is what keeps the success metric and the panels right: the canvas
// takes its keys before any window can, and the shortcuts and scene takes act before
// the panels draw, so the panels show this frame's state.

#include "ui/EditorUi.hpp"
#include "editor/ActionRegistry.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "output/OutputManager.hpp"
#include "scene/MeshObject.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"
#include "ui/panels/MainMenuBar.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

using namespace quadmapper;

namespace {

using Step = EditorUi::FrameStep;

const char* stepName(Step step) {
    switch (step) {
        case Step::CanvasKeys:    return "CanvasKeys";
        case Step::Canvas:        return "Canvas";
        case Step::DockSpace:     return "DockSpace";
        case Step::MenuBar:       return "MenuBar";
        case Step::CanvasOverlay: return "CanvasOverlay";
        case Step::Shortcuts:     return "Shortcuts";
        case Step::SceneTakes:    return "SceneTakes";
        case Step::Panels:        return "Panels";
        case Step::Overlays:      return "Overlays";
    }
    return "?";
}

// Records into the frame's log when the host draws it.
class RecordingPanel : public Panel {
public:
    RecordingPanel(const char* id, PanelKind kind, std::vector<std::string>& log)
        : id_(id), kind_(kind), log_(log) {}
    const char* id() const override { return id_; }
    const char* menuLabel() const override { return nullptr; }
    PanelKind kind() const override { return kind_; }
    void drawContent(EditorContext&) override { log_.push_back(id_); }

private:
    const char* id_;
    PanelKind kind_;
    std::vector<std::string>& log_;
};

class EditorUiFrameTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!glfwInit()) GTEST_SKIP() << "no GLFW";
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        window_ = glfwCreateWindow(800, 600, "editor-ui-frame", nullptr, nullptr);
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
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.ConfigMacOSXBehaviors = false;  // inject the chord's own modifiers: ImGui swaps Ctrl and Super on macOS
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

        registerEditActions(registry);
        Action probe;  // stands in for any editor-wide chord
        probe.id = "test.probe";
        probe.label = "Probe";
        probe.chords = {Chord{ImGuiKey_F5, MOD_NONE}};
        probe.run = [this](EditorContext&) { log.push_back("shortcut"); };
        ASSERT_EQ(registry.add(probe), AddResult::Added);

        ctx.window = window_;
        ctx.actions = &registry;

        ui = std::make_unique<EditorUi>(prefs);
        // Only the recording panels draw: the real ones need the app's components.
        for (Panel* panel : ui->panels().panels()) panel->visible = false;
        ui->panels().add(windowPanel);
        ui->panels().add(overlayPanel);
        ui->onFrameStep = [this](Step step) {
            log.push_back(stepName(step));
            if (step == Step::DockSpace) cornerAtDockSpace = es.cornerSelectedVertex;
        };

        quad = es.quadManager().createQuad("A");
        es.quadManager().createQuad("B");
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
        ui->frame(ctx, [this](EditorContext&) {
            log.push_back("take");
            ++takes;
        });
        ImGui::Render();
    }

    // Press `keys` together for one frame (logged), then release them.
    void press(std::initializer_list<ImGuiKey> keys) {
        ImGuiIO& io = ImGui::GetIO();
        for (ImGuiKey key : keys) io.AddKeyEvent(key, true);
        log.clear();
        frame();
        pressLog = log;
        pressCornerAtDockSpace = cornerAtDockSpace;
        for (ImGuiKey key : keys) io.AddKeyEvent(key, false);
        frame();
    }

    GLFWwindow* window_ = nullptr;
    ImGuiContext* imgui_ = nullptr;
    EditorState es;
    OutputManager om;
    EditorContext ctx{es, nullptr, nullptr, nullptr, &om, nullptr, nullptr};
    ActionRegistry registry;
    PreferencesDialog prefs;
    std::vector<std::string> log;
    std::vector<std::string> pressLog;
    RecordingPanel windowPanel{"panel", PanelKind::Window, log};
    RecordingPanel overlayPanel{"overlay", PanelKind::Overlay, log};
    std::unique_ptr<EditorUi> ui;
    Quad* quad = nullptr;
    int cornerAtDockSpace = -2;       // the selected vertex when the dockspace step ended
    int pressCornerAtDockSpace = -2;  // ... in the frame press() pressed its keys
    int takes = 0;
};

// The item `draw` draws, in a plain window: its rendered text, and a click on it.
struct MenuRig {
    std::function<void()> draw;
    ImVec2 center{0.0f, 0.0f};
    std::string text;

    void frame() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f));
        ImGui::Begin("Menu");
        ImGui::LogToBuffer();
        draw();
        text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        center = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        ImGui::End();
        ImGui::Render();
    }
    std::string render() {
        frame();
        frame();
        return text;
    }
    void click() {
        ImGuiIO& io = ImGui::GetIO();
        frame();
        io.AddMousePosEvent(center.x, center.y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        frame();
    }
};

}  // namespace

// Canvas keys claimed, then the canvas, before the dockspace and the menu; the
// shortcuts and the scene takes before the panels; the panels before the overlays.
// The recorded "shortcut", "take", "panel" and "overlay" are the real calls.
TEST_F(EditorUiFrameTest, TheFrameRunsInItsOrder) {
    es.quadManager().selectQuad(quad->getId());
    frame();  // the canvas claims its keys from the next frame
    const int cornerBefore = es.cornerSelectedVertex;

    press({ImGuiKey_Tab, ImGuiKey_F5});

    const std::vector<std::string> expected = {
        "CanvasKeys", "Canvas",   "DockSpace", "MenuBar", "CanvasOverlay", "shortcut",
        "Shortcuts",  "take",     "SceneTakes", "panel",  "Panels",        "overlay",
        "Overlays"};
    EXPECT_EQ(pressLog, expected);
    EXPECT_NE(es.cornerSelectedVertex, cornerBefore) << "Tab cycled the selected quad's vertex";
    EXPECT_EQ(pressCornerAtDockSpace, es.cornerSelectedVertex)
        << "the canvas read Tab before the dockspace and the menu ran";
}

// The success metric: Tab with a quad selected goes to the canvas, never to an action.
TEST_F(EditorUiFrameTest, TabWithAQuadSelectedIsNeverRouted) {
    es.quadManager().selectQuad(quad->getId());
    frame();
    const int cornerBefore = es.cornerSelectedVertex;
    press({ImGuiKey_Tab});
    EXPECT_EQ(std::count(pressLog.begin(), pressLog.end(), "shortcut"), 0);
    EXPECT_NE(es.cornerSelectedVertex, cornerBefore);
    EXPECT_EQ(es.quadManager().getQuadCount(), 2u);
    EXPECT_EQ(es.quadManager().getSelectionCount(), 1u);
}

// The menu and the key invoke one action: the same effect, the same guards.
TEST_F(EditorUiFrameTest, MenuDeleteAndTheDeleteKeyDeleteTheSelectedObject) {
    Scene& scene = es.activeScene();
    const auto selectObject = [&](MeshObject* object) {
        es.quadManager().deselectAll();
        es.selectedObjectId = object->getId();
        es.currentSelectionType = EditorState::SelectionType::Object;
    };
    MeshObject* byKey = scene.createObject("By key");
    const UUID byKeyId = byKey->getId();
    selectObject(byKey);
    press({ImGuiKey_Delete});
    EXPECT_EQ(scene.getObject(byKeyId), nullptr) << "the key deletes the selected object";

    MeshObject* byMenu = scene.createObject("By menu");
    const UUID byMenuId = byMenu->getId();
    selectObject(byMenu);
    MenuRig menu{[&] { actionMenuItem(ctx, action_id::DELETE_SELECTION); }};
    EXPECT_NE(menu.render().find("Delete Selected"), std::string::npos);
    EXPECT_NE(menu.text.find("Delete"), std::string::npos);
    menu.click();
    EXPECT_EQ(scene.getObject(byMenuId), nullptr) << "the menu deletes it too (it used to need a quad)";
    EXPECT_EQ(es.quadManager().getQuadCount(), 2u);
}

TEST_F(EditorUiFrameTest, MenuSelectAllAndTheKeyHoldOffWhileCalibrating) {
    press({ImGuiMod_Ctrl, ImGuiKey_A});
    EXPECT_EQ(es.quadManager().getSelectionCount(), 2u);
    es.quadManager().deselectAll();
    MenuRig menu{[&] { actionMenuItem(ctx, action_id::SELECT_ALL); }};
    EXPECT_NE(menu.render().find("Ctrl+A"), std::string::npos);
    menu.click();
    EXPECT_EQ(es.quadManager().getSelectionCount(), 2u);

    es.quadManager().deselectAll();
    OutputWindow* output = om.createOutput("Projector");
    es.beginCalibration(*output);
    press({ImGuiMod_Ctrl, ImGuiKey_A});
    menu.click();
    EXPECT_FALSE(es.quadManager().hasSelection()) << "calibrating: neither selects the hidden quads";
}

// Deselect All has no key (Esc is the perform / calibration chain): no chord shown.
TEST_F(EditorUiFrameTest, MenuDeselectAllShowsNoChordAndDeselects) {
    es.quadManager().selectAll();
    MenuRig menu{[&] { actionMenuItem(ctx, action_id::DESELECT_ALL); }};
    const std::string text = menu.render();
    EXPECT_NE(text.find("Deselect All"), std::string::npos);
    EXPECT_EQ(text.find("Esc"), std::string::npos) << text;
    menu.click();
    EXPECT_FALSE(es.quadManager().hasSelection());

    es.quadManager().selectAll();
    press({ImGuiKey_Escape});
    EXPECT_TRUE(es.quadManager().hasSelection()) << "Esc is not Deselect All";
}
