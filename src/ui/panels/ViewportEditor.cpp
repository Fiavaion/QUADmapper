#include "ViewportEditor.hpp"

#include "app/EditorContext.hpp"
#include "app/EditorState.hpp"
#include "app/TextureResolve.hpp"
#include "app/GridUndo.hpp"
#include "app/MeshEditKeys.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "scene/QuadGeometry.hpp"
#include "scene/Commands.hpp"
#include "scene/Scene.hpp"
#include "scene/CalibrationSession.hpp"
#include "renderer/Renderer.hpp"
#include "renderer/Texture.hpp"
#include "output/OutputManager.hpp"
#include "output/OutputWindow.hpp"
#include "project/Project.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "input/VertexKeyboard.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <glm/glm.hpp>

#include <memory>
#include <array>
#include <unordered_map>

namespace quadmapper {

namespace {

// Calibration mode on the canvas (ADR-052 §6): place, drag and nudge the output's
// calibration pixels with the same press rule and keyboard model as quad vertices.
// Pixels are canvas-normalised positions times the projector image size.
void editCalibrationPoints(EditorState& es, OutputWindow& output, const Vec2& mouseNorm,
                           bool mouseDown, bool canvasPress, Project* project) {
    CalibrationSession& cal = es.calibration;
    auto& points = output.getCalibrationPoints();
    const ProjectorIntrinsics& k = output.getProjector().intrinsics;
    const int count = static_cast<int>(points.size());
    if (cal.selected >= count) cal.selected = -1;
    if (cal.dragged >= count) cal.dragged = -1;
    const Vec2 mousePixel = canvasToPixel(mouseNorm, k);
    auto changed = [&]() {
        cal.markChanged();
        if (project) project->setModified(true);
    };

    // Second half of Add Point: this press places the picked 3D point's pixel.
    if (cal.pick == CalibrationSession::Pick::Pixel) {
        if (canvasPress) {
            CalibrationPoint point;
            point.world = cal.pendingWorld;
            point.pixel = mousePixel;
            points.push_back(point);
            cal.selected = static_cast<int>(points.size()) - 1;
            cal.pick = CalibrationSession::Pick::None;
            changed();
        }
        return;
    }

    constexpr float HIT_RADIUS = 0.015f;  // as for quad corners, in canvas units
    if (cal.dragged >= 0) {
        if (mouseDown) {
            const Vec2 pixel = cal.dragStartPixel + (mousePixel - cal.dragStartMousePixel);
            if (pixel != points[cal.dragged].pixel) {
                points[cal.dragged].pixel = pixel;
                changed();
            }
        } else {
            cal.dragged = -1;
        }
    } else if (canvasPress) {
        int hit = -1;
        float closest = HIT_RADIUS;
        for (int i = 0; i < count; ++i) {
            const float d = glm::length(mouseNorm - pixelToCanvas(points[i].pixel, k));
            if (d < closest) {
                closest = d;
                hit = i;
            }
        }
        if (hit >= 0) {
            cal.selected = cal.dragged = hit;
            cal.dragStartMousePixel = mousePixel;
            cal.dragStartPixel = points[hit].pixel;
        }
    }

    // Keyboard: Tab / Shift+Tab cycles the points, arrows nudge the selected pixel.
    const ImGuiIO& io = ImGui::GetIO();
    if (canvasOwnsEditingKeys(count > 0)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
            cal.selected = cycleVertex(cal.selected, count, io.KeyShift);
        }
        if (cal.selected >= 0 && cal.selected < count) {
            const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
            const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
            const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
            const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
            if (up || down || left || right) {
                const Vec2 delta = nudgeDelta(up, down, left, right,
                                              nudgeStep(io.KeyShift, io.KeyAlt));
                points[cal.selected].pixel += Vec2(delta.x * static_cast<float>(k.width),
                                                   delta.y * static_cast<float>(k.height));
                changed();
            }
        }
    }
}

}  // namespace

void ViewportEditor::render(EditorContext& ctx) {
    // Same-named aliases keep the moved render()/interaction body verbatim
    // (LESSON-ARCH-006). All editing state lives in EditorState; the manager
    // pointers come from the per-frame EditorContext.
    EditorState& es = ctx.es;
    auto* m_renderer = ctx.renderer;
    auto* m_window = ctx.window;
    auto* m_project = ctx.project;
    auto* m_outputManager = ctx.outputManager;

    auto& quadManager = es.quadManager();

    // ---- View transform (ADR-041) -----------------------------------------
    // Render the [0,1] canvas into the dockspace central node, letterboxed to the
    // projector's aspect, scaled by zoom and translated by pan. Render AND the
    // vertex hit-test below read the same (scale, offset) so handles stay under the
    // cursor (the success-metric UX). All of this is identity-equivalent for the
    // output frames, which never touch the renderer transform.
    int fbWidth = 0, fbHeight = 0;
    glfwGetFramebufferSize(m_window, &fbWidth, &fbHeight);
    int winWidth = 0, winHeight = 0;
    glfwGetWindowSize(m_window, &winWidth, &winHeight);

    // Region = central node rect (logical coords), captured last frame in
    // Application::render(). Fall back to the whole window before it is known.
    float regionX = es.canvasRegionPos.x;
    float regionY = es.canvasRegionPos.y;
    float regionW = es.canvasRegionSize.x;
    float regionH = es.canvasRegionSize.y;
    if (regionW < 1.0f || regionH < 1.0f) {
        regionX = 0.0f; regionY = 0.0f;
        regionW = static_cast<float>(winWidth);
        regionH = static_cast<float>(winHeight);
    }

    // Canvas aspect from the active scene's assigned output (getEffectiveResolution
    // already swaps W/H for 90/270 rotation, so portrait is automatic). 16:9 default.
    // That output's projector also draws the scene's 3D objects (ADR-052); with no
    // output the default camera stands in, as it would for a fresh output.
    float canvasAspect = 16.0f / 9.0f;
    // No output: a fresh output's camera (16:9), framed on the scene's objects.
    Vec3 objectsMin, objectsMax;
    const bool haveObjects = es.activeScene().visibleObjectBounds(objectsMin, objectsMax);
    const ProjectorCamera defaultProjector = ProjectorCamera::fitToOutput(
        ProjectorCamera{}, 1920, 1080, haveObjects ? &objectsMin : nullptr,
        haveObjects ? &objectsMax : nullptr);
    const ProjectorCamera* canvasProjector = &defaultProjector;
    OutputWindow* canvasOutput = nullptr;
    if (m_outputManager) {
        const UUID outId = es.activeScene().getAssignedOutputId();
        if (!outId.isNull()) {
            if (OutputWindow* ow = m_outputManager->getOutput(outId)) {
                canvasOutput = ow;
                canvasProjector = &ow->getProjector();
                IVec2 res = ow->getEffectiveResolution();
                if (res.x > 0 && res.y > 0) {
                    canvasAspect = static_cast<float>(res.x) / static_cast<float>(res.y);
                }
            }
        }
    }

    const float regionAspect = (regionH > 0.0f) ? regionW / regionH : 1.0f;
    constexpr float kBaseFit = 0.92f;  // margin so the frame isn't flush to the panels
    float fitX, fitY;
    if (canvasAspect >= regionAspect) {
        fitX = kBaseFit;
        fitY = kBaseFit * regionAspect / canvasAspect;
    } else {
        fitY = kBaseFit;
        fitX = kBaseFit * canvasAspect / regionAspect;
    }
    const float viewScaleX = fitX * es.canvasZoom;
    const float viewScaleY = fitY * es.canvasZoom;
    const float viewOffsetX = (1.0f - viewScaleX) * 0.5f + es.canvasPan.x;
    const float viewOffsetY = (1.0f - viewScaleY) * 0.5f + es.canvasPan.y;
    es.lastViewScale = Vec2(viewScaleX, viewScaleY);
    es.lastViewOffset = Vec2(viewOffsetX, viewOffsetY);

    // GL viewport + scissor for the region, in framebuffer pixels (GL origin is
    // bottom-left, so flip Y). Scissor clips drawing to the canvas region so quads
    // never bleed under the panels.
    const float dpiX = (winWidth > 0) ? static_cast<float>(fbWidth) / winWidth : 1.0f;
    const float dpiY = (winHeight > 0) ? static_cast<float>(fbHeight) / winHeight : 1.0f;
    const int vpX = static_cast<int>(regionX * dpiX + 0.5f);
    const int vpY = static_cast<int>((winHeight - (regionY + regionH)) * dpiY + 0.5f);
    const int vpW = static_cast<int>(regionW * dpiX + 0.5f);
    const int vpH = static_cast<int>(regionH * dpiY + 0.5f);

    auto& cornerHoveredVertex = es.cornerHoveredVertex;
    auto& cornerDraggedVertex = es.cornerDraggedVertex;
    auto& cornerSelectedVertex = es.cornerSelectedVertex;
    auto& cornerDragStartMousePos = es.cornerDragStartMousePos;
    auto& cornerDragStartPos = es.cornerDragStartPos;

    auto& meshHoveredVertex = es.meshHoveredVertex;
    auto& meshDraggedVertex = es.meshDraggedVertex;
    auto& meshSelectedVertex = es.meshSelectedVertex;
    auto& meshDragStartMousePos = es.meshDragStartMousePos;

    auto& snapActive = es.snapActive;
    auto& snapTargetPos = es.snapTargetPos;
    auto& meshDragStartOffset = es.meshDragStartOffset;

    // Projector calibration suspends quad editing (ADR-052 §6).
    const bool calibrating = !es.calibratingOutputId.isNull();

    // ---- Interaction -------------------------------------------------------
    // Mouse and keyboard editing runs BEFORE the canvas is drawn, so a drag or a
    // key handled this frame is already in this frame's picture and in the output
    // frames drawn after it (TD-21). A lambda so calibration can leave early.
    auto handleInput = [&]() {
        // Mouse -> canvas-normalized coordinates by inverting the same view transform
        // used to render (ADR-041): cursor -> region-normalized [0,1] -> canvas via
        // (vn - offset) / scale. Render and hit-test share (scale, offset) so a handle
        // stays under the cursor at any zoom/pan (the success-metric warp UX). Computed
        // ONCE here, before selection handling, so click-to-select works with no
        // current selection too.
        double mouseX, mouseY;
        glfwGetCursorPos(m_window, &mouseX, &mouseY);

        const float vnX = (regionW > 0.0f)
            ? (static_cast<float>(mouseX) - regionX) / regionW : -1.0f;
        const float vnY = (regionH > 0.0f)
            ? (static_cast<float>(mouseY) - regionY) / regionH : -1.0f;
        const Vec2 mouseNorm(
            (viewScaleX != 0.0f) ? (vnX - viewOffsetX) / viewScaleX : 0.0f,
            (viewScaleY != 0.0f) ? (vnY - viewOffsetY) / viewScaleY : 0.0f
        );

        const bool mouseDown = glfwGetMouseButton(m_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        const bool mousePressed = mouseDown && !es.canvasMouseDownPrev;  // press edge
        es.canvasMouseDownPrev = mouseDown;
        const bool inRegion = mouseX >= regionX && mouseX <= regionX + regionW &&
                              mouseY >= regionY && mouseY <= regionY + regionH;
        // A fresh press on the canvas itself, not on or over an ImGui window, menu or
        // popup (io holds this frame's hit-test against last frame's windows). Clicks and drags
        // start only on such a press, so a slider or a bin clip dragged across a handle
        // never grabs it and clicks on overlays above the canvas never reselect quads.
        const bool canvasPress = mousePressed && inRegion && !ImGui::GetIO().WantCaptureMouse;

        // Calibrating: the canvas edits the calibration points of the projector it shows
        // (when it shows the one being calibrated), and quad vertices stay untouched.
        if (calibrating) {
            if (canvasOutput && canvasOutput->getId() == es.calibratingOutputId) {
                editCalibrationPoints(es, *canvasOutput, mouseNorm, mouseDown, canvasPress,
                                      m_project);
            }
            cornerHoveredVertex = cornerDraggedVertex = -1;
            meshHoveredVertex = meshDraggedVertex = -1;
            es.meshHoveredHandle = es.meshDraggedHandle = -1;
            es.bezierHovered = es.bezierDragged = -1;
            snapActive = false;
            return;
        }

        constexpr float HIT_RADIUS = 0.015f;  // Slightly larger hit radius for corners
        constexpr float SNAP_RADIUS = 0.02f;  // Snap threshold for vertex snapping

        Quad* selectedQuad = quadManager.getPrimarySelectedQuad();

        // Click-to-select (ADR-042): a fresh left-press on the canvas that is NOT
        // grabbing the current selection's handle picks the topmost quad under the
        // cursor. The handle guard keeps corner-warp drags working when a handle
        // overlaps another quad's body. The division-insert tool takes precedence.
        if (canvasPress && es.gridInsert == EditorState::GridInsert::None) {
            bool onSelectedHandle = false;
            if (selectedQuad) {
                const QuadGeometry& g = selectedQuad->getGeometry();
                if (g.getMeshMode() == MeshMode::CORNERS_ONLY) {
                    const auto& c = g.getCorners();
                    for (int i = 0; i < 4; ++i)
                        if (glm::length(mouseNorm - c[i]) < HIT_RADIUS) { onSelectedHandle = true; break; }
                } else {
                    auto pos = g.generateMeshPositions();
                    for (const auto& p : pos)
                        if (glm::length(mouseNorm - p) < 0.012f) { onSelectedHandle = true; break; }
                    const unsigned handles = g.availableHandles(meshSelectedVertex);
                    for (int h = 0; h < GRID_HANDLE_COUNT; ++h)
                        if (((handles >> h) & 1u) &&
                            glm::length(mouseNorm - g.gridHandlePosition(meshSelectedVertex, h)) < 0.012f) {
                            onSelectedHandle = true;
                            break;
                        }
                }
            }
            if (!onSelectedHandle) {
                if (Quad* hit = quadManager.quadAtPoint(mouseNorm)) {
                    if (!selectedQuad || hit->getId() != selectedQuad->getId()) {
                        quadManager.selectQuad(hit->getId());
                        selectedQuad = quadManager.getPrimarySelectedQuad();
                        // Drop stale vertex selection carried from the previous quad.
                        cornerHoveredVertex = cornerDraggedVertex = cornerSelectedVertex = -1;
                        meshHoveredVertex = meshDraggedVertex = meshSelectedVertex = -1;
                        es.meshSelectedHandle = es.meshHoveredHandle = es.meshDraggedHandle = -1;
                    }
                }
            }
        }

        if (selectedQuad) {
            QuadGeometry& geometry = selectedQuad->getGeometry();
            MeshMode mode = geometry.getMeshMode();

            // Reset snap state at start of each frame
            snapActive = false;

            // Helper lambda to find snap target from other quads
            auto findSnapTarget = [&](const Vec2& pos, const UUID& excludeQuadId) -> Vec2 {
                Vec2 snappedPos = pos;
                float closestSnapDist = SNAP_RADIUS;

                // Check all other quads for snap targets
                const auto& allQuads = quadManager.getQuads();
                for (const auto& otherQuad : allQuads) {
                    if (otherQuad->getId() == excludeQuadId) continue;  // Skip same quad

                    const auto& otherCorners = otherQuad->getGeometry().getCorners();
                    for (int i = 0; i < 4; ++i) {
                        float dist = glm::length(pos - otherCorners[i]);
                        if (dist < closestSnapDist) {
                            closestSnapDist = dist;
                            snappedPos = otherCorners[i];
                        }
                    }

                    // Also check mesh vertices if in mesh edit mode
                    if (otherQuad->getGeometry().getMeshMode() == MeshMode::MESH_EDIT) {
                        auto meshPositions = otherQuad->getGeometry().generateMeshPositions();
                        for (const auto& meshPos : meshPositions) {
                            float dist = glm::length(pos - meshPos);
                            if (dist < closestSnapDist) {
                                closestSnapDist = dist;
                                snappedPos = meshPos;
                            }
                        }
                    }
                }

                // Set snap indicator state if snapping occurred
                if (snappedPos != pos) {
                    snapActive = true;
                    snapTargetPos = snappedPos;
                }

                return snappedPos;
            };

            // Grid-division insert tool (ADR-038): when armed, pressing inside the
            // quad inserts ONE division at the clicked param position and then lets
            // you DRAG it to fine-position the line; releasing commits and disarms
            // (one-shot, so dragging vertices resumes). Esc cancels. This avoids the
            // trap of an armed mode silently swallowing every click with no way out.
            if (es.gridInsert != EditorState::GridInsert::None) {
                const bool isColumn = (es.gridInsert == EditorState::GridInsert::Column);
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    es.gridInsert = EditorState::GridInsert::None;
                    es.gridInsertMouseLatch = false;
                    es.gridInsertDragIndex = -1;
                } else if (mouseDown && (es.gridInsertMouseLatch || canvasPress)) {
                    // The surface parameters under the cursor, as drawn (ADR-049).
                    Vec2 uv = geometry.surfaceParamsAt(mouseNorm);
                    if (!es.gridInsertMouseLatch) {
                        // Press edge: snapshot the grid (for one-step undo), then
                        // insert the new line where the user clicked.
                        es.gridInsertMouseLatch = true;
                        es.gridInsertBefore = captureGrid(geometry);
                        if (uv.x > 0.001f && uv.x < 0.999f &&
                            uv.y > 0.001f && uv.y < 0.999f) {
                            es.gridInsertDragIndex = isColumn
                                ? geometry.insertUDivision(uv.x)
                                : geometry.insertVDivision(uv.y);
                            m_project->setModified(true);
                        }
                    } else if (es.gridInsertDragIndex >= 0 && uv.x >= 0.0f) {
                        // Held over the surface: drag the just-inserted line to
                        // fine-position it (off the surface it stays put).
                        if (isColumn) geometry.moveUDivision(es.gridInsertDragIndex, uv.x);
                        else          geometry.moveVDivision(es.gridInsertDragIndex, uv.y);
                        m_project->setModified(true);
                    }
                } else {
                    // Release: commit + disarm only if a line was actually placed,
                    // recording the whole insert+drag as ONE undo step.
                    es.gridInsertMouseLatch = false;
                    if (es.gridInsertDragIndex >= 0) {
                        pushGridChange(es.commandHistory, es.quadRef(*selectedQuad), "Insert Division",
                                       es.gridInsertBefore, captureGrid(geometry));
                        es.gridInsert = EditorState::GridInsert::None;
                        es.gridInsertDragIndex = -1;
                    }
                }
            }
            const bool insertArmed = (es.gridInsert != EditorState::GridInsert::None);

            if (mode == MeshMode::CORNERS_ONLY) {
                // ===== CORNER PIN MODE (Default) =====
                const auto& corners = geometry.getCorners();

                if (cornerDraggedVertex >= 0) {
                    // Currently dragging a corner
                    if (mouseDown) {
                        Vec2 mouseDelta = mouseNorm - cornerDragStartMousePos;
                        Vec2 newPos = cornerDragStartPos + mouseDelta;

                        // Apply snapping to other quads' vertices
                        newPos = findSnapTarget(newPos, selectedQuad->getId());

                        geometry.setCorner(cornerDraggedVertex, newPos);
                        m_project->setModified(true);
                    } else {
                        // Mouse released — record the completed drag as ONE undo step.
                        if (cornerDraggedVertex >= 0 &&
                            corners[cornerDraggedVertex] != cornerDragStartPos) {
                            es.commandHistory.execute(std::make_unique<MoveCornerCommand>(
                                es.quadRef(*selectedQuad), cornerDraggedVertex,
                                cornerDragStartPos, corners[cornerDraggedVertex]));
                        }
                        cornerSelectedVertex = cornerDraggedVertex;
                        cornerDraggedVertex = -1;
                    }
                } else {
                    // Check for hover over corners
                    cornerHoveredVertex = -1;
                    float closestDist = HIT_RADIUS;

                    for (int i = 0; i < 4; ++i) {
                        float dist = glm::length(mouseNorm - corners[i]);
                        if (dist < closestDist) {
                            closestDist = dist;
                            cornerHoveredVertex = i;
                        }
                    }

                    // Start drag if mouse pressed on a corner (not while the
                    // division-insert tool is armed).
                    if (canvasPress && cornerHoveredVertex >= 0 && !insertArmed) {
                        cornerDraggedVertex = cornerHoveredVertex;
                        cornerSelectedVertex = cornerHoveredVertex;
                        cornerDragStartMousePos = mouseNorm;
                        cornerDragStartPos = corners[cornerHoveredVertex];
                    }
                }

                // Bezier edge-handle editing (ADR-043): drag the 8 control points to
                // bow the edges. Corners take precedence (skip while a corner drags).
                if (geometry.isBezierEnabled() && cornerDraggedVertex < 0) {
                    if (es.bezierDragged >= 0) {
                        if (mouseDown) {
                            geometry.setBezierControl(es.bezierDragged / 2,
                                                      es.bezierDragged % 2, mouseNorm);
                            m_project->setModified(true);
                        } else {
                            // Release — record the whole bend as one undo step.
                            es.commandHistory.execute(
                                std::make_unique<QuadPropertyCommand<std::array<Vec2, 8>>>(
                                    "Bezier Edge", es.quadRef(*selectedQuad),
                                    es.bezierDragBefore, geometry.getBezierOffsets(),
                                    [](Quad& q, const std::array<Vec2, 8>& o) {
                                        q.getGeometry().setBezierOffsets(o);
                                    }));
                            es.bezierDragged = -1;
                        }
                    } else {
                        es.bezierHovered = -1;
                        float best = HIT_RADIUS;
                        for (int i = 0; i < 8; ++i) {
                            Vec2 cp = geometry.getBezierControl(i / 2, i % 2);
                            float d = glm::length(mouseNorm - cp);
                            if (d < best) { best = d; es.bezierHovered = i; }
                        }
                        if (canvasPress && es.bezierHovered >= 0 && !insertArmed) {
                            es.bezierDragged = es.bezierHovered;
                            es.bezierDragBefore = geometry.getBezierOffsets();
                        }
                    }
                } else if (!geometry.isBezierEnabled()) {
                    es.bezierHovered = -1;
                }

                // Keyboard controls for corners (Tab cycles TL, TR, BR, BL; arrows nudge)
                ImGuiIO& cornerIO = ImGui::GetIO();
                if (canvasOwnsEditingKeys(true)) {
                    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
                        cornerSelectedVertex = cycleVertex(cornerSelectedVertex, 4, cornerIO.KeyShift);
                    }

                    if (cornerSelectedVertex >= 0 && cornerSelectedVertex < 4) {
                        const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
                        const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
                        const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
                        const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
                        const Vec2 oldPos = corners[cornerSelectedVertex];
                        const Vec2 pos = oldPos + nudgeDelta(up, down, left, right,
                                             nudgeStep(cornerIO.KeyShift, cornerIO.KeyAlt));

                        if (up || down || left || right) {
                            // Command applies the move; consecutive nudges of the same
                            // corner merge into one undo step (CommandHistory::execute).
                            es.commandHistory.execute(std::make_unique<MoveCornerCommand>(
                                es.quadRef(*selectedQuad), cornerSelectedVertex, oldPos, pos));
                            m_project->setModified(true);
                        }
                    }
                }

                // Reset mesh state when in corner mode
                meshHoveredVertex = -1;
                meshDraggedVertex = -1;
                meshSelectedVertex = -1;
                es.meshSelectedHandle = es.meshHoveredHandle = es.meshDraggedHandle = -1;

            } else {
                // ===== MESH EDIT MODE =====
                auto positions = geometry.generateMeshPositions();
                auto basePositions = geometry.generateBasePositions();
                constexpr float MESH_HIT_RADIUS = 0.012f;  // mesh handles are smaller

                // Grid-line handles of the selected vertex (ADR-049 §3). A handle drag
                // records one undo step on release, as a vertex drag does.
                if (es.meshDraggedHandle >= 0) {
                    if (mouseDown) {
                        geometry.setGridHandlePosition(
                            meshSelectedVertex, es.meshDraggedHandle,
                            es.meshHandleDragStartPos + (mouseNorm - es.meshHandleDragStartMousePos));
                        m_project->setModified(true);
                    } else {
                        const VertexTangents after = geometry.getVertexTangent(meshSelectedVertex);
                        if (after != es.meshHandleDragBefore) {
                            es.commandHistory.execute(std::make_unique<MoveGridHandleCommand>(
                                es.quadRef(*selectedQuad), meshSelectedVertex,
                                es.meshDraggedHandle, es.meshHandleDragBefore, after));
                        }
                        es.meshSelectedHandle = es.meshDraggedHandle;
                        es.meshDraggedHandle = -1;
                    }
                }

                if (es.meshDraggedHandle >= 0) {
                    meshHoveredVertex = -1;  // a handle drag owns the mouse
                } else if (meshDraggedVertex >= 0) {
                    if (mouseDown) {
                        Vec2 mouseDelta = mouseNorm - meshDragStartMousePos;
                        Vec2 newOffset = meshDragStartOffset + mouseDelta;

                        // Calculate actual position, apply snapping, then compute the
                        // residual from the base
                        Vec2 basePos = basePositions[meshDraggedVertex];
                        Vec2 actualPos = basePos + newOffset;
                        Vec2 snappedPos = findSnapTarget(actualPos, selectedQuad->getId());
                        newOffset = snappedPos - basePos;

                        geometry.setMeshVertexByIndex(meshDraggedVertex, newOffset);
                        m_project->setModified(true);
                    } else {
                        // Mouse released — record the completed mesh drag as one undo step.
                        if (meshDraggedVertex >= 0) {
                            Vec2 finalOffset = geometry.getMeshVertexByIndex(meshDraggedVertex);
                            if (finalOffset != meshDragStartOffset) {
                                es.commandHistory.execute(std::make_unique<MoveMeshVertexCommand>(
                                    es.quadRef(*selectedQuad), meshDraggedVertex,
                                    meshDragStartOffset, finalOffset));
                            }
                        }
                        meshSelectedVertex = meshDraggedVertex;
                        meshDraggedVertex = -1;
                    }
                } else {
                    // Hover the nearest vertex or handle of the selected vertex; a
                    // handle wins only when it is the closer one, so a short handle
                    // never hides its own vertex.
                    meshHoveredVertex = -1;
                    es.meshHoveredHandle = -1;
                    float closestDist = MESH_HIT_RADIUS;
                    for (size_t i = 0; i < positions.size(); ++i) {
                        float dist = glm::length(mouseNorm - positions[i]);
                        if (dist < closestDist) {
                            closestDist = dist;
                            meshHoveredVertex = static_cast<int>(i);
                        }
                    }
                    const unsigned handles = geometry.availableHandles(meshSelectedVertex);
                    for (int h = 0; h < GRID_HANDLE_COUNT; ++h) {
                        if (!((handles >> h) & 1u)) continue;
                        const float dist = glm::length(
                            mouseNorm - geometry.gridHandlePosition(meshSelectedVertex, h));
                        if (dist < closestDist) {
                            closestDist = dist;
                            es.meshHoveredHandle = h;
                            meshHoveredVertex = -1;
                        }
                    }

                    if (canvasPress && es.meshHoveredHandle >= 0 && !insertArmed) {
                        es.meshDraggedHandle = es.meshSelectedHandle = es.meshHoveredHandle;
                        es.meshHandleDragStartMousePos = mouseNorm;
                        es.meshHandleDragStartPos =
                            geometry.gridHandlePosition(meshSelectedVertex, es.meshHoveredHandle);
                        es.meshHandleDragBefore = geometry.getVertexTangent(meshSelectedVertex);
                    } else if (canvasPress && meshHoveredVertex >= 0 && !insertArmed) {
                        if (meshHoveredVertex != meshSelectedVertex) es.meshSelectedHandle = -1;
                        meshDraggedVertex = meshHoveredVertex;
                        meshSelectedVertex = meshHoveredVertex;
                        meshDragStartMousePos = mouseNorm;
                        meshDragStartOffset = geometry.getMeshVertexByIndex(meshHoveredVertex);
                    }
                }

                // Keyboard: Tab cycles vertices, H their grid-line handles, arrows nudge.
                // Not mid handle-drag, which edits the selected vertex's handle.
                if (es.meshDraggedHandle < 0 && canvasOwnsEditingKeys(true) &&
                    applyMeshEditKeys(readCanvasKeys(), es.quadRef(*selectedQuad), geometry,
                                      es.commandHistory, meshSelectedVertex,
                                      es.meshSelectedHandle)) {
                    m_project->setModified(true);
                }

                // Reset corner state when in mesh mode
                cornerHoveredVertex = -1;
                cornerDraggedVertex = -1;
                cornerSelectedVertex = -1;
            }
        } else {
            // No quad selected, reset all state
            cornerHoveredVertex = -1;
            cornerDraggedVertex = -1;
            cornerSelectedVertex = -1;
            meshHoveredVertex = -1;
            meshDraggedVertex = -1;
            meshSelectedVertex = -1;
            es.meshSelectedHandle = es.meshHoveredHandle = es.meshDraggedHandle = -1;
        }
    };
    handleInput();

    // Disable depth test for 2D rendering
    glDisable(GL_DEPTH_TEST);

    // Constrain drawing to the canvas region and apply the view transform.
    glViewport(vpX, vpY, vpW, vpH);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vpX, vpY, vpW, vpH);

    // Render all quads
    if (m_renderer) {
        m_renderer->setViewTransform(glm::vec2(viewScaleX, viewScaleY),
                                     glm::vec2(viewOffsetX, viewOffsetY), canvasAspect);
        m_renderer->beginFrame();

        // Canvas frame border so the projector-frame bounds are visible.
        m_renderer->renderCanvasBorder(glm::vec4(0.45f, 0.45f, 0.5f, 0.9f), 1.0f);

        // Resolve every quad's texture through the shared builder: image via
        // TextureBinding::mediaId -> SourceManager, video via the legacy wrapper
        // (ADR-040 P2). The output frames use the same builder with the projector's
        // missing-media look.
        auto texturePtrs = buildQuadTextureMap(es, ctx.sourceManager, SurfaceView::Editor);

        // 3D objects as the canvas's projector sees them (WYSIWYG with the output),
        // under the quads. The scissor limits the depth clear to the canvas region.
        glClear(GL_DEPTH_BUFFER_BIT);
        m_renderer->renderMeshObjects(es.activeScene().getObjects(), *canvasProjector,
                                      texturePtrs);

        m_renderer->renderQuads(quadManager, texturePtrs);

        // Render handles for selected quad based on mode (none while calibrating, when
        // the canvas edits calibration points instead).
        Quad* selectedQuadForHandles =
            calibrating ? nullptr : quadManager.getPrimarySelectedQuad();
        if (selectedQuadForHandles) {
            MeshMode mode = selectedQuadForHandles->getGeometry().getMeshMode();
            if (mode == MeshMode::MESH_EDIT) {
                // Draw the control-grid wireframe so division lines are visible,
                // then the vertex handles on top.
                m_renderer->renderMeshGrid(*selectedQuadForHandles,
                                           glm::vec4(0.0f, 0.8f, 1.0f, 0.6f), 1.0f);
                m_renderer->renderMeshHandles(*selectedQuadForHandles, meshHoveredVertex, meshDraggedVertex, meshSelectedVertex);
                m_renderer->renderGridHandles(*selectedQuadForHandles, meshSelectedVertex,
                                              es.meshSelectedHandle, es.meshHoveredHandle,
                                              es.meshDraggedHandle);
            } else {
                // CORNERS_ONLY mode (default) - show just the 4 corner handles
                m_renderer->renderCornerHandles(*selectedQuadForHandles, cornerHoveredVertex, cornerDraggedVertex, cornerSelectedVertex);
                // Bezier edge handles on top (no-op unless bezier is enabled).
                m_renderer->renderBezierHandles(*selectedQuadForHandles,
                                                es.bezierHovered, es.bezierDragged);
            }

            // Render snap indicator if actively snapping to another quad's vertex
            if (snapActive) {
                m_renderer->renderSnapIndicator(snapTargetPos);
            }
        }

        m_renderer->endFrame();
        m_renderer->resetViewTransform();
    }

    // Restore full-framebuffer viewport + drop scissor so the output frames and
    // ImGui render over the whole window unaffected.
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, fbWidth, fbHeight);

    // Re-enable depth test
    glEnable(GL_DEPTH_TEST);
}

}  // namespace quadmapper
