#include "Renderer.hpp"
#include "core/GlObjectStats.hpp"
#include "core/Logger.hpp"
#include "ShaderSources.hpp"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <cmath>

namespace quadmapper {

Renderer::Renderer() = default;

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::initialize() {
    if (initialized_) {
        return true;
    }

    Logger::debug("Initializing renderer...");

    // Create quad shader
    if (!quadShader_.create(shaders::QUAD_VERT, shaders::QUAD_FRAG)) {
        Logger::error("Failed to create quad shader");
        return false;
    }

    // Create outline shader
    if (!outlineShader_.create(shaders::OUTLINE_VERT, shaders::OUTLINE_FRAG)) {
        Logger::error("Failed to create outline shader");
        return false;
    }

    // Create handle shader
    if (!handleShader_.create(shaders::HANDLE_VERT, shaders::HANDLE_FRAG)) {
        Logger::error("Failed to create handle shader");
        return false;
    }

    // Create buffers
    createQuadBuffers();
    createOutlineBuffers();
    createHandleBuffers();

    // Create default texture
    createDefaultTexture();

    initialized_ = true;
    Logger::info("Renderer initialized successfully");
    return true;
}

void Renderer::shutdown() {
    if (!initialized_) {
        return;
    }

    quadShader_.destroy();
    outlineShader_.destroy();
    handleShader_.destroy();
    defaultTexture_.destroy();

    if (quadVAO_ != 0) {
        glDeleteVertexArrays(1, &quadVAO_);
        GlObjectStats::onVertexArrayDestroyed();
        quadVAO_ = 0;
    }
    if (quadVBO_ != 0) {
        glDeleteBuffers(1, &quadVBO_);
        GlObjectStats::onVertexBufferDestroyed();
        quadVBO_ = 0;
    }
    if (quadEBO_ != 0) {
        glDeleteBuffers(1, &quadEBO_);
        GlObjectStats::onVertexBufferDestroyed();
        quadEBO_ = 0;
    }
    if (outlineVAO_ != 0) {
        glDeleteVertexArrays(1, &outlineVAO_);
        GlObjectStats::onVertexArrayDestroyed();
        outlineVAO_ = 0;
    }
    if (outlineVBO_ != 0) {
        glDeleteBuffers(1, &outlineVBO_);
        GlObjectStats::onVertexBufferDestroyed();
        outlineVBO_ = 0;
    }
    if (gridVAO_ != 0) {
        glDeleteVertexArrays(1, &gridVAO_);
        GlObjectStats::onVertexArrayDestroyed();
        gridVAO_ = 0;
    }
    if (gridVBO_ != 0) {
        glDeleteBuffers(1, &gridVBO_);
        GlObjectStats::onVertexBufferDestroyed();
        gridVBO_ = 0;
    }
    if (handleVAO_ != 0) {
        glDeleteVertexArrays(1, &handleVAO_);
        GlObjectStats::onVertexArrayDestroyed();
        handleVAO_ = 0;
    }
    if (handleVBO_ != 0) {
        glDeleteBuffers(1, &handleVBO_);
        GlObjectStats::onVertexBufferDestroyed();
        handleVBO_ = 0;
    }

    initialized_ = false;
    Logger::debug("Renderer shutdown complete");
}

void Renderer::beginFrame() {
    // Frame setup if needed
}

void Renderer::endFrame() {
    // Frame cleanup if needed
}

void Renderer::setViewTransform(const glm::vec2& scale, const glm::vec2& offset, float canvasAspect) {
    viewScale_ = scale;
    viewOffset_ = offset;
    canvasAspect_ = canvasAspect;
}

void Renderer::resetViewTransform() {
    viewScale_ = glm::vec2(1.0f, 1.0f);
    viewOffset_ = glm::vec2(0.0f, 0.0f);
    canvasAspect_ = 0.0f;
}

void Renderer::applyViewUniforms(ShaderProgram& shader) {
    shader.setVec2("u_ViewScale", viewScale_);
    shader.setVec2("u_ViewOffset", viewOffset_);
}

void Renderer::createQuadBuffers() {
    // Create VAO
    glGenVertexArrays(1, &quadVAO_);
    GlObjectStats::onVertexArrayCreated();
    glGenBuffers(1, &quadVBO_);
    GlObjectStats::onVertexBufferCreated();
    glGenBuffers(1, &quadEBO_);
    GlObjectStats::onVertexBufferCreated();

    glBindVertexArray(quadVAO_);

    // Setup VBO
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO_);
    // Reserve space for max mesh size (65x65 vertices for 64 resolution)
    glBufferData(GL_ARRAY_BUFFER, 65 * 65 * sizeof(QuadVertex), nullptr, GL_DYNAMIC_DRAW);

    // Setup EBO
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEBO_);
    // Reserve space for max indices (64x64 quads = 64x64x6 indices)
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 64 * 64 * 6 * sizeof(uint32_t), nullptr, GL_DYNAMIC_DRAW);

    // Position attribute
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(QuadVertex), (void*)offsetof(QuadVertex, position));

    // TexCoord attribute
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(QuadVertex), (void*)offsetof(QuadVertex, texCoord));

    glBindVertexArray(0);
}

void Renderer::createOutlineBuffers() {
    glGenVertexArrays(1, &outlineVAO_);
    GlObjectStats::onVertexArrayCreated();
    glGenBuffers(1, &outlineVBO_);
    GlObjectStats::onVertexBufferCreated();

    glBindVertexArray(outlineVAO_);

    glBindBuffer(GL_ARRAY_BUFFER, outlineVBO_);
    // 5 vertices for a closed line loop (4 corners + back to start)
    glBufferData(GL_ARRAY_BUFFER, 5 * 2 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    // Position attribute only
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

    glBindVertexArray(0);

    // Mesh-grid wireframe buffer (dynamic; reuses the outline shader). Sized for the
    // worst-case 64x64 control grid: 2 axes * 65 lines * 64 segments * 2 endpoints.
    glGenVertexArrays(1, &gridVAO_);
    GlObjectStats::onVertexArrayCreated();
    glGenBuffers(1, &gridVBO_);
    GlObjectStats::onVertexBufferCreated();
    glBindVertexArray(gridVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO_);
    glBufferData(GL_ARRAY_BUFFER, 2 * 65 * 64 * 2 * 2 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

void Renderer::updateQuadBuffers(const Quad& quad) {
    const QuadGeometry& geometry = quad.getGeometry();

    // Smooth path (ADR-043): a CORNERS_ONLY quad is a projective (or bezier) corner
    // patch — its surface is fully defined by the corners, so render it as a fine
    // N×N tessellation sampled from surfacePoint(). This decouples render density
    // from the 1×1 control grid and removes the diagonal "fold" on extreme warps.
    if (geometry.getMeshMode() == MeshMode::CORNERS_ONLY) {
        constexpr int N = 32;  // sub-divisions per axis (33×33 verts; within buffers)
        const int line = N + 1;
        vertices_.resize(static_cast<size_t>(line) * line);
        for (int r = 0; r < line; ++r) {
            for (int c = 0; c < line; ++c) {
                const float u = static_cast<float>(c) / N;
                const float v = static_cast<float>(r) / N;
                Vec2 p = geometry.surfacePoint(u, v);
                const int index = r * line + c;
                vertices_[index].position[0] = p.x;
                vertices_[index].position[1] = p.y;
                vertices_[index].texCoord[0] = u;
                vertices_[index].texCoord[1] = v;
            }
        }
        indices_.resize(static_cast<size_t>(N) * N * 6);
        int o = 0;
        for (int r = 0; r < N; ++r) {
            for (int c = 0; c < N; ++c) {
                uint32_t tl = static_cast<uint32_t>(r * line + c);
                uint32_t tr = tl + 1;
                uint32_t bl = static_cast<uint32_t>((r + 1) * line + c);
                uint32_t br = bl + 1;
                indices_[o++] = tl; indices_[o++] = bl; indices_[o++] = tr;
                indices_[o++] = tr; indices_[o++] = bl; indices_[o++] = br;
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, quadVBO_);
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertices_.size() * sizeof(QuadVertex), vertices_.data());
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEBO_);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indices_.size() * sizeof(uint32_t), indices_.data());
        return;
    }

    // Control grid may be non-uniform (ADR-038): iterate per-axis using the
    // actual division coordinates for tex coords, and index the vpr-wide grid.
    const int vpr = geometry.vertsPerRow();
    const int vpc = geometry.vertsPerCol();
    const auto& uCoords = geometry.getUCoords();
    const auto& vCoords = geometry.getVCoords();

    vertices_.resize(static_cast<size_t>(vpr) * vpc);
    auto meshPositions = geometry.generateMeshPositions();

    for (int row = 0; row < vpc; ++row) {
        for (int col = 0; col < vpr; ++col) {
            int index = row * vpr + col;
            vertices_[index].position[0] = meshPositions[index].x;
            vertices_[index].position[1] = meshPositions[index].y;
            vertices_[index].texCoord[0] = uCoords[col];
            vertices_[index].texCoord[1] = vCoords[row];
        }
    }

    // Generate index data (two triangles per cell)
    indices_.resize(static_cast<size_t>(geometry.getColumns()) * geometry.getRows() * 6);

    int indexOffset = 0;
    for (int row = 0; row < vpc - 1; ++row) {
        for (int col = 0; col < vpr - 1; ++col) {
            uint32_t topLeft = static_cast<uint32_t>(row * vpr + col);
            uint32_t topRight = topLeft + 1;
            uint32_t bottomLeft = static_cast<uint32_t>((row + 1) * vpr + col);
            uint32_t bottomRight = bottomLeft + 1;

            // First triangle (top-left, bottom-left, top-right)
            indices_[indexOffset++] = topLeft;
            indices_[indexOffset++] = bottomLeft;
            indices_[indexOffset++] = topRight;

            // Second triangle (top-right, bottom-left, bottom-right)
            indices_[indexOffset++] = topRight;
            indices_[indexOffset++] = bottomLeft;
            indices_[indexOffset++] = bottomRight;
        }
    }

    // Upload to GPU
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices_.size() * sizeof(QuadVertex), vertices_.data());

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadEBO_);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indices_.size() * sizeof(uint32_t), indices_.data());
}

void Renderer::updateOutlineBuffers(const Quad& quad) {
    const auto& corners = quad.getGeometry().getCorners();

    // Build line loop: TL -> TR -> BR -> BL -> TL
    outlineVertices_.resize(10);
    outlineVertices_[0] = corners[0].x; outlineVertices_[1] = corners[0].y; // TL
    outlineVertices_[2] = corners[1].x; outlineVertices_[3] = corners[1].y; // TR
    outlineVertices_[4] = corners[2].x; outlineVertices_[5] = corners[2].y; // BR
    outlineVertices_[6] = corners[3].x; outlineVertices_[7] = corners[3].y; // BL
    outlineVertices_[8] = corners[0].x; outlineVertices_[9] = corners[0].y; // Back to TL

    glBindBuffer(GL_ARRAY_BUFFER, outlineVBO_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, outlineVertices_.size() * sizeof(float), outlineVertices_.data());
}

void Renderer::createDefaultTexture() {
    // Create a 2x2 white texture
    unsigned char whitePixels[16] = {
        255, 255, 255, 255,
        255, 255, 255, 255,
        255, 255, 255, 255,
        255, 255, 255, 255
    };
    defaultTexture_.create(2, 2, 4, whitePixels);
}

void Renderer::renderQuad(const Quad& quad, const Texture* texture) {
    renderQuad(quad, texture, false);
}

void Renderer::renderQuad(const Quad& quad, const Texture* texture, bool selected) {
    if (!initialized_ || !quad.isVisible()) {
        return;
    }

    // Update mesh buffers with quad geometry
    updateQuadBuffers(quad);

    // Bind shader
    quadShader_.bind();
    applyViewUniforms(quadShader_);

    // Set uniforms
    const TextureBinding& texBinding = quad.getTexture();
    quadShader_.setFloat("u_Opacity", quad.getOpacity());
    quadShader_.setVec2("u_UVOffset", texBinding.uvOffset);
    quadShader_.setVec2("u_UVScale", texBinding.uvScale);
    quadShader_.setFloat("u_UVRotation", texBinding.uvRotation);
    quadShader_.setInt("u_FlipH", texBinding.flipH ? 1 : 0);
    quadShader_.setInt("u_FlipV", texBinding.flipV ? 1 : 0);
    quadShader_.setInt("u_Texture", 0);

    // Per-quad crop (ADR-040). Identity {0,0,1,1} is a render no-op.
    const NormalizedRect& crop = texBinding.cropRect;
    quadShader_.setVec2("u_CropOffset", glm::vec2(crop.x, crop.y));
    quadShader_.setVec2("u_CropSize", glm::vec2(crop.width, crop.height));

    // Per-quad color correction (ADR-036). Identity values are a render no-op.
    const ColorCorrection& cc = quad.getColorCorrection();
    quadShader_.setVec3("u_ColorRGBGain", cc.rgbGain);
    quadShader_.setFloat("u_ColorBrightness", cc.brightness);
    quadShader_.setFloat("u_ColorContrast", cc.contrast);
    quadShader_.setFloat("u_ColorGamma", cc.gamma);

    // Per-quad soft-edge blending (ADR-037). Identity widths (0) are a no-op.
    const SoftEdge& se = quad.getSoftEdge();
    quadShader_.setFloat("u_EdgeLeft", se.left);
    quadShader_.setFloat("u_EdgeRight", se.right);
    quadShader_.setFloat("u_EdgeTop", se.top);
    quadShader_.setFloat("u_EdgeBottom", se.bottom);
    quadShader_.setFloat("u_EdgeGamma", se.gamma);

    // Colorspace uniform (ADR-044, A4): 1 = YCoCg decode for Hap Q, 0 = RGB passthrough.
    // Sourced from the texture's colorSpace() so every draw path (canvas, per-output
    // renderer, preview FBO) picks it up automatically through this single call site.
    {
        int cs = (texture && texture->isValid() &&
                  texture->colorSpace() == TextureColorSpace::YCoCg) ? 1 : 0;
        quadShader_.setInt("u_ColorSpace", cs);
    }

    // Bind texture
    if (texture && texture->isValid()) {
        texture->bind(0);
    } else {
        defaultTexture_.bind(0);
    }

    // Draw
    glBindVertexArray(quadVAO_);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices_.size()), GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    Texture::unbind();
    ShaderProgram::unbind();

    // Draw selection outline if selected
    if (selected) {
        renderQuadOutline(quad, glm::vec4(0.0f, 0.7f, 1.0f, 1.0f), 2.0f);
    }
}

void Renderer::renderQuads(const QuadManager& quadManager,
                           const std::unordered_map<UUID, Texture*>& textures) {
    if (!initialized_) {
        return;
    }

    // Get quads sorted by z-order
    auto sortedQuads = quadManager.getQuadsByZOrder();

    for (const Quad* quad : sortedQuads) {
        if (!quad || !quad->isVisible()) {
            continue;
        }

        // Find texture for this quad
        const Texture* texture = nullptr;
        auto it = textures.find(quad->getId());
        if (it != textures.end()) {
            texture = it->second;
        }

        bool isSelected = quadManager.isSelected(quad->getId());
        renderQuad(*quad, texture, isSelected);
    }
}

void Renderer::renderQuadOutline(const Quad& quad, const glm::vec4& color, float lineWidth) {
    if (!initialized_) {
        return;
    }

    updateOutlineBuffers(quad);

    outlineShader_.bind();
    applyViewUniforms(outlineShader_);
    outlineShader_.setVec4("u_OutlineColor", color);

    glLineWidth(lineWidth);
    glBindVertexArray(outlineVAO_);
    glDrawArrays(GL_LINE_STRIP, 0, 5);
    glBindVertexArray(0);

    ShaderProgram::unbind();
    glLineWidth(1.0f);
}

void Renderer::renderMeshGrid(const Quad& quad, const glm::vec4& color, float lineWidth) {
    if (!initialized_) {
        return;
    }

    const QuadGeometry& geometry = quad.getGeometry();
    const auto positions = geometry.generateMeshPositions();  // includes edit offsets
    const int vpr = geometry.vertsPerRow();
    const int vpc = geometry.vertsPerCol();

    // Build a GL_LINES list: every horizontal and vertical edge of the control grid.
    gridVertices_.clear();
    auto pushSeg = [&](const Vec2& a, const Vec2& b) {
        gridVertices_.push_back(a.x); gridVertices_.push_back(a.y);
        gridVertices_.push_back(b.x); gridVertices_.push_back(b.y);
    };
    for (int r = 0; r < vpc; ++r)
        for (int c = 0; c + 1 < vpr; ++c)
            pushSeg(positions[r * vpr + c], positions[r * vpr + c + 1]);
    for (int c = 0; c < vpr; ++c)
        for (int r = 0; r + 1 < vpc; ++r)
            pushSeg(positions[r * vpr + c], positions[(r + 1) * vpr + c]);
    if (gridVertices_.empty()) {
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, gridVBO_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, gridVertices_.size() * sizeof(float), gridVertices_.data());

    outlineShader_.bind();
    applyViewUniforms(outlineShader_);
    outlineShader_.setVec4("u_OutlineColor", color);
    glLineWidth(lineWidth);
    glBindVertexArray(gridVAO_);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(gridVertices_.size() / 2));
    glBindVertexArray(0);
    ShaderProgram::unbind();
    glLineWidth(1.0f);
}

void Renderer::renderCanvasBorder(const glm::vec4& color, float lineWidth) {
    if (!initialized_) {
        return;
    }

    // Reuse the outline buffer (5 floats-pairs capacity): the [0,1] canvas frame as
    // a closed line loop. The active view transform places it at the projector-frame
    // bounds so the user can see the edges of the output even past the content.
    outlineVertices_.resize(10);
    const float pts[10] = {0.f, 0.f,  1.f, 0.f,  1.f, 1.f,  0.f, 1.f,  0.f, 0.f};
    for (int i = 0; i < 10; ++i) outlineVertices_[i] = pts[i];

    glBindBuffer(GL_ARRAY_BUFFER, outlineVBO_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, outlineVertices_.size() * sizeof(float), outlineVertices_.data());

    outlineShader_.bind();
    applyViewUniforms(outlineShader_);
    outlineShader_.setVec4("u_OutlineColor", color);
    glLineWidth(lineWidth);
    glBindVertexArray(outlineVAO_);
    glDrawArrays(GL_LINE_STRIP, 0, 5);
    glBindVertexArray(0);
    ShaderProgram::unbind();
    glLineWidth(1.0f);
}

void Renderer::createHandleBuffers() {
    // Create a unit circle for handle rendering
    // We'll use 16 segments for a smooth circle
    constexpr int SEGMENTS = 16;
    std::vector<float> circleVertices;
    circleVertices.reserve((SEGMENTS + 2) * 2);

    // Center vertex
    circleVertices.push_back(0.0f);
    circleVertices.push_back(0.0f);

    // Circle vertices
    for (int i = 0; i <= SEGMENTS; ++i) {
        float angle = static_cast<float>(i) / SEGMENTS * 2.0f * 3.14159265f;
        circleVertices.push_back(std::cos(angle));
        circleVertices.push_back(std::sin(angle));
    }

    glGenVertexArrays(1, &handleVAO_);
    GlObjectStats::onVertexArrayCreated();
    glGenBuffers(1, &handleVBO_);
    GlObjectStats::onVertexBufferCreated();

    glBindVertexArray(handleVAO_);

    glBindBuffer(GL_ARRAY_BUFFER, handleVBO_);
    glBufferData(GL_ARRAY_BUFFER, circleVertices.size() * sizeof(float), circleVertices.data(), GL_STATIC_DRAW);

    // Position attribute
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

    glBindVertexArray(0);
}

namespace {
// Set the handle shader's aspect uniform so the unit-circle handles render round.
// With a canvas view transform (ADR-041) the handles live in canvas space, so the
// canvas pixel aspect keeps them round; without one, fall back to the GL viewport.
void setHandleAspect(ShaderProgram& shader, float canvasAspect) {
    float aspect;
    if (canvasAspect > 0.0f) {
        aspect = canvasAspect;
    } else {
        GLint vp[4] = {0, 0, 1, 1};
        glGetIntegerv(GL_VIEWPORT, vp);
        aspect = (vp[3] > 0) ? static_cast<float>(vp[2]) / vp[3] : 1.0f;
    }
    shader.setFloat("u_Aspect", aspect);
}
}  // namespace

void Renderer::renderCornerHandles(const Quad& quad, int hoveredIndex, int draggedIndex, int selectedIndex) {
    if (!initialized_) {
        return;
    }

    const QuadGeometry& geometry = quad.getGeometry();
    const auto& corners = geometry.getCorners();  // TL, TR, BR, BL

    // Handle sizes - corners are slightly larger than mesh handles
    constexpr float HANDLE_SIZE = 0.009f;
    constexpr float SELECTED_HANDLE_SIZE = 0.012f;

    handleShader_.bind();

    setHandleAspect(handleShader_, canvasAspect_);
    applyViewUniforms(handleShader_);

    // Draw each corner handle
    constexpr int SEGMENTS = 16;
    for (int i = 0; i < 4; ++i) {
        // Set handle center
        handleShader_.setVec2("u_HandleCenter", corners[i]);

        // Choose color and size based on state (priority: dragged > hovered > selected > normal)
        glm::vec4 color;
        float size = HANDLE_SIZE;

        if (i == draggedIndex) {
            // Dragged: bright yellow
            color = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
            size = SELECTED_HANDLE_SIZE;
        } else if (i == hoveredIndex) {
            // Hovered: bright cyan
            color = glm::vec4(0.0f, 1.0f, 1.0f, 1.0f);
        } else if (i == selectedIndex) {
            // Selected (keyboard): bright green
            color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
            size = SELECTED_HANDLE_SIZE;
        } else {
            // Normal: white with some transparency
            color = glm::vec4(1.0f, 1.0f, 1.0f, 0.9f);
        }

        handleShader_.setFloat("u_HandleSize", size);
        handleShader_.setVec4("u_HandleColor", color);

        // Draw the circle (triangle fan)
        glBindVertexArray(handleVAO_);
        glDrawArrays(GL_TRIANGLE_FAN, 0, SEGMENTS + 2);
    }

    glBindVertexArray(0);
    ShaderProgram::unbind();
}

void Renderer::renderMeshHandles(const Quad& quad, int hoveredIndex, int draggedIndex, int selectedIndex) {
    if (!initialized_) {
        return;
    }

    const QuadGeometry& geometry = quad.getGeometry();

    // Only render handles in MESH_EDIT mode
    if (geometry.getMeshMode() != MeshMode::MESH_EDIT) {
        return;
    }

    // Get mesh positions (with offsets applied)
    auto positions = geometry.generateMeshPositions();

    // Mesh handles are small so a dense control grid stays readable (not a blob
    // field). Sizes are radii in normalized-Y units; the shader keeps them round.
    constexpr float HANDLE_SIZE = 0.005f;
    constexpr float SELECTED_HANDLE_SIZE = 0.008f;  // Slightly larger for selected

    handleShader_.bind();

    setHandleAspect(handleShader_, canvasAspect_);
    applyViewUniforms(handleShader_);

    // Draw each handle
    constexpr int SEGMENTS = 16;
    for (size_t i = 0; i < positions.size(); ++i) {
        int idx = static_cast<int>(i);

        // Set handle center
        handleShader_.setVec2("u_HandleCenter", positions[i]);

        // Choose color and size based on state (priority: dragged > hovered > selected > normal)
        glm::vec4 color;
        float size = HANDLE_SIZE;

        if (idx == draggedIndex) {
            // Dragged: bright yellow
            color = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
            size = SELECTED_HANDLE_SIZE;
        } else if (idx == hoveredIndex) {
            // Hovered: bright cyan
            color = glm::vec4(0.0f, 1.0f, 1.0f, 1.0f);
        } else if (idx == selectedIndex) {
            // Selected (keyboard): bright green
            color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
            size = SELECTED_HANDLE_SIZE;
        } else {
            // Normal: white with some transparency
            color = glm::vec4(1.0f, 1.0f, 1.0f, 0.8f);
        }

        handleShader_.setFloat("u_HandleSize", size);
        handleShader_.setVec4("u_HandleColor", color);

        // Draw the circle (triangle fan)
        glBindVertexArray(handleVAO_);
        glDrawArrays(GL_TRIANGLE_FAN, 0, SEGMENTS + 2);
    }

    glBindVertexArray(0);
    ShaderProgram::unbind();
}

void Renderer::renderSnapIndicator(const Vec2& position) {
    if (!initialized_) {
        return;
    }

    // Snap indicator: magenta pulsing circle, larger than normal handles
    constexpr float SNAP_INDICATOR_SIZE = 0.014f;
    constexpr int SEGMENTS = 16;

    handleShader_.bind();

    setHandleAspect(handleShader_, canvasAspect_);
    applyViewUniforms(handleShader_);

    // Set handle center
    handleShader_.setVec2("u_HandleCenter", position);

    // Magenta color for snap indicator (distinct from all other handle colors)
    glm::vec4 color(1.0f, 0.0f, 1.0f, 0.9f);  // Bright magenta
    handleShader_.setVec4("u_HandleColor", color);
    handleShader_.setFloat("u_HandleSize", SNAP_INDICATOR_SIZE);

    // Draw the circle (triangle fan)
    glBindVertexArray(handleVAO_);
    glDrawArrays(GL_TRIANGLE_FAN, 0, SEGMENTS + 2);

    glBindVertexArray(0);
    ShaderProgram::unbind();
}

void Renderer::renderBezierHandles(const Quad& quad, int hovered, int dragged) {
    if (!initialized_) {
        return;
    }
    const QuadGeometry& g = quad.getGeometry();
    if (!g.isBezierEnabled()) {
        return;
    }
    const auto& corners = g.getCorners();

    // Tangent lines (corner -> its control point), drawn via the grid-line buffer.
    gridVertices_.clear();
    auto seg = [&](const Vec2& a, const Vec2& b) {
        gridVertices_.push_back(a.x); gridVertices_.push_back(a.y);
        gridVertices_.push_back(b.x); gridVertices_.push_back(b.y);
    };
    for (int e = 0; e < 4; ++e) {
        seg(corners[e], g.getBezierControl(e, 0));
        seg(corners[(e + 1) % 4], g.getBezierControl(e, 1));
    }
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, gridVertices_.size() * sizeof(float), gridVertices_.data());
    outlineShader_.bind();
    applyViewUniforms(outlineShader_);
    outlineShader_.setVec4("u_OutlineColor", glm::vec4(0.7f, 0.6f, 0.2f, 0.7f));
    glLineWidth(1.0f);
    glBindVertexArray(gridVAO_);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(gridVertices_.size() / 2));
    glBindVertexArray(0);
    ShaderProgram::unbind();

    // Control points as round amber handles.
    handleShader_.bind();
    setHandleAspect(handleShader_, canvasAspect_);
    applyViewUniforms(handleShader_);
    constexpr int SEGMENTS = 16;
    for (int i = 0; i < 8; ++i) {
        Vec2 cp = g.getBezierControl(i / 2, i % 2);
        handleShader_.setVec2("u_HandleCenter", cp);
        glm::vec4 color = (i == dragged) ? glm::vec4(1.0f, 1.0f, 0.0f, 1.0f)
                        : (i == hovered) ? glm::vec4(0.0f, 1.0f, 1.0f, 1.0f)
                        :                  glm::vec4(1.0f, 0.8f, 0.2f, 0.95f);
        float size = (i == dragged || i == hovered) ? 0.011f : 0.008f;
        handleShader_.setFloat("u_HandleSize", size);
        handleShader_.setVec4("u_HandleColor", color);
        glBindVertexArray(handleVAO_);
        glDrawArrays(GL_TRIANGLE_FAN, 0, SEGMENTS + 2);
    }
    glBindVertexArray(0);
    ShaderProgram::unbind();
}

} // namespace quadmapper
