#include "SliceEditor.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "app/SliceEditing.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "project/Project.hpp"
#include "scene/Scene.hpp"
#include "scene/Slice.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace quadmapper {

namespace {

const char* const EDGE_NAMES[SLICE_EDGE_COUNT] = {"Left", "Right", "Top", "Bottom"};
const char* const CORNER_NAMES[4] = {"Top left", "Top right", "Bottom right", "Bottom left"};

void setStatus(EditorState& es, const std::string& message) {
    std::snprintf(es.statusMessage, sizeof(es.statusMessage), "%s", message.c_str());
    es.statusTimer = 3.0f;
}

}  // namespace

void SliceEditor::render(EditorContext& ctx, OutputWindow& output) {
    EditorState& es = ctx.es;
    OutputManager* om = ctx.outputManager;
    if (!ImGui::CollapsingHeader("Slices", ImGuiTreeNodeFlags_DefaultOpen)) return;

    const Scene* scene = OutputManager::sceneForOutput(es.scenes, output);
    if (!scene || !om) {
        ImGui::TextDisabled("Show a scene on this output to edit its slices.");
        return;
    }
    if (es.calibratingOutputId == output.getId()) {
        ImGui::TextDisabled("Leave calibration to edit this output's slices.");
        return;
    }
    const IVec2 canvas = scene->getCanvasSize();
    ImGui::TextDisabled("Canvas of '%s': %d x %d px", scene->getName().c_str(), canvas.x, canvas.y);

    const UUID outputId = output.getId();
    auto modified = [&]() {
        if (ctx.project) ctx.project->setModified(true);
    };

    // ---- List: select, add, remove ------------------------------------------------
    const auto& slices = output.getSlices();
    if (findSliceIndex(slices, es.selectedSliceId) < 0 && !slices.empty()) {
        es.selectedSliceId = slices.front().id;  // another output, or the slice went
    }
    const bool warpingThis = es.sliceWarpOutputId == outputId;
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    const float listHeight = rowHeight * static_cast<float>(std::clamp<size_t>(slices.size(), 1, 5)) +
                             ImGui::GetStyle().WindowPadding.y * 2.0f;
    ImGui::BeginChild("##SliceList", ImVec2(0.0f, listHeight), true);
    for (const Slice& slice : slices) {
        ImGui::PushID(&slice);
        char label[256];
        std::snprintf(label, sizeof(label), "%s   (%.0f, %.0f  %.0f x %.0f)", slice.name.c_str(),
                      slice.input.x, slice.input.y, slice.input.width, slice.input.height);
        if (ImGui::Selectable(label, slice.id == es.selectedSliceId)) {
            es.selectedSliceId = slice.id;
            if (warpingThis) es.beginSliceWarp(output, slice.id);  // keep editing, on this one
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (ImGui::Button("Add Slice")) {
        Slice added = makeFullCanvasSlice(canvas);
        added.name = "Slice " + std::to_string(slices.size() + 1);
        const UUID addedId = added.id;
        editSlices(es, output, "Add slice", "", [&](std::vector<Slice>& s) { s.push_back(added); });
        es.selectedSliceId = addedId;
        modified();
        return;  // the list changed under this frame's references
    }
    ImGui::SetItemTooltip("Add a slice of the whole canvas to this output");
    ImGui::SameLine();
    ImGui::BeginDisabled(slices.size() <= 1);
    if (ImGui::Button("Remove Slice")) {
        const int index = findSliceIndex(slices, es.selectedSliceId);
        if (index >= 0) {
            editSlices(es, output, "Remove slice", "",
                       [&](std::vector<Slice>& s) { s.erase(s.begin() + index); });
            const auto& now = output.getSlices();
            es.selectedSliceId = now[static_cast<size_t>(std::min<int>(index, static_cast<int>(now.size()) - 1))].id;
            modified();
        }
        ImGui::EndDisabled();
        return;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(slices.size() <= 1 ? "An output keeps at least one slice"
                                             : "Remove the selected slice");

    const int index = findSliceIndex(slices, es.selectedSliceId);
    if (index < 0) return;
    const Slice& slice = slices[static_cast<size_t>(index)];
    const UUID sliceId = slice.id;
    const std::string idText = sliceId.toString();

    // One undoable edit of the selected slice; edits with the same key merge (a drag).
    auto edit = [&](const char* description, const std::string& key, auto&& apply) {
        editSlices(es, output, description, key.empty() ? std::string() : key + " " + idText,
                   [&](std::vector<Slice>& s) { apply(s[static_cast<size_t>(index)]); });
        modified();
    };

    ImGui::Separator();
    // ---- Name ----------------------------------------------------------------------
    // The field follows the slice (an undo can rename it) except while it is typed in.
    if (nameSliceId_ != sliceId || !nameEditing_) {
        std::strncpy(nameBuffer_, slice.name.c_str(), sizeof(nameBuffer_) - 1);
        nameBuffer_[sizeof(nameBuffer_) - 1] = '\0';
        nameSliceId_ = sliceId;
    }
    const bool renamed = ImGui::InputText("Name##slice", nameBuffer_, sizeof(nameBuffer_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    nameEditing_ = ImGui::IsItemActive();
    if (renamed && slice.name != nameBuffer_) {
        const std::string name = nameBuffer_;
        edit("Rename slice", "", [&](Slice& s) { s.name = name; });
    }

    // ---- Input rectangle -----------------------------------------------------------
    float rect[4] = {slice.input.x, slice.input.y, slice.input.width, slice.input.height};
    if (ImGui::DragFloat4("Input (px)", rect, 1.0f, 0.0f, 0.0f, "%.0f")) {
        const CanvasRect input{rect[0], rect[1], std::max(1.0f, rect[2]), std::max(1.0f, rect[3])};
        edit("Slice input", "input", [&](Slice& s) { s.input = input; });
    }
    ImGui::SetItemTooltip("x, y, width, height of the part of the canvas this slice shows,\n"
                          "in canvas pixels (the outline on the canvas while this output is selected)");

    // ---- Warp -------------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Text("Warp (output frame)");
    if (warpingThis) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.65f, 0.2f, 1.0f));
        if (ImGui::Button("Editing Warp on Canvas - Done")) es.endSliceWarp();
        ImGui::PopStyleColor(2);
        ImGui::TextWrapped("Drag a corner, or Tab / Shift+Tab to pick one and the arrows to nudge "
                           "it (Shift 10x, Alt 0.1x). Esc when done.");
    } else {
        if (ImGui::Button("Edit Warp on Canvas")) es.beginSliceWarp(output, sliceId);
        ImGui::SetItemTooltip("The canvas shows this output's frame; move the warp corners with "
                              "the mouse or the keyboard, as for a quad");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Warp")) {
        edit("Reset slice warp", "", [&](Slice& s) { s.warp = fullFrameWarp(); });
    }
    for (int c = 0; c < 4; ++c) {
        Vec2 corner = output.getSlices()[static_cast<size_t>(index)].warp.getCorners()[static_cast<size_t>(c)];
        float value[2] = {corner.x, corner.y};
        ImGui::PushID(c);
        if (ImGui::DragFloat2(CORNER_NAMES[c], value, 0.001f, -1.0f, 2.0f, "%.4f")) {
            const Vec2 moved(value[0], value[1]);
            editSlices(es, output, "Move slice warp corner", sliceWarpCornerKey(sliceId, c),
                       [&](std::vector<Slice>& s) { s[static_cast<size_t>(index)].warp.setCorner(c, moved); });
            modified();
        }
        ImGui::PopID();
    }

    // ---- Blend edges ----------------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Text("Edge blend (width px, gamma, curve)");
    for (int e = 0; e < SLICE_EDGE_COUNT; ++e) {
        const BlendEdge edge = output.getSlices()[static_cast<size_t>(index)].blend[static_cast<size_t>(e)];
        const Slice& now = output.getSlices()[static_cast<size_t>(index)];
        const float maxWidth = (e == SLICE_EDGE_LEFT || e == SLICE_EDGE_RIGHT) ? now.input.width : now.input.height;
        ImGui::PushID(e);
        const float fieldWidth = std::max(40.0f, (ImGui::GetContentRegionAvail().x - 60.0f) / 3.0f);
        ImGui::SetNextItemWidth(fieldWidth);
        float width = edge.width;
        if (ImGui::DragFloat("##width", &width, 1.0f, 0.0f, maxWidth, "%.0f px")) {
            width = std::clamp(width, 0.0f, maxWidth);
            edit("Blend width", "blend width " + std::to_string(e),
                 [&](Slice& s) { s.blend[static_cast<size_t>(e)].width = width; });
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fieldWidth);
        float gamma = edge.gamma;
        if (ImGui::DragFloat("##gamma", &gamma, 0.01f, MIN_BLEND_GAMMA, MAX_BLEND_GAMMA, "g %.2f")) {
            gamma = std::clamp(gamma, MIN_BLEND_GAMMA, MAX_BLEND_GAMMA);
            edit("Blend gamma", "blend gamma " + std::to_string(e),
                 [&](Slice& s) { s.blend[static_cast<size_t>(e)].gamma = gamma; });
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fieldWidth);
        float curve = edge.curve;
        if (ImGui::DragFloat("##curve", &curve, 0.01f, MIN_BLEND_CURVE, MAX_BLEND_CURVE, "c %.2f")) {
            curve = std::clamp(curve, MIN_BLEND_CURVE, MAX_BLEND_CURVE);
            edit("Blend curve", "blend curve " + std::to_string(e),
                 [&](Slice& s) { s.blend[static_cast<size_t>(e)].curve = curve; });
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(EDGE_NAMES[e]);
        ImGui::PopID();
    }
    float black = output.getSlices()[static_cast<size_t>(index)].blackLevel;
    if (ImGui::SliderFloat("Black level", &black, 0.0f, MAX_BLACK_LEVEL, "%.3f")) {
        black = std::clamp(black, 0.0f, MAX_BLACK_LEVEL);
        edit("Black level", "black", [&](Slice& s) { s.blackLevel = black; });
    }
    ImGui::SetItemTooltip("Lift of black outside the blend zones, to match the brighter black "
                          "where two projectors overlap");

    // ---- Scene-wide: auto-blend and the test pattern ------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    int showing = 0;
    for (const auto& o : om->getOutputs()) {
        if (o && o->getSceneId() == scene->getId()) ++showing;
    }
    ImGui::BeginDisabled(showing < 2);
    if (ImGui::Button("Auto-blend")) {
        const int edges = autoBlendScene(es, *om, scene->getId());
        if (edges > 0) {
            modified();
            setStatus(es, "Auto-blend: set " + std::to_string(edges) + " blend edge" +
                              (edges == 1 ? "" : "s") + " of '" + scene->getName() + "'");
        } else {
            setStatus(es, "Auto-blend: no slices of different outputs overlap on the canvas");
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(showing < 2 ? "Needs two or more outputs showing this scene"
                                      : "Set the facing edges of every overlap between this scene's "
                                        "outputs to the overlap width (gamma 2.2)");
    ImGui::SameLine();
    const bool pattern = om->isBlendTestPattern();
    if (pattern) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.55f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.65f, 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
    }
    if (ImGui::Button(pattern ? "Blend Test Pattern: ON" : "Blend Test Pattern: off")) {
        om->setBlendTestPattern(!pattern);
    }
    if (pattern) ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip("Show a grid and white bands on every output instead of the show, "
                          "to judge the overlaps by eye (not saved)");
}

}  // namespace quadmapper
