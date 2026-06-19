#pragma once

#include "ShaderProgram.hpp"
#include "Texture.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <cstdint>
#include <unordered_map>

namespace quadmapper {

// Vertex structure for quad mesh
struct QuadVertex {
    float position[2];
    float texCoord[2];
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool initialize();
    void shutdown();

    void beginFrame();
    void endFrame();

    // Render a single quad
    void renderQuad(const Quad& quad, const Texture* texture);

    // Render a quad with selection highlight
    void renderQuad(const Quad& quad, const Texture* texture, bool selected);

    // Render all quads from a QuadManager with selection highlighting
    void renderQuads(const QuadManager& quadManager,
                     const std::unordered_map<UUID, Texture*>& textures);

    // Render selection outline for a quad
    void renderQuadOutline(const Quad& quad, const glm::vec4& color, float lineWidth = 2.0f);

    // Render the control-grid wireframe (division lines) for mesh-edit feedback.
    void renderMeshGrid(const Quad& quad, const glm::vec4& color, float lineWidth = 1.0f);

    // Render corner handles for corner-pin warping (CORNERS_ONLY mode)
    // hoveredIndex: index of handle under mouse (-1 if none, 0-3 for corners)
    // draggedIndex: index of handle being dragged (-1 if none)
    // selectedIndex: index of keyboard-selected handle (-1 if none)
    void renderCornerHandles(const Quad& quad, int hoveredIndex = -1, int draggedIndex = -1, int selectedIndex = -1);

    // Render mesh vertex handles for editing (MESH_EDIT mode)
    // hoveredIndex: index of handle under mouse (-1 if none)
    // draggedIndex: index of handle being dragged (-1 if none)
    // selectedIndex: index of keyboard-selected handle (-1 if none)
    void renderMeshHandles(const Quad& quad, int hoveredIndex = -1, int draggedIndex = -1, int selectedIndex = -1);

    // Render snap indicator at the given position (shows when vertex is snapping to another)
    void renderSnapIndicator(const Vec2& position);

    // Render the 8 bezier edge-control handles (+ tangent lines to the corners) for a
    // bezier-enabled quad. hovered/dragged are control indices (0..7) or -1 (ADR-043).
    void renderBezierHandles(const Quad& quad, int hovered = -1, int dragged = -1);

    // Render the canvas-frame border (the [0,1] projector rectangle) so the user can
    // see the bounds of the output frame in the editor. Uses the active view transform.
    void renderCanvasBorder(const glm::vec4& color, float lineWidth = 1.0f);

    // Editor view transform (ADR-041). The on-canvas editor sets this so [0,1] canvas
    // space is letterboxed (by canvasAspect) and zoom/pan'd into the viewport region;
    // it must reset to identity afterward so the per-output windows and preview FBOs
    // (which never call this) render the full frame unchanged. canvasAspect > 0 also
    // drives the round-handle correction; <= 0 falls back to the GL viewport aspect.
    void setViewTransform(const glm::vec2& scale, const glm::vec2& offset, float canvasAspect);
    void resetViewTransform();

    // Create a default white texture for quads without texture
    Texture* getDefaultTexture() { return &defaultTexture_; }

private:
    void createQuadBuffers();
    void createOutlineBuffers();
    void createHandleBuffers();
    void updateQuadBuffers(const Quad& quad);
    void updateOutlineBuffers(const Quad& quad);
    void createDefaultTexture();

    // Push the current view transform into a bound shader (u_ViewScale/u_ViewOffset).
    void applyViewUniforms(ShaderProgram& shader);

    ShaderProgram quadShader_;
    ShaderProgram outlineShader_;
    ShaderProgram handleShader_;
    Texture defaultTexture_;

    // OpenGL objects for quad mesh
    uint32_t quadVAO_ = 0;
    uint32_t quadVBO_ = 0;
    uint32_t quadEBO_ = 0;

    // OpenGL objects for outline
    uint32_t outlineVAO_ = 0;
    uint32_t outlineVBO_ = 0;

    // OpenGL objects for the mesh-grid wireframe (dynamic, edit-mode only)
    uint32_t gridVAO_ = 0;
    uint32_t gridVBO_ = 0;
    std::vector<float> gridVertices_;

    // OpenGL objects for handle circles
    uint32_t handleVAO_ = 0;
    uint32_t handleVBO_ = 0;

    // Buffers for mesh data
    std::vector<QuadVertex> vertices_;
    std::vector<uint32_t> indices_;

    // Buffers for outline
    std::vector<float> outlineVertices_;

    // Editor view transform state (ADR-041). Identity by default so any render path
    // that does not set it (outputs, previews) draws the full frame unchanged.
    glm::vec2 viewScale_{1.0f, 1.0f};
    glm::vec2 viewOffset_{0.0f, 0.0f};
    float canvasAspect_ = 0.0f;  // <= 0: derive handle aspect from the GL viewport

    bool initialized_ = false;
};

} // namespace quadmapper
