#pragma once

#include "ShaderProgram.hpp"
#include "Texture.hpp"
#include "scene/Quad.hpp"
#include "scene/QuadManager.hpp"
#include <memory>
#include <cstdint>
#include <unordered_map>

namespace quadmapper {

class SceneManager;

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

    // Render all quads in a scene
    void renderScene(const SceneManager& sceneManager);

    // Render selection outline for a quad
    void renderQuadOutline(const Quad& quad, const glm::vec4& color, float lineWidth = 2.0f);

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

    // Create a default white texture for quads without texture
    Texture* getDefaultTexture() { return &defaultTexture_; }

private:
    void createQuadBuffers();
    void createOutlineBuffers();
    void createHandleBuffers();
    void updateQuadBuffers(const Quad& quad);
    void updateOutlineBuffers(const Quad& quad);
    void createDefaultTexture();

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

    // OpenGL objects for handle circles
    uint32_t handleVAO_ = 0;
    uint32_t handleVBO_ = 0;

    // Buffers for mesh data
    std::vector<QuadVertex> vertices_;
    std::vector<uint32_t> indices_;

    // Buffers for outline
    std::vector<float> outlineVertices_;

    bool initialized_ = false;
};

} // namespace quadmapper
