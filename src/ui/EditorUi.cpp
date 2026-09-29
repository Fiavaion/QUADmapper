#include "EditorUi.hpp"

#include "audio/AudioEngine.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorServices.hpp"
#include "editor/EditorState.hpp"
#include "editor/MediaAssign.hpp"
#include "editor/Preferences.hpp"
#include "editor/SliceEditing.hpp"
#include "input/VertexKeyboard.hpp"
#include "media/MediaItem.hpp"
#include "media/MediaLibrary.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
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
    host_.add(preview_);
    host_.add(outputs_);
    host_.add(videoPlayer_);
    host_.add(control_);
    host_.add(preferences);
    host_.add(missingMedia_);
    host_.add(calibration_);
    host_.add(banners_);
    host_.add(conversions_);
    host_.add(sessionDialogs_);
}

EditorUi::~EditorUi() = default;

void EditorUi::attach(EditorContext& ctx) { host_.attach(ctx); }

void EditorUi::projectReplaced(EditorContext& ctx) { host_.projectReplaced(ctx); }

void EditorUi::beginFrame(EditorContext& ctx) {
    // The canvas takes Tab/arrows for the selected quad's vertices or, while
    // calibrating the projector it shows, for the calibration points (ADR-052), or,
    // while a slice warp is edited, for its corners (ADR-047).
    claimCanvasEditingKeys(canvasKeyOwner(ctx.es, ctx.outputManager) != CanvasKeyOwner::None);

    // On-canvas viewport: the corner-pin / mesh-edit warp interaction, then every
    // quad drawn to the main GL framebuffer. Runs right after NewFrame(), ahead of
    // every panel, so it acts on this frame's input first (the success-metric
    // vertex-warp UX) and a nudge shows in the very next presented frame (TD-21).
    // See ADR-0034 / ViewportEditor.
    viewport_.render(ctx);

    drawDockSpace(ctx);
    menuBar_.render(ctx, host_);
    savePreferenceChanges(ctx);
    drawCanvasOverlay(ctx);
}

void EditorUi::drawPanels(EditorContext& ctx) {
    host_.drawWindows(ctx);
    savePreferenceChanges(ctx);  // a device or mode picked in the Preferences window
}

void EditorUi::drawOverlays(EditorContext& ctx) { host_.drawOverlays(ctx); }

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

    // Create the dockspace
    ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

    // The default dock layout, built on the first frame of each run.
    if (!dockLayoutBuilt_) {
        dockLayoutBuilt_ = true;

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
        ImGui::DockBuilderDockWindow(ResourcesPanel::ID, dockBottom);   // Tabbed panel with Project, Media, Outputs

        // Finish the layout
        ImGui::DockBuilderFinish(dockspaceId);
    }

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

    // Middle-drag pan (per-frame delta in region-normalized units).
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
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
        ImGui::SameLine();
        ImGui::Text("%3.0f%%", es.canvasZoom * 100.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("+")) es.canvasZoom = std::min(12.0f, es.canvasZoom * 1.12f);
    }
    ImGui::End();

    // Drop media onto the quad under the cursor (ADR-042). A custom ImGui drop
    // target can't register over the passthrough central node (no window is hovered
    // there), so detect the drop manually: while a MEDIA_ITEM_ID drag is active,
    // on left-release over the canvas, assign to the topmost quad under the cursor.
    const ImGuiPayload* drag = ImGui::GetDragDropPayload();
    if (drag && drag->IsDataType("MEDIA_ITEM_ID") &&
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
    if (changed) ctx.services->savePreferences();
}

}  // namespace quadmapper
