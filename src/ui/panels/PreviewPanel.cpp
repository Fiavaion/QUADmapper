#include "PreviewPanel.hpp"
#include "scene/SceneManager.hpp"
#include "scene/Scene.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadGeometry.hpp"
#include "scene/QuadManager.hpp"
#include "renderer/Renderer.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace quadmapper {

PreviewPanel::PreviewPanel() : Panel("Preview") {}

void PreviewPanel::render() {
    if (!visible_) return;

    ImGui::Begin(title_.c_str(), &visible_);

    if (!sceneManager_) {
        ImGui::TextDisabled("No scene manager connected");
        ImGui::End();
        return;
    }

    renderToolbar();
    ImGui::Separator();
    renderPreviewArea();

    ImGui::End();
}

void PreviewPanel::renderToolbar() {
    // Edit mode buttons
    bool selectMode = (editMode_ == PreviewEditMode::SELECT);
    bool cornerMode = (editMode_ == PreviewEditMode::CORNERS);
    bool meshMode = (editMode_ == PreviewEditMode::MESH);

    if (selectMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("Select")) {
        editMode_ = PreviewEditMode::SELECT;
    }
    if (selectMode) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click to select quads (V)");
    }

    ImGui::SameLine();

    if (cornerMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("Corners")) {
        editMode_ = PreviewEditMode::CORNERS;
    }
    if (cornerMode) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Drag quad corners (C)");
    }

    ImGui::SameLine();

    if (meshMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("Mesh")) {
        editMode_ = PreviewEditMode::MESH;
    }
    if (meshMode) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Drag mesh vertices (M)");
    }

    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();

    // View controls
    ImGui::Checkbox("Grid", &showGrid_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Show/hide grid (G)");
    }

    ImGui::SameLine();

    if (ImGui::Button("Reset View")) {
        resetView();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Reset zoom and pan (Home)");
    }

    ImGui::SameLine();

    // Zoom display
    ImGui::TextDisabled("Zoom: %.0f%%", zoom_ * 100.0f);
}

void PreviewPanel::renderPreviewArea() {
    // Get available space for the preview canvas
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.x < 50.0f || canvasSize.y < 50.0f) {
        return;
    }

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    // Create invisible button for mouse interaction
    ImGui::InvisibleButton("canvas", canvasSize,
        ImGuiButtonFlags_MouseButtonLeft |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);

    bool isHovered = ImGui::IsItemHovered();
    (void)ImGui::IsItemActive();  // Reserved for future use

    // Handle input
    if (isHovered) {
        handleZoom(canvasPos, canvasSize);
    }
    if (isHovered || isPanning_) {
        handlePan();
    }
    if (isHovered || isDragging_) {
        handleMouseInput(canvasPos, canvasSize);
    }

    // Draw canvas background
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasPos,
        ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
        IM_COL32(30, 30, 30, 255));

    // Draw grid
    if (showGrid_) {
        renderGrid(canvasPos, canvasSize);
    }

    // Render scene content
    renderSceneContent(canvasPos, canvasSize);

    // Render selection handles
    renderSelectionHandles(canvasPos, canvasSize);

    // Draw canvas border
    drawList->AddRect(canvasPos,
        ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
        IM_COL32(80, 80, 80, 255));
}

void PreviewPanel::renderSceneContent(ImVec2 canvasPos, ImVec2 canvasSize) {
    Scene* scene = sceneManager_->getActiveScene();
    if (!scene) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const QuadManager& quadManager = scene->getQuadManager();
    const auto& selectedIds = quadManager.getSelectedIds();

    // Render quads in z-order
    auto quads = quadManager.getQuadsByZOrder();
    for (Quad* quad : quads) {
        if (!quad->isVisible()) continue;

        const QuadGeometry& geom = quad->getGeometry();
        const auto& corners = geom.getCorners();
        bool isSelected = selectedIds.count(quad->getId()) > 0;

        // Convert corners to screen positions
        ImVec2 screenCorners[4];
        for (int i = 0; i < 4; i++) {
            screenCorners[i] = sceneToScreen(corners[i], canvasPos, canvasSize);
        }

        // Draw quad fill
        ImU32 fillColor = isSelected ?
            IM_COL32(100, 150, 200, 100) :
            IM_COL32(80, 80, 80, 100);
        drawList->AddQuadFilled(
            screenCorners[0], screenCorners[1],
            screenCorners[2], screenCorners[3],
            fillColor);

        // Draw quad outline
        ImU32 outlineColor = isSelected ?
            IM_COL32(100, 200, 255, 255) :
            IM_COL32(150, 150, 150, 255);
        drawList->AddQuad(
            screenCorners[0], screenCorners[1],
            screenCorners[2], screenCorners[3],
            outlineColor, isSelected ? 2.0f : 1.0f);

        // Draw mesh lines if in mesh edit mode and quad has mesh modifications
        if (editMode_ == PreviewEditMode::MESH && isSelected &&
            geom.getMeshMode() == MeshMode::MESH_EDIT) {
            int resolution = geom.getMeshResolution();
            auto meshPositions = geom.generateMeshPositions();

            // Draw horizontal lines
            for (int row = 0; row <= resolution; row++) {
                for (int col = 0; col < resolution; col++) {
                    int idx1 = row * (resolution + 1) + col;
                    int idx2 = row * (resolution + 1) + col + 1;
                    ImVec2 p1 = sceneToScreen(meshPositions[idx1], canvasPos, canvasSize);
                    ImVec2 p2 = sceneToScreen(meshPositions[idx2], canvasPos, canvasSize);
                    drawList->AddLine(p1, p2, IM_COL32(100, 100, 100, 128), 1.0f);
                }
            }

            // Draw vertical lines
            for (int row = 0; row < resolution; row++) {
                for (int col = 0; col <= resolution; col++) {
                    int idx1 = row * (resolution + 1) + col;
                    int idx2 = (row + 1) * (resolution + 1) + col;
                    ImVec2 p1 = sceneToScreen(meshPositions[idx1], canvasPos, canvasSize);
                    ImVec2 p2 = sceneToScreen(meshPositions[idx2], canvasPos, canvasSize);
                    drawList->AddLine(p1, p2, IM_COL32(100, 100, 100, 128), 1.0f);
                }
            }
        }

        // Draw quad name label
        if (isSelected) {
            Vec2 center = (corners[0] + corners[1] + corners[2] + corners[3]) * 0.25f;
            ImVec2 labelPos = sceneToScreen(center, canvasPos, canvasSize);
            ImVec2 textSize = ImGui::CalcTextSize(quad->getName().c_str());
            labelPos.x -= textSize.x * 0.5f;
            labelPos.y -= textSize.y * 0.5f;
            drawList->AddText(labelPos, IM_COL32(255, 255, 255, 200), quad->getName().c_str());
        }
    }
}

void PreviewPanel::renderGrid(ImVec2 canvasPos, ImVec2 canvasSize) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Grid spacing in scene coordinates
    float gridSpacing = 0.1f;  // 10% of normalized space

    // Adjust grid spacing based on zoom
    while (gridSpacing * zoom_ * std::min(canvasSize.x, canvasSize.y) < 20.0f) {
        gridSpacing *= 2.0f;
    }
    while (gridSpacing * zoom_ * std::min(canvasSize.x, canvasSize.y) > 100.0f) {
        gridSpacing *= 0.5f;
    }

    ImU32 gridColor = IM_COL32(60, 60, 60, 128);
    ImU32 axisColor = IM_COL32(100, 100, 100, 200);

    // Calculate visible range in scene coordinates
    Vec2 topLeft = screenToScene(canvasPos, canvasPos, canvasSize);
    Vec2 bottomRight = screenToScene(
        ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
        canvasPos, canvasSize);

    // Draw vertical lines
    float startX = std::floor(topLeft.x / gridSpacing) * gridSpacing;
    for (float x = startX; x <= bottomRight.x; x += gridSpacing) {
        ImVec2 p1 = sceneToScreen(Vec2(x, topLeft.y), canvasPos, canvasSize);
        ImVec2 p2 = sceneToScreen(Vec2(x, bottomRight.y), canvasPos, canvasSize);
        bool isAxis = std::abs(x) < 0.001f || std::abs(x - 1.0f) < 0.001f;
        drawList->AddLine(p1, p2, isAxis ? axisColor : gridColor, isAxis ? 1.5f : 1.0f);
    }

    // Draw horizontal lines
    float startY = std::floor(topLeft.y / gridSpacing) * gridSpacing;
    for (float y = startY; y <= bottomRight.y; y += gridSpacing) {
        ImVec2 p1 = sceneToScreen(Vec2(topLeft.x, y), canvasPos, canvasSize);
        ImVec2 p2 = sceneToScreen(Vec2(bottomRight.x, y), canvasPos, canvasSize);
        bool isAxis = std::abs(y) < 0.001f || std::abs(y - 1.0f) < 0.001f;
        drawList->AddLine(p1, p2, isAxis ? axisColor : gridColor, isAxis ? 1.5f : 1.0f);
    }
}

void PreviewPanel::renderSelectionHandles(ImVec2 canvasPos, ImVec2 canvasSize) {
    Scene* scene = sceneManager_->getActiveScene();
    if (!scene) return;

    QuadManager& quadManager = scene->getQuadManager();
    Quad* primaryQuad = quadManager.getPrimarySelectedQuad();

    if (!primaryQuad) return;

    // Render appropriate handles based on edit mode
    if (editMode_ == PreviewEditMode::CORNERS) {
        renderCornerHandles(primaryQuad, canvasPos, canvasSize);
    } else if (editMode_ == PreviewEditMode::MESH) {
        if (primaryQuad->getGeometry().getMeshMode() == MeshMode::MESH_EDIT) {
            renderMeshHandles(primaryQuad, canvasPos, canvasSize);
        } else {
            // Show message that mesh mode needs to be enabled
            ImGui::SetTooltip("Enable Mesh Edit mode in the Inspector to edit mesh vertices");
        }
    }
}

void PreviewPanel::renderCornerHandles(Quad* quad, ImVec2 canvasPos, ImVec2 canvasSize) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const QuadGeometry& geom = quad->getGeometry();
    const auto& corners = geom.getCorners();

    const char* cornerLabels[] = {"TL", "TR", "BR", "BL"};

    for (int i = 0; i < 4; i++) {
        ImVec2 screenPos = sceneToScreen(corners[i], canvasPos, canvasSize);

        bool isHovered = (hoveredCornerIndex_ == i);
        bool isDragged = (dragCornerIndex_ == i);

        ImU32 color = isDragged ? IM_COL32(255, 200, 0, 255) :
                      (isHovered ? IM_COL32(255, 255, 100, 255) :
                                   IM_COL32(255, 255, 255, 255));

        // Draw handle
        drawList->AddCircleFilled(screenPos, CORNER_HANDLE_SIZE, color);
        drawList->AddCircle(screenPos, CORNER_HANDLE_SIZE, IM_COL32(0, 0, 0, 255), 0, 2.0f);

        // Draw label
        ImVec2 labelPos(screenPos.x - 6.0f, screenPos.y - 5.0f);
        drawList->AddText(labelPos, IM_COL32(0, 0, 0, 255), cornerLabels[i]);
    }
}

void PreviewPanel::renderMeshHandles(Quad* quad, ImVec2 canvasPos, ImVec2 canvasSize) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const QuadGeometry& geom = quad->getGeometry();
    auto meshPositions = geom.generateMeshPositions();
    int resolution = geom.getMeshResolution();

    for (size_t i = 0; i < meshPositions.size(); i++) {
        ImVec2 screenPos = sceneToScreen(meshPositions[i], canvasPos, canvasSize);

        bool isHovered = (hoveredMeshIndex_ == static_cast<int>(i));
        bool isDragged = (dragMeshIndex_ == static_cast<int>(i));

        // Corners have different color
        bool isCorner = (i == 0 ||
                        i == static_cast<size_t>(resolution) ||
                        i == static_cast<size_t>((resolution + 1) * (resolution + 1) - resolution - 1) ||
                        i == meshPositions.size() - 1);

        ImU32 color = isDragged ? IM_COL32(255, 200, 0, 255) :
                      (isHovered ? IM_COL32(255, 255, 100, 255) :
                      (isCorner ? IM_COL32(255, 100, 100, 255) :
                                  IM_COL32(200, 200, 200, 255)));

        // Draw handle
        drawList->AddCircleFilled(screenPos, MESH_HANDLE_SIZE, color);
        drawList->AddCircle(screenPos, MESH_HANDLE_SIZE, IM_COL32(0, 0, 0, 200), 0, 1.0f);
    }
}

void PreviewPanel::handleMouseInput(ImVec2 canvasPos, ImVec2 canvasSize) {
    ImVec2 mousePos = ImGui::GetMousePos();
    Scene* scene = sceneManager_->getActiveScene();
    if (!scene) return;

    QuadManager& quadManager = scene->getQuadManager();
    Quad* primaryQuad = quadManager.getPrimarySelectedQuad();

    // Update hovered state
    hoveredCornerIndex_ = -1;
    hoveredMeshIndex_ = -1;

    if (primaryQuad && !isDragging_) {
        if (editMode_ == PreviewEditMode::CORNERS) {
            hoveredCornerIndex_ = findCornerHandle(primaryQuad, mousePos, canvasPos, canvasSize);
        } else if (editMode_ == PreviewEditMode::MESH &&
                   primaryQuad->getGeometry().getMeshMode() == MeshMode::MESH_EDIT) {
            hoveredMeshIndex_ = findMeshHandle(primaryQuad, mousePos, canvasPos, canvasSize);
        }
    }

    // Handle mouse button press
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Vec2 scenePos = screenToScene(mousePos, canvasPos, canvasSize);
        dragStartScenePos_ = scenePos;

        if (editMode_ == PreviewEditMode::SELECT) {
            // Select quad at click position
            Quad* clickedQuad = findQuadAtPosition(scenePos);
            if (clickedQuad) {
                if (ImGui::GetIO().KeyCtrl) {
                    quadManager.toggleSelection(clickedQuad->getId());
                } else if (ImGui::GetIO().KeyShift) {
                    quadManager.addToSelection(clickedQuad->getId());
                } else {
                    quadManager.selectQuad(clickedQuad->getId());
                }
            } else {
                quadManager.deselectAll();
            }
        } else if (editMode_ == PreviewEditMode::CORNERS && primaryQuad) {
            if (hoveredCornerIndex_ >= 0) {
                isDragging_ = true;
                dragCornerIndex_ = hoveredCornerIndex_;
            }
        } else if (editMode_ == PreviewEditMode::MESH && primaryQuad) {
            if (hoveredMeshIndex_ >= 0) {
                isDragging_ = true;
                dragMeshIndex_ = hoveredMeshIndex_;
            }
        }
    }

    // Handle dragging
    if (isDragging_ && primaryQuad) {
        Vec2 currentScenePos = screenToScene(mousePos, canvasPos, canvasSize);

        if (dragCornerIndex_ >= 0) {
            applyCornerDrag(primaryQuad, dragCornerIndex_, currentScenePos);
        } else if (dragMeshIndex_ >= 0) {
            applyMeshDrag(primaryQuad, dragMeshIndex_, currentScenePos);
        }
    }

    // Handle mouse release
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        isDragging_ = false;
        dragCornerIndex_ = -1;
        dragMeshIndex_ = -1;
    }

    lastMousePos_ = mousePos;
}

void PreviewPanel::handleZoom(ImVec2 canvasPos, ImVec2 canvasSize) {
    float wheel = ImGui::GetIO().MouseWheel;
    if (std::abs(wheel) < 0.01f) return;

    ImVec2 mousePos = ImGui::GetMousePos();

    // Calculate scene position under mouse before zoom
    Vec2 scenePosBefore = screenToScene(mousePos, canvasPos, canvasSize);

    // Apply zoom
    float zoomDelta = wheel * ZOOM_SPEED * zoom_;
    zoom_ = std::clamp(zoom_ + zoomDelta, MIN_ZOOM, MAX_ZOOM);

    // Calculate scene position under mouse after zoom
    Vec2 scenePosAfter = screenToScene(mousePos, canvasPos, canvasSize);

    // Adjust pan to keep the same scene position under mouse
    pan_ += (scenePosBefore - scenePosAfter);
}

void PreviewPanel::handlePan() {
    // Start panning with middle mouse
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        isPanning_ = true;
        lastMousePos_ = ImGui::GetMousePos();
    }

    // Continue panning
    if (isPanning_ && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 delta(mousePos.x - lastMousePos_.x, mousePos.y - lastMousePos_.y);

        // Convert delta to scene space (invert for natural panning)
        float sceneScale = zoom_ * 0.5f;  // Simplified conversion
        pan_.x -= delta.x / (sceneScale * 800.0f);  // Approximate
        pan_.y -= delta.y / (sceneScale * 600.0f);

        lastMousePos_ = mousePos;
    }

    // Stop panning
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
        isPanning_ = false;
    }
}

Vec2 PreviewPanel::screenToScene(ImVec2 screenPos, ImVec2 canvasPos, ImVec2 canvasSize) const {
    // Convert screen position to normalized canvas position (0-1)
    float canvasX = (screenPos.x - canvasPos.x) / canvasSize.x;
    float canvasY = (screenPos.y - canvasPos.y) / canvasSize.y;

    // Apply inverse of zoom and pan
    float sceneX = (canvasX - 0.5f) / zoom_ + 0.5f + pan_.x;
    float sceneY = (canvasY - 0.5f) / zoom_ + 0.5f + pan_.y;

    return Vec2(sceneX, sceneY);
}

ImVec2 PreviewPanel::sceneToScreen(Vec2 scenePos, ImVec2 canvasPos, ImVec2 canvasSize) const {
    // Apply zoom and pan
    float canvasX = (scenePos.x - 0.5f - pan_.x) * zoom_ + 0.5f;
    float canvasY = (scenePos.y - 0.5f - pan_.y) * zoom_ + 0.5f;

    // Convert to screen coordinates
    float screenX = canvasPos.x + canvasX * canvasSize.x;
    float screenY = canvasPos.y + canvasY * canvasSize.y;

    return ImVec2(screenX, screenY);
}

Quad* PreviewPanel::findQuadAtPosition(Vec2 scenePos) {
    Scene* scene = sceneManager_->getActiveScene();
    if (!scene) return nullptr;

    // Check quads in reverse z-order (top-most first)
    auto quads = scene->getQuadManager().getQuadsByZOrderDescending();
    for (Quad* quad : quads) {
        if (!quad->isVisible()) continue;

        const auto& corners = quad->getGeometry().getCorners();

        // Simple point-in-quad test using cross products
        // Check if point is on the same side of all edges
        auto sign = [](Vec2 p1, Vec2 p2, Vec2 p3) {
            return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
        };

        // Split quad into two triangles for robust test
        bool inTri1 = true;
        bool inTri2 = true;

        float d1 = sign(scenePos, corners[0], corners[1]);
        float d2 = sign(scenePos, corners[1], corners[2]);
        float d3 = sign(scenePos, corners[2], corners[0]);

        bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        inTri1 = !(hasNeg && hasPos);

        d1 = sign(scenePos, corners[0], corners[2]);
        d2 = sign(scenePos, corners[2], corners[3]);
        d3 = sign(scenePos, corners[3], corners[0]);

        hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        inTri2 = !(hasNeg && hasPos);

        if (inTri1 || inTri2) {
            return quad;
        }
    }

    return nullptr;
}

int PreviewPanel::findCornerHandle(Quad* quad, ImVec2 screenPos, ImVec2 canvasPos, ImVec2 canvasSize) const {
    const auto& corners = quad->getGeometry().getCorners();

    for (int i = 0; i < 4; i++) {
        ImVec2 handlePos = sceneToScreen(corners[i], canvasPos, canvasSize);
        float dx = screenPos.x - handlePos.x;
        float dy = screenPos.y - handlePos.y;
        float dist = std::sqrt(dx * dx + dy * dy);

        if (dist <= HANDLE_HIT_RADIUS) {
            return i;
        }
    }

    return -1;
}

int PreviewPanel::findMeshHandle(Quad* quad, ImVec2 screenPos, ImVec2 canvasPos, ImVec2 canvasSize) const {
    auto meshPositions = quad->getGeometry().generateMeshPositions();

    for (size_t i = 0; i < meshPositions.size(); i++) {
        ImVec2 handlePos = sceneToScreen(meshPositions[i], canvasPos, canvasSize);
        float dx = screenPos.x - handlePos.x;
        float dy = screenPos.y - handlePos.y;
        float dist = std::sqrt(dx * dx + dy * dy);

        if (dist <= HANDLE_HIT_RADIUS) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void PreviewPanel::applyCornerDrag(Quad* quad, int cornerIndex, Vec2 newPos) {
    quad->getGeometry().setCorner(cornerIndex, newPos);
}

void PreviewPanel::applyMeshDrag(Quad* quad, int vertexIndex, Vec2 newPos) {
    // Calculate the offset from the bilinear position
    auto bilinearPositions = quad->getGeometry().generateBilinearPositions();
    if (vertexIndex >= 0 && static_cast<size_t>(vertexIndex) < bilinearPositions.size()) {
        Vec2 offset = newPos - bilinearPositions[vertexIndex];
        quad->getGeometry().setMeshVertexByIndex(vertexIndex, offset);
    }
}

void PreviewPanel::resetView() {
    zoom_ = 1.0f;
    pan_ = Vec2(0.0f);
}

} // namespace quadmapper
