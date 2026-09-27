#pragma once

#include "ShaderProgram.hpp"
#include "Texture.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include "scene/MeshObject.hpp"
#include "scene/MeshSubdivision.hpp"
#include "scene/ProjectorCamera.hpp"
#include "scene/Slice.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace quadmapper {

struct GpuMesh;  // a MeshObject's cached VAO/VBO/EBO (Renderer.cpp)
struct GpuQuad;  // a quad's cached tessellation VAO/VBO/EBO (Renderer.cpp)

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool initialize();
    void shutdown();

    void beginFrame();
    // Surface draws (renderQuad, renderMeshObject) leave their program, texture and
    // VAO bound for the next draw; this unbinds them. Call at the end of a pass,
    // before other GL code runs.
    void endFrame();

    // Render a single quad
    void renderQuad(const Quad& quad, const Texture* texture);

    // Render a quad with selection highlight
    void renderQuad(const Quad& quad, const Texture* texture, bool selected);

    // Render all quads from a QuadManager with selection highlighting
    void renderQuads(const QuadManager& quadManager,
                     const std::unordered_map<UUID, Texture*>& textures);

    // Draw one 3D object as `camera` sees it (ADR-052), textured through the mesh's
    // UVs (planar from its XY bounds when it has none) by quad.frag, so crop and
    // opacity behave as for quads. The view transform and output rotation apply as
    // for quads. nearZ/farZ must be shared by every object in a pass, or depth does
    // not compare between them. Depth state is the caller's (renderMeshObjects sets
    // it). The GPU mesh is cached per object and re-uploaded only when the object's
    // mesh revision changes (MeshObject::getMeshRevision, TD-15).
    void renderMeshObject(const MeshObject& object, const ProjectorCamera& camera,
                          const Texture* texture, float nearZ, float farZ);

    // Draw every visible object through `camera` with the depth test on (off again
    // afterwards) and culling off; textures come from `textures` by object id. Call
    // before the quads, after clearing the depth buffer for the frame.
    void renderMeshObjects(const std::vector<std::unique_ptr<MeshObject>>& objects,
                           const ProjectorCamera& camera,
                           const std::unordered_map<UUID, Texture*>& textures);

    // Draw one slice of a scene canvas into the bound output frame (ADR-047 §4): the
    // slice's input rectangle of `canvasTexture` through its warp, faded by its blend
    // zones, lifted by its black level. The texture holds the canvas (canvasSize
    // pixels, unrotated) drawn with output rotation `canvasRotation`. The output
    // rotation applies as for quads. `outputSize` is the frame's unrotated (effective)
    // size: an unwarped slice whose input is exactly that size copies texel for texel,
    // so an output showing a whole canvas of its own size, drawn in its rotation, is
    // unchanged by the pass. The caller sets the blend function (the canvas is
    // composited with ONE, ONE_MINUS_SRC_ALPHA). The warp's mesh is cached by slice
    // id, like a quad's. The view transform applies (the slice editor draws on the
    // editor canvas); it is the identity for output frames.
    void renderSlice(const Slice& slice, uint32_t canvasTexture, IVec2 canvasSize,
                     Rotation canvasRotation, IVec2 outputSize);

    // Render selection outline for a quad
    void renderQuadOutline(const Quad& quad, const glm::vec4& color, float lineWidth = 2.0f);

    // Render the control-grid wireframe for mesh-edit feedback: each division line
    // drawn along the surface (ADR-049), so it bends as the warp does.
    void renderMeshGrid(const Quad& quad, const glm::vec4& color, float lineWidth = 1.0f);

    // Render the grid tangent handles of control vertex `vertex` (ADR-049 §3) with
    // lines from the vertex. Handle indices are GridHandle values, -1 for none.
    void renderGridHandles(const Quad& quad, int vertex, int selectedHandle = -1,
                           int hoveredHandle = -1, int draggedHandle = -1);

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

    // Output rotation (QM-087): turn the whole rendered frame clockwise so content
    // laid out on the editor canvas (whose aspect is the output's rotated, effective
    // resolution) appears upright on a projector mounted on its side. Applies to
    // renderQuad and renderMeshObject only; editor overlays are never drawn to
    // outputs. Default R0.
    void setOutputRotation(Rotation rotation) { outputRotation_ = rotation; }

    // Create a default white texture for quads without texture
    Texture* getDefaultTexture() { return &defaultTexture_; }

private:
    void createOutlineBuffers();
    void createHandleBuffers();
    void createCornerIndexBuffer();
    // Fill vertices_ and indices_ with the geometry's render mesh (MeshSubdivision).
    // Returns true when it is a kCornerPatchDivisions-square grid, whose triangles are
    // the ones in cornerIndexBuffer_.
    bool tessellate(const QuadGeometry& geometry);
    // Upload gridVertices_ as GL_LINES and draw them with the outline shader.
    void drawGridLines(const glm::vec4& color, float lineWidth);
    // The GPU mesh of a quad or slice (by its id), tessellated and uploaded on first
    // draw or when its geometry changed since the last upload.
    const GpuQuad& surfaceBuffersFor(const UUID& id, const QuadGeometry& geometry);
    void updateOutlineBuffers(const Quad& quad);
    void createDefaultTexture();

    // Bind a surface program (quad or 3D mesh) unless the previous surface draw left
    // it bound. Only valid between endFrame() calls: other GL code must not change
    // the program in the middle of a pass.
    void useSurfaceProgram(ShaderProgram& shader);
    // Unbind the program and forget which surface program was bound. Every other
    // draw path unbinds through this, so the next surface draw binds again.
    void unbindProgram();

    // Push the current view transform into a bound shader (u_ViewScale/u_ViewOffset).
    void applyViewUniforms(ShaderProgram& shader);

    // Set quad.frag's uniforms (UV transform, crop, colour, soft edge, opacity,
    // colour space) on a bound program and bind the texture to unit 0.
    void applySurfaceUniforms(ShaderProgram& shader, const TextureBinding& binding,
                              float opacity, const ColorCorrection& color,
                              const SoftEdge& edge, const Texture* texture);

    // The object's GPU mesh, uploaded on first use or when its mesh revision
    // changed; nullptr when the mesh is empty or has out-of-range indices.
    const GpuMesh* meshBuffersFor(const MeshObject& object);

    ShaderProgram quadShader_;
    ShaderProgram mesh3dShader_;  // mesh3d.vert + quad.frag (ADR-052)
    ShaderProgram outlineShader_;
    ShaderProgram handleShader_;
    ShaderProgram sliceShader_;  // slice.vert + slice.frag (ADR-047)
    Texture defaultTexture_;

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

    // Tessellation scratch (reused, so re-tessellating allocates nothing)
    std::vector<MeshVertex> vertices_;
    std::vector<uint32_t> indices_;

    // A quad whose surface is its corner pin tessellates to an N×N grid; every such
    // grid shares these indices (TD-22), and so does any mesh-edit one of that size.
    static constexpr int kCornerPatchDivisions = RENDER_MIN_SEGMENTS;
    uint32_t cornerIndexBuffer_ = 0;
    int cornerIndexCount_ = 0;

    // Buffers for outline
    std::vector<float> outlineVertices_;

    // 3D objects' GPU meshes by object id, and quads' by quad id. VAOs are per
    // context, like this Renderer. meshPass_ counts scene passes (renderMeshObjects
    // calls, which every scene render path makes first); an entry unused for
    // kCacheIdlePasses passes is dropped, so a deleted object or quad does not keep
    // its buffers.
    std::unordered_map<UUID, std::unique_ptr<GpuMesh>> meshCache_;
    std::unordered_map<UUID, std::unique_ptr<GpuQuad>> quadCache_;
    uint64_t meshPass_ = 0;
    static constexpr uint64_t kCacheIdlePasses = 600;

    // Editor view transform state (ADR-041). Identity by default so any render path
    // that does not set it (outputs, previews) draws the full frame unchanged.
    glm::vec2 viewScale_{1.0f, 1.0f};
    glm::vec2 viewOffset_{0.0f, 0.0f};
    float canvasAspect_ = 0.0f;  // <= 0: derive handle aspect from the GL viewport
    Rotation outputRotation_ = Rotation::R0;

    uint32_t boundProgram_ = 0;  // surface program a draw left bound; 0 = unknown/none

    bool initialized_ = false;
};

} // namespace quadmapper
