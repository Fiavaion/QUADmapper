#include "ViewportEditor.hpp"

#include "editor/EditActions.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EditorState.hpp"
#include "editor/PerformLock.hpp"
#include "editor/TextureResolve.hpp"
#include "editor/GridUndo.hpp"
#include "editor/MeshEditKeys.hpp"
#include "editor/QuadSelection.hpp"
#include "editor/SliceEditing.hpp"
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

#include <algorithm>
#include <cmath>
#include <memory>
#include <array>
#include <unordered_map>

namespace quadmapper {

namespace {

// Calibration mode on the canvas (ADR-052 §6): place, drag and nudge the output's
// calibration pixels with the same press rule and keyboard model as quad vertices.
// The canvas shows the output's frame edge to edge (calibrationViewOutput, TD-29), so
// pixels are canvas-normalised positions times the projector image size, whatever
// the output's slices.
void editCalibrationPoints(EditorState& es, OutputWindow& output, const Vec2& mouseNorm,
                           bool mouseDown, bool canvasPress) {
    CalibrationSession& cal = es.calibration;
    auto& points = output.getCalibrationPoints();
    const ProjectorIntrinsics& k = output.getProjector().intrinsics;
    const int count = static_cast<int>(points.size());
    if (cal.selected >= count) cal.selected = -1;
    if (cal.dragged >= count) cal.dragged = -1;
    const Vec2 mousePixel = canvasToPixel(mouseNorm, k);
    // Every change is named for its undo step (TD-17); CalibrationPanel records it.
    auto changed = [&](const std::string& description, int mergeKey) {
        cal.noteEdit(description, mergeKey);
        cal.markChanged();
    };
    auto moved = [&](int index) {
        changed("Move calibration point " + std::to_string(index + 1),
                CalibrationSession::moveKey(index));
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
            changed("Add calibration point " + std::to_string(points.size()), 0);
        }
        return;
    }

    constexpr float HIT_RADIUS = 0.015f;  // as for quad corners, in canvas units
    if (cal.dragged >= 0) {
        if (mouseDown) {
            const Vec2 pixel = cal.dragStartPixel + (mousePixel - cal.dragStartMousePixel);
            if (pixel != points[cal.dragged].pixel) {
                points[cal.dragged].pixel = pixel;
                moved(cal.dragged);
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
                moved(cal.selected);
            }
        }
    }
}

// Slice warp editing on the canvas (ADR-047 part B): the canvas is the output's frame,
// and the selected slice's warp corners are dragged and nudged with the same press
// rule and keyboard model as a quad's corners. A drag is one undo step on release.
void editSliceWarp(EditorState& es, OutputWindow& output, const Vec2& mouseNorm, bool mouseDown,
                   bool canvasPress) {
    auto& slices = output.getSlices();
    const int index = findSliceIndex(slices, es.selectedSliceId);
    if (index < 0) return;
    QuadGeometry& warp = slices[static_cast<size_t>(index)].warp;
    const auto& corners = warp.getCorners();
    constexpr float HIT_RADIUS = 0.015f;  // as for quad corners
    int& dragged = es.sliceWarpDraggedCorner;
    if (dragged >= 0) {
        if (mouseDown) {
            const Vec2 pos = es.sliceWarpDragStartPos + (mouseNorm - es.sliceWarpDragStartMouse);
            if (pos != corners[static_cast<size_t>(dragged)]) {
                warp.setCorner(dragged, pos);
            }
        } else {
            es.finishSliceWarpDrag();
        }
    } else {
        es.sliceWarpHoveredCorner = -1;
        float closest = HIT_RADIUS;
        for (int i = 0; i < 4; ++i) {
            const float d = glm::length(mouseNorm - corners[static_cast<size_t>(i)]);
            if (d < closest) {
                closest = d;
                es.sliceWarpHoveredCorner = i;
            }
        }
        if (canvasPress && es.sliceWarpHoveredCorner >= 0) {
            dragged = es.sliceWarpSelectedCorner = es.sliceWarpHoveredCorner;
            es.sliceWarpDragStartMouse = mouseNorm;
            es.sliceWarpDragStartPos = corners[static_cast<size_t>(dragged)];
            es.sliceWarpDragBefore = OutputSlices{slices, output.getSlicesCanvas()};
        }
    }
    if (dragged < 0 && canvasOwnsEditingKeys(true)) {
        applySliceWarpKeys(readCanvasKeys(), es, output);
    }
}

// A throwaway quad carrying `geometry`, for the Renderer's outline and handle drawing.
Quad overlayQuad(const UUID& id, const QuadGeometry& geometry) {
    Quad quad(id);
    quad.getGeometry() = geometry;
    return quad;
}

// The output's frame on the canvas while its slice warp is edited: its slices of the
// scene's canvas as the projector gets them (from the canvas the output frames were
// drawn from), every slice's warp outline, and the selected slice's corner handles.
void drawSliceWarpCanvas(EditorState& es, Renderer& renderer, const OutputManager& outputs,
                         const OutputWindow& output) {
    const Scene* shown = OutputManager::sceneForOutput(es.scenes, output);
    const Framebuffer* canvas = shown ? outputs.sceneCanvas(shown->getId()) : nullptr;
    if (canvas) {
        GLint prevBlend[4] = {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA};
        glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlend[0]);
        glGetIntegerv(GL_BLEND_DST_RGB, &prevBlend[1]);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlend[2]);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlend[3]);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        for (const Slice& slice : output.getSlices()) {
            renderer.renderSlice(slice, canvas->colorTexture(), shown->getCanvasSize(),
                                 outputs.sceneCanvasRotation(shown->getId()),
                                 output.getEffectiveResolution());
        }
        renderer.endFrame();
        glBlendFuncSeparate(static_cast<GLenum>(prevBlend[0]), static_cast<GLenum>(prevBlend[1]),
                            static_cast<GLenum>(prevBlend[2]), static_cast<GLenum>(prevBlend[3]));
    }
    for (const Slice& slice : output.getSlices()) {
        const bool selected = slice.id == es.selectedSliceId;
        const Quad outline = overlayQuad(slice.id, slice.warp);
        renderer.renderQuadOutline(outline, selected ? glm::vec4(1.0f, 0.75f, 0.2f, 1.0f)
                                                     : glm::vec4(0.6f, 0.6f, 0.65f, 0.8f),
                                   selected ? 2.0f : 1.0f);
        if (selected) {
            renderer.renderCornerHandles(outline, es.sliceWarpHoveredCorner,
                                         es.sliceWarpDraggedCorner, es.sliceWarpSelectedCorner);
        }
    }
}

// The calibration view (TD-29): the calibrated output's frame as its projector shows
// it outside a crossfade and blackout (mid-fade the view draws the incoming scene alone,
// and it keeps the picture during blackout), drawn by the call that draws the output
// frames (OutputManager::renderSceneFrame):
// black, the scene's 3D objects from the projector, then the scene's canvas through the
// output's slices, warp, blend zones and black level. `frameMin`..`frameMax` is the
// frame on the editor framebuffer in GL pixels; the scissor (the canvas region) clips it.
void drawCalibrationView(EditorState& es, Renderer& renderer, OutputManager& outputs,
                         const OutputWindow& output,
                         const std::unordered_map<UUID, Texture*>& textures, const IVec2& frameMin,
                         const IVec2& frameMax) {
    const Scene* shown = OutputManager::sceneForOutput(es.scenes, output);
    if (!shown) return;

    // The frame is black where the projector sends no light (outside the slices, in
    // blend zones), not the editor's grey.
    GLint scissor[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    GLfloat clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    const int x0 = std::max(frameMin.x, scissor[0]);
    const int y0 = std::max(frameMin.y, scissor[1]);
    const int x1 = std::min(frameMax.x, scissor[0] + scissor[2]);
    const int y1 = std::min(frameMax.y, scissor[1] + scissor[3]);
    glScissor(x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0));
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);

    GLint prevBlend[4] = {GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA};
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlend[0]);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevBlend[1]);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlend[2]);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlend[3]);
    outputs.renderSceneFrame(output, *shown, output.getEffectiveResolution(), textures, renderer);
    renderer.endFrame();
    glBlendFuncSeparate(static_cast<GLenum>(prevBlend[0]), static_cast<GLenum>(prevBlend[1]),
                        static_cast<GLenum>(prevBlend[2]), static_cast<GLenum>(prevBlend[3]));
}

// Outlines of the selected output's slice input rectangles on the scene canvas, while
// the slice editor shows that output (an output selected in the Inspector) and it shows
// the active scene. The selected slice is drawn brighter.
void drawSliceInputOverlay(EditorState& es, Renderer& renderer, OutputManager* outputs) {
    if (!outputs || es.currentSelectionType != EditorState::SelectionType::Output ||
        es.selectedOutputId.isNull()) {
        return;
    }
    const OutputWindow* output = outputs->getOutput(es.selectedOutputId);
    if (!output || output->getSceneId() != es.activeScene().getId()) return;
    const IVec2 size = es.activeScene().getCanvasSize();
    const float w = static_cast<float>(size.x);
    const float h = static_cast<float>(size.y);
    for (const Slice& slice : output->getSlices()) {
        const CanvasRect& r = slice.input;
        QuadGeometry rect;
        rect.setCorners({Vec2(r.x / w, r.y / h), Vec2((r.x + r.width) / w, r.y / h),
                         Vec2((r.x + r.width) / w, (r.y + r.height) / h),
                         Vec2(r.x / w, (r.y + r.height) / h)});
        const bool selected = slice.id == es.selectedSliceId;
        renderer.renderQuadOutline(overlayQuad(slice.id, rect),
                                   selected ? glm::vec4(1.0f, 0.75f, 0.2f, 1.0f)
                                            : glm::vec4(1.0f, 0.75f, 0.2f, 0.45f),
                                   selected ? 2.0f : 1.0f);
    }
}

}  // namespace

void ViewportEditor::render(EditorContext& ctx) {
    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(ctx.window, &mouseX, &mouseY);
    const bool leftDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool rightDown = glfwGetMouseButton(ctx.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    render(ctx, Pointer{Vec2(static_cast<float>(mouseX), static_cast<float>(mouseY)), leftDown,
                        rightDown});
}

void ViewportEditor::render(EditorContext& ctx, const Pointer& pointer) {
    // Same-named aliases keep the moved render()/interaction body verbatim
    // (LESSON-ARCH-006). All editing state lives in EditorState; the manager
    // pointers come from the per-frame EditorContext.
    EditorState& es = ctx.es;
    auto* m_renderer = ctx.renderer;
    auto* m_window = ctx.window;
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

    // Canvas aspect from the active scene's canvas (ADR-047). The projector of an
    // output showing the scene also draws its 3D objects (ADR-052); with no output
    // the default camera stands in, as it would for a fresh output.
    float canvasAspect = 16.0f / 9.0f;
    // No output: a fresh output's camera (16:9), framed on the scene's objects.
    Vec3 objectsMin, objectsMax;
    const bool haveObjects = es.activeScene().visibleObjectBounds(objectsMin, objectsMax);
    const ProjectorCamera defaultProjector = ProjectorCamera::fitToOutput(
        ProjectorCamera{}, 1920, 1080, haveObjects ? &objectsMin : nullptr,
        haveObjects ? &objectsMax : nullptr);
    const ProjectorCamera* canvasProjector = &defaultProjector;
    OutputWindow* canvasOutput = nullptr;
    const IVec2 canvasSize = es.activeScene().getCanvasSize();
    canvasAspect = static_cast<float>(canvasSize.x) / static_cast<float>(canvasSize.y);
    // Slice warp editing and calibration show that output's frame instead (its
    // effective size).
    OutputWindow* warpOutput = sliceWarpOutput(es, m_outputManager);
    OutputWindow* calibrationView = calibrationViewOutput(es, m_outputManager);
    if (OutputWindow* frameOutput = warpOutput ? warpOutput : calibrationView) {
        const IVec2 frame = frameOutput->getEffectiveResolution();
        if (frame.x > 0 && frame.y > 0) {
            canvasAspect = static_cast<float>(frame.x) / static_cast<float>(frame.y);
        }
    }
    if (m_outputManager) {
        // The projector being calibrated when it shows this scene, else the first
        // output showing it.
        canvasOutput = calibrationView ? calibrationView
                                       : m_outputManager->firstOutputShowing(es.activeScene().getId());
        if (canvasOutput) canvasProjector = &canvasOutput->getProjector();
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
        const float mouseX = pointer.position.x;
        const float mouseY = pointer.position.y;

        const float vnX = (regionW > 0.0f)
            ? (mouseX - regionX) / regionW : -1.0f;
        const float vnY = (regionH > 0.0f)
            ? (mouseY - regionY) / regionH : -1.0f;
        const Vec2 mouseNorm(
            (viewScaleX != 0.0f) ? (vnX - viewOffsetX) / viewScaleX : 0.0f,
            (viewScaleY != 0.0f) ? (vnY - viewOffsetY) / viewScaleY : 0.0f
        );

        const bool mouseDown = pointer.leftDown;
        const bool mousePressed = mouseDown && !es.canvasMouseDownPrev;  // press edge
        es.canvasMouseDownPrev = mouseDown;
        const bool inRegion = mouseX >= regionX && mouseX <= regionX + regionW &&
                              mouseY >= regionY && mouseY <= regionY + regionH;
        // A fresh press on the canvas itself, not on or over an ImGui window, menu or
        // popup (io holds this frame's hit-test against last frame's windows). Clicks and drags
        // start only on such a press, so a slider or a bin clip dragged across a handle
        // never grabs it and clicks on overlays above the canvas never reselect quads.
        const bool canvasPress = mousePressed && inRegion && !ImGui::GetIO().WantCaptureMouse;
        const bool rightPressed = pointer.rightDown && !es.canvasRightDownPrev;
        es.canvasRightDownPrev = pointer.rightDown;
        const bool canvasRightPress = rightPressed && inRegion && !ImGui::GetIO().WantCaptureMouse;

        // A quad drag the canvas is taken from (lock, slice warp editing, calibration)
        // ends where the mouse left it, as its undo step; then nothing is hovered.
        const auto leaveQuadEditing = [&]() {
            commitCanvasDrags(es);
            cornerHoveredVertex = meshHoveredVertex = -1;
            es.meshHoveredHandle = es.bezierHovered = -1;
            snapActive = false;
        };

        // The perform lock (ADR-051 §4): no picking, dragging or keys. The canvas still
        // shows the show, and zoom and pan (EditorUi's canvas overlay) still work.
        if (es.performLock.isOn()) {
            leaveQuadEditing();
            return;
        }

        // Space held: the hand tool (EditorUi) pans the canvas with the left button, so
        // nothing is picked, dragged or nudged here meanwhile.
        if (es.canvasHandTool) {
            leaveQuadEditing();
            return;
        }

        // Slice warp editing: the canvas edits the selected slice's warp corners, and
        // quad vertices stay untouched (as while calibrating).
        if (warpOutput) {
            leaveQuadEditing();
            editSliceWarp(es, *warpOutput, mouseNorm, mouseDown, canvasPress);
            return;
        }

        // Calibrating: the canvas edits the calibration points of the projector whose
        // frame it shows, and quad vertices stay untouched.
        if (calibrating) {
            leaveQuadEditing();
            if (calibrationView) {
                editCalibrationPoints(es, *calibrationView, mouseNorm, mouseDown, canvasPress);
            }
            return;
        }

        constexpr float HIT_RADIUS = 0.015f;  // Slightly larger hit radius for corners
        constexpr float SNAP_RADIUS = 0.02f;  // Snap threshold for vertex snapping
        constexpr float EDGE_RADIUS = 0.015f;  // a press this near the outline is on the edge
        constexpr float LINE_RADIUS = 0.008f;  // a press this near a grid line grabs it

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
                    // A grid line of the selected quad is grabbed, not a quad behind it.
                    bool column = false;
                    if (g.gridLineNear(mouseNorm, LINE_RADIUS, column) >= 0) onSelectedHandle = true;
                }
            }
            if (pickQuadOnCanvas(es, quadManager, mouseNorm, onSelectedHandle)) {
                selectedQuad = quadManager.getPrimarySelectedQuad();
                // Drop stale vertex selection carried from the previous quad.
                cornerHoveredVertex = cornerDraggedVertex = cornerSelectedVertex = -1;
                meshHoveredVertex = meshDraggedVertex = meshSelectedVertex = -1;
                es.meshSelectedHandle = es.meshHoveredHandle = es.meshDraggedHandle = -1;
            }
        }

        if (selectedQuad) {
            QuadGeometry& geometry = selectedQuad->getGeometry();
            MeshMode mode = geometry.getMeshMode();

            // A locked quad (the Quad List's lock) can be selected and its vertices
            // cycled, but not reshaped: no drag or insert starts on it and no arrow
            // nudges it; the press or key that was refused says why on the status line.
            const bool locked = selectedQuad->isLocked();
            const auto mayStartDrag = [&]() { return !refusedByQuadLock(es, *selectedQuad); };
            const auto readKeys = [&]() {
                const CanvasKeys keys = readCanvasKeys();
                if (keys.arrow() && locked) refusedByQuadLock(es, *selectedQuad);
                return keys;
            };

            // Reset snap state at start of each frame
            snapActive = false;

            // Helper lambda to find snap target from other quads
            auto findSnapTarget = [&](const Vec2& pos, const UUID& excludeQuadId) -> Vec2 {
                if (!es.vertexSnapping) return pos;  // the canvas Snap toggle
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
                } else if (mouseDown && (es.gridInsertMouseLatch || (canvasPress && mayStartDrag()))) {
                    // The surface parameters under the cursor, as drawn (ADR-049); on
                    // or just outside the outline, those of the nearest edge point
                    // (owner, 2026-11-23), and while dragging, wherever the mouse goes.
                    Vec2 uv = geometry.surfaceParamsAt(mouseNorm);
                    if (uv.x < 0.0f) {
                        uv = geometry.boundaryParamsNear(
                            mouseNorm, es.gridInsertMouseLatch ? 1e9f : EDGE_RADIUS);
                    }
                    if (!es.gridInsertMouseLatch) {
                        // Press edge: snapshot the grid (for one-step undo), then
                        // insert the new line where the user clicked: a column from
                        // the top or bottom edge, a row from the left or right.
                        es.gridInsertMouseLatch = true;
                        es.gridInsertBefore = captureGrid(geometry);
                        const float t = isColumn ? uv.x : uv.y;
                        if (t > 0.001f && t < 0.999f) {
                            es.gridInsertDragIndex = isColumn
                                ? geometry.insertUDivision(uv.x, es.newGridPointsLinear)
                                : geometry.insertVDivision(uv.y, es.newGridPointsLinear);
                        }
                    } else if (es.gridInsertDragIndex >= 0 && uv.x >= 0.0f) {
                        // Held over the surface: drag the just-inserted line to
                        // fine-position it (off the surface it stays put).
                        if (isColumn) geometry.moveUDivision(es.gridInsertDragIndex, uv.x);
                        else          geometry.moveVDivision(es.gridInsertDragIndex, uv.y);
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
                    if (canvasPress && cornerHoveredVertex >= 0 && !insertArmed && mayStartDrag()) {
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
                        if (canvasPress && es.bezierHovered >= 0 && !insertArmed && mayStartDrag()) {
                            es.bezierDragged = es.bezierHovered;
                            es.bezierDragBefore = geometry.getBezierOffsets();
                        }
                    }
                } else if (!geometry.isBezierEnabled()) {
                    es.bezierHovered = -1;
                }

                // Keyboard: Tab cycles TL, TR, BR, BL; arrows nudge (MeshEditKeys.hpp).
                if (canvasOwnsEditingKeys(true)) {
                    applyCornerKeys(readKeys(), es.quadRef(*selectedQuad), geometry,
                                    es.commandHistory, cornerSelectedVertex, locked);
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

                // A grid line dragged to a new place (owner, 2026-11-23): the mouse
                // maps to (u,v) on the quad as the drag began, so moving the line does
                // not move what it is measured against. One undo step on release.
                if (es.gridLineDragIndex >= 0) {
                    if (mouseDown) {
                        Vec2 uv = es.gridLineDragFrom.surfaceParamsAt(mouseNorm);
                        if (uv.x < 0.0f) uv = es.gridLineDragFrom.boundaryParamsNear(mouseNorm, 1e9f);
                        if (uv.x >= 0.0f) {
                            if (es.gridLineDragIsColumn) geometry.moveUDivision(es.gridLineDragIndex, uv.x);
                            else geometry.moveVDivision(es.gridLineDragIndex, uv.y);
                        }
                        ImGui::SetMouseCursor(es.gridLineDragIsColumn ? ImGuiMouseCursor_ResizeEW
                                                                      : ImGuiMouseCursor_ResizeNS);
                    } else {
                        commitCanvasDrags(es);
                    }
                }

                // Grid-line handles of the selected vertex (ADR-049 §3). A handle drag
                // records one undo step on release, as a vertex drag does.
                if (es.meshDraggedHandle >= 0) {
                    if (mouseDown) {
                        geometry.setGridHandlePosition(
                            meshSelectedVertex, es.meshDraggedHandle,
                            es.meshHandleDragStartPos + (mouseNorm - es.meshHandleDragStartMousePos));
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

                if (es.meshDraggedHandle >= 0 || es.gridLineDragIndex >= 0) {
                    meshHoveredVertex = -1;  // a handle or line drag owns the mouse
                    es.gridLineHovered = -1;
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

                    // Nothing nearer: a grid line under the mouse can be dragged.
                    es.gridLineHovered = -1;
                    if (meshHoveredVertex < 0 && es.meshHoveredHandle < 0 && !insertArmed) {
                        bool column = false;
                        es.gridLineHovered = geometry.gridLineNear(mouseNorm, LINE_RADIUS, column);
                        es.gridLineHoveredIsColumn = column;
                        if (es.gridLineHovered >= 0) {
                            ImGui::SetMouseCursor(column ? ImGuiMouseCursor_ResizeEW
                                                         : ImGuiMouseCursor_ResizeNS);
                        }
                    }

                    // Right-click a point: its Linear / Spline menu.
                    if (canvasRightPress && meshHoveredVertex >= 0) {
                        if (meshHoveredVertex != meshSelectedVertex) es.meshSelectedHandle = -1;
                        meshSelectedVertex = es.gridPointMenuVertex = meshHoveredVertex;
                        ImGui::OpenPopup("##GridPointMenu");
                    }

                    if (canvasPress && es.gridLineHovered >= 0 && mayStartDrag()) {
                        es.gridLineDragIndex = es.gridLineHovered;
                        es.gridLineDragIsColumn = es.gridLineHoveredIsColumn;
                        es.gridLineDragBefore = captureGrid(geometry);
                        es.gridLineDragFrom = geometry;
                    } else if (canvasPress && es.meshHoveredHandle >= 0 && !insertArmed && mayStartDrag()) {
                        es.meshDraggedHandle = es.meshSelectedHandle = es.meshHoveredHandle;
                        es.meshHandleDragStartMousePos = mouseNorm;
                        es.meshHandleDragStartPos =
                            geometry.gridHandlePosition(meshSelectedVertex, es.meshHoveredHandle);
                        es.meshHandleDragBefore = geometry.getVertexTangent(meshSelectedVertex);
                    } else if (canvasPress && meshHoveredVertex >= 0 && !insertArmed &&
                               mayStartDrag()) {
                        if (meshHoveredVertex != meshSelectedVertex) es.meshSelectedHandle = -1;
                        meshDraggedVertex = meshHoveredVertex;
                        meshSelectedVertex = meshHoveredVertex;
                        meshDragStartMousePos = mouseNorm;
                        meshDragStartOffset = geometry.getMeshVertexByIndex(meshHoveredVertex);
                    }
                }

                // Keyboard: Tab cycles vertices, H their grid-line handles, arrows nudge.
                // Not mid handle-drag, which edits the selected vertex's handle.
                if (es.meshDraggedHandle < 0 && canvasOwnsEditingKeys(true)) {
                    applyMeshEditKeys(readKeys(), es.quadRef(*selectedQuad), geometry,
                                      es.commandHistory, meshSelectedVertex,
                                      es.meshSelectedHandle, locked);
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

    // A grid point's menu (right-click on the canvas, owner 2026-11-23): Linear or
    // Spline for it or for every point, one undo step each.
    if (ImGui::BeginPopup("##GridPointMenu")) {
        Quad* quad = quadManager.getPrimarySelectedQuad();
        const int point = es.gridPointMenuVertex;
        if (quad && quad->getGeometry().getMeshMode() == MeshMode::MESH_EDIT && point >= 0 &&
            point < quad->getGeometry().getVertexCount()) {
            QuadGeometry& g = quad->getGeometry();
            const bool editable = !quad->isLocked() && !es.performLock.isOn();
            auto step = [&](const char* name, auto&& change) {
                const GridSnapshot before = captureGrid(g);
                change();
                pushGridChange(es.commandHistory, es.quadRef(*quad), name, before, captureGrid(g));
            };
            const bool linear = g.isVertexLinear(point);
            ImGui::TextDisabled("Point %d", point + 1);
            if (ImGui::MenuItem("Spline", nullptr, !linear, editable) && linear) {
                step("Point Spline", [&] { g.setVertexLinear(point, false); });
            }
            if (ImGui::MenuItem("Linear", nullptr, linear, editable) && !linear) {
                step("Point Linear", [&] { g.setVertexLinear(point, true); });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("All points Spline", nullptr, false, editable)) {
                step("All Points Spline", [&] { g.setAllVerticesLinear(false); });
            }
            if (ImGui::MenuItem("All points Linear", nullptr, false, editable)) {
                step("All Points Linear", [&] { g.setAllVerticesLinear(true); });
            }
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Disable depth test for 2D rendering
    glDisable(GL_DEPTH_TEST);

    // Constrain drawing to the canvas region and apply the view transform.
    glViewport(vpX, vpY, vpW, vpH);
    glEnable(GL_SCISSOR_TEST);
    glScissor(vpX, vpY, vpW, vpH);

    // Restore full-framebuffer viewport + drop scissor so the output frames and
    // ImGui render over the whole window unaffected; re-enable depth test.
    const auto restoreViewport = [&]() {
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, fbWidth, fbHeight);
        glEnable(GL_DEPTH_TEST);
    };

    // Render all quads
    if (m_renderer) {
        m_renderer->setViewTransform(glm::vec2(viewScaleX, viewScaleY),
                                     glm::vec2(viewOffsetX, viewOffsetY), canvasAspect);
        m_renderer->beginFrame();

        // Canvas frame border so the projector-frame bounds are visible.
        m_renderer->renderCanvasBorder(glm::vec4(0.45f, 0.45f, 0.5f, 0.9f), 1.0f);

        // Slice warp editing: the output's frame and the warp handles, no quads.
        if (warpOutput && m_outputManager) {
            drawSliceWarpCanvas(es, *m_renderer, *m_outputManager, *warpOutput);
            m_renderer->endFrame();
            m_renderer->resetViewTransform();
            restoreViewport();
            return;
        }

        // Resolve every quad's texture through the shared builder: image via
        // TextureBinding::mediaId -> SourceManager, video via the legacy wrapper
        // (ADR-040 P2). The output frames use the same builder with the projector's
        // missing-media look.
        auto texturePtrs = buildQuadTextureMap(es, ctx.sourceManager, SurfaceView::Editor);

        // Calibrating: the projector's output frame, no quad handles (TD-29). The
        // calibration points are drawn over it by CalibrationPanel::drawCanvasOverlay.
        if (calibrationView && m_outputManager) {
            // The frame [0,1] on the framebuffer, in GL pixels (origin bottom-left).
            const float left = regionX + viewOffsetX * regionW;
            const float top = regionY + viewOffsetY * regionH;
            const float right = left + viewScaleX * regionW;
            const float bottom = top + viewScaleY * regionH;
            const IVec2 frameMin(static_cast<int>(std::floor(left * dpiX)),
                                 static_cast<int>(std::floor((winHeight - bottom) * dpiY)));
            const IVec2 frameMax(static_cast<int>(std::ceil(right * dpiX)),
                                 static_cast<int>(std::ceil((winHeight - top) * dpiY)));
            drawCalibrationView(es, *m_renderer, *m_outputManager, *calibrationView, texturePtrs,
                                frameMin, frameMax);
            m_renderer->renderCanvasBorder(glm::vec4(0.45f, 0.45f, 0.5f, 0.9f), 1.0f);
            m_renderer->endFrame();
            m_renderer->resetViewTransform();
            restoreViewport();
            return;
        }

        // 3D objects as the canvas's projector sees them (WYSIWYG with the output),
        // under the quads. The scissor limits the depth clear to the canvas region.
        glClear(GL_DEPTH_BUFFER_BIT);
        m_renderer->renderMeshObjects(es.activeScene().getObjects(), *canvasProjector,
                                      texturePtrs);

        m_renderer->renderQuads(quadManager, texturePtrs);
        drawSliceInputOverlay(es, *m_renderer, m_outputManager);

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
    restoreViewport();
}

}  // namespace quadmapper
