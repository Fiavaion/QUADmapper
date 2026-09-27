#pragma once

// A 3D object in a scene (ADR-052): an imported mesh placed in world space and
// textured through its own UVs with the same media binding quads use. Each output
// renders it from its projector's calibrated viewpoint.

#include "core/Types.hpp"
#include "core/UUID.hpp"
#include "Quad.hpp"  // TextureBinding

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace quadmapper {

// Indexed triangle mesh. uvs and normals are either empty or one per position.
struct MeshData {
    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    std::vector<Vec3> normals;
    std::vector<uint32_t> indices;  // three per triangle

    bool empty() const { return indices.empty(); }
    size_t triangleCount() const { return indices.size() / 3; }
};

class MeshObject {
public:
    MeshObject() : id_(UUID::generate()) {}
    explicit MeshObject(const UUID& id) : id_(id) {}

    // A copy of this object's full state (mesh, transform, texture binding) under a
    // different id. Used by duplicate scene.
    MeshObject cloneWithId(const UUID& id) const {
        MeshObject copy(*this);
        copy.id_ = id;
        return copy;
    }

    const UUID& getId() const { return id_; }
    const std::string& getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }

    // The file the mesh was imported from (absolute in memory; the serializer writes it
    // relative to the project when it can, as for media sources).
    const std::string& getSourcePath() const { return sourcePath_; }
    void setSourcePath(const std::string& path) { sourcePath_ = path; }

    const MeshData& getMesh() const { return mesh_; }
    // Every way to change the mesh gives it a new revision (TD-15), which is what the
    // renderer's GPU copy is keyed on: replace it whole with setMesh, or edit it in
    // place through editMesh. editMesh bumps the revision when called, so call it for
    // each edit and do not keep the reference past the frame.
    void setMesh(MeshData mesh) {
        mesh_ = std::move(mesh);
        meshRevision_ = nextMeshRevision();
    }
    MeshData& editMesh() {
        meshRevision_ = nextMeshRevision();
        return mesh_;
    }
    // Unique across every object in the process, so two meshes never share one: a
    // copy (duplicate scene, undo snapshot) keeps it because it holds the same data.
    uint64_t getMeshRevision() const { return meshRevision_; }

    // Model -> world.
    const glm::mat4& getTransform() const { return transform_; }
    void setTransform(const glm::mat4& transform) { transform_ = transform; }

    TextureBinding& getTexture() { return texture_; }
    const TextureBinding& getTexture() const { return texture_; }

    bool isVisible() const { return visible_; }
    void setVisible(bool visible) { visible_ = visible; }
    float getOpacity() const { return opacity_; }
    void setOpacity(float opacity) { opacity_ = opacity; }

    // World-space bounding box of the mesh (min > max when the mesh is empty).
    void worldBounds(Vec3& min, Vec3& max) const;

private:
    static uint64_t nextMeshRevision() {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }

    UUID id_;
    std::string name_ = "Object";
    std::string sourcePath_;
    MeshData mesh_;
    uint64_t meshRevision_ = nextMeshRevision();
    glm::mat4 transform_{1.0f};
    TextureBinding texture_;
    bool visible_ = true;
    float opacity_ = 1.0f;
};

inline void MeshObject::worldBounds(Vec3& min, Vec3& max) const {
    min = Vec3(1e30f);
    max = Vec3(-1e30f);
    for (const Vec3& p : mesh_.positions) {
        const Vec3 w = Vec3(transform_ * glm::vec4(p, 1.0f));
        min = glm::min(min, w);
        max = glm::max(max, w);
    }
}

}  // namespace quadmapper
