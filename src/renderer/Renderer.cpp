#include "Renderer.hpp"
#include "core/GlObjectStats.hpp"
#include "core/Logger.hpp"
#include "ShaderSources.hpp"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

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

    // 3D objects share quad.frag so every surface option behaves the same (ADR-052).
    if (!mesh3dShader_.create(shaders::MESH3D_VERT, shaders::QUAD_FRAG)) {
        Logger::error("Failed to create 3D mesh shader");
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

    // Create buffers (each quad's surface buffers are made on first draw)
    createCornerIndexBuffer();
    createOutlineBuffers();
    createHandleBuffers();

    // Create default texture
    createDefaultTexture();

    // Blend state is per GL context. Every Renderer lives in its own context (the
    // editor, or one per output window), so each sets it here; otherwise opacity
    // and soft edge would only composite in the editor.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    initialized_ = true;
    Logger::info("Renderer initialized successfully");
    return true;
}

void Renderer::shutdown() {
    if (!initialized_) {
        return;
    }

    meshCache_.clear();
    quadCache_.clear();  // before the shared corner indices their VAOs use
    if (cornerIndexBuffer_ != 0) {
        glDeleteBuffers(1, &cornerIndexBuffer_);
        GlObjectStats::onVertexBufferDestroyed();
        cornerIndexBuffer_ = 0;
        cornerIndexCount_ = 0;
    }
    quadShader_.destroy();
    mesh3dShader_.destroy();
    outlineShader_.destroy();
    handleShader_.destroy();
    defaultTexture_.destroy();

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

    boundProgram_ = 0;  // the ids may be reused by programs made later
    initialized_ = false;
    Logger::debug("Renderer shutdown complete");
}

void Renderer::beginFrame() {
    // Frame setup if needed
}

void Renderer::endFrame() {
    // Surface draws leave their program, texture and VAO bound for the next one
    // (ADR-054); unbind them so no other GL code can change a quad's VAO, and so
    // the next pass binds its program again whatever ran in between.
    glBindVertexArray(0);
    Texture::unbind();
    unbindProgram();
}

void Renderer::useSurfaceProgram(ShaderProgram& shader) {
    // glUseProgram costs as much as several uniform uploads even when the program
    // is already current, and consecutive surface draws share it.
    if (boundProgram_ != shader.getId()) {
        shader.bind();
        boundProgram_ = shader.getId();
    }
}

void Renderer::unbindProgram() {
    ShaderProgram::unbind();
    boundProgram_ = 0;
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

// A quad's tessellated surface on the GPU. VAOs are per context, like the Renderer
// that owns it. Rebuilt only when the quad's geometry changes, so drawing an
// unchanged quad costs no tessellation and no buffer upload. A corner-pin patch
// draws with the Renderer's shared index buffer (every patch has the same
// triangles, TD-22); only a mesh warp of another tessellation size owns its indices.
static_assert(sizeof(MeshVertex) == 5 * sizeof(float), "MeshVertex is uploaded as 5 packed floats");
struct GpuQuad {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ownIndices = 0;  // 0 while the VAO uses the shared corner-patch indices
    GLsizei indexCount = 0;
    QuadGeometry builtFrom;  // the geometry the buffers hold
    uint64_t lastPass = 0;

    GpuQuad() {
        glGenVertexArrays(1, &vao);
        GlObjectStats::onVertexArrayCreated();
        glGenBuffers(1, &vbo);
        GlObjectStats::onVertexBufferCreated();

        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)offsetof(MeshVertex, position));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), (void*)offsetof(MeshVertex, texCoord));
        glBindVertexArray(0);
    }
    ~GpuQuad() {
        glDeleteVertexArrays(1, &vao);
        GlObjectStats::onVertexArrayDestroyed();
        glDeleteBuffers(1, &vbo);
        GlObjectStats::onVertexBufferDestroyed();
        releaseOwnIndices();
    }
    GpuQuad(const GpuQuad&) = delete;
    GpuQuad& operator=(const GpuQuad&) = delete;

    // Upload the vertices and draw them with `indices`, or, when that is null, with
    // the shared buffer `sharedIndices` holding `sharedCount` indices.
    void upload(const std::vector<MeshVertex>& vertices, const std::vector<uint32_t>* indices,
                GLuint sharedIndices, GLsizei sharedCount) {
        glBindVertexArray(vao);  // the element-array binding is VAO state
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(MeshVertex)),
                     vertices.data(), GL_DYNAMIC_DRAW);
        if (indices) {
            if (ownIndices == 0) {
                glGenBuffers(1, &ownIndices);
                GlObjectStats::onVertexBufferCreated();
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ownIndices);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices->size() * sizeof(uint32_t)),
                         indices->data(), GL_DYNAMIC_DRAW);
            indexCount = static_cast<GLsizei>(indices->size());
        } else {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sharedIndices);
            indexCount = sharedCount;
        }
        glBindVertexArray(0);
        if (!indices) {
            releaseOwnIndices();  // no longer bound to the VAO
        }
    }

private:
    void releaseOwnIndices() {
        if (ownIndices != 0) {
            glDeleteBuffers(1, &ownIndices);
            GlObjectStats::onVertexBufferDestroyed();
            ownIndices = 0;
        }
    }
};

namespace {

// True when two geometries tessellate identically: compares every field
// tessellate() reads (all of QuadGeometry's state).
bool sameSurface(const QuadGeometry& a, const QuadGeometry& b) {
    return a.getMeshMode() == b.getMeshMode() && a.getCorners() == b.getCorners() &&
           a.isBezierEnabled() == b.isBezierEnabled() &&
           a.getBezierOffsets() == b.getBezierOffsets() && a.getUCoords() == b.getUCoords() &&
           a.getVCoords() == b.getVCoords() && a.getMeshVertices() == b.getMeshVertices() &&
           a.getVertexTangents() == b.getVertexTangents();
}

}  // namespace

const GpuQuad& Renderer::quadBuffersFor(const Quad& quad) {
    const QuadGeometry& geometry = quad.getGeometry();
    std::unique_ptr<GpuQuad>& slot = quadCache_[quad.getId()];
    const bool fresh = !slot;
    if (fresh) {
        slot = std::make_unique<GpuQuad>();
    }
    if (fresh || !sameSurface(slot->builtFrom, geometry)) {
        // An N×N tessellation draws with the shared index set.
        const bool ownIndices = !tessellate(geometry);
        slot->upload(vertices_, ownIndices ? &indices_ : nullptr, cornerIndexBuffer_,
                     cornerIndexCount_);
        slot->builtFrom = geometry;
    }
    slot->lastPass = meshPass_;
    return *slot;
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

    // Line-list buffer for the mesh-grid wireframe and handle lines (reuses the
    // outline shader); drawGridLines sizes it for each draw.
    glGenVertexArrays(1, &gridVAO_);
    GlObjectStats::onVertexArrayCreated();
    glGenBuffers(1, &gridVBO_);
    GlObjectStats::onVertexBufferCreated();
    glBindVertexArray(gridVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

bool Renderer::tessellate(const QuadGeometry& geometry) {
    // One surface for every mode (ADR-049): S sampled at a density independent of the
    // control grid, with the base's projective texture coordinates.
    const IVec2 cells = MeshSubdivision::generateMesh(geometry, vertices_, indices_);
    return cells.x == kCornerPatchDivisions && cells.y == kCornerPatchDivisions;
}

void Renderer::createCornerIndexBuffer() {
    // The triangles of every N×N tessellation, as MeshSubdivision lays them out.
    std::vector<uint32_t> indices;
    MeshSubdivision::generateIndices(kCornerPatchDivisions, indices);
    glGenBuffers(1, &cornerIndexBuffer_);
    GlObjectStats::onVertexBufferCreated();
    // With no VAO bound, so no VAO records this element-array binding.
    glBindVertexArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cornerIndexBuffer_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(uint32_t)),
                 indices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    cornerIndexCount_ = static_cast<int>(indices.size());
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

    const GpuQuad& gpu = quadBuffersFor(quad);

    // Uniforms equal to the previous draw's are skipped (ShaderProgram), and the
    // program, texture and VAO stay bound until the next draw or endFrame().
    useSurfaceProgram(quadShader_);
    applyViewUniforms(quadShader_);
    quadShader_.setInt("u_OutputRotation", static_cast<int>(outputRotation_) / 90);

    applySurfaceUniforms(quadShader_, quad.getTexture(), quad.getOpacity(),
                         quad.getColorCorrection(), quad.getSoftEdge(), texture);

    glBindVertexArray(gpu.vao);
    glDrawElements(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_INT, nullptr);

    // Draw selection outline if selected
    if (selected) {
        renderQuadOutline(quad, glm::vec4(0.0f, 0.7f, 1.0f, 1.0f), 2.0f);
    }
}

void Renderer::applySurfaceUniforms(ShaderProgram& shader, const TextureBinding& binding,
                                    float opacity, const ColorCorrection& color,
                                    const SoftEdge& edge, const Texture* texture) {
    shader.setFloat("u_Opacity", opacity);
    shader.setVec2("u_UVOffset", binding.uvOffset);
    shader.setVec2("u_UVScale", binding.uvScale);
    shader.setFloat("u_UVRotation", binding.uvRotation);
    shader.setInt("u_FlipH", binding.flipH ? 1 : 0);
    shader.setInt("u_FlipV", binding.flipV ? 1 : 0);
    shader.setInt("u_Texture", 0);

    // Crop (ADR-040). Identity {0,0,1,1} is a render no-op.
    const NormalizedRect& crop = binding.cropRect;
    shader.setVec2("u_CropOffset", glm::vec2(crop.x, crop.y));
    shader.setVec2("u_CropSize", glm::vec2(crop.width, crop.height));

    // Color correction (ADR-036). Identity values are a render no-op.
    shader.setVec3("u_ColorRGBGain", color.rgbGain);
    shader.setFloat("u_ColorBrightness", color.brightness);
    shader.setFloat("u_ColorContrast", color.contrast);
    shader.setFloat("u_ColorGamma", color.gamma);

    // Soft-edge blending (ADR-037). Identity widths (0) are a no-op.
    shader.setFloat("u_EdgeLeft", edge.left);
    shader.setFloat("u_EdgeRight", edge.right);
    shader.setFloat("u_EdgeTop", edge.top);
    shader.setFloat("u_EdgeBottom", edge.bottom);
    shader.setFloat("u_EdgeGamma", edge.gamma);

    // Colorspace uniform (ADR-044, A4): 1 = YCoCg decode for Hap Q, 0 = RGB passthrough.
    // Sourced from the texture's colorSpace() so every draw path (canvas, per-output
    // renderer, preview FBO) picks it up automatically through this single call site.
    const bool valid = texture && texture->isValid();
    shader.setInt("u_ColorSpace",
                  (valid && texture->colorSpace() == TextureColorSpace::YCoCg) ? 1 : 0);

    if (valid) {
        texture->bind(0);
    } else {
        defaultTexture_.bind(0);
    }
}

// A MeshObject's vertex data on the GPU (ADR-052): interleaved position + uv, and
// indices, owned RAII. Built once per MeshData; the recorded storage lets the
// Renderer tell when the object's mesh was replaced and must be uploaded again.
struct GpuMesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;  // 0 = nothing drawable (empty or invalid indices)
    Vec3 boundsMin{0.0f};    // model space
    Vec3 boundsMax{0.0f};
    uint64_t lastPass = 0;

    explicit GpuMesh(const MeshData& mesh) : source_(signature(mesh)) { upload(mesh); }
    ~GpuMesh() {
        if (vao != 0) { glDeleteVertexArrays(1, &vao); GlObjectStats::onVertexArrayDestroyed(); }
        if (vbo != 0) { glDeleteBuffers(1, &vbo); GlObjectStats::onVertexBufferDestroyed(); }
        if (ebo != 0) { glDeleteBuffers(1, &ebo); GlObjectStats::onVertexBufferDestroyed(); }
    }
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;

    bool builtFrom(const MeshData& mesh) const { return signature(mesh) == source_; }

private:
    // Storage identity plus the end vertices: constant-time, and it changes whenever
    // the MeshData is replaced (a reload or a different mesh).
    struct Signature {
        const void* positions = nullptr;
        const void* uvs = nullptr;
        const void* indices = nullptr;
        size_t positionCount = 0, uvCount = 0, indexCount = 0;
        Vec3 first{0.0f}, last{0.0f};
        bool operator==(const Signature& o) const {
            return positions == o.positions && uvs == o.uvs && indices == o.indices &&
                   positionCount == o.positionCount && uvCount == o.uvCount &&
                   indexCount == o.indexCount && first == o.first && last == o.last;
        }
    };
    static Signature signature(const MeshData& m) {
        Signature s;
        s.positions = m.positions.data();
        s.uvs = m.uvs.data();
        s.indices = m.indices.data();
        s.positionCount = m.positions.size();
        s.uvCount = m.uvs.size();
        s.indexCount = m.indices.size();
        if (!m.positions.empty()) {
            s.first = m.positions.front();
            s.last = m.positions.back();
        }
        return s;
    }

    void upload(const MeshData& mesh) {
        const size_t vertexCount = mesh.positions.size();
        const size_t indexTotal = mesh.triangleCount() * 3;
        for (size_t i = 0; i < indexTotal; ++i) {
            if (mesh.indices[i] >= vertexCount) {
                Logger::warning("Mesh index {} out of range ({} vertices); object not drawn",
                                mesh.indices[i], vertexCount);
                return;
            }
        }
        if (indexTotal == 0) return;

        boundsMin = Vec3(std::numeric_limits<float>::max());
        boundsMax = Vec3(std::numeric_limits<float>::lowest());
        for (const Vec3& p : mesh.positions) {
            boundsMin = glm::min(boundsMin, p);
            boundsMax = glm::max(boundsMax, p);
        }

        // Without UVs, map the XY bounds onto the texture: +X right, +Y up, so the
        // image stands upright seen from the front (v = 0 is the image's top row).
        const bool hasUvs = mesh.uvs.size() == vertexCount;
        const Vec2 extent = glm::max(Vec2(boundsMax - boundsMin), Vec2(1e-6f));
        std::vector<float> interleaved;
        interleaved.reserve(vertexCount * 5);
        for (size_t i = 0; i < vertexCount; ++i) {
            const Vec3& p = mesh.positions[i];
            const Vec2 uv = hasUvs ? mesh.uvs[i]
                                   : Vec2((p.x - boundsMin.x) / extent.x,
                                          (boundsMax.y - p.y) / extent.y);
            interleaved.insert(interleaved.end(), {p.x, p.y, p.z, uv.x, uv.y});
        }

        glGenVertexArrays(1, &vao);
        GlObjectStats::onVertexArrayCreated();
        glGenBuffers(1, &vbo);
        GlObjectStats::onVertexBufferCreated();
        glGenBuffers(1, &ebo);
        GlObjectStats::onVertexBufferCreated();

        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
                     interleaved.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexTotal * sizeof(uint32_t)),
                     mesh.indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glBindVertexArray(0);  // before any other EBO bind, which would rebind ours

        indexCount = static_cast<GLsizei>(indexTotal);
    }

    Signature source_;
};

const GpuMesh* Renderer::meshBuffersFor(const MeshObject& object) {
    const MeshData& mesh = object.getMesh();
    if (mesh.empty()) {
        meshCache_.erase(object.getId());
        return nullptr;
    }
    std::unique_ptr<GpuMesh>& slot = meshCache_[object.getId()];
    if (!slot || !slot->builtFrom(mesh)) {
        slot.reset();  // free the old buffers before allocating the new ones
        slot = std::make_unique<GpuMesh>(mesh);
    }
    slot->lastPass = meshPass_;
    return slot->indexCount > 0 ? slot.get() : nullptr;
}

void Renderer::renderMeshObject(const MeshObject& object, const ProjectorCamera& camera,
                                const Texture* texture, float nearZ, float farZ) {
    if (!initialized_ || !object.isVisible() || !(nearZ > 0.0f) || !(farZ > nearZ)) {
        return;
    }
    const GpuMesh* gpu = meshBuffersFor(object);
    if (!gpu) {
        return;
    }

    const glm::mat4 mvp =
        camera.projectionMatrix(nearZ, farZ) * camera.viewMatrix() * object.getTransform();

    useSurfaceProgram(mesh3dShader_);
    mesh3dShader_.setMat4("u_MVP", mvp);
    applyViewUniforms(mesh3dShader_);
    mesh3dShader_.setInt("u_OutputRotation", static_cast<int>(outputRotation_) / 90);
    // Objects carry no colour correction or soft edge of their own: identity.
    applySurfaceUniforms(mesh3dShader_, object.getTexture(), object.getOpacity(),
                         ColorCorrection{}, SoftEdge{}, texture);

    // Left bound, as by renderQuad, until the next draw or endFrame().
    glBindVertexArray(gpu->vao);
    glDrawElements(GL_TRIANGLES, gpu->indexCount, GL_UNSIGNED_INT, nullptr);
}

void Renderer::renderMeshObjects(const std::vector<std::unique_ptr<MeshObject>>& objects,
                                 const ProjectorCamera& camera,
                                 const std::unordered_map<UUID, Texture*>& textures) {
    if (!initialized_) {
        return;
    }
    // Every scene render path starts here, so this is where both GPU caches drop the
    // entries of objects and quads that stopped being drawn.
    ++meshPass_;
    auto pruneIdle = [this](auto& cache) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (meshPass_ - it->second->lastPass > kCacheIdlePasses) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
    };
    pruneIdle(meshCache_);
    pruneIdle(quadCache_);

    // One depth range for the whole pass, spanning every object's bounding box in
    // the projector's eye space, so depth compares correctly between objects.
    const glm::mat4 view = camera.viewMatrix();
    float nearest = std::numeric_limits<float>::max();
    float farthest = 0.0f;
    for (const auto& object : objects) {
        if (!object || !object->isVisible()) continue;
        const GpuMesh* gpu = meshBuffersFor(*object);
        if (!gpu) continue;
        const glm::mat4 modelView = view * object->getTransform();
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 p((corner & 1) ? gpu->boundsMax.x : gpu->boundsMin.x,
                         (corner & 2) ? gpu->boundsMax.y : gpu->boundsMin.y,
                         (corner & 4) ? gpu->boundsMax.z : gpu->boundsMin.z);
            const float depth = -(modelView * glm::vec4(p, 1.0f)).z;  // GL eye looks down -z
            nearest = std::min(nearest, depth);
            farthest = std::max(farthest, depth);
        }
    }
    if (farthest <= 0.0f) {
        return;  // nothing in front of the projector
    }
    const float farZ = farthest * 1.01f;
    const float nearZ = std::max(nearest * 0.99f, farZ * 1e-4f);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    for (const auto& object : objects) {
        if (!object || !object->isVisible()) continue;
        const Texture* texture = nullptr;
        auto it = textures.find(object->getId());
        if (it != textures.end()) {
            texture = it->second;
        }
        renderMeshObject(*object, camera, texture, nearZ, farZ);
    }
    glDisable(GL_DEPTH_TEST);
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

    unbindProgram();
    glLineWidth(1.0f);
}

void Renderer::renderMeshGrid(const Quad& quad, const glm::vec4& color, float lineWidth) {
    if (!initialized_) {
        return;
    }

    // Each division line as a polyline along the surface, sampled at the render
    // tessellation's parameters so it lies on the drawn image.
    const QuadGeometry& geometry = quad.getGeometry();
    const std::vector<float>& uLines = geometry.getUCoords();
    const std::vector<float>& vLines = geometry.getVCoords();
    const std::vector<float> uSteps = geometry.tessellationU();
    const std::vector<float> vSteps = geometry.tessellationV();
    const auto columns = geometry.sampleSurface(uLines, vSteps);  // vertical lines
    const auto rows = geometry.sampleSurface(uSteps, vLines);     // horizontal lines

    gridVertices_.clear();
    auto pushSeg = [&](const Vec2& a, const Vec2& b) {
        gridVertices_.push_back(a.x); gridVertices_.push_back(a.y);
        gridVertices_.push_back(b.x); gridVertices_.push_back(b.y);
    };
    for (size_t c = 0; c < uLines.size(); ++c)
        for (size_t s = 0; s + 1 < vSteps.size(); ++s)
            pushSeg(columns[s * uLines.size() + c].position,
                    columns[(s + 1) * uLines.size() + c].position);
    for (size_t r = 0; r < vLines.size(); ++r)
        for (size_t s = 0; s + 1 < uSteps.size(); ++s)
            pushSeg(rows[r * uSteps.size() + s].position, rows[r * uSteps.size() + s + 1].position);
    drawGridLines(color, lineWidth);
}

void Renderer::drawGridLines(const glm::vec4& color, float lineWidth) {
    if (gridVertices_.empty()) {
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, gridVBO_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(gridVertices_.size() * sizeof(float)),
                 gridVertices_.data(), GL_DYNAMIC_DRAW);

    outlineShader_.bind();
    applyViewUniforms(outlineShader_);
    outlineShader_.setVec4("u_OutlineColor", color);
    glLineWidth(lineWidth);
    glBindVertexArray(gridVAO_);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(gridVertices_.size() / 2));
    glBindVertexArray(0);
    unbindProgram();
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
    unbindProgram();
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
    unbindProgram();
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
    unbindProgram();
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
    unbindProgram();
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
    drawGridLines(glm::vec4(0.7f, 0.6f, 0.2f, 0.7f), 1.0f);

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
    unbindProgram();
}

void Renderer::renderGridHandles(const Quad& quad, int vertex, int selectedHandle,
                                 int hoveredHandle, int draggedHandle) {
    if (!initialized_) {
        return;
    }
    const QuadGeometry& g = quad.getGeometry();
    const unsigned available = g.availableHandles(vertex);
    if (available == 0) {
        return;
    }
    const Vec2 origin = g.generateMeshPositions()[static_cast<size_t>(vertex)];
    std::array<Vec2, GRID_HANDLE_COUNT> handles{};
    gridVertices_.clear();
    for (int h = 0; h < GRID_HANDLE_COUNT; ++h) {
        if (!(available & (1u << h))) continue;
        handles[static_cast<size_t>(h)] = g.gridHandlePosition(vertex, h);
        gridVertices_.push_back(origin.x); gridVertices_.push_back(origin.y);
        gridVertices_.push_back(handles[static_cast<size_t>(h)].x);
        gridVertices_.push_back(handles[static_cast<size_t>(h)].y);
    }
    drawGridLines(glm::vec4(0.7f, 0.6f, 0.2f, 0.7f), 1.0f);

    // Handles in the bezier-handle style: amber once set, dimmer while automatic;
    // the keyboard-selected one green, as a selected vertex is.
    const VertexTangents tangents = g.getVertexTangent(vertex);
    handleShader_.bind();
    setHandleAspect(handleShader_, canvasAspect_);
    applyViewUniforms(handleShader_);
    constexpr int SEGMENTS = 16;
    for (int h = 0; h < GRID_HANDLE_COUNT; ++h) {
        if (!(available & (1u << h))) continue;
        handleShader_.setVec2("u_HandleCenter", handles[static_cast<size_t>(h)]);
        glm::vec4 color = tangents.isExplicit(h) ? glm::vec4(1.0f, 0.8f, 0.2f, 0.95f)
                                                 : glm::vec4(1.0f, 0.8f, 0.2f, 0.5f);
        float size = 0.006f;
        if (h == draggedHandle) {
            color = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
            size = 0.009f;
        } else if (h == hoveredHandle) {
            color = glm::vec4(0.0f, 1.0f, 1.0f, 1.0f);
        } else if (h == selectedHandle) {
            color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
            size = 0.009f;
        }
        handleShader_.setFloat("u_HandleSize", size);
        handleShader_.setVec4("u_HandleColor", color);
        glBindVertexArray(handleVAO_);
        glDrawArrays(GL_TRIANGLE_FAN, 0, SEGMENTS + 2);
    }
    glBindVertexArray(0);
    unbindProgram();
}

} // namespace quadmapper
