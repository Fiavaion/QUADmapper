// The perform lock in the UI (ADR-051 §4) against real headless ImGui: the PanelHost
// draws the show-editing panels disabled, and, with the real EditorUi in a hidden GL 3.3
// window (skips without GL), Tab and the arrows do nothing on the canvas while locked
// and work again after the unlock, and the menu's Perform Lock asks before unlocking.
// The unlock prompt never takes the show off air (Esc, scene takes), and a video pick
// or a media drop reaching the show without a panel is refused.
// The editor-level rules are in editor/PerformLockTest.cpp.

#include "ui/EditorUi.hpp"
#include "ui/PanelHost.hpp"
#include "ui/ShortcutRouter.hpp"
#include "core/FileSystem.hpp"
#include "editor/ActionRegistry.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/PerformLock.hpp"
#include "media/MediaItem.hpp"
#include "media/MediaLibrary.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "source/SourceManager.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"
#include "ui/panels/MainMenuBar.hpp"
#include "ui/panels/VideoPlayerPanel.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>

#include <array>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

    void addAction(const std::string& id, std::function<void(EditorContext&)> run) {
        std::optional<Action> action = makeAppAction(id, std::move(run));
        ASSERT_TRUE(action) << id;
        ASSERT_EQ(registry.add(std::move(*action)), AddResult::Added) << id;
    }

    // Lock from the menu, then ask to unlock: the "Unlock the Show?" modal is up.
    void openUnlockPrompt() {
        clickPerformLockItem();
        clickPerformLockItem();
        frame();
        frame();
    }

    // Perform mode on `outputs`, set up on this window: one output on monitor 0,
    // hidden again at once as the stress harness keeps them. False where no display
    // takes it.
    bool enterPerformMode(OutputManager& outputs) {
        if (!outputs.initialize(window_) || outputs.getMonitorCount() == 0) return false;
        OutputWindow* output = outputs.createOutput("Projector");
        glfwMakeContextCurrent(window_);
        if (!output) return false;
        outputs.assignMonitor(*output, 0);
        outputs.enterPerformMode(/*borderless=*/true);
        for (const auto& o : outputs.getOutputs()) {
            if (o->isCreated()) o->hide();
        }
        glfwMakeContextCurrent(window_);
        ctx.outputManager = &outputs;
        return outputs.isInPerformMode();
    }

    // One frame with the 1-9 presses in `queued` (as Application::runSceneTakes).
    void takeFrame(std::vector<int>& queued) {
        ImGui::NewFrame();
        ui->frame(ctx, [&](EditorContext& c) { runSceneTakeKeys(registry, c, queued); });
        ImGui::Render();
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

// ---- The unlock prompt never takes the show off air -----------------------------------

// perform.escape acts with a popup open (the way out of fullscreen is one press), and
// the shortcuts run before the session modals: one Esc on the prompt must answer it
// alone, not also leave perform mode.
TEST_F(PerformLockUiTest, EscOnTheUnlockPromptKeepsPerformModeAndTheLock) {
    addAction(action_id::PERFORM_ESCAPE, performEscape);
    OutputManager outputs;
    if (!enterPerformMode(outputs)) GTEST_SKIP() << "no display for a projector output";

    openUnlockPrompt();
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    press({ImGuiKey_Escape});
    EXPECT_TRUE(es.performLock.isOn());
    EXPECT_FALSE(es.performLock.isConfirmingUnlock());
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr) << "Stay Locked closed the prompt";
    EXPECT_TRUE(outputs.isInPerformMode()) << "one Esc took every projector dark";

    press({ImGuiKey_Escape});
    EXPECT_FALSE(outputs.isInPerformMode()) << "with no modal up, Esc leaves perform mode";
}

TEST_F(PerformLockUiTest, EscOnUnsavedChangesOnlyCancelsIt) {
    addAction(action_id::PERFORM_ESCAPE, performEscape);
    OutputManager outputs;
    if (!enterPerformMode(outputs)) GTEST_SKIP() << "no display for a projector output";

    es.unsavedPromptFor = EditorState::FileAction::Exit;
    frame();
    frame();
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    press({ImGuiKey_Escape});
    EXPECT_EQ(es.unsavedPromptFor, EditorState::FileAction::None) << "Esc is Cancel";
    EXPECT_EQ(es.pendingFileAction, EditorState::FileAction::None);
    EXPECT_TRUE(outputs.isInPerformMode());
}

// Scene takes 1-9 are show control: the unlock prompt does not hold them off.
TEST_F(PerformLockUiTest, SceneTakeKeysWorkWhileTheUnlockPromptIsUp) {
    std::vector<int> taken;
    for (int n = 1; n <= 9; ++n) {
        addAction(sceneTakeId(n), [&taken, n](EditorContext&) { taken.push_back(n); });
    }
    openUnlockPrompt();
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);

    std::vector<int> queued{2};
    takeFrame(queued);
    EXPECT_EQ(taken, std::vector<int>{2});
    EXPECT_TRUE(queued.empty());
    EXPECT_TRUE(es.performLock.isConfirmingUnlock()) << "the prompt is still up";
}

// Behind any other popup a take key is refused, and the status line says so.
TEST_F(PerformLockUiTest, ATakeKeyHeldOffByAModalSaysSo) {
    std::vector<int> taken;
    addAction(sceneTakeId(3), [&taken](EditorContext&) { taken.push_back(3); });
    es.unsavedPromptFor = EditorState::FileAction::Exit;
    frame();
    frame();
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);

    std::vector<int> queued{3};
    takeFrame(queued);
    EXPECT_TRUE(taken.empty());
    EXPECT_TRUE(queued.empty());
    EXPECT_NE(std::string(es.statusMessage).find("Scene 3 not taken"), std::string::npos)
        << es.statusMessage;
}

// ---- Edits that reach the show without a panel ----------------------------------------

// The Video Player's "Load Video..." dialog is ownerless: answered after locking, the
// pick is refused rather than bound to the selected quad and played on air.
TEST_F(PerformLockUiTest, AVideoPickedAfterLockingIsNotLoaded) {
    namespace fs = std::filesystem;
    const fs::path clip = fs::path(__FILE__).parent_path().parent_path().parent_path() /
                          "fixtures" / "videos" / "h264_closed_gop.mp4";
    std::error_code ec;
    if (!fs::exists(clip, ec)) GTEST_SKIP() << "no fixture clip at " << clip.string();
    SourceManager sources;
    ctx.sourceManager = &sources;
    VideoPlayerPanel player;
    snprintf(es.videoPath, sizeof(es.videoPath), "%s", utf8FromPath(clip).c_str());

    lockPerform(es);
    player.loadVideo(ctx, /*simpleMode=*/true);
    EXPECT_TRUE(quad->getTexture().mediaId.isNull()) << "not bound to the selected quad";
    EXPECT_TRUE(es.activeVideoSourceId.isNull()) << "not loaded";
    EXPECT_NE(std::string(es.statusMessage).find("the show is locked"), std::string::npos)
        << es.statusMessage;

    requestPerformUnlock(es);
    ASSERT_TRUE(confirmPerformUnlock(es));
    player.loadVideo(ctx, /*simpleMode=*/true);
    EXPECT_FALSE(quad->getTexture().mediaId.isNull()) << "unlocked, the pick loads onto the quad";
    ctx.sourceManager = nullptr;
}

// A media item dragged from the bin and dropped on a quad on the canvas is ignored while
// locked (and assigned once unlocked).
TEST_F(PerformLockUiTest, MediaDroppedOnTheCanvasIsIgnoredWhileLocked) {
    MediaLibrary library;
    auto pattern = std::make_unique<MediaItem>();
    pattern->setType(MediaType::PROCEDURAL);
    pattern->setName("Pattern");
    const UUID sourceId = UUID::generate();
    pattern->setSourceId(sourceId);
    const UUID mediaId = library.addItem(std::move(pattern))->getId();
    SourceManager sources;
    ctx.mediaLibrary = &library;
    ctx.sourceManager = &sources;

    frame();
    frame();  // the canvas region and its view transform
    ASSERT_GT(es.canvasRegionSize.x, 1.0f);
    ASSERT_NE(es.lastViewScale.x, 0.0f);
    // The screen point of the selected quad's centre, and the quad on top there.
    Vec2 centre(0.0f);
    for (const Vec2& c : quad->getGeometry().getCorners()) centre += c * 0.25f;
    const ImVec2 origin = ImGui::GetMainViewport()->Pos;
    const ImVec2 target(
        origin.x + es.canvasRegionPos.x + (centre.x * es.lastViewScale.x + es.lastViewOffset.x) * es.canvasRegionSize.x,
        origin.y + es.canvasRegionPos.y + (centre.y * es.lastViewScale.y + es.lastViewOffset.y) * es.canvasRegionSize.y);
    Quad* onTop = es.quadManager().quadAtPoint(centre);
    ASSERT_NE(onTop, nullptr);

    // Drag the item out of a bin-like window and release it at `target`.
    const auto dragAndDrop = [&] {
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 button;
        const auto dragFrame = [&] {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20.0f, 480.0f));
            ImGui::SetNextWindowSize(ImVec2(140.0f, 80.0f));
            ImGui::Begin("Bin");
            ImGui::Button("Pattern");
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            button = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("MEDIA_ITEM_ID", &mediaId, sizeof(UUID));
                ImGui::EndDragDropSource();
            }
            ImGui::End();
            ui->frame(ctx, {});
            ImGui::Render();
        };
        dragFrame();
        io.AddMousePosEvent(button.x, button.y);
        dragFrame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        dragFrame();
        io.AddMousePosEvent(button.x + 30.0f, button.y - 30.0f);
        dragFrame();
        io.AddMousePosEvent(target.x, target.y);
        dragFrame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        dragFrame();
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        dragFrame();
    };

    lockPerform(es);
    dragAndDrop();
    EXPECT_TRUE(onTop->getTexture().mediaId.isNull()) << "the drop was ignored";

    requestPerformUnlock(es);
    ASSERT_TRUE(confirmPerformUnlock(es));
    dragAndDrop();
    EXPECT_EQ(onTop->getTexture().mediaId, sourceId) << "unlocked, the same drop assigns";
    ctx.mediaLibrary = nullptr;
    ctx.sourceManager = nullptr;
}
