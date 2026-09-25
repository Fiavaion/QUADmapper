#pragma once

// Wavefront OBJ import for 3D objects (ADR-052 §2). Geometry only: materials,
// objects, groups and smoothing groups are ignored.

#include "MeshObject.hpp"  // MeshData
#include "core/Result.hpp"

#include <filesystem>

namespace quadmapper {

// Reads v / vt / vn and f (v, v/vt, v//vn or v/vt/vn, negative indices allowed),
// fan-triangulates polygons and de-duplicates vertices by their (v, vt, vn) triple.
// Without normals in the file, smooth normals are computed; without uvs, uvs stay
// empty. Malformed, empty or oversized files return Err with the offending line.
Result<MeshData, Error> loadObj(const std::filesystem::path& path);

}  // namespace quadmapper
