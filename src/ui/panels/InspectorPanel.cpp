#include "InspectorPanel.hpp"

#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/MediaAssign.hpp"
#include "editor/QuadSelection.hpp"
#include "editor/MeshLoads.hpp"
#include "editor/GridUndo.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "scene/QuadGeometry.hpp"
#include "scene/Commands.hpp"
#include "scene/CropInsets.hpp"
#include "scene/MeshObject.hpp"
#include "scene/Scene.hpp"
#include "renderer/Texture.hpp"
#include "renderer/UvTestGrid.hpp"
#include "media/MediaLibrary.hpp"
#include "media/MediaItem.hpp"
#include "source/SourceManager.hpp"
#include "source/procedural/ProceduralSource.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "project/Project.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "core/Types.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_internal.h>  // ClearActiveID
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <cstdio>
#include <cstring>
#include <cfloat>  // FLT_MIN (full-width button sizing)

namespace quadmapper {

namespace {

constexpr const char* LOCKED_SHAPE_NOTE =
    "Locked: untick the lock in the Quad List to edit the shape.";

// A quad property the Inspector edits, read and written whole, so that one drag or one
// click is one undo step (QuadPropertyCommand: keyed by quad id, so a deleted quad
// makes the step a no-op).
template <typename T>
struct QuadProp {
    const char* name;
    T (*get)(const Quad&);
    void (*set)(Quad&, const T&);
};

// The texture's UV transform: pan, scale (coverage), rotation and flips.
struct UvTransform {
    Vec2 offset{0.0f};
    Vec2 scale{1.0f};
    float rotation = 0.0f;
    bool flipH = false;
    bool flipV = false;
};

// The Bezier edges: on or off, and the eight control offsets.
struct Curves {
    bool enabled = false;
    std::array<Vec2, 8> offsets{};
};

const QuadProp<std::array<Vec2, 4>> CORNERS{
    "Corners", [](const Quad& q) { return q.getGeometry().getCorners(); },
    [](Quad& q, const std::array<Vec2, 4>& c) { q.getGeometry().setCorners(c); }};
const QuadProp<UvTransform> UV{
    "UV Transform",
    [](const Quad& q) {
        const TextureBinding& t = q.getTexture();
        return UvTransform{t.uvOffset, t.uvScale, t.uvRotation, t.flipH, t.flipV};
    },
    [](Quad& q, const UvTransform& uv) {
        TextureBinding& t = q.getTexture();
        t.uvOffset = uv.offset;
        t.uvScale = uv.scale;
        t.uvRotation = uv.rotation;
        t.flipH = uv.flipH;
        t.flipV = uv.flipV;
    }};
const QuadProp<NormalizedRect> CROP{
    "Crop", [](const Quad& q) { return q.getTexture().cropRect; },
    [](Quad& q, const NormalizedRect& r) { q.getTexture().cropRect = r; }};
const QuadProp<float> OPACITY{
    "Opacity", [](const Quad& q) { return q.getOpacity(); },
    [](Quad& q, const float& o) { q.setOpacity(o); }};
const QuadProp<ColorCorrection> COLOR{
    "Color Correction", [](const Quad& q) { return q.getColorCorrection(); },
    [](Quad& q, const ColorCorrection& cc) { q.getColorCorrection() = cc; }};
const QuadProp<SoftEdge> SOFT_EDGE{
    "Soft Edge", [](const Quad& q) { return q.getSoftEdge(); },
    [](Quad& q, const SoftEdge& se) { q.getSoftEdge() = se; }};
const QuadProp<Curves> CURVES{
    "Curved Edges",
    [](const Quad& q) {
        return Curves{q.getGeometry().isBezierEnabled(), q.getGeometry().getBezierOffsets()};
    },
    [](Quad& q, const Curves& c) {
        q.getGeometry().setBezierEnabled(c.enabled);
        q.getGeometry().setBezierOffsets(c.offsets);
    }};
const QuadProp<MeshMode> MESH_MODE{
    "Mesh Edit", [](const Quad& q) { return q.getGeometry().getMeshMode(); },
    [](Quad& q, const MeshMode& m) { q.getGeometry().setMeshMode(m); }};
const QuadProp<GridSnapshot> EVEN_GRID{
    "Even Grid", [](const Quad& q) { return captureGrid(q.getGeometry()); }, applyGridSnapshot};

// A click (button, checkbox): `value` becomes the property's value as one undo step.
template <typename T>
void setAsStep(EditorState& es, Quad& quad, const QuadProp<T>& prop, const T& value) {
    es.commandHistory.execute(std::make_unique<QuadPropertyCommand<T>>(
        prop.name, es.quadRef(quad), prop.get(quad), value, prop.set));
}

// A drag or slider: `widget` draws one ImGui item that edits `prop` live. The value
// read before it, on the frame it becomes active, is kept in `pending`; its release
// after an edit records that value -> the value now as one undo step.
template <typename T, typename Widget>
void editAsStep(EditorState& es, Quad& quad, const QuadProp<T>& prop,
                std::function<void()>& pending, Widget&& widget) {
    T before = prop.get(quad);
    widget();
    if (ImGui::IsItemActivated()) {
        pending = [&es, ref = es.quadRef(quad), prop, before = std::move(before)]() {
            if (const Quad* now = ref.quad()) {
                es.commandHistory.execute(std::make_unique<QuadPropertyCommand<T>>(
                    prop.name, ref, before, prop.get(*now), prop.set));
            }
        };
    }
    if (ImGui::IsItemDeactivated()) {
        if (ImGui::IsItemDeactivatedAfterEdit() && pending) pending();
        pending = nullptr;
    }
}

// Corner pin at the default place (QuadGeometry::resetCorners).
std::array<Vec2, 4> defaultCorners(const QuadGeometry& geometry) {
    QuadGeometry reset = geometry;
    reset.resetCorners();
    return reset.getCorners();
}

// Clear Texture as one undo step: the quad's media binding, its legacy texture (the
// UV grid) and its saved media path go, and undo puts all three back. The command
// keeps the texture while it is cleared, and its source stays loaded while the step
// is in the history (collectMediaIds).
class ClearQuadTextureCommand : public Command {
public:
    ClearQuadTextureCommand(EditorState& es, const Quad& quad)
        : es_(es), target_(es.quadRef(quad)), mediaId_(quad.getTexture().mediaId) {}

    bool execute() override {
        Quad* quad = resolveForCommand(target_, getDescription());
        if (!quad) return false;
        quad->getTexture().mediaId = UUID();
        auto texture = es_.quadTextures.find(target_.quadId);
        if (texture != es_.quadTextures.end()) {
            texture_ = std::move(texture->second);
            es_.quadTextures.erase(texture);
        }
        auto path = es_.quadTexturePaths.find(target_.quadId);
        hadPath_ = path != es_.quadTexturePaths.end();
        if (hadPath_) {
            path_ = path->second;
            es_.quadTexturePaths.erase(path);
        }
        return true;
    }

    bool undo() override {
        Quad* quad = resolveForCommand(target_, getDescription());
        if (!quad) return false;
        quad->getTexture().mediaId = mediaId_;
        if (texture_) es_.quadTextures[target_.quadId] = std::move(texture_);
        if (hadPath_) es_.quadTexturePaths[target_.quadId] = path_;
        return true;
    }

    std::string getDescription() const override { return "Clear Texture"; }

    void collectMediaIds(std::unordered_set<UUID>& out) const override {
        if (!mediaId_.isNull()) out.insert(mediaId_);
    }
    void remapMediaId(const UUID& from, const UUID& to) override {
        if (mediaId_ == from) mediaId_ = to;
    }

private:
    EditorState& es_;
    QuadRef target_;
    UUID mediaId_;
    std::unique_ptr<Texture> texture_;
    std::string path_;
    bool hadPath_ = false;
};

}  // namespace

void InspectorPanel::drawContent(EditorContext& ctx) {
    EditorState& es = ctx.es;
    using SelectionType = EditorState::SelectionType;

    // A drag or slider still held when the Properties move to another quad (a scene
    // take, a delete) ends on the quad it began on, as that quad's undo step: the
    // widget at the same place must not go on editing the new quad without one.
    const Quad* shownQuad = es.currentSelectionType == SelectionType::Quad
                                ? es.quadManager().getPrimarySelectedQuad()
                                : nullptr;
    const UUID shownId = shownQuad ? shownQuad->getId() : UUID::null();
    if (pendingStep_ && shownId != pendingStepQuad_) {
        pendingStep_();
        pendingStep_ = nullptr;
        ImGui::ClearActiveID();
    }
    pendingStepQuad_ = shownId;

    // Show header indicating current selection type
    switch (es.currentSelectionType) {
        case SelectionType::Quad:
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Quad Properties");
            break;
        case SelectionType::Output:
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Output Properties");
            break;
        case SelectionType::Media:
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "Media Properties");
            break;
        case SelectionType::Object:
            ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "Object Properties");
            break;
        default:
            ImGui::TextDisabled("Properties");
            break;
    }
    ImGui::Separator();

    if (es.currentSelectionType == SelectionType::Quad) {
        renderQuadProperties(ctx);
    } else if (es.currentSelectionType == SelectionType::Output) {
        // Simple mode (ADR-042 amendment): the Outputs tab edits the output; scenes,
        // slices and calibration stay in Advanced mode.
        if (es.uiMode == EditorState::UiMode::Simple) {
            ImGui::TextDisabled("Edit this output in the Outputs tab.");
            ImGui::TextDisabled("Scenes, slices and calibration: Advanced mode.");
        } else {
            renderOutputProperties(ctx);
        }
    } else if (es.currentSelectionType == SelectionType::Media) {
        renderMediaProperties(ctx);
    } else if (es.currentSelectionType == SelectionType::Object) {
        renderObjectProperties(ctx);
    } else {
        // -------- No Selection --------
        ImGui::TextDisabled("Nothing selected");
        ImGui::TextDisabled("Select a quad, output, or media item");
    }
}

// ============================================================
//  Quad Properties
// ============================================================
void InspectorPanel::renderQuadProperties(EditorContext& ctx) {
    EditorState& es = ctx.es;
    auto& quadManager        = es.quadManager();
    auto& quadTextures       = es.quadTextures;
    auto& quadTexturePaths   = es.quadTexturePaths;
    auto& currentTexturePath = es.currentTexturePath;
    auto& statusMessage      = es.statusMessage;
    auto& statusTimer        = es.statusTimer;

    Quad* selectedQuad = quadManager.getPrimarySelectedQuad();

    // Simple mode (ADR-042): a curated panel — Flip U/V, Rotate, Crop, Reset Quad —
    // so a non-technical user isn't faced with the full control set.
    if (es.uiMode == EditorState::UiMode::Simple) {
        if (selectedQuad) renderSimpleQuadProperties(ctx, *selectedQuad);
        else ImGui::TextDisabled("Select a quad to edit it");
        return;
    }

    if (selectedQuad) {
        // Editable name
        strncpy(quadName_, selectedQuad->getName().c_str(), sizeof(quadName_) - 1);
        quadName_[sizeof(quadName_) - 1] = '\0';

        if (ImGui::InputText("Name", quadName_, sizeof(quadName_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            ctx.es.commandHistory.execute(
                std::make_unique<RenameQuadCommand>(ctx.es.quadRef(*selectedQuad), quadName_));
        }

        ImGui::Separator();

        // Texture section
        ImGui::Text("Texture:");

        // Drag-drop target for media from Media Bin
        ImGui::BeginGroup();
        followTexturePath(es, selectedQuad, pathFieldQuad_, pathFieldPath_);
        ImGui::InputText("Path", currentTexturePath, sizeof(currentTexturePath));
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            if (strlen(currentTexturePath) > 0) {
                // ADR-040 P2: bind via a (deduped) SourceManager source + mediaId
                // instead of loading a per-quad Texture into quadTextures.
                UUID sid = ctx.sourceManager
                    ? ctx.sourceManager->getOrCreateImageSource(pathFromUtf8(currentTexturePath)) : UUID();
                if (!sid.isNull()) {
                    bindSourceToQuad(ctx.es, *selectedQuad, sid,
                                     currentTexturePath, /*isVideo=*/false);
                    Logger::info("Loaded texture for quad '{}': {}",
                                 selectedQuad->getName(), currentTexturePath);
                    snprintf(statusMessage, sizeof(statusMessage),
                             "Loaded texture: %s", currentTexturePath);
                    statusTimer = 3.0f;
                } else {
                    Logger::error("Failed to load texture: {}", currentTexturePath);
                    snprintf(statusMessage, sizeof(statusMessage),
                             "Failed to load texture: %s", currentTexturePath);
                    statusTimer = 5.0f;
                }
            } else {
                snprintf(statusMessage, sizeof(statusMessage),
                         "Please enter a texture path");
                statusTimer = 3.0f;
            }
        }

        // Show the bound texture's size: image via mediaId -> SourceManager, with
        // a fallback to any legacy quadTextures entry (e.g. the UV test grid).
        Texture* boundTex = nullptr;
        if (ctx.sourceManager && !selectedQuad->getTexture().mediaId.isNull()) {
            boundTex = ctx.sourceManager->getTexture(selectedQuad->getTexture().mediaId);
        }
        if (!boundTex) {
            auto texIt = quadTextures.find(selectedQuad->getId());
            if (texIt != quadTextures.end()) boundTex = texIt->second.get();
        }
        if (boundTex) {
            ImGui::Text("Size: %dx%d", boundTex->getWidth(), boundTex->getHeight());
            if (ImGui::Button("Clear Texture")) {
                es.commandHistory.execute(
                    std::make_unique<ClearQuadTextureCommand>(es, *selectedQuad));
                currentTexturePath[0] = '\0';
            }
        }

        // UV test grid (#2): apply a colored UV reference grid to this quad, and
        // toggle whether new quads get it automatically.
        if (ImGui::Button("Apply UV Test Grid")) {
            assignUvTestGrid(quadTextures, quadTexturePaths,
                             selectedQuad->getId(), 1024, 8);
            // Override any assigned source (renderer resolves mediaId first).
            selectedQuad->getTexture().mediaId = UUID();
            currentTexturePath[0] = '\0';
            snprintf(statusMessage, sizeof(statusMessage), "Applied UV test grid");
            statusTimer = 3.0f;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Colored UV reference grid — check orientation/stretch");
        }
        ImGui::Checkbox("UV grid on new quads", &ctx.es.uvGridOnNewQuads);
        ImGui::EndGroup();

        // Drag-drop target for media from Media Bin
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload =
                    ImGui::AcceptDragDropPayload("MEDIA_ITEM_ID")) {
                UUID mediaId = *(const UUID*)payload->Data;
                MediaItem* item = ctx.mediaLibrary->getItem(mediaId);
                if (item) {
                    // Centralized bind (ADR-042 P2): resolves image vs video correctly.
                    UUID sid = assignMediaToQuad(ctx.es, ctx.sourceManager,
                                                 *selectedQuad, *item);
                    if (!sid.isNull()) {
                        Logger::info("Assigned media '{}' to quad '{}'",
                                     item->getName(), selectedQuad->getName());
                        snprintf(statusMessage, sizeof(statusMessage),
                                 "Assigned: %s", item->getName().c_str());
                        statusTimer = 3.0f;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        // UV Controls section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("UV Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
            Quad& quad = *selectedQuad;
            TextureBinding& texBinding = quad.getTexture();
            const auto setUv = [&](UvTransform uv) { setAsStep(es, quad, UV, uv); };
            const auto editUv = [&](auto&& widget) { editAsStep(es, quad, UV, pendingStep_, widget); };

            // Flip buttons at the top
            if (ImGui::Button(texBinding.flipH ? "Flip H [ON]" : "Flip H")) {
                UvTransform uv = UV.get(quad);
                uv.flipH = !uv.flipH;
                setUv(uv);
            }
            ImGui::SameLine();
            if (ImGui::Button(texBinding.flipV ? "Flip V [ON]" : "Flip V")) {
                UvTransform uv = UV.get(quad);
                uv.flipV = !uv.flipV;
                setUv(uv);
            }

            // Prominent one-click reset for the whole UV transform (offset / coverage
            // / scale / rotation / flips) back to defaults.
            ImGui::Spacing();
            if (ImGui::Button("Reset Transform (Offset / Coverage / Scale / Rotation)",
                              ImVec2(-FLT_MIN, 0.0f))) {
                setUv(UvTransform{});  // offset 0, scale 1 (=100% coverage), rotation 0, no flips
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Reset pan, coverage/scale, rotation and flips to defaults");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Offset (Pan) - DragFloat with Ctrl+Click for input
            ImGui::Text("Offset (Pan):");
            ImGui::TextDisabled("Ctrl+Click to type value");
            ImGui::SetNextItemWidth(200.0f);
            editUv([&] {
                ImGui::DragFloat("Offset X", &texBinding.uvOffset.x, 0.01f, -2.0f, 2.0f, "%.3f");
            });
            ImGui::SetNextItemWidth(200.0f);
            editUv([&] {
                ImGui::DragFloat("Offset Y", &texBinding.uvOffset.y, 0.01f, -2.0f, 2.0f, "%.3f");
            });

            ImGui::Spacing();

            // Coverage (easier for multi-quad spanning) - inverse of scale
            // Coverage 100% = full texture, 50% = half texture visible (scale = 2.0)
            ImGui::Text("Coverage (for spanning):");
            ImGui::TextDisabled("50%% = texture spans 2 quads");

            float coverageU = (texBinding.uvScale.x > 0.001f)
                ? (100.0f / texBinding.uvScale.x) : 100.0f;
            float coverageV = (texBinding.uvScale.y > 0.001f)
                ? (100.0f / texBinding.uvScale.y) : 100.0f;

            ImGui::SetNextItemWidth(200.0f);
            editUv([&] {
                if (ImGui::DragFloat("U Coverage", &coverageU, 1.0f, 10.0f, 400.0f, "%.0f%%")) {
                    texBinding.uvScale.x = 100.0f / std::max(coverageU, 1.0f);
                }
            });
            ImGui::SetNextItemWidth(200.0f);
            editUv([&] {
                if (ImGui::DragFloat("V Coverage", &coverageV, 1.0f, 10.0f, 400.0f, "%.0f%%")) {
                    texBinding.uvScale.y = 100.0f / std::max(coverageV, 1.0f);
                }
            });

            ImGui::Spacing();

            // Scale (direct control)
            ImGui::Text("Scale (Zoom):");
            ImGui::SetNextItemWidth(200.0f);
            editUv([&] {
                ImGui::DragFloat("Scale X", &texBinding.uvScale.x, 0.01f, 0.1f, 10.0f, "%.3f");
            });
            ImGui::SetNextItemWidth(200.0f);
            editUv([&] {
                ImGui::DragFloat("Scale Y", &texBinding.uvScale.y, 0.01f, 0.1f, 10.0f, "%.3f");
            });

            ImGui::Spacing();

            // Rotation with direct input
            float rotationDegrees = texBinding.uvRotation * 180.0f / 3.14159265f;
            ImGui::Text("Rotation:");
            ImGui::SetNextItemWidth(200.0f);
            editUv([&] {
                if (ImGui::DragFloat("Rotation##uv", &rotationDegrees,
                                     0.5f, -180.0f, 180.0f, "%.1f deg")) {
                    texBinding.uvRotation = rotationDegrees * 3.14159265f / 180.0f;
                }
            });
            // Quick rotation buttons
            const auto rotateTo = [&](float degrees) {
                UvTransform uv = UV.get(quad);
                uv.rotation = degrees * 3.14159265f / 180.0f;
                setUv(uv);
            };
            ImGui::SameLine();
            if (ImGui::Button("0##rot")) rotateTo(0.0f);
            ImGui::SameLine();
            if (ImGui::Button("90##rot")) rotateTo(90.0f);
            ImGui::SameLine();
            if (ImGui::Button("180##rot")) rotateTo(180.0f);
            ImGui::SameLine();
            if (ImGui::Button("270##rot")) rotateTo(270.0f);

            // Crop (ADR-040) — sample a sub-rectangle of the source, stretched to
            // fill the quad. Collapsed by default to keep the inspector tidy.
            ImGui::Spacing();
            if (ImGui::TreeNode("Crop (source region)")) {
                renderCropControls(ctx, quad);
                ImGui::TreePop();
            }
        }

        // Geometry section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
            Quad& quad = *selectedQuad;
            auto& geometry = quad.getGeometry();
            // A locked quad (the Quad List's lock) keeps its shape.
            const bool locked = quad.isLocked();
            if (locked) ImGui::TextDisabled("%s", LOCKED_SHAPE_NOTE);
            ImGui::BeginDisabled(locked);

            // Each corner field applies live; one drag is one undo step.
            ImGui::Text("Corners (normalized 0-1):");
            const char* cornerLabels[4] = {"Top Left", "Top Right", "Bottom Right", "Bottom Left"};
            for (int i = 0; i < 4; ++i) {
                editAsStep(es, quad, CORNERS, pendingStep_, [&] {
                    auto corners = geometry.getCorners();
                    if (ImGui::DragFloat2(cornerLabels[i], &corners[static_cast<size_t>(i)].x,
                                          0.01f, 0.0f, 1.0f)) {
                        geometry.setCorners(corners);
                    }
                });
            }

            if (ImGui::Button("Reset Corners")) {
                setAsStep(es, quad, CORNERS, defaultCorners(geometry));
            }

            // Curved (bezier) edges — drag the amber handles on the canvas to bow the
            // four edges. Only meaningful in corner-pin (CORNERS_ONLY) mode.
            {
                bool bez = geometry.isBezierEnabled();
                if (ImGui::Checkbox("Curved edges (bezier)", &bez)) {
                    setAsStep(es, quad, CURVES, Curves{bez, geometry.getBezierOffsets()});
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Drag the amber edge handles on the canvas to curve the edges");
                }
                if (bez) {
                    ImGui::SameLine();
                    if (ImGui::Button("Reset Curves")) {
                        setAsStep(es, quad, CURVES, Curves{});
                    }
                }
            }

            ImGui::Separator();
            // Even grid (uniform N×N) — the convenience action; resets divisions. The
            // grid follows the slider live; one drag is one undo step.
            int meshRes = geometry.getMeshResolution();
            editAsStep(es, quad, EVEN_GRID, pendingStep_, [&] {
                if (ImGui::SliderInt("Even Grid (N x N)", &meshRes, 1, 64)) {
                    geometry.setUniformGrid(meshRes);
                }
            });

            // Non-uniform grid divisions (ADR-038 Tier 1): add/remove division
            // lines at an arbitrary position, then warp the control vertices in
            // mesh-edit mode. Columns split U (vertical lines), rows split V.
            ImGui::Text("Grid: %d x %d cells", geometry.getColumns(),
                        geometry.getRows());
            ImGui::SetNextItemWidth(160.0f);
            ImGui::SliderFloat("Insert pos", &gridInsertPos_, 0.02f, 0.98f, "%.2f");
            if (ImGui::Button("Add Column")) {
                GridSnapshot before = captureGrid(geometry);
                geometry.insertUDivision(gridInsertPos_);
                pushGridChange(ctx.es.commandHistory, ctx.es.quadRef(*selectedQuad), "Add Column",
                               before, captureGrid(geometry));
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Row")) {
                GridSnapshot before = captureGrid(geometry);
                geometry.insertVDivision(gridInsertPos_);
                pushGridChange(ctx.es.commandHistory, ctx.es.quadRef(*selectedQuad), "Add Row",
                               before, captureGrid(geometry));
            }
            ImGui::SameLine();
            if (ImGui::Button("Del Column") && geometry.getColumns() > MIN_MESH_RESOLUTION) {
                GridSnapshot before = captureGrid(geometry);
                geometry.removeUDivision(geometry.getColumns() - 1);
                pushGridChange(ctx.es.commandHistory, ctx.es.quadRef(*selectedQuad), "Delete Column",
                               before, captureGrid(geometry));
            }
            ImGui::SameLine();
            if (ImGui::Button("Del Row") && geometry.getRows() > MIN_MESH_RESOLUTION) {
                GridSnapshot before = captureGrid(geometry);
                geometry.removeVDivision(geometry.getRows() - 1);
                pushGridChange(ctx.es.commandHistory, ctx.es.quadRef(*selectedQuad), "Delete Row",
                               before, captureGrid(geometry));
            }

            // Click-on-edge insert tool (ADR-038 Tier 1 batch 3b): arm, then click
            // in the viewport to place a division where you click. Arming switches
            // to mesh-edit so the grid is visible.
            using GridInsert = EditorState::GridInsert;
            const bool armCol = (ctx.es.gridInsert == GridInsert::Column);
            const bool armRow = (ctx.es.gridInsert == GridInsert::Row);
            if (ImGui::Button(armCol ? "Click-add Column [ON]" : "Click-add Column")) {
                ctx.es.gridInsert = armCol ? GridInsert::None : GridInsert::Column;
                if (!armCol) geometry.setMeshMode(MeshMode::MESH_EDIT);
            }
            ImGui::SameLine();
            if (ImGui::Button(armRow ? "Click-add Row [ON]" : "Click-add Row")) {
                ctx.es.gridInsert = armRow ? GridInsert::None : GridInsert::Row;
                if (!armRow) geometry.setMeshMode(MeshMode::MESH_EDIT);
            }
            if (ctx.es.gridInsert != GridInsert::None) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f),
                                   "Press in the viewport and drag to position the line "
                                   "(Esc to cancel)");
            }

            // Mesh Edit Mode
            ImGui::Separator();
            ImGui::Text("Mesh Editing:");

            bool meshEditMode = (geometry.getMeshMode() == MeshMode::MESH_EDIT);
            if (ImGui::Checkbox("Edit Mesh Vertices", &meshEditMode)) {
                setAsStep(es, quad, MESH_MODE,
                          meshEditMode ? MeshMode::MESH_EDIT : MeshMode::CORNERS_ONLY);
                // Reset all handle state when toggling mode
                es.meshDraggedVertex   = -1;
                es.meshHoveredVertex   = -1;
                es.meshSelectedVertex  = -1;
                es.meshSelectedHandle = es.meshHoveredHandle = es.meshDraggedHandle = -1;
                es.cornerDraggedVertex = -1;
                es.cornerHoveredVertex = -1;
                es.cornerSelectedVertex = -1;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Enable to drag individual mesh vertices for fine-tuned warping");
            }

            if (meshEditMode) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f),
                                   "Click and drag vertex handles to edit");

                if (geometry.hasMeshModifications()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Reset Mesh")) {
                        const GridSnapshot before = captureGrid(geometry);
                        geometry.resetMesh();
                        pushGridChange(es.commandHistory, es.quadRef(quad), "Reset Mesh", before,
                                       captureGrid(geometry));
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Clear every mesh vertex offset and grid-line handle, back"
                            " to the corner-pin surface");
                    }
                }
                ImGui::TextDisabled("H / Shift+H: the selected vertex's grid-line handles");

                // Show vertex count info
                ImGui::Text("Vertices: %d (%dx%d grid)",
                            geometry.getVertexCount(),
                            meshRes + 1, meshRes + 1);
            }
            ImGui::EndDisabled();
        }

        // Display section
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
            float opacity = selectedQuad->getOpacity();
            editAsStep(es, *selectedQuad, OPACITY, pendingStep_, [&] {
                if (ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f)) {
                    selectedQuad->setOpacity(opacity);
                }
            });

            ImGui::Separator();
            ImGui::Text("Z-Order");
            int zOrder = selectedQuad->getZOrder();
            int maxZ = static_cast<int>(quadManager.getQuadCount()) - 1;
            ImGui::Text("Current: %d / %d (higher = on top)", zOrder, maxZ);

            // Z-order buttons with proper labels
            if (ImGui::Button("Bring to Front")) {
                quadManager.moveQuadToBottom(selectedQuad->getId());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Ctrl+Shift+]");
            }
            ImGui::SameLine();
            if (ImGui::Button("Send to Back")) {
                quadManager.moveQuadToTop(selectedQuad->getId());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Ctrl+Shift+[");
            }

            if (ImGui::Button("Bring Forward")) {
                quadManager.moveQuadDown(selectedQuad->getId());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Ctrl+]");
            }
            ImGui::SameLine();
            if (ImGui::Button("Send Backward")) {
                quadManager.moveQuadUp(selectedQuad->getId());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Ctrl+[");
            }
        }

        // Color Correction section (ADR-036 / US-104). Slider ranges match the
        // ColorCorrection valid ranges; edits flag the project dirty.
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Color Correction")) {
            ColorCorrection& cc = selectedQuad->getColorCorrection();
            const auto editColor = [&](auto&& widget) {
                editAsStep(es, *selectedQuad, COLOR, pendingStep_, widget);
            };
            editColor([&] { ImGui::SliderFloat("Brightness", &cc.brightness, -1.0f, 1.0f); });
            editColor([&] { ImGui::SliderFloat("Contrast", &cc.contrast, 0.0f, 2.0f); });
            editColor([&] { ImGui::SliderFloat("Gamma", &cc.gamma, 0.1f, 4.0f); });
            editColor([&] { ImGui::SliderFloat3("RGB Gain", &cc.rgbGain.x, 0.0f, 2.0f); });
            if (ImGui::Button("Reset Color")) {
                setAsStep(es, *selectedQuad, COLOR, ColorCorrection{});
            }
        }

        // Soft Edge Blending section (ADR-037 / US-101). Per-edge feather widths
        // (fraction of the quad) + falloff gamma; for multi-projector overlaps.
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Soft Edge Blending")) {
            SoftEdge& se = selectedQuad->getSoftEdge();
            const auto editEdge = [&](auto&& widget) {
                editAsStep(es, *selectedQuad, SOFT_EDGE, pendingStep_, widget);
            };
            editEdge([&] { ImGui::SliderFloat("Left", &se.left, 0.0f, 0.5f); });
            editEdge([&] { ImGui::SliderFloat("Right", &se.right, 0.0f, 0.5f); });
            editEdge([&] { ImGui::SliderFloat("Top", &se.top, 0.0f, 0.5f); });
            editEdge([&] { ImGui::SliderFloat("Bottom", &se.bottom, 0.0f, 0.5f); });
            editEdge([&] { ImGui::SliderFloat("Edge Gamma", &se.gamma, 0.1f, 4.0f); });
            if (ImGui::Button("Reset Edges")) {
                setAsStep(es, *selectedQuad, SOFT_EDGE, SoftEdge{});
            }
        }
    } else {
        ImGui::TextDisabled("No quad selected");
        ImGui::TextDisabled("Select a quad from the Quad List");
    }
}

// Crop controls (Left/Right/Top/Bottom edge insets). Shared by the Advanced crop
// tree and the Simple-mode panel so the two never drift (ADR-042). The stored model
// stays normalized {x,y,w,h}; the insets are a pure UI re-parameterization
// (CropInsets.hpp), shown in source pixels when the source has a real pixel size and
// as a percentage otherwise (procedural sources, or media not loaded yet).
void InspectorPanel::renderCropControls(EditorContext& ctx, Quad& quad) {
    EditorState& es = ctx.es;
    const TextureBinding& texBinding = quad.getTexture();
    NormalizedRect& crop = quad.getTexture().cropRect;

    int srcWidth = 0;
    int srcHeight = 0;
    if (ctx.sourceManager && !texBinding.mediaId.isNull()) {
        const TextureSource* source = ctx.sourceManager->getSource(texBinding.mediaId);
        if (source && !dynamic_cast<const ProceduralSource*>(source)) {
            srcWidth = source->getWidth();
            srcHeight = source->getHeight();
        }
    }
    const bool inPixels = srcWidth > 0 && srcHeight > 0;

    struct EdgeControl { const char* label; CropEdge edge; };
    constexpr EdgeControl edges[] = {
        {"Crop Left##crop", CropEdge::Left},   {"Crop Right##crop", CropEdge::Right},
        {"Crop Top##crop", CropEdge::Top},     {"Crop Bottom##crop", CropEdge::Bottom},
    };

    if (inPixels) {
        ImGui::TextDisabled("Crop in from each edge (source pixels, %d x %d)",
                            srcWidth, srcHeight);
        for (const EdgeControl& c : edges) {
            int px = cropInsetPixels(crop, c.edge, srcWidth, srcHeight);
            const int maxPx = maxCropInsetPixels(crop, c.edge, srcWidth, srcHeight);
            ImGui::SetNextItemWidth(200.0f);
            editAsStep(es, quad, CROP, pendingStep_, [&] {
                if (ImGui::DragInt(c.label, &px, 1.0f, 0, maxPx, "%d px",
                                   ImGuiSliderFlags_AlwaysClamp)) {
                    crop = withCropInsetPixels(crop, c.edge, px, srcWidth, srcHeight);
                }
            });
        }
        const int left = cropInsetPixels(crop, CropEdge::Left, srcWidth, srcHeight);
        const int top = cropInsetPixels(crop, CropEdge::Top, srcWidth, srcHeight);
        const int right = cropInsetPixels(crop, CropEdge::Right, srcWidth, srcHeight);
        const int bottom = cropInsetPixels(crop, CropEdge::Bottom, srcWidth, srcHeight);
        ImGui::TextDisabled("Region: %d x %d px at (%d, %d)",
                            srcWidth - left - right, srcHeight - top - bottom, left, top);
    } else {
        ImGui::TextDisabled("Crop in from each edge (%% of source)");
        for (const EdgeControl& c : edges) {
            float pct = static_cast<float>(cropInset(crop, c.edge) * 100.0);
            ImGui::SetNextItemWidth(200.0f);
            editAsStep(es, quad, CROP, pendingStep_, [&] {
                if (ImGui::DragFloat(c.label, &pct, 0.5f, 0.0f, 100.0f, "%.1f%%")) {
                    crop = withCropInset(crop, c.edge, pct / 100.0, 0.01);
                }
            });
        }
    }

    auto setCrop = [&](float x, float y, float w, float h) {
        setAsStep(es, quad, CROP, NormalizedRect{x, y, w, h});
    };
    ImGui::TextDisabled("Halves (two-face split):");
    if (ImGui::Button("Left 1/2"))   setCrop(0.0f, 0.0f, 0.5f, 1.0f); ImGui::SameLine();
    if (ImGui::Button("Right 1/2"))  setCrop(0.5f, 0.0f, 0.5f, 1.0f); ImGui::SameLine();
    if (ImGui::Button("Top 1/2"))    setCrop(0.0f, 0.0f, 1.0f, 0.5f); ImGui::SameLine();
    if (ImGui::Button("Bottom 1/2")) setCrop(0.0f, 0.5f, 1.0f, 0.5f);

    ImGui::TextDisabled("Vertical quarters (box-wrap):");
    auto column = [&](const char* label, float x) {
        if (ImGui::Button(label)) setCrop(x, 0.0f, 0.25f, 1.0f);
    };
    column("1##cq", 0.0f);  ImGui::SameLine();
    column("2##cq", 0.25f); ImGui::SameLine();
    column("3##cq", 0.5f);  ImGui::SameLine();
    column("4##cq", 0.75f); ImGui::SameLine();
    if (ImGui::Button("Full##cq")) {
        setAsStep(es, quad, CROP, NormalizedRect{});
    }
}

// Simple-mode quad panel (ADR-042): only the handful of controls a basic projection
// mapping needs — flip, rotate, crop, and a one-click full reset. On-canvas corner
// warping (Tab/arrows/drag) stays available in both modes via the ViewportEditor.
void InspectorPanel::renderSimpleQuadProperties(EditorContext& ctx, Quad& quad) {
    constexpr float kPi = 3.14159265f;
    TextureBinding& texBinding = quad.getTexture();

    // Editable name (rename works in Simple mode too).
    strncpy(quadName_, quad.getName().c_str(), sizeof(quadName_) - 1);
    quadName_[sizeof(quadName_) - 1] = '\0';
    if (ImGui::InputText("Name", quadName_, sizeof(quadName_),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        ctx.es.commandHistory.execute(
            std::make_unique<RenameQuadCommand>(ctx.es.quadRef(quad), quadName_));
    }
    ImGui::Separator();

    EditorState& es = ctx.es;
    // A locked quad (the Quad List's lock) keeps its shape: no curves, no Reset Quad.
    const bool locked = quad.isLocked();

    // Flip
    if (ImGui::Button(texBinding.flipH ? "Flip U [ON]" : "Flip U")) {
        UvTransform uv = UV.get(quad);
        uv.flipH = !uv.flipH;
        setAsStep(es, quad, UV, uv);
    }
    ImGui::SameLine();
    if (ImGui::Button(texBinding.flipV ? "Flip V [ON]" : "Flip V")) {
        UvTransform uv = UV.get(quad);
        uv.flipV = !uv.flipV;
        setAsStep(es, quad, UV, uv);
    }

    // Rotation
    float deg = texBinding.uvRotation * 180.0f / kPi;
    ImGui::SetNextItemWidth(200.0f);
    editAsStep(es, quad, UV, pendingStep_, [&] {
        if (ImGui::DragFloat("Rotation", &deg, 1.0f, -360.0f, 360.0f, "%.0f deg")) {
            texBinding.uvRotation = deg * kPi / 180.0f;
        }
    });
    auto rotBtn = [&](const char* label, float d) {
        if (ImGui::Button(label)) {
            UvTransform uv = UV.get(quad);
            uv.rotation = d * kPi / 180.0f;
            setAsStep(es, quad, UV, uv);
        }
    };
    rotBtn("0##r",   0.0f);   ImGui::SameLine();
    rotBtn("90##r",  90.0f);  ImGui::SameLine();
    rotBtn("180##r", 180.0f); ImGui::SameLine();
    rotBtn("270##r", 270.0f);

    // Curved (bezier) edges — drag the amber handles on the canvas to bow the edges.
    if (locked) ImGui::TextDisabled("%s", LOCKED_SHAPE_NOTE);
    ImGui::BeginDisabled(locked);
    bool bez = quad.getGeometry().isBezierEnabled();
    if (ImGui::Checkbox("Curved edges (bezier)", &bez)) {
        setAsStep(es, quad, CURVES, Curves{bez, quad.getGeometry().getBezierOffsets()});
    }
    if (bez) {
        ImGui::SameLine();
        if (ImGui::Button("Reset Curves")) {
            setAsStep(es, quad, CURVES, Curves{});
        }
    }
    ImGui::EndDisabled();

    // Crop
    ImGui::Separator();
    renderCropControls(ctx, quad);

    // Reset Quad — full reset, keeps the assigned media (confirmed UX). One undo step.
    ImGui::Separator();
    ImGui::BeginDisabled(locked);
    if (ImGui::Button("Reset Quad")) {
        es.commandHistory.beginCompoundCommand("Reset Quad");
        setAsStep(es, quad, UV, UvTransform{});
        setAsStep(es, quad, CROP, NormalizedRect{});
        setAsStep(es, quad, CORNERS, defaultCorners(quad.getGeometry()));
        setAsStep(es, quad, CURVES, Curves{});
        es.commandHistory.endCompoundCommand();
    }
    ImGui::SetItemTooltip("Reset warp, flip, rotation and crop (keeps the assigned media)");
    ImGui::EndDisabled();
}

// ============================================================
//  Output Properties
// ============================================================
void InspectorPanel::renderOutputProperties(EditorContext& ctx) {
    EditorState& es = ctx.es;

    OutputWindow* selectedOutput = nullptr;
    if (!es.selectedOutputId.isNull() && ctx.outputManager) {
        selectedOutput =
            ctx.outputManager->getOutput(es.selectedOutputId);
    }

    if (selectedOutput) {
        // Editable name
        strncpy(outputName_, selectedOutput->getName().c_str(),
                sizeof(outputName_) - 1);
        outputName_[sizeof(outputName_) - 1] = '\0';
        if (ImGui::InputText("Name", outputName_, sizeof(outputName_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            selectedOutput->setName(outputName_);
        }

        // The scene this output shows (ADR-047); several outputs may show one scene.
        ImGui::Text("Scene:");
        const Scene* shown = OutputManager::sceneForOutput(es.scenes, *selectedOutput);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##OutputScene", shown ? shown->getName().c_str() : "(no scene)")) {
            if (ImGui::Selectable("(no scene)", shown == nullptr)) {
                OutputManager::showScene(*selectedOutput, nullptr, es.scenes);
            }
            for (const auto& scene : es.scenes) {
                if (!scene) continue;
                ImGui::PushID(scene.get());
                const bool isShown = scene.get() == shown;
                if (ImGui::Selectable(scene->getName().c_str(), isShown) && !isShown) {
                    ctx.outputManager->routeScene(*selectedOutput, scene.get(), es.scenes);
                }
                if (isShown) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();

        // Monitor dropdown
        ImGui::Text("Display:");
        const auto& monitors = ctx.outputManager->getMonitors();
        const int currentMonitor = selectedOutput->getMonitor();  // -1: none connected
        const bool automatic = selectedOutput->isMonitorAutomatic();
        const std::string monitorPreview =
            OutputManager::describeMonitorChoice(
                monitors, *selectedOutput,
                OutputManager::onlyScreenPreview(monitors, ctx.outputManager->getOutputs()) ==
                    selectedOutput);

        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##Monitor", monitorPreview.c_str())) {
            if (ImGui::Selectable("Automatic (a projector, or this screen if it is the only one)", automatic)) {
                ctx.outputManager->assignMonitor(*selectedOutput, -1);
            }
            ImGui::Separator();
            for (size_t i = 0; i < monitors.size(); ++i) {
                const auto& mon = monitors[i];
                const std::string label = mon.getDisplayString();
                const bool isSelected = !automatic && static_cast<int>(i) == currentMonitor;
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    ctx.outputManager->assignMonitor(*selectedOutput, static_cast<int>(i));
                    selectedOutput->setResolution(mon.resolution);
                    if (selectedOutput->isCreated()) {
                        selectedOutput->applyResolution();
                    }
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        // Resolution dropdown
        ImGui::Text("Resolution:");
        IVec2 currentRes = selectedOutput->getResolution();
        std::string resPreview =
            std::to_string(currentRes.x) + " x " + std::to_string(currentRes.y);

        auto videoModes = ctx.outputManager->getMonitorVideoModes(currentMonitor);

        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##Resolution", resPreview.c_str())) {
            for (const auto& mode : videoModes) {
                std::string label =
                    std::to_string(mode.width) + " x " + std::to_string(mode.height);
                bool isSelected =
                    (mode.width == currentRes.x && mode.height == currentRes.y);
                if (ImGui::Selectable(label.c_str(), isSelected)) {
                    selectedOutput->setResolution(IVec2(mode.width, mode.height));
                    if (selectedOutput->isCreated()) {
                        selectedOutput->applyResolution();
                    }
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        // Rotation dropdown
        ImGui::Text("Rotation:");
        int rotationDeg = selectedOutput->getRotationDegrees();
        const char* rotLabels[] = {
            "0 degrees", "90 degrees", "180 degrees", "270 degrees"
        };
        int rotIndex = rotationDeg / 90;
        if (rotIndex < 0 || rotIndex > 3) rotIndex = 0;

        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##Rotation", rotLabels[rotIndex])) {
            for (int i = 0; i < 4; ++i) {
                bool isSelected = (i == rotIndex);
                if (ImGui::Selectable(rotLabels[i], isSelected)) {
                    selectedOutput->setRotationDegrees(i * 90);
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        // Slices of the scene's canvas (ADR-047 part B)
        ImGui::Separator();
        sliceEditor_.render(ctx, *selectedOutput);

        // Projector calibration (ADR-052 §6), however the output is sliced (TD-29).
        ImGui::Separator();
        const bool calibratingThis = es.calibratingOutputId == selectedOutput->getId();
        ImGui::BeginDisabled(calibratingThis || es.aiScan.running);  // a scan owns Esc
        if (ImGui::Button("Calibrate Projector...")) {
            es.beginCalibration(*selectedOutput);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Pair points on the 3D objects with projector pixels to solve "
                              "this projector's lens and position");
        ImGui::SameLine();
        ImGui::TextDisabled(selectedOutput->getProjector().calibrated ? "calibrated" : "not calibrated");

        // Enabled checkbox
        ImGui::Separator();
        bool enabled = selectedOutput->isEnabled();
        if (ImGui::Checkbox("Enabled", &enabled)) {
            selectedOutput->setEnabled(enabled);
        }

        // State info
        ImGui::Separator();
        ImGui::TextDisabled("Status:");
        ImGui::Text("Window: %s",
                    selectedOutput->isCreated() ? "Created" : "Not Created");
        ImGui::Text("Fullscreen: %s",
                    selectedOutput->isFullscreen() ? "Yes" : "No");

        // Window control buttons
        ImGui::Separator();
        if (selectedOutput->isCreated()) {
            if (selectedOutput->isVisible()) {
                if (ImGui::Button("Hide Window")) {
                    selectedOutput->hide();
                }
            } else {
                if (ImGui::Button("Show Window")) {
                    selectedOutput->show();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Destroy Window")) {
                selectedOutput->destroy();
            }
        } else {
            if (ImGui::Button("Create Window")) {
                selectedOutput->create(ctx.outputManager->getMainWindow());
            }
        }
    } else {
        ImGui::TextDisabled("No output selected");
        ImGui::TextDisabled("Select an output from the Outputs tab");
    }
}

// ============================================================
//  Media Properties
// ============================================================
void InspectorPanel::renderMediaProperties(EditorContext& ctx) {
    EditorState& es = ctx.es;

    MediaItem* selectedMedia = nullptr;
    if (!es.selectedMediaId.isNull() && ctx.mediaLibrary) {
        selectedMedia = ctx.mediaLibrary->getItem(es.selectedMediaId);
    }

    if (selectedMedia) {
        // Editable name
        strncpy(mediaName_, selectedMedia->getName().c_str(),
                sizeof(mediaName_) - 1);
        mediaName_[sizeof(mediaName_) - 1] = '\0';
        if (ImGui::InputText("Name", mediaName_, sizeof(mediaName_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            selectedMedia->setName(mediaName_);
        }

        ImGui::Separator();

        // Type
        const char* typeStr = "Unknown";
        switch (selectedMedia->getType()) {
            case MediaType::IMAGE:      typeStr = "Image";      break;
            case MediaType::VIDEO:      typeStr = "Video";      break;
            case MediaType::AUDIO:      typeStr = "Audio";      break;
            case MediaType::PROCEDURAL: typeStr = "Procedural"; break;
        }
        ImGui::Text("Type: %s", typeStr);

        // Path (for file-based media)
        if (selectedMedia->hasPath()) {
            ImGui::Separator();
            ImGui::Text("File:");
            ImGui::TextWrapped("%s", utf8FromPath(selectedMedia->getPath()).c_str());
        }

        // Procedural source parameters
        if (selectedMedia->getType() == MediaType::PROCEDURAL &&
            selectedMedia->hasSource()) {
            ImGui::Separator();
            const char* procTypeStr = "Unknown";
            switch (selectedMedia->getProceduralType()) {
                case ProceduralType::SOLID_COLOR:  procTypeStr = "Solid Color";  break;
                case ProceduralType::GRADIENT:     procTypeStr = "Gradient";     break;
                case ProceduralType::CHECKERBOARD: procTypeStr = "Checkerboard"; break;
                case ProceduralType::TEST_PATTERN: procTypeStr = "Test Pattern"; break;
                case ProceduralType::NOISE:        procTypeStr = "Noise";        break;
                case ProceduralType::PLASMA:       procTypeStr = "Plasma";       break;
                case ProceduralType::GRID:         procTypeStr = "Grid";         break;
            }
            ImGui::Text("Procedural Type: %s", procTypeStr);

            // Get the procedural source and show editable parameters
            TextureSource* source =
                ctx.sourceManager->getSource(selectedMedia->getSourceId());
            ProceduralSource* procSource =
                dynamic_cast<ProceduralSource*>(source);
            if (procSource) {
                ImGui::Separator();
                ImGui::Text("Parameters:");
                auto params = procSource->getParameters();
                for (const auto& param : params) {
                    ImGui::PushID(param.name.c_str());
                    switch (param.type) {
                        case SourceParameter::Type::Float: {
                            float val = std::get<float>(param.value);
                            if (ImGui::SliderFloat(param.displayName.c_str(),
                                                   &val,
                                                   param.minValue, param.maxValue)) {
                                procSource->setParameter(param.name, val);
                            }
                            break;
                        }
                        case SourceParameter::Type::Int: {
                            int val = std::get<int>(param.value);
                            if (ImGui::SliderInt(param.displayName.c_str(),
                                                 &val,
                                                 (int)param.minValue,
                                                 (int)param.maxValue)) {
                                procSource->setParameter(param.name, val);
                            }
                            break;
                        }
                        case SourceParameter::Type::Bool: {
                            bool val = std::get<bool>(param.value);
                            if (ImGui::Checkbox(param.displayName.c_str(), &val)) {
                                procSource->setParameter(param.name, val);
                            }
                            break;
                        }
                        case SourceParameter::Type::Color: {
                            Color col = std::get<Color>(param.value);
                            float colArr[4] = { col.r, col.g, col.b, col.a };
                            if (ImGui::ColorEdit4(
                                    param.displayName.c_str(), colArr,
                                    ImGuiColorEditFlags_NoInputs |
                                    ImGuiColorEditFlags_AlphaBar)) {
                                procSource->setParameter(
                                    param.name,
                                    Color(colArr[0], colArr[1],
                                          colArr[2], colArr[3]));
                            }
                            break;
                        }
                    }
                    ImGui::PopID();
                }

                // Reset button
                if (ImGui::Button("Reset to Defaults")) {
                    procSource->resetParameters();
                }

                // Show dimensions
                ImGui::Separator();
                ImGui::Text("Size: %dx%d",
                            procSource->getWidth(), procSource->getHeight());
            }
        }
    } else {
        ImGui::TextDisabled("No media selected");
        ImGui::TextDisabled("Select media from the Media tab");
    }
}

// ============================================================
//  3D Object Properties (ADR-052)
// ============================================================
namespace {

constexpr float kDegPerRad = 57.2957795f;

// model = translate(position) * Rx * Ry * Rz (degrees) * uniform scale.
glm::mat4 composeObjectTransform(const Vec3& position, const Vec3& rotationDeg, float scale) {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, rotationDeg.x / kDegPerRad, Vec3(1, 0, 0));
    m = glm::rotate(m, rotationDeg.y / kDegPerRad, Vec3(0, 1, 0));
    m = glm::rotate(m, rotationDeg.z / kDegPerRad, Vec3(0, 0, 1));
    return glm::scale(m, Vec3(scale));
}

// The inverse of composeObjectTransform for a rotation + uniform scale + translation
// (a sheared or non-uniformly scaled matrix is read by its first axis's length).
void decomposeObjectTransform(const glm::mat4& m, Vec3& position, Vec3& rotationDeg, float& scale) {
    position = Vec3(m[3]);
    scale = glm::length(Vec3(m[0]));
    if (scale < 1e-6f) scale = 1.0f;
    const glm::mat3 r = glm::mat3(m) / scale;  // r[col][row]
    // R = Rx(a) Ry(b) Rz(c): R(0,2) = sin b, R(1,2) = -sin a cos b, R(2,2) = cos a cos b,
    // R(0,1) = -cos b sin c, R(0,0) = cos b cos c.
    const float b = std::atan2(r[2][0], std::sqrt(r[2][1] * r[2][1] + r[2][2] * r[2][2]));
    float a, c;
    if (std::cos(b) > 1e-5f) {
        a = std::atan2(-r[2][1], r[2][2]);
        c = std::atan2(-r[1][0], r[0][0]);
    } else {  // gimbal lock: fold the whole remaining rotation into x
        a = std::atan2(r[1][2], r[1][1]);
        c = 0.0f;
    }
    rotationDeg = Vec3(a, b, c) * kDegPerRad;
}

}  // namespace

void InspectorPanel::renderObjectProperties(EditorContext& ctx) {
    EditorState& es = ctx.es;
    MeshObject* object = es.selectedObjectId.isNull()
                             ? nullptr : es.activeScene().getObject(es.selectedObjectId);
    if (!object) {
        ImGui::TextDisabled("No object selected");
        ImGui::TextDisabled("Select an object in the Quad List, or use");
        ImGui::TextDisabled("File > Import 3D Object (OBJ)...");
        return;
    }

    snprintf(objectName_, sizeof(objectName_), "%s", object->getName().c_str());
    if (ImGui::InputText("Name", objectName_, sizeof(objectName_),
                         ImGuiInputTextFlags_EnterReturnsTrue) &&
        object->getName() != objectName_) {
        es.commandHistory.execute(makeRenameObjectCommand(es.objectRef(*object), objectName_));
    }

    if (isMeshLoading(es, *object)) {
        ImGui::TextDisabled("Loading mesh...");
    } else if (object->getMesh().empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Mesh not loaded");
    } else {
        ImGui::Text("Mesh: %zu triangles", object->getMesh().triangleCount());
    }
    if (!object->getSourcePath().empty()) {
        ImGui::TextWrapped("File: %s", object->getSourcePath().c_str());
    }

    bool objectVisible = object->isVisible();
    if (ImGui::Checkbox("Visible", &objectVisible)) {
        es.commandHistory.execute(makeSetObjectVisibilityCommand(es.objectRef(*object), objectVisible));
    }
    float opacity = object->getOpacity();
    if (ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f, "%.2f")) {
        es.commandHistory.execute(makeSetObjectOpacityCommand(es.objectRef(*object), opacity));
    }

    // Transform, shown as position / rotation / uniform scale. The fields are re-read
    // from the matrix only when the object or its matrix changed elsewhere, so editing
    // one field never re-derives (and nudges) the others.
    ImGui::Separator();
    ImGui::Text("Transform:");
    if (transformObjectId_ != object->getId() || transformShown_ != object->getTransform()) {
        decomposeObjectTransform(object->getTransform(), objectPosition_, objectRotationDeg_,
                                 objectScale_);
        transformObjectId_ = object->getId();
        transformShown_ = object->getTransform();
    }
    bool changed = false;
    changed |= ImGui::DragFloat3("Position", &objectPosition_.x, 0.01f, 0.0f, 0.0f, "%.3f");
    changed |= ImGui::DragFloat3("Rotation (deg)", &objectRotationDeg_.x, 0.5f, -360.0f, 360.0f, "%.1f");
    changed |= ImGui::DragFloat("Scale", &objectScale_, 0.01f, 0.001f, 1000.0f, "%.3f",
                                ImGuiSliderFlags_AlwaysClamp);
    bool reset = false;
    if (ImGui::Button("Reset Transform")) {
        objectPosition_ = Vec3(0.0f);
        objectRotationDeg_ = Vec3(0.0f);
        objectScale_ = 1.0f;
        reset = true;
    }
    if (changed || reset) {
        // A drag is one undo step (merged); Reset is a step of its own.
        transformShown_ = composeObjectTransform(objectPosition_, objectRotationDeg_, objectScale_);
        es.commandHistory.execute(
            makeSetObjectTransformCommand(es.objectRef(*object), transformShown_, !reset));
    }

    // Media: the same bin items quads can be given (audio cannot texture anything).
    ImGui::Separator();
    ImGui::Text("Media:");
    const UUID& boundId = object->getTexture().mediaId;
    const std::filesystem::path boundPath =
        (ctx.sourceManager && !boundId.isNull()) ? ctx.sourceManager->getSourcePath(boundId)
                                                 : std::filesystem::path();
    auto isBound = [&](const MediaItem& item) {
        if (boundId.isNull()) return false;
        if (item.hasSource()) return item.getSourceId() == boundId;
        return item.hasPath() && !boundPath.empty() && item.getPath() == boundPath;
    };
    std::string preview = boundId.isNull() ? "None" : "(assigned)";
    if (ctx.mediaLibrary) {
        for (const auto& item : ctx.mediaLibrary->getItems()) {
            if (item && isBound(*item)) { preview = item->getName(); break; }
        }
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##ObjectMedia", preview.c_str())) {
        if (ImGui::Selectable("None", boundId.isNull()) && !boundId.isNull()) {
            TextureBinding unbound = object->getTexture();
            unbound.mediaId = UUID();
            es.commandHistory.execute(makeSetObjectTextureCommand(es.objectRef(*object), unbound));
        }
        if (ctx.mediaLibrary) {
            for (const auto& item : ctx.mediaLibrary->getItems()) {
                if (!item || item->getType() == MediaType::AUDIO) continue;
                ImGui::PushID(item->getId().toString().c_str());
                if (ImGui::Selectable(item->getName().c_str(), isBound(*item))) {
                    assignMediaToObject(es, ctx.sourceManager, es.objectRef(*object), *item);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndCombo();
    }
}

} // namespace quadmapper
