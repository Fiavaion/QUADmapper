#include "EditorUi.hpp"

#include "audio/AudioEngine.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorServices.hpp"
#include "editor/EditorState.hpp"
#include "editor/MediaAssign.hpp"
#include "editor/Preferences.hpp"
#include "editor/ShowTransport.hpp"
#include "editor/SliceEditing.hpp"
#include "input/VertexKeyboard.hpp"
#include "media/MediaItem.hpp"
#include "media/MediaLibrary.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "ui/ShortcutRouter.hpp"
#include "ui/dialogs/PreferencesDialog.hpp"

#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder API

#include <algorithm>
#include <cstdio>

namespace quadmapper {

EditorUi::EditorUi(PreferencesDialog& preferences) {
    host_.add(quadList_);
    host_.add(inspector_);
    host_.add(resources_);
    host_.add(projectTab_);
    host_.add(mediaBin_);
    host_.add(scenes_);
    host_.add(compositions_);
    host_.add(preview_);
    host_.add(outputs_);
    host_.add(videoPlayer_);
    host_.add(control_);
    host_.add(aiPhoto_);
    host_.add(preferences);
    host_.add(missingMedia_);
    host_.add(exportVideo_);
    host_.add(calibration_);
    host_.add(banners_);
    host_.add(conversions_);
    host_.add(sessionDialogs_);
}

EditorUi::~EditorUi() = default;

void EditorUi::attach(EditorContext& ctx) { host_.attach(ctx); }

void EditorUi::projectReplaced(EditorContext& ctx) { host_.projectReplaced(ctx); }

void EditorUi::frame(EditorContext& ctx, const std::function<void(EditorContext&)>& sceneTakes) {
    // The perform lock (ADR-051 §4) takes no drop. Every in-app drag edits the show
    // where it lands (a bin clip on a quad, a quad row onto another or into a group, a
    // scene row), and the drop targets work on disabled panels too, so a drag held when
    // the lock was taken is let go here, ahead of the canvas and every panel. The bin
    // and the lists, disabled, cannot start another.
    if (ctx.es.performLock.isOn() && ImGui::GetDragDropPayload()) ImGui::ClearDragDrop();

    // The canvas takes Tab/arrows for the selected quad's vertices or, while
    // calibrating the projector it shows, for the calibration points (ADR-052), or,
    // while a slice warp is edited, for its corners (ADR-047).
    const CanvasKeyOwner keys = canvasKeyOwner(ctx.es, ctx.outputManager);
    claimCanvasEditingKeys(keys != CanvasKeyOwner::None);
    updateSpaceKey(ctx);
    stepDone(FrameStep::CanvasKeys);

    // On-canvas viewport: the corner-pin / mesh-edit warp interaction, then every
    // quad drawn to the main GL framebuffer. Runs right after NewFrame(), ahead of
    // every panel, so it acts on this frame's input first (the success-metric
    // vertex-warp UX) and a nudge shows in the very next presented frame (TD-21).
    // See ADR-0034 / ViewportEditor.
    viewport_.render(ctx);
    stepDone(FrameStep::Canvas);

    drawDockSpace(ctx);
    stepDone(FrameStep::DockSpace);
    menuBar_.render(ctx, host_);
    drawStatusBar(ctx, keys);
    savePreferenceChanges(ctx);
    stepDone(FrameStep::MenuBar);
    drawCanvasOverlay(ctx);
    stepDone(FrameStep::CanvasOverlay);

    // The editor-wide keys, after the canvas has read its own (ADR-051 §3).
    if (ctx.actions) routeShortcuts(*ctx.actions, ctx);
    stepDone(FrameStep::Shortcuts);
    if (sceneTakes) sceneTakes(ctx);
    stepDone(FrameStep::SceneTakes);

    host_.drawWindows(ctx);
    savePreferenceChanges(ctx);  // a device or mode picked in the Preferences window
    stepDone(FrameStep::Panels);
    host_.drawOverlays(ctx);
    stepDone(FrameStep::Overlays);
}

void EditorUi::drawDockSpace(EditorContext& ctx) {
    EditorState& es = ctx.es;

    // Create dockspace over the entire viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags dockspaceFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpace", nullptr, dockspaceFlags);
    ImGui::PopStyleVar(3);

    // Create the dockspace. The user's layout, which ImGui restored from the layout
    // file, stands; the default one is built only when there is none (a first run, a
    // run without a layout file) and when Window > Reset Layout asks for it.
    ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    bool buildDefault = host_.takeLayoutReset();
    if (!dockLayoutChecked_) {
        dockLayoutChecked_ = true;
        buildDefault = buildDefault || ImGui::DockBuilderGetNode(dockspaceId) == nullptr;
    }
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    if (buildDefault) {
        // Clear any existing layout
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        // Split the dockspace into regions
        ImGuiID dockLeft, dockRight, dockCenter, dockBottom;

        // First split: left panel (20% width)
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.20f, &dockLeft, &dockCenter);

        // Second split: right panel from center (25% of remaining)
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.30f, &dockRight, &dockCenter);

        // Third split: bottom panel from center (25% height)
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.25f, &dockBottom, &dockCenter);

        // Dock windows to their respective nodes
        ImGui::DockBuilderDockWindow(QuadListPanel::ID, dockLeft);
        ImGui::DockBuilderDockWindow(VideoPlayerPanel::ID, dockLeft);  // Tabbed with Quad List
        ImGui::DockBuilderDockWindow(InspectorPanel::ID, dockRight);
        ImGui::DockBuilderDockWindow(CalibrationPanel::ID, dockRight);  // Tabbed with Properties
        ImGui::DockBuilderDockWindow(ControlPanel::ID, dockRight);      // Tabbed with Properties (ADR-048)
        ImGui::DockBuilderDockWindow(AiPhotoPanel::ID, dockRight);      // Tabbed with Properties (ADR-053)
        ImGui::DockBuilderDockWindow(ResourcesPanel::ID, dockBottom);   // Tabbed panel with Project, Media, Outputs

        // Finish the layout
        ImGui::DockBuilderFinish(dockspaceId);
        focusQuadListIn_ = 2;
    }
    // A dock node shows the tab of the focused window, and each window takes the focus
    // as it first appears, so the Video Player (drawn last) would cover the Quad List.
    // Once the windows have appeared, the focus goes to the Quad List.
    if (focusQuadListIn_ > 0 && --focusQuadListIn_ == 0) es.requestPanelOpen(QuadListPanel::ID);

    // Capture the central node rect (the transparent canvas region between panels)
    // for next frame's ViewportEditor view transform (ADR-041). Convert from ImGui
    // screen space to window-local coords so it matches glfwGetCursorPos.
    if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
        const ImVec2 vpPos = viewport->Pos;
        es.canvasRegionPos = Vec2(centralNode->Pos.x - vpPos.x,
                                  centralNode->Pos.y - vpPos.y);
        es.canvasRegionSize = Vec2(centralNode->Size.x, centralNode->Size.y);
    }

    ImGui::End();
}

void EditorUi::drawStatusBar(EditorContext& ctx, CanvasKeyOwner keys) {
    EditorState& es = ctx.es;
    if (es.statusTimer > 0.0f) es.statusTimer -= ImGui::GetIO().DeltaTime;

    const char* hint = "Click a quad to select it.  Scroll: zoom the canvas.  Middle-drag: pan.";
    if (es.performLock.isOn()) {
        hint = "The show is locked: Output > Perform Lock unlocks it.";
    } else if (keys != CanvasKeyOwner::None) {
        hint = "Tab / Shift+Tab: next or previous point    Arrows: nudge    "
               "Shift+Arrows: 10x    Alt+Arrows: 0.1x    Space: play/pause (hold: pan)";
    }

    // A menu-bar window, as ImGui's own status bars are: it gets the menu bar's height
    // and the dockspace above shrinks to make room.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down,
                                    ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            if (es.statusTimer > 0.0f && es.statusMessage[0] != '\0') {
                ImGui::TextUnformatted(es.statusMessage);
            } else {
                ImGui::TextDisabled("%s", hint);
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

void EditorUi::updateSpaceKey(EditorContext& ctx) {
    // Held this long without the mouse, Space is the hand tool and its release is no tap.
    constexpr double SPACE_HOLD_SECONDS = 0.25;
    const ImGuiIO& io = ImGui::GetIO();
    // Not while typing, while a widget is being edited, or with a popup open.
    const bool free = !io.WantTextInput && !io.WantCaptureKeyboard &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // Owned, so keyboard navigation does not also press the focused button. Ownership
    // set now applies from the next frame, as for the canvas keys.
    if (free) ImGui::SetKeyOwner(ImGuiKey_Space, ImHashStr("QUADmapper.SpaceKey"));

    // A bare Space only: a chord with it (Ctrl+Space, ...) may be bound to an action.
    if (free && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper &&
        ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        spaceHeld_ = true;
        spaceIsHand_ = false;
        spacePressedAt_ = ImGui::GetTime();
    }
    if (spaceHeld_) {
        if (!ImGui::IsKeyDown(ImGuiKey_Space)) {
            spaceHeld_ = false;
            if (!spaceIsHand_ && ctx.sourceManager) toggleShow(*ctx.sourceManager);
        } else if (ImGui::GetTime() - spacePressedAt_ >= SPACE_HOLD_SECONDS ||
                   ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            spaceIsHand_ = true;
        }
    }
    ctx.es.canvasHandTool = spaceHeld_ && spaceIsHand_;
}

void EditorUi::drawCanvasOverlay(EditorContext& ctx) {
    EditorState& es = ctx.es;
    ImGuiIO& io = ImGui::GetIO();

    // Viewport zoom/pan overlay + input (ADR-041). The central node is a passthrough
    // hole, so the mouse is over the canvas (not a panel) exactly when it is in the
    // region and no ImGui window has capture. Mutates canvasZoom/canvasPan, which the
    // next frame's ViewportEditor consumes; anchored zoom reuses last frame's view.
    if (es.canvasRegionSize.x <= 1.0f || es.canvasRegionSize.y <= 1.0f) return;

    const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
    const ImVec2 regionAbs(es.canvasRegionPos.x + vpPos.x,
                           es.canvasRegionPos.y + vpPos.y);
    const ImVec2 regionSz(es.canvasRegionSize.x, es.canvasRegionSize.y);
    const ImVec2 mouse = ImGui::GetMousePos();
    const bool overRegion =
        mouse.x >= regionAbs.x && mouse.x <= regionAbs.x + regionSz.x &&
        mouse.y >= regionAbs.y && mouse.y <= regionAbs.y + regionSz.y;

    // Scroll-wheel zoom, anchored on the cursor (uses last frame's transform).
    if (overRegion && !io.WantCaptureMouse && io.MouseWheel != 0.0f &&
        es.lastViewScale.x != 0.0f && es.lastViewScale.y != 0.0f) {
        const float oldZoom = es.canvasZoom;
        float newZoom = oldZoom * (1.0f + 0.12f * io.MouseWheel);
        newZoom = std::min(12.0f, std::max(0.1f, newZoom));
        const float cx = (mouse.x - regionAbs.x) / regionSz.x;
        const float cy = (mouse.y - regionAbs.y) / regionSz.y;
        const float px = (cx - es.lastViewOffset.x) / es.lastViewScale.x;
        const float py = (cy - es.lastViewOffset.y) / es.lastViewScale.y;
        const float nsx = es.lastViewScale.x * (newZoom / oldZoom);
        const float nsy = es.lastViewScale.y * (newZoom / oldZoom);
        es.canvasPan.x = (cx - px * nsx) - (1.0f - nsx) * 0.5f;
        es.canvasPan.y = (cy - py * nsy) - (1.0f - nsy) * 0.5f;
        es.canvasZoom = newZoom;
    }

    // Pan (per-frame delta in region-normalized units): a middle drag, or a left drag
    // with the hand tool (Space held). Only a drag that began on the canvas pans it: one
    // that began on a panel leaves the canvas alone.
    const bool onCanvas = overRegion && !io.WantCaptureMouse;
    if (es.canvasHandTool && onCanvas) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        canvasPanButton_ = ImGuiMouseButton_Middle;
        canvasPanning_ = onCanvas;
    } else if (es.canvasHandTool && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        canvasPanButton_ = ImGuiMouseButton_Left;
        canvasPanning_ = onCanvas;
    }
    if (!ImGui::IsMouseDown(canvasPanButton_) ||
        (canvasPanButton_ == ImGuiMouseButton_Left && !es.canvasHandTool)) {
        canvasPanning_ = false;
    }
    if (canvasPanning_ && ImGui::IsMouseDragging(canvasPanButton_)) {
        const ImVec2 d = ImGui::GetMouseDragDelta(canvasPanButton_);
        ImGui::ResetMouseDragDelta(canvasPanButton_);
        es.canvasPan.x += d.x / regionSz.x;
        es.canvasPan.y += d.y / regionSz.y;
    }

    // Compact controls overlay in the top-left corner of the canvas region.
    ImGui::SetNextWindowPos(ImVec2(regionAbs.x + 8.0f, regionAbs.y + 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags overlayFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##ViewportZoom", nullptr, overlayFlags)) {
        if (ImGui::SmallButton("Fit")) { es.canvasZoom = 1.0f; es.canvasPan = Vec2(0.0f, 0.0f); }
        ImGui::SameLine();
        if (ImGui::SmallButton("-")) es.canvasZoom = std::max(0.1f, es.canvasZoom / 1.12f);
        ImGui::SetItemTooltip("Zoom out");
        ImGui::SameLine();
        ImGui::Text("%3.0f%%", es.canvasZoom * 100.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) es.canvasZoom = std::min(12.0f, es.canvasZoom * 1.12f);
        ImGui::SetItemTooltip("Zoom in");
        ImGui::SameLine();
        const bool snapping = es.vertexSnapping;
        if (snapping) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(snapping ? "Snap: On" : "Snap: Off")) es.vertexSnapping = !snapping;
        if (snapping) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Snap a dragged vertex to the corners and mesh points of other quads");
        if (es.scenes.size() > 1) {
            ImGui::SameLine();
            const bool others = es.showOtherScenes;
            if (others) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(others ? "Other scenes: On" : "Other scenes: Off")) {
                es.showOtherScenes = !others;
            }
            if (others) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Outline the quads of the other scenes (other projectors) here, and snap "
                                  "to them. For projectors stacked on one surface; placed by pixel from "
                                  "the top-left.");
        }
    }
    ImGui::End();

    // Drop media onto the quad under the cursor (ADR-042). A custom ImGui drop
    // target can't register over the passthrough central node (no window is hovered
    // there), so detect the drop manually: while a MEDIA_ITEM_ID drag is active,
    // on left-release over the canvas, assign to the topmost quad under the cursor.
    // Under the perform lock frame() has let go of the drag already. While the canvas
    // shows an output's frame (calibrating, or editing a slice warp) the quads are not
    // where the cursor is, so nothing is assigned.
    const bool showsScene = !calibrationViewOutput(es, ctx.outputManager) &&
                            !sliceWarpOutput(es, ctx.outputManager);
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    if (showsScene && drag && drag->IsDataType("MEDIA_ITEM_ID") &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left) && overRegion &&
        es.lastViewScale.x != 0.0f && es.lastViewScale.y != 0.0f) {
        UUID mediaId = *(const UUID*)drag->Data;
        MediaItem* item = ctx.mediaLibrary ? ctx.mediaLibrary->getItem(mediaId) : nullptr;
        if (item) {
            const float cx = (mouse.x - regionAbs.x) / regionSz.x;
            const float cy = (mouse.y - regionAbs.y) / regionSz.y;
            const Vec2 canvasPt((cx - es.lastViewOffset.x) / es.lastViewScale.x,
                                (cy - es.lastViewOffset.y) / es.lastViewScale.y);
            if (Quad* target = es.quadManager().quadAtPoint(canvasPt)) {
                UUID sid = assignMediaToQuad(es, ctx.sourceManager, *target, *item);
                if (!sid.isNull()) {
                    es.quadManager().selectQuad(target->getId());
                    snprintf(es.statusMessage, sizeof(es.statusMessage),
                             "Assigned '%s' to %s", item->getName().c_str(),
                             target->getName().c_str());
                    es.statusTimer = 3.0f;
                }
            }
        }
    }
}

void EditorUi::savePreferenceChanges(EditorContext& ctx) {
    if (!ctx.prefs || !ctx.services) return;
    AppPreferences& prefs = *ctx.prefs;
    bool changed = false;

    std::string& device = prefs.performance.audioDevice;
    if (ctx.audio && ctx.audio->preferredDevice() != device) {
        device = ctx.audio->preferredDevice();
        changed = true;
    }
    const int mode = ctx.es.uiMode == EditorState::UiMode::Advanced ? 1 : 0;
    if (mode != prefs.general.uiMode) {
        prefs.general.uiMode = mode;
        changed = true;
    }
    auto follow = [&changed](bool editor, bool& saved) {
        if (editor != saved) {
            saved = editor;
            changed = true;
        }
    };
    follow(ctx.es.vertexSnapping, prefs.general.vertexSnapping);
    follow(ctx.es.uvGridOnNewQuads, prefs.general.testGridOnNewQuads);
    follow(ctx.es.showOtherScenes, prefs.general.showOtherScenes);
    follow(ctx.es.newGridPointsLinear, prefs.general.newGridPointsLinear);
    follow(ctx.es.cropInPercent, prefs.general.cropInPercent);
    if (changed) ctx.services->savePreferences();
}

}  // namespace quadmapper
