#pragma once

#include "Panel.hpp"
#include "core/Types.hpp"
#include "core/UUID.hpp"
#include <imgui.h>
#include <vector>
#include <algorithm>

namespace quadmapper {

class SceneManager;
class Renderer;
class Quad;
class QuadGeometry;

/**
 * Editing mode for the preview panel.
 */
enum class PreviewEditMode {
    SELECT,      // Click to select quads
    CORNERS,     // Edit quad corners
    MESH         // Edit mesh vertices (when in MESH_EDIT mode)
};

/**
 * PreviewPanel displays a real-time preview of the active scene with
 * interactive editing capabilities for quad corners and mesh vertices.
 */
class PreviewPanel : public Panel {
public:
    PreviewPanel();

    /**
     * Set the scene manager for rendering and editing.
     */
    void setSceneManager(SceneManager* manager) { sceneManager_ = manager; }

    /**
     * Set the renderer for preview rendering.
     */
    void setRenderer(Renderer* renderer) { renderer_ = renderer; }

    /**
     * Render the panel.
     */
    void render() override;

    // Edit mode
    PreviewEditMode getEditMode() const { return editMode_; }
    void setEditMode(PreviewEditMode mode) { editMode_ = mode; }

    // View controls
    float getZoom() const { return zoom_; }
    void setZoom(float z) { zoom_ = std::clamp(z, MIN_ZOOM, MAX_ZOOM); }
    Vec2 getPan() const { return pan_; }
    void setPan(Vec2 p) { pan_ = p; }
    void resetView();

    // Grid display
    bool isGridVisible() const { return showGrid_; }
    void setGridVisible(bool visible) { showGrid_ = visible; }

private:
    // Render the toolbar with mode buttons and view controls
    void renderToolbar();

    // Render the preview area
    void renderPreviewArea();

    // Render the scene content
    void renderSceneContent(ImVec2 canvasPos, ImVec2 canvasSize);

    // Render grid overlay
    void renderGrid(ImVec2 canvasPos, ImVec2 canvasSize);

    // Render selection indicators
    void renderSelectionHandles(ImVec2 canvasPos, ImVec2 canvasSize);

    // Render quad corner handles for selected quad
    void renderCornerHandles(Quad* quad, ImVec2 canvasPos, ImVec2 canvasSize);

    // Render mesh vertex handles for selected quad
    void renderMeshHandles(Quad* quad, ImVec2 canvasPos, ImVec2 canvasSize);

    // Handle mouse input for editing
    void handleMouseInput(ImVec2 canvasPos, ImVec2 canvasSize);

    // Handle zoom with mouse wheel
    void handleZoom(ImVec2 canvasPos, ImVec2 canvasSize);

    // Handle pan with middle mouse
    void handlePan();

    // Convert screen position to normalized scene coordinates
    Vec2 screenToScene(ImVec2 screenPos, ImVec2 canvasPos, ImVec2 canvasSize) const;

    // Convert scene coordinates to screen position
    ImVec2 sceneToScreen(Vec2 scenePos, ImVec2 canvasPos, ImVec2 canvasSize) const;

    // Find quad at screen position (for selection)
    Quad* findQuadAtPosition(Vec2 scenePos);

    // Find corner handle at screen position (-1 if none)
    int findCornerHandle(Quad* quad, ImVec2 screenPos, ImVec2 canvasPos, ImVec2 canvasSize) const;

    // Find mesh vertex handle at screen position (-1 if none)
    int findMeshHandle(Quad* quad, ImVec2 screenPos, ImVec2 canvasPos, ImVec2 canvasSize) const;

    // Apply corner drag to quad geometry
    void applyCornerDrag(Quad* quad, int cornerIndex, Vec2 newPos);

    // Apply mesh vertex drag to quad geometry
    void applyMeshDrag(Quad* quad, int vertexIndex, Vec2 newPos);

    SceneManager* sceneManager_ = nullptr;
    Renderer* renderer_ = nullptr;

    // View state
    float zoom_ = 1.0f;
    Vec2 pan_ = Vec2(0.0f);
    PreviewEditMode editMode_ = PreviewEditMode::SELECT;
    bool showGrid_ = true;

    // Interaction state
    bool isDragging_ = false;
    bool isPanning_ = false;
    int dragCornerIndex_ = -1;
    int dragMeshIndex_ = -1;
    int hoveredCornerIndex_ = -1;
    int hoveredMeshIndex_ = -1;
    ImVec2 lastMousePos_;
    Vec2 dragStartScenePos_;

    // Handle sizes
    static constexpr float CORNER_HANDLE_SIZE = 10.0f;
    static constexpr float MESH_HANDLE_SIZE = 6.0f;
    static constexpr float HANDLE_HIT_RADIUS = 12.0f;

    // Zoom limits
    static constexpr float MIN_ZOOM = 0.1f;
    static constexpr float MAX_ZOOM = 10.0f;
    static constexpr float ZOOM_SPEED = 0.1f;
};

} // namespace quadmapper
