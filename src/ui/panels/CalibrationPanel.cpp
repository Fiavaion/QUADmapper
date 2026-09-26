#include "CalibrationPanel.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "app/TextureResolve.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "project/Project.hpp"
#include "renderer/Framebuffer.hpp"
#include "renderer/Renderer.hpp"
#include "scene/CalibrationSession.hpp"
#include "scene/MeshObject.hpp"
#include "scene/RayPick.hpp"
#include "ui/Theme.hpp"
#include "scene/Scene.hpp"

#include <glad/glad.h>
#include <imgui.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace quadmapper {

namespace {

constexpr float ERROR_WARN_PX = 3.0f;     // per-point error shown red above this
constexpr float SNAP_RADIUS_PX = 10.0f;   // model-view vertex snapping
constexpr float ORBIT_DEG_PER_PX = 0.4f;

const ImVec4 ERROR_COLOR(1.0f, 0.35f, 0.3f, 1.0f);
const ImVec4 HINT_COLOR(1.0f, 0.75f, 0.3f, 1.0f);

// Union of the visible objects' world bounds. False when there is nothing to frame.
bool objectBounds(const Scene* scene, Vec3& boxMin, Vec3& boxMax) {
    boxMin = Vec3(1e30f);
    boxMax = Vec3(-1e30f);
    if (!scene) return false;
    for (const auto& object : scene->getObjects()) {
        if (!object || !object->isVisible() || object->getMesh().positions.empty()) continue;
        Vec3 lo, hi;
        object->worldBounds(lo, hi);
        boxMin = glm::min(boxMin, lo);
        boxMax = glm::max(boxMax, hi);
    }
    return boxMin.x <= boxMax.x;
}

// A numbered point marker: orange when selected, grey when disabled.
void drawPointMarker(ImDrawList* dl, const ImVec2& at, int number, bool selected, bool enabled) {
    const ImU32 fill = selected ? IM_COL32(255, 170, 40, 255)
                     : enabled  ? IM_COL32(60, 200, 255, 230)
                                : IM_COL32(140, 140, 140, 200);
    const float radius = selected ? 7.0f : 5.5f;
    dl->AddCircleFilled(at, radius, fill);
    dl->AddCircle(at, radius, IM_COL32(0, 0, 0, 220), 0, 1.5f);
    if (selected) dl->AddCircle(at, radius + 4.0f, fill, 0, 1.5f);
    char label[16];
    std::snprintf(label, sizeof(label), "%d", number);
    const ImVec2 textAt(at.x + radius + 3.0f, at.y - radius - 12.0f);
    dl->AddText(ImVec2(textAt.x + 1.0f, textAt.y + 1.0f), IM_COL32(0, 0, 0, 255), label);
    dl->AddText(textAt, fill, label);
}

void drawCross(ImDrawList* dl, const ImVec2& at, float half, ImU32 color) {
    dl->AddLine(ImVec2(at.x - half, at.y - half), ImVec2(at.x + half, at.y + half), color, 1.5f);
    dl->AddLine(ImVec2(at.x - half, at.y + half), ImVec2(at.x + half, at.y - half), color, 1.5f);
}

OutputWindow* calibratingOutput(EditorContext& ctx) {
    if (ctx.es.calibratingOutputId.isNull() || !ctx.outputManager) return nullptr;
    return ctx.outputManager->getOutput(ctx.es.calibratingOutputId);
}

}  // namespace

CalibrationPanel::CalibrationPanel() = default;
CalibrationPanel::~CalibrationPanel() = default;

void CalibrationPanel::releaseGlResources() { modelView_.reset(); }

void CalibrationPanel::render(EditorContext& ctx) {
    EditorState& es = ctx.es;
    if (es.calibratingOutputId.isNull()) {
        shownOutputId_ = UUID::null();
        return;
    }
    OutputWindow* output = calibratingOutput(ctx);
    if (!output) {  // the output was removed (or a project load replaced it)
        es.endCalibration();
        return;
    }
    CalibrationSession& cal = es.calibration;
    auto& points = output->getCalibrationPoints();
    ProjectorCamera& projector = output->getProjector();
    const Scene* scene = OutputManager::sceneForOutput(es.scenes, output->getId());
    auto setModified = [&]() { if (ctx.project) ctx.project->setModified(true); };

    if (cal.selected >= static_cast<int>(points.size())) cal.selected = -1;
    if (shownOutputId_ != es.calibratingOutputId) {
        ImGui::SetNextWindowFocus();
        shownOutputId_ = es.calibratingOutputId;
        orbitDistance_ = 0.0f;
    }

    bool open = true;
    if (ImGui::Begin("Calibration", &open)) {
        ImGui::Text("Projector: %s", output->getName().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Done")) open = false;
        ImGui::SetItemTooltip("Leave calibration (Esc)");
        const bool hasObjects = scene && !scene->getObjects().empty();
        if (!scene) {
            ImGui::TextColored(ERROR_COLOR, "No scene is routed to this output.");
            ImGui::TextDisabled("Assign one in Resources > Scenes.");
        } else if (!hasObjects) {
            ImGui::TextDisabled("No 3D objects: File > Import 3D Object (OBJ)...");
        }
        if (scene && &es.activeScene() != scene) {
            // Points are placed on the canvas, which shows the active scene's projector.
            ImGui::TextColored(HINT_COLOR, "The canvas shows another projector.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Show this one")) es.setActiveScene(scene->getId());
        }

        // ---- Lens ----
        ImGui::SeparatorText("Lens");
        int mode = static_cast<int>(cal.mode);
        bool modeChanged = ImGui::RadioButton("Auto", &mode, static_cast<int>(CalibrationMode::Auto));
        ImGui::SetItemTooltip("Solve the lens once 6+ points are not on one plane");
        ImGui::SameLine();
        modeChanged |= ImGui::RadioButton("Keep lens", &mode, static_cast<int>(CalibrationMode::PoseOnly));
        ImGui::SetItemTooltip("Use the throw ratio below and solve only the position (4+ points)");
        modeChanged |= ImGui::RadioButton("Solve lens (6+ points at different depths)", &mode,
                                          static_cast<int>(CalibrationMode::Full));
        if (modeChanged) {
            cal.mode = static_cast<CalibrationMode>(mode);
            cal.markChanged();
            setModified();
        }

        const ProjectorIntrinsics& lens = cal.lens;
        float throwRatio = lens.width > 0 ? lens.fx / static_cast<float>(lens.width) : 1.5f;
        float lensShift = lens.height > 0 ? lens.cy / static_cast<float>(lens.height) - 0.5f : 0.0f;
        ImGui::SetNextItemWidth(120.0f);
        bool lensChanged = ImGui::InputFloat("Throw ratio", &throwRatio, 0.01f, 0.1f, "%.3f");
        ImGui::SetItemTooltip("Throw distance / image width, from the projector's spec sheet");
        ImGui::SetNextItemWidth(120.0f);
        lensChanged |= ImGui::InputFloat("Lens shift", &lensShift, 0.01f, 0.1f, "%.3f");
        ImGui::SetItemTooltip("Vertical shift as a fraction of the image height (0 = centred)");
        if (lensChanged) {
            cal.lens = ProjectorIntrinsics::fromThrowRatio(std::max(throwRatio, 0.05f), lens.width,
                                                           lens.height, lensShift);
            if (!projector.calibrated) projector.intrinsics = cal.lens;
            cal.markChanged();
            setModified();
        }
        ImGui::TextDisabled("fx %.0f  fy %.0f  cx %.0f  cy %.0f  (%dx%d)", cal.lens.fx, cal.lens.fy,
                            cal.lens.cx, cal.lens.cy, cal.lens.width, cal.lens.height);

        // ---- Points ----
        ImGui::SeparatorText("Points");
        ImGui::BeginDisabled(!hasObjects);
        if (cal.pick == CalibrationSession::Pick::None) {
            if (ImGui::Button("Add Point")) cal.pick = CalibrationSession::Pick::World;
            ImGui::SetItemTooltip("Click the model, then click the canvas where that point must land");
        } else if (ImGui::Button("Cancel Pick")) {
            cal.cancelPick();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(points.empty());
        if (ImGui::Button("Clear All")) {
            points.clear();
            cal.selected = -1;
            cal.cancelPick();
            cal.markChanged();
            setModified();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        Vec3 boxMin, boxMax;
        const bool haveBounds = objectBounds(scene, boxMin, boxMax);
        ImGui::BeginDisabled(!haveBounds);
        if (ImGui::Button("Reset view")) {
            // Back to the uncalibrated default aimed at the objects. No re-solve, or the
            // points would recalibrate it straight away.
            projector = ProjectorCamera::framing(boxMin, boxMax, cal.lens);
            cal.solved = false;
            cal.statusIsError = false;
            cal.status = "View reset: edit a point to solve again";
            setModified();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Aim the uncalibrated projector at the objects with this lens");

        if (cal.pick == CalibrationSession::Pick::World) {
            ImGui::TextColored(HINT_COLOR, "Click the model below to pick point %d (Esc cancels)",
                               static_cast<int>(points.size()) + 1);
        } else if (cal.pick == CalibrationSession::Pick::Pixel) {
            ImGui::TextColored(HINT_COLOR, "Click the canvas where point %d must land (Esc cancels)",
                               static_cast<int>(points.size()) + 1);
        }

        int deleteIndex = -1;
        const float rowHeight = ImGui::GetFrameHeightWithSpacing();
        const float tableHeight =
            rowHeight * static_cast<float>(std::min<size_t>(points.size(), 7) + 1) + 4.0f;
        const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
        if (!points.empty() &&
            ImGui::BeginTable("##CalibrationPoints", 6, tableFlags, ImVec2(0.0f, tableHeight))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("World xyz", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Pixel uv");
            ImGui::TableSetupColumn("Error px");
            ImGui::TableSetupColumn("On");
            ImGui::TableSetupColumn("##Delete");
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(points.size()); ++i) {
                CalibrationPoint& p = points[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(std::to_string(i + 1).c_str(), cal.selected == i,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap)) {
                    cal.selected = i;
                }
                ImGui::TableNextColumn();
                ImGui::Text("%.3f, %.3f, %.3f", p.world.x, p.world.y, p.world.z);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f, %.1f", p.pixel.x, p.pixel.y);
                ImGui::TableNextColumn();
                const float error = reprojectionError(p, projector);
                if (error < 0.0f) {
                    ImGui::TextColored(ERROR_COLOR, "behind");
                } else if (error > ERROR_WARN_PX) {
                    ImGui::TextColored(ERROR_COLOR, "%.2f", error);
                } else {
                    ImGui::Text("%.2f", error);
                }
                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##On", &p.enabled)) {
                    cal.markChanged();
                    setModified();
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("X")) deleteIndex = i;
                ImGui::SetItemTooltip("Delete point %d", i + 1);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (deleteIndex >= 0) {
            cal.removePoint(points, deleteIndex);
            setModified();
        }
        if (!points.empty()) {
            ImGui::TextDisabled("Tab/Shift+Tab: select   Arrows: nudge (Shift 10x, Alt 0.1x)");
        }

        // ---- Status ----
        if (cal.statusIsError) {
            ImGui::PushStyleColor(ImGuiCol_Text, ERROR_COLOR);
            ImGui::TextWrapped("%s", cal.status.c_str());
            ImGui::PopStyleColor();
        } else if (cal.solved) {
            ImGui::TextWrapped("%s | RMS %.2f px | %d points", cal.status.c_str(), cal.rmsError,
                               cal.pointsUsed);
            if (!cal.warning.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Theme::Colors::Warning));
                ImGui::TextWrapped("%s", cal.warning.c_str());
                ImGui::PopStyleColor();
            }
        } else if (!cal.status.empty()) {
            ImGui::TextDisabled("%s", cal.status.c_str());
        }

        // ---- Model view ----
        ImGui::SeparatorText("Model");
        ImGui::Checkbox("Snap to vertex", &snapToVertex_);
        ImGui::SameLine();
        if (ImGui::SmallButton("Frame")) {
            orbitDistance_ = 0.0f;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("drag: orbit  wheel: zoom");

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 size(std::max(avail.x, 16.0f), std::max(avail.y, 160.0f));
        const int width = static_cast<int>(size.x);
        const int height = static_cast<int>(size.y);

        const Vec3 target = haveBounds ? 0.5f * (boxMin + boxMax) : Vec3(0.0f);
        const float radius = haveBounds ? std::max(0.5f * glm::length(boxMax - boxMin), 1e-3f) : 1.0f;
        if (orbitDistance_ <= 0.0f) orbitDistance_ = 3.0f * radius;
        const ProjectorCamera orbit =
            orbitCamera(target, orbitYaw_, orbitPitch_, orbitDistance_, width, height);

        if (!modelView_) modelView_ = std::make_unique<Framebuffer>();
        const bool viewReady = ctx.renderer && modelView_->resize(width, height);
        if (viewReady) {
            GLint prevFbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
            GLint prevViewport[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VIEWPORT, prevViewport);
            GLfloat prevClear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            glGetFloatv(GL_COLOR_CLEAR_VALUE, prevClear);
            modelView_->bind();
            glClearColor(0.11f, 0.11f, 0.13f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (scene) {
                ctx.renderer->renderMeshObjects(scene->getObjects(), orbit,
                                                buildQuadTextureMap(es, ctx.sourceManager, SurfaceView::Editor));
                ctx.renderer->endFrame();  // unbind what the draws left bound
            }
            glClearColor(prevClear[0], prevClear[1], prevClear[2], prevClear[3]);
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
            glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        }

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);
        ImGui::InvisibleButton("##ModelView", size);
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);  // the wheel zooms, not scrolls
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemActivated()) orbitDragged_ = false;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            orbitDragged_ = true;
            orbitYaw_ -= io.MouseDelta.x * ORBIT_DEG_PER_PX;
            orbitPitch_ = std::clamp(orbitPitch_ + io.MouseDelta.y * ORBIT_DEG_PER_PX, -89.0f, 89.0f);
        }
        if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
            orbitDistance_ = std::clamp(orbitDistance_ * std::pow(0.88f, io.MouseWheel),
                                        0.05f * radius, 100.0f * radius);
        }
        // A click (press and release without orbiting) picks the 3D point.
        if (ImGui::IsItemDeactivated() && !orbitDragged_ && scene &&
            cal.pick == CalibrationSession::Pick::World) {
            const Vec2 pixel(io.MousePos.x - p0.x, io.MousePos.y - p0.y);
            Vec3 world(0.0f);
            bool found = snapToVertex_ &&
                         snapToVertex(scene->getObjects(), orbit, pixel, SNAP_RADIUS_PX, world);
            MeshHit hit;
            if (!found && pickObjects(scene->getObjects(), pixelRay(orbit, pixel), hit)) {
                world = hit.point;
                found = true;
            }
            if (found) {
                cal.pendingWorld = world;
                cal.pick = CalibrationSession::Pick::Pixel;
            }
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (viewReady) {
            // The FBO fills bottom-up: flip V.
            dl->AddImage(static_cast<ImTextureID>(static_cast<uintptr_t>(modelView_->colorTexture())),
                         p0, p1, ImVec2(0, 1), ImVec2(1, 0));
        }
        dl->PushClipRect(p0, p1, true);
        for (int i = 0; i < static_cast<int>(points.size()); ++i) {
            Vec2 at;
            if (orbit.project(points[i].world, at)) {
                drawPointMarker(dl, ImVec2(p0.x + at.x, p0.y + at.y), i + 1, cal.selected == i,
                                points[i].enabled);
            }
        }
        Vec2 pending;
        if (cal.pick == CalibrationSession::Pick::Pixel && orbit.project(cal.pendingWorld, pending)) {
            drawCross(dl, ImVec2(p0.x + pending.x, p0.y + pending.y), 6.0f, IM_COL32(255, 170, 40, 255));
        }
        dl->PopClipRect();
    }
    ImGui::End();

    if (!open) {
        es.endCalibration();
        return;
    }
    cal.update(points, projector);
}

void CalibrationPanel::drawCanvasOverlay(EditorContext& ctx) {
    EditorState& es = ctx.es;
    OutputWindow* output = calibratingOutput(ctx);
    // Only while the canvas shows the projector being calibrated.
    if (!output || es.activeScene().getAssignedOutputId() != output->getId()) return;
    if (es.canvasRegionSize.x < 1.0f || es.canvasRegionSize.y < 1.0f) return;

    const CalibrationSession& cal = es.calibration;
    const auto& points = output->getCalibrationPoints();
    const ProjectorCamera& projector = output->getProjector();
    const ProjectorIntrinsics& k = projector.intrinsics;

    const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
    const ImVec2 regionPos(es.canvasRegionPos.x + vpPos.x, es.canvasRegionPos.y + vpPos.y);
    const ImVec2 regionEnd(regionPos.x + es.canvasRegionSize.x, regionPos.y + es.canvasRegionSize.y);
    CanvasView view;
    view.scale = es.lastViewScale;
    view.offset = es.lastViewOffset;
    auto toScreen = [&](const Vec2& pixel) {
        const Vec2 r = view.toRegion(pixelToCanvas(pixel, k));
        return ImVec2(regionPos.x + r.x * es.canvasRegionSize.x,
                      regionPos.y + r.y * es.canvasRegionSize.y);
    };

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(regionPos, regionEnd, false);
    for (int i = 0; i < static_cast<int>(points.size()); ++i) {
        const CalibrationPoint& p = points[i];
        const ImVec2 target = toScreen(p.pixel);
        Vec2 landed;
        if (projector.project(p.world, landed)) {
            const bool bad = glm::length(landed - p.pixel) > ERROR_WARN_PX;
            const ImU32 color = bad ? IM_COL32(255, 90, 80, 230) : IM_COL32(120, 230, 120, 230);
            const ImVec2 at = toScreen(landed);
            dl->AddLine(target, at, color, 1.5f);
            drawCross(dl, at, 4.0f, color);
        }
        drawPointMarker(dl, target, i + 1, cal.selected == i, p.enabled);
    }
    if (cal.pick == CalibrationSession::Pick::Pixel) {
        const char* hint = "Click where the picked point must land (Esc cancels)";
        const ImVec2 textSize = ImGui::CalcTextSize(hint);
        const ImVec2 at(0.5f * (regionPos.x + regionEnd.x - textSize.x), regionPos.y + 12.0f);
        dl->AddRectFilled(ImVec2(at.x - 6.0f, at.y - 3.0f),
                          ImVec2(at.x + textSize.x + 6.0f, at.y + textSize.y + 3.0f),
                          IM_COL32(0, 0, 0, 170), 4.0f);
        dl->AddText(at, ImGui::GetColorU32(HINT_COLOR), hint);
    }
    dl->PopClipRect();
}

}  // namespace quadmapper
