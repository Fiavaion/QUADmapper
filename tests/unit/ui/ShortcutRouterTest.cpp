// The shortcut router (ui/ShortcutRouter, ADR-051 §3) against real headless ImGui,
// with key presses fed through io.AddKeyEvent: the guards, exact modifiers, and the
// canvas keys it must never touch.

#include "ui/ShortcutRouter.hpp"
#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "input/VertexKeyboard.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <gtest/gtest.h>

#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

using namespace quadmapper;

namespace {

class RouterRig {
public:
    RouterRig() {
        imgui_ = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280.0f, 800.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

        registerEditActions(registry);
        auto blackout = makeAppAction(action_id::BLACKOUT, [this](EditorContext&) { ++blackouts; });
        registry.add(*blackout);
        auto save = makeAppAction(action_id::SAVE, [this](EditorContext&) { ++saves; });
        registry.add(*save);

        quad = es.quadManager().createQuad("A");
        es.quadManager().createQuad("B");
    }
    ~RouterRig() { ImGui::DestroyContext(imgui_); }

    // One frame: the canvas claims its keys (as Application does right after NewFrame),
    // `ui` draws, then the router runs. Returns what it invoked.
    std::vector<std::string> frame(const std::function<void()>& ui = {}) {
        ImGui::NewFrame();
        claimCanvasEditingKeys(es.quadManager().hasSelection());
        if (ui) ui();
        std::vector<std::string> invoked = routeShortcuts(registry, ctx);
        if (afterRoute) afterRoute();
        ImGui::Render();
        return invoked;
    }

    // Press `keys` together for one frame, then release them.
    std::vector<std::string> press(std::initializer_list<ImGuiKey> keys,
                                   const std::function<void()>& ui = {}) {
        ImGuiIO& io = ImGui::GetIO();
        for (ImGuiKey key : keys) io.AddKeyEvent(key, true);
        std::vector<std::string> invoked = frame(ui);
        for (ImGuiKey key : keys) io.AddKeyEvent(key, false);
        frame(ui);
        return invoked;
    }

    EditorState es;
    OutputManager om;
    EditorContext ctx{es, nullptr, nullptr, nullptr, &om, nullptr, nullptr};
    ActionRegistry registry;
    Quad* quad = nullptr;
    int blackouts = 0;
    int saves = 0;
    std::function<void()> afterRoute;

private:
    ImGuiContext* imgui_ = nullptr;
};

const std::vector<std::string> NOTHING;

}  // namespace

TEST(ShortcutRouterTest, DeleteRoutesToTheDeleteAction) {
    RouterRig rig;
    rig.es.quadManager().selectQuad(rig.quad->getId());
    EXPECT_EQ(rig.press({ImGuiKey_Delete}), std::vector<std::string>{action_id::DELETE_SELECTION});
    EXPECT_EQ(rig.es.quadManager().getQuadCount(), 1u);
}

// Audit 0.7: Delete typed into a text field edits the text, never the show.
TEST(ShortcutRouterTest, DeleteInAnActiveTextFieldDoesNothing) {
    RouterRig rig;
    rig.es.quadManager().selectQuad(rig.quad->getId());
    char text[32] = "Quad name";
    bool focus = true;
    const auto field = [&] {
        ImGui::Begin("Fields");
        if (focus) ImGui::SetKeyboardFocusHere();
        focus = false;
        ImGui::InputText("##name", text, sizeof(text));
        ImGui::End();
    };
    for (int i = 0; i < 5 && !ImGui::GetIO().WantTextInput; ++i) rig.frame(field);
    ASSERT_TRUE(ImGui::GetIO().WantTextInput);

    EXPECT_EQ(rig.press({ImGuiKey_Delete}, field), NOTHING);
    EXPECT_EQ(rig.es.quadManager().getQuadCount(), 2u);
    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiKey_B}, field), NOTHING)
        << "a text field keeps even the Show keys";
    EXPECT_EQ(rig.blackouts, 0);
}

// Ctrl+B is the panic key (Show guard): a popup does not hold it, as it holds Delete.
TEST(ShortcutRouterTest, BlackoutFiresWithAPopupOpen) {
    RouterRig rig;
    rig.es.quadManager().selectQuad(rig.quad->getId());
    bool open = true;
    const auto popup = [&] {
        ImGui::Begin("Host");
        if (open) ImGui::OpenPopup("Unsaved changes");
        open = false;
        if (ImGui::BeginPopup("Unsaved changes")) {
            ImGui::TextUnformatted("Save?");
            ImGui::EndPopup();
        }
        ImGui::End();
    };
    rig.frame(popup);
    ASSERT_TRUE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));

    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiKey_B}, popup),
              std::vector<std::string>{action_id::BLACKOUT});
    EXPECT_EQ(rig.blackouts, 1);
    EXPECT_EQ(rig.press({ImGuiKey_Delete}, popup), NOTHING) << "Editing keys wait for the popup";
    EXPECT_EQ(rig.es.quadManager().getQuadCount(), 2u);
    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiKey_S}, popup), NOTHING);
    EXPECT_EQ(rig.saves, 0);
}

// The success metric: with a quad selected, Tab (and the arrows, and H) belong to the
// canvas. The router neither invokes anything for them nor takes their ownership, so
// the canvas still reads the press after the router has run.
TEST(ShortcutRouterTest, CanvasKeysAreNeverRoutedOrConsumed) {
    RouterRig rig;
    rig.es.quadManager().selectQuad(rig.quad->getId());
    const ImGuiID canvasOwner = ImHashStr("QUADmapper.CanvasEditingKeys");
    rig.frame();  // the claim applies from the next frame

    for (ImGuiKey key : {ImGuiKey_Tab, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow,
                         ImGuiKey_RightArrow, ImGuiKey_H}) {
        bool canvasSawIt = false;
        rig.afterRoute = [&] {
            EXPECT_EQ(ImGui::GetKeyOwner(key), canvasOwner) << ImGui::GetKeyName(key);
            canvasSawIt = canvasSawIt || ImGui::IsKeyPressed(key, false);
        };
        EXPECT_EQ(rig.press({key}), NOTHING) << ImGui::GetKeyName(key);
        EXPECT_TRUE(canvasSawIt) << ImGui::GetKeyName(key);
        rig.afterRoute = {};
    }
    bool shiftTab = false;
    rig.afterRoute = [&] {
        const CanvasKeys keys = readCanvasKeys();
        shiftTab = shiftTab || (keys.tab && keys.shift);
    };
    EXPECT_EQ(rig.press({ImGuiMod_Shift, ImGuiKey_Tab}), NOTHING);
    EXPECT_TRUE(shiftTab) << "the canvas still reads Shift+Tab";
    rig.afterRoute = {};
    EXPECT_EQ(rig.es.quadManager().getSelectionCount(), 1u);
    EXPECT_EQ(rig.es.quadManager().getQuadCount(), 2u);
}

TEST(ShortcutRouterTest, AnUnknownChordDoesNothing) {
    RouterRig rig;
    rig.es.quadManager().selectQuad(rig.quad->getId());
    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiKey_Q}), NOTHING);
    EXPECT_EQ(rig.press({ImGuiKey_Q}), NOTHING);
    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiMod_Alt, ImGuiKey_M}), NOTHING)
        << "Ctrl+Alt+M is not Ctrl+M";
    EXPECT_EQ(rig.quad->getGeometry().getMeshMode(), MeshMode::CORNERS_ONLY);
    EXPECT_EQ(rig.es.quadManager().getQuadCount(), 2u);
}

TEST(ShortcutRouterTest, ModifiersMatchExactly) {
    RouterRig rig;
    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_S}), NOTHING);
    EXPECT_EQ(rig.saves, 0) << "Ctrl+Shift+S is not Save";
    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiKey_S}), std::vector<std::string>{action_id::SAVE});
    EXPECT_EQ(rig.saves, 1);

    rig.es.quadManager().selectQuad(rig.quad->getId());
    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiKey_RightBracket}),
              std::vector<std::string>{action_id::BRING_TO_FRONT});
    EXPECT_EQ(rig.es.quadManager().getQuads().back().get(), rig.quad);
}

// The menu and the key invoke the same action, so they have the same effect.
TEST(ShortcutRouterTest, TheMenuAndTheKeyHaveTheSameEffect) {
    RouterRig rig;
    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiKey_A}),
              std::vector<std::string>{action_id::SELECT_ALL});
    EXPECT_EQ(rig.es.quadManager().getSelectionCount(), 2u);
    rig.es.quadManager().deselectAll();
    EXPECT_EQ(rig.registry.invoke(action_id::SELECT_ALL, rig.ctx, Source::Menu), Outcome::Ran);
    EXPECT_EQ(rig.es.quadManager().getSelectionCount(), 2u);

    // While calibrating both are held off (the menu used to select anyway).
    rig.es.quadManager().deselectAll();
    OutputWindow* output = rig.om.createOutput("Projector");
    rig.es.beginCalibration(*output);
    EXPECT_EQ(rig.press({ImGuiMod_Ctrl, ImGuiKey_A}),
              std::vector<std::string>{action_id::SELECT_ALL});
    EXPECT_EQ(rig.registry.invoke(action_id::SELECT_ALL, rig.ctx, Source::Menu), Outcome::Disabled);
    EXPECT_FALSE(rig.es.quadManager().hasSelection());
}

TEST(ShortcutRouterTest, ChordTextIsWhatTheMenuShows) {
    EXPECT_EQ(chordText(Chord{ImGuiKey_RightBracket, MOD_CTRL | MOD_SHIFT}), "Ctrl+Shift+]");
    EXPECT_EQ(chordText(Chord{ImGuiKey_Equal, MOD_ALT}), "Alt+=");
    EXPECT_EQ(chordText(Chord{ImGuiKey_Minus, MOD_ALT}), "Alt+-");
    EXPECT_EQ(chordText(Chord{ImGuiKey_Comma, MOD_CTRL}), "Ctrl+,");
    EXPECT_EQ(chordText(Chord{ImGuiKey_Z, MOD_CTRL}), "Ctrl+Z");
    EXPECT_EQ(chordText(Chord{ImGuiKey_Delete, MOD_NONE}), "Delete");
    EXPECT_EQ(chordText(Chord{ImGuiKey_F11, MOD_NONE}), "F11");
    EXPECT_EQ(chordText(Chord{ImGuiKey_Escape, MOD_NONE}), "Escape");
}
