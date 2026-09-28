#include "ProjectSerializer.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include "scene/Scene.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace quadmapper {

// .qmap format versioning (see ADR-032, ADR-039).
//   1.x = flat single-quad-list format
//   2.x = legacy scene-based format ("scenes" array) — read-only (ADR-033)
//   3.x = multi-scene + per-output routing: "scenes" (id/name/quads/groups/view/
//         assignedOutputId) + "outputs" arrays (ADR-039). The format the app writes
//         once a project has scenes; v1/v2 are migrated in memory on read.
//   3.1 = additive "sources" table + per-quad textureBinding.mediaId (ADR-046);
//         texturePath is still written (absolute) so 3.0 readers keep file media.
//   3.2 = additive per-scene "objects" (3D meshes by path) + per-output "projector"
//         (lens, pose, calibration points) (ADR-052); older readers ignore both.
//   3.3 = additive, all ignored by older readers:
//         - "control": {"midi": [...]} MIDI mappings (ADR-048), written only when a
//           show has mappings; a missing one = none. A mapping's "bits": 14 (14-bit
//           CC) is written only for 14; missing = 7.
//         - sources[].transport.timelineLocked (ADR-050 §3); missing = unlocked.
//         - a file source whose file is gone is kept, flagged missing, with its
//           references and path (PRD §4.1.8), so saving writes it back unchanged;
//           older readers dropped it.
//         - "audio": {masterVolume, masterMuted} and sources[].transport.volume /
//           muted (ADR-050 §5); missing = full volume, not muted.
//         - quad geometry "meshBase": "surface" (ADR-049), written with meshOffsets:
//           the offsets are residuals from the corner-pin surface. Without it they are
//           offsets from the bilinear corner patch (every earlier file), converted on
//           load so each control vertex keeps its absolute position. Older readers
//           take new residuals as bilinear offsets (vertices shift on keystones).
//         - quad geometry "meshTangents" (ADR-049 §3): grid-line handles, per vertex
//           [{"vertex": i, "handles": [u-, u+, v-, v+]}], each [x, y] or null (auto);
//           written only for vertices with a handle set. Missing = all automatic.
//         - objects[].absPath: the mesh's absolute path, written beside a relative
//           "path" (TD-16). A reader tries "path" first, then "absPath"; missing =
//           "path" only.
//   3.4 = scene canvas -> output slices (ADR-047), additive:
//         - scenes[].canvas: [width, height] in pixels.
//         - scenes[].canvasLocked: true when the operator set the canvas size, so
//           routing keeps it (ADR-047 Amendment 3); written only when true, absent
//           (false) in files written before it.
//         - outputs[].sceneId: the scene the output shows (several outputs may show
//           one scene), and outputs[].slices: [{id, name, input: [x, y, w, h] canvas
//           pixels, warp: quad geometry in output space, blend: {left, right, top,
//           bottom: {width, gamma, curve}}, blackLevel}].
//         - scenes[].assignedOutputId is still written (the first output showing
//           the scene) so 3.0-3.3 readers keep their routing; 3.4 readers ignore it.
//         Files before 3.4 are migrated on load: output.sceneId from the scene whose
//         assignedOutputId names it, canvas = that output's effective (rotated)
//         resolution (a scene no output shows: the first output's, else 1920x1080),
//         and one full-canvas slice with no warp and no blend on every output that
//         shows a scene. A 3.4 output with a sceneId and no slices gets one
//         full-canvas slice. Canvases are clamped to MAX_CANVAS_DIMENSION.
// Readers accept any major <= kMaxSupportedMajor; v1/v2 are wrapped/mapped into the
// scene model so every read path yields >= 1 scene.
static constexpr int kMaxSupportedMajor = 3;
static constexpr const char* kSceneFormatVersion = "3.4.0";

// Load-time clamps for untrusted values (ADR-046 §3).
static constexpr int kMaxMonitorIndex = 63;
static constexpr int kMaxOutputDimension = 16384;
static constexpr size_t kMaxCalibrationPoints = 4096;
static constexpr size_t kMaxSlicesPerOutput = 256;
static constexpr float kMinPlaybackSpeed = 0.1f;  // VideoPlayer::setSpeed range
static constexpr float kMaxPlaybackSpeed = 4.0f;

namespace fs = std::filesystem;

// Single version policy for every read path (ADR-032). Returns the validated MAJOR version,
// or an error message: missing/malformed version rejects, and a major newer than this build
// supports rejects with an actionable message instead of silently half-parsing the file.
static Result<int, std::string> validateVersion(const nlohmann::json& json) {
    if (!json.contains("version")) {
        return Result<int, std::string>::error("Missing version field in project file");
    }
    std::string version = json["version"].get<std::string>();
    int major = 0;
    try {
        size_t dot = version.find('.');
        major = std::stoi(dot == std::string::npos ? version : version.substr(0, dot));
    } catch (const std::exception&) {
        return Result<int, std::string>::error("Malformed project version: " + version);
    }
    if (major < 1) {
        return Result<int, std::string>::error("Malformed project version: " + version);
    }
    if (major > kMaxSupportedMajor) {
        return Result<int, std::string>::error(
            "Project version " + version + " was created by a newer QUADmapper; please update.");
    }
    return Result<int, std::string>::success(major);
}

// Helper to serialize a Vec2
static nlohmann::json serializeVec2(const Vec2& v) {
    return nlohmann::json::array({v.x, v.y});
}

// Checked readers (ADR-046 §3). nlohmann's const operator[] past the end of an array
// is undefined behaviour, so every vector is size-, type- and finiteness-checked. A
// malformed value throws; load()/deserialize() turn that into Err like any parse error.
static float readFiniteFloat(const nlohmann::json& j) {
    if (!j.is_number()) {
        throw std::runtime_error("Malformed project file: expected a number");
    }
    const float f = j.get<float>();
    if (!std::isfinite(f)) {
        throw std::runtime_error("Malformed project file: number out of range");
    }
    return f;
}

static const nlohmann::json& checkedArray(const nlohmann::json& j, size_t size) {
    if (!j.is_array() || j.size() != size) {
        throw std::runtime_error("Malformed project file: expected an array of " +
                                 std::to_string(size) + " numbers");
    }
    return j;
}

// Helper to deserialize a Vec2
static Vec2 deserializeVec2(const nlohmann::json& j) {
    const auto& a = checkedArray(j, 2);
    return Vec2(readFiniteFloat(a[0]), readFiniteFloat(a[1]));
}

// Helper to serialize a Vec3
static nlohmann::json serializeVec3(const Vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

// Helper to deserialize a Vec3
static Vec3 deserializeVec3(const nlohmann::json& j) {
    const auto& a = checkedArray(j, 3);
    return Vec3(readFiniteFloat(a[0]), readFiniteFloat(a[1]), readFiniteFloat(a[2]));
}

// Grid division coords (ADR-038) from an untrusted array: non-numeric/non-finite
// entries (NaN is written as null) are dropped, values clamped to [0,1], the 0/1
// endpoints guaranteed, then sorted and deduplicated. Too many divisions is an error.
static std::vector<float> readGridCoords(const nlohmann::json& j) {
    std::vector<float> coords{0.0f, 1.0f};
    for (const auto& v : j) {
        if (!v.is_number()) continue;
        const float f = v.get<float>();
        if (!std::isfinite(f)) continue;
        coords.push_back(std::clamp(f, 0.0f, 1.0f));
    }
    std::sort(coords.begin(), coords.end());
    coords.erase(std::unique(coords.begin(), coords.end(),
                             [](float a, float b) { return b - a < 1e-6f; }),
                 coords.end());
    coords.back() = 1.0f;  // a near-1 survivor of the dedupe snaps to the endpoint
    if (coords.size() > static_cast<size_t>(MAX_MESH_RESOLUTION) + 1) {
        throw std::runtime_error("Malformed project file: too many grid divisions");
    }
    return coords;
}

// Helper to serialize quad geometry
static nlohmann::json serializeQuadGeometry(const QuadGeometry& geometry) {
    nlohmann::json j;

    // Serialize corners
    const auto& corners = geometry.getCorners();
    j["corners"] = nlohmann::json::array({
        serializeVec2(corners[0]),
        serializeVec2(corners[1]),
        serializeVec2(corners[2]),
        serializeVec2(corners[3])
    });

    j["meshResolution"] = geometry.getMeshResolution();
    j["meshMode"] = static_cast<int>(geometry.getMeshMode());

    // Non-uniform control grid (ADR-038): persist the division coordinates.
    // Uniform grids stay byte-clean (reconstructed from meshResolution on load).
    if (!geometry.isUniform()) {
        j["uCoords"] = geometry.getUCoords();
        j["vCoords"] = geometry.getVCoords();
    }

    // Mesh residuals (ADR-049) if the mesh is warped, marked as surface-relative.
    if (geometry.hasMeshModifications()) {
        const auto& meshVertices = geometry.getMeshVertices();
        nlohmann::json meshOffsetsJson = nlohmann::json::array();
        for (const auto& offset : meshVertices) {
            meshOffsetsJson.push_back(serializeVec2(offset));
        }
        j["meshOffsets"] = meshOffsetsJson;
        j["meshBase"] = "surface";
    }

    // Grid-line handles (ADR-049 §3): only the vertices that have one set.
    if (geometry.hasExplicitTangents()) {
        nlohmann::json tangentsJson = nlohmann::json::array();
        const auto& tangents = geometry.getVertexTangents();
        for (size_t i = 0; i < tangents.size(); ++i) {
            if (tangents[i].explicitMask == 0) continue;
            nlohmann::json handles = nlohmann::json::array();
            for (int h = 0; h < GRID_HANDLE_COUNT; ++h) {
                handles.push_back(tangents[i].isExplicit(h)
                                      ? serializeVec2(tangents[i].derivative[static_cast<size_t>(h)])
                                      : nlohmann::json(nullptr));
            }
            tangentsJson.push_back({{"vertex", i}, {"handles", handles}});
        }
        j["meshTangents"] = tangentsJson;
    }

    // Bezier edges (ADR-043): persist only when enabled, so pre-bezier files and
    // straight-edge quads stay byte-clean and old readers ignore the extra keys.
    if (geometry.isBezierEnabled()) {
        j["bezierEnabled"] = true;
        nlohmann::json bez = nlohmann::json::array();
        for (const auto& o : geometry.getBezierOffsets()) {
            bez.push_back(serializeVec2(o));
        }
        j["bezierOffsets"] = bez;
    }

    return j;
}

// Helper to deserialize quad geometry
static void deserializeQuadGeometry(const nlohmann::json& j, QuadGeometry& geometry) {
    if (j.contains("corners") && j["corners"].is_array() && j["corners"].size() == 4) {
        std::array<Vec2, 4> corners;
        for (int i = 0; i < 4; ++i) {
            corners[i] = deserializeVec2(j["corners"][i]);
        }
        geometry.setCorners(corners);
    }

    // Control grid (ADR-038): prefer explicit division coords; otherwise migrate
    // the legacy uniform meshResolution into an even grid. Either way this sets
    // the vertex count before the offsets below are applied.
    if (j.contains("uCoords") && j["uCoords"].is_array() &&
        j.contains("vCoords") && j["vCoords"].is_array()) {
        geometry.setGridCoords(readGridCoords(j["uCoords"]), readGridCoords(j["vCoords"]));
    } else if (j.contains("meshResolution")) {
        geometry.setMeshResolution(j["meshResolution"].get<int>());  // clamps to [MIN,MAX]
    }

    if (j.contains("meshMode")) {
        // Anything but a known mode falls back to the default corner-pin mode.
        const int mode = j["meshMode"].get<int>();
        geometry.setMeshMode(mode == static_cast<int>(MeshMode::MESH_EDIT)
                                 ? MeshMode::MESH_EDIT : MeshMode::CORNERS_ONLY);
    }

    // Mesh vertex offsets, if present; converted below once the bezier edges are in.
    bool bilinearOffsets = false;
    if (j.contains("meshOffsets") && j["meshOffsets"].is_array()) {
        const auto& offsetsJson = j["meshOffsets"];
        int expectedCount = geometry.getVertexCount();
        if (static_cast<int>(offsetsJson.size()) == expectedCount) {
            std::vector<Vec2> offsets;
            offsets.reserve(expectedCount);
            for (const auto& offsetJson : offsetsJson) {
                offsets.push_back(deserializeVec2(offsetJson));
            }
            geometry.setMeshVertices(offsets);
            const auto base = j.find("meshBase");
            bilinearOffsets = base == j.end() || *base != "surface";
        }
    }

    // Grid-line handles (ADR-049 §3). An entry for a vertex the grid does not have
    // is dropped, as offsets of the wrong count are; a malformed one is an error.
    if (j.contains("meshTangents")) {
        const auto& tangentsJson = j["meshTangents"];
        if (!tangentsJson.is_array()) {
            throw std::runtime_error("Malformed project file: meshTangents must be an array");
        }
        for (const auto& entry : tangentsJson) {
            if (!entry.is_object() || !entry.contains("vertex") ||
                !entry["vertex"].is_number_integer() || !entry.contains("handles")) {
                throw std::runtime_error("Malformed project file: bad meshTangents entry");
            }
            const auto& handles = checkedArray(entry["handles"], GRID_HANDLE_COUNT);
            const long long vertex = entry["vertex"].get<long long>();
            if (vertex < 0 || vertex >= geometry.getVertexCount()) continue;
            // A handle pointing out of the grid is dropped too.
            const unsigned available = geometry.availableHandles(static_cast<int>(vertex));
            VertexTangents tangents;
            for (int h = 0; h < GRID_HANDLE_COUNT; ++h) {
                if (handles[static_cast<size_t>(h)].is_null()) continue;
                const Vec2 derivative = deserializeVec2(handles[static_cast<size_t>(h)]);
                if (!((available >> h) & 1u)) continue;
                tangents.derivative[static_cast<size_t>(h)] = derivative;
                tangents.explicitMask |= 1u << h;
            }
            geometry.setVertexTangent(static_cast<int>(vertex), tangents);
        }
    }

    // Bezier edges (ADR-043). Absent in pre-bezier files -> stays disabled.
    if (j.contains("bezierEnabled") && j["bezierEnabled"].get<bool>()) {
        geometry.setBezierEnabled(true);
        if (j.contains("bezierOffsets") && j["bezierOffsets"].is_array() &&
            j["bezierOffsets"].size() == 8) {
            std::array<Vec2, 8> offs;
            for (int i = 0; i < 8; ++i) {
                offs[i] = deserializeVec2(j["bezierOffsets"][i]);
            }
            geometry.setBezierOffsets(offs);
        }
    }

    // Offsets from a file without "meshBase" are relative to the bilinear corner
    // patch; keep each control vertex where the file put it (ADR-049 §2). All-zero
    // offsets are no warp, and stay on the corner-pin surface.
    if (bilinearOffsets && geometry.hasMeshModifications()) {
        geometry.convertBilinearOffsetsToResiduals();
    }
}

// Helper to serialize texture binding UV transforms
static nlohmann::json serializeTextureBinding(const TextureBinding& binding) {
    nlohmann::json j;
    j["uvOffset"] = serializeVec2(binding.uvOffset);
    j["uvScale"] = serializeVec2(binding.uvScale);
    j["uvRotation"] = binding.uvRotation;
    j["flipH"] = binding.flipH;
    j["flipV"] = binding.flipV;
    // ADR-040 crop: written only when non-identity so untouched quads (and all
    // pre-crop files) stay byte-identical.
    if (!binding.isCropIdentity()) {
        j["crop"] = {
            {"x", binding.cropRect.x}, {"y", binding.cropRect.y},
            {"width", binding.cropRect.width}, {"height", binding.cropRect.height},
        };
    }
    return j;
}

// Parse a UUID string defensively: anything missing or malformed (older/hand-edited
// files) yields null so the caller can fall back instead of throwing.
static UUID parseUuidOrNull(const nlohmann::json& v) {
    if (!v.is_string()) return UUID();
    const std::string s = v.get<std::string>();
    if (s.length() != 36) return UUID();
    try { return UUID::fromString(s); } catch (...) { return UUID(); }
}

// Parse an object's "id" field defensively (see parseUuidOrNull).
static UUID parseIdOrNull(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("id")) return UUID();
    return parseUuidOrNull(j["id"]);
}

// Helper to deserialize texture binding UV transforms
static void deserializeTextureBinding(const nlohmann::json& j, TextureBinding& binding) {
    // v3.1 source reference (ADR-046); resolved against the "sources" table after load.
    if (j.contains("mediaId")) {
        binding.mediaId = parseUuidOrNull(j["mediaId"]);
    }
    if (j.contains("uvOffset") && j["uvOffset"].is_array()) {
        binding.uvOffset = deserializeVec2(j["uvOffset"]);
    }
    if (j.contains("uvScale") && j["uvScale"].is_array()) {
        binding.uvScale = deserializeVec2(j["uvScale"]);
    }
    if (j.contains("uvRotation")) {
        binding.uvRotation = j["uvRotation"].get<float>();
    }
    if (j.contains("flipH")) {
        binding.flipH = j["flipH"].get<bool>();
    }
    if (j.contains("flipV")) {
        binding.flipV = j["flipV"].get<bool>();
    }
    if (j.contains("crop") && j["crop"].is_object()) {
        const auto& c = j["crop"];
        if (c.contains("x"))      binding.cropRect.x      = c["x"].get<float>();
        if (c.contains("y"))      binding.cropRect.y      = c["y"].get<float>();
        if (c.contains("width"))  binding.cropRect.width  = c["width"].get<float>();
        if (c.contains("height")) binding.cropRect.height = c["height"].get<float>();
    }
}

// Helper to serialize per-quad color correction (ADR-036). Optional on disk;
// only written when it differs from identity so untouched scenes are unchanged.
static nlohmann::json serializeColorCorrection(const ColorCorrection& cc) {
    nlohmann::json j;
    j["brightness"] = cc.brightness;
    j["contrast"] = cc.contrast;
    j["gamma"] = cc.gamma;
    j["rgbGain"] = serializeVec3(cc.rgbGain);
    return j;
}

// Helper to deserialize per-quad color correction. Each field is optional so
// older files (and partial objects) fall back to identity defaults.
static void deserializeColorCorrection(const nlohmann::json& j, ColorCorrection& cc) {
    if (j.contains("brightness")) cc.brightness = j["brightness"].get<float>();
    if (j.contains("contrast"))   cc.contrast   = j["contrast"].get<float>();
    if (j.contains("gamma"))      cc.gamma      = j["gamma"].get<float>();
    if (j.contains("rgbGain") && j["rgbGain"].is_array()) {
        cc.rgbGain = deserializeVec3(j["rgbGain"]);
    }
}

// Helper to serialize per-quad soft-edge blending (ADR-037). Optional on disk;
// only written when non-identity so untouched scenes are unchanged.
static nlohmann::json serializeSoftEdge(const SoftEdge& se) {
    nlohmann::json j;
    j["left"] = se.left;
    j["right"] = se.right;
    j["top"] = se.top;
    j["bottom"] = se.bottom;
    j["gamma"] = se.gamma;
    return j;
}

// Helper to deserialize per-quad soft-edge blending. Each field is optional so
// older files (and partial objects) fall back to identity (no feather) defaults.
static void deserializeSoftEdge(const nlohmann::json& j, SoftEdge& se) {
    if (j.contains("left"))   se.left   = j["left"].get<float>();
    if (j.contains("right"))  se.right  = j["right"].get<float>();
    if (j.contains("top"))    se.top    = j["top"].get<float>();
    if (j.contains("bottom")) se.bottom = j["bottom"].get<float>();
    if (j.contains("gamma"))  se.gamma  = j["gamma"].get<float>();
}

// Helper to serialize a single quad
static nlohmann::json serializeQuad(const Quad& quad, const std::string& texturePath) {
    nlohmann::json j;

    j["id"] = quad.getId().toString();
    j["name"] = quad.getName();
    j["zOrder"] = quad.getZOrder();
    j["visible"] = quad.isVisible();
    j["locked"] = quad.isLocked();
    j["opacity"] = quad.getOpacity();
    j["geometry"] = serializeQuadGeometry(quad.getGeometry());
    j["textureBinding"] = serializeTextureBinding(quad.getTexture());
    if (!quad.getColorCorrection().isIdentity()) {
        j["colorCorrection"] = serializeColorCorrection(quad.getColorCorrection());
    }
    if (!quad.getSoftEdge().isIdentity()) {
        j["softEdge"] = serializeSoftEdge(quad.getSoftEdge());
    }

    if (!texturePath.empty()) {
        j["texturePath"] = texturePath;
    }

    return j;
}

// Helper to deserialize a single quad
static Quad deserializeQuad(const nlohmann::json& j, std::string& texturePath) {
    // Preserve the persisted id so group membership (which references quad ids by value)
    // re-links to the right quad on load. The id has always been written; older files
    // without it fall back to a fresh id.
    UUID id = parseIdOrNull(j);
    Quad quad = id.isNull() ? Quad() : Quad(id);

    if (j.contains("name")) {
        quad.setName(j["name"].get<std::string>());
    }
    if (j.contains("zOrder")) {
        quad.setZOrder(j["zOrder"].get<int>());
    }
    if (j.contains("visible")) {
        quad.setVisible(j["visible"].get<bool>());
    }
    if (j.contains("locked")) {
        quad.setLocked(j["locked"].get<bool>());
    }
    if (j.contains("opacity")) {
        quad.setOpacity(j["opacity"].get<float>());
    }
    if (j.contains("geometry")) {
        deserializeQuadGeometry(j["geometry"], quad.getGeometry());
    }
    if (j.contains("textureBinding")) {
        deserializeTextureBinding(j["textureBinding"], quad.getTexture());
    }
    if (j.contains("colorCorrection")) {
        ColorCorrection cc;
        deserializeColorCorrection(j["colorCorrection"], cc);
        quad.setColorCorrection(cc);  // clamps to valid ranges
    }
    if (j.contains("softEdge")) {
        SoftEdge se;
        deserializeSoftEdge(j["softEdge"], se);
        quad.setSoftEdge(se);  // clamps to valid ranges
    }
    if (j.contains("texturePath")) {
        texturePath = j["texturePath"].get<std::string>();
    } else {
        texturePath.clear();
    }

    return quad;
}

// Helper to serialize a single group
static nlohmann::json serializeGroup(const QuadGroup& group) {
    nlohmann::json j;

    j["id"] = group.getId().toString();
    j["name"] = group.getName();
    j["visible"] = group.isVisible();
    j["locked"] = group.isLocked();
    j["collapsed"] = group.isCollapsed();

    // Serialize member quad IDs
    nlohmann::json membersJson = nlohmann::json::array();
    for (const UUID& memberId : group.getMemberQuadIds()) {
        membersJson.push_back(memberId.toString());
    }
    j["members"] = membersJson;

    // Serialize parent group if present
    if (group.getParentGroup().has_value()) {
        j["parentGroup"] = group.getParentGroup()->toString();
    }

    return j;
}

// Helper to deserialize a single group
static QuadGroup deserializeGroup(const nlohmann::json& j) {
    // Preserve the persisted id so nested-group parent references re-link on load.
    UUID id = parseIdOrNull(j);
    QuadGroup group = id.isNull() ? QuadGroup() : QuadGroup(id);

    if (j.contains("name")) {
        group.setName(j["name"].get<std::string>());
    }
    if (j.contains("visible")) {
        group.setVisible(j["visible"].get<bool>());
    }
    if (j.contains("locked")) {
        group.setLocked(j["locked"].get<bool>());
    }
    if (j.contains("collapsed")) {
        group.setCollapsed(j["collapsed"].get<bool>());
    }

    // Deserialize member quad IDs
    if (j.contains("members") && j["members"].is_array()) {
        for (const auto& memberJson : j["members"]) {
            UUID memberId = parseUuidOrNull(memberJson);
            if (!memberId.isNull()) group.addMember(memberId);
        }
    }

    // Deserialize parent group if present
    if (j.contains("parentGroup")) {
        UUID parentId = parseUuidOrNull(j["parentGroup"]);
        if (!parentId.isNull()) group.setParentGroup(parentId);
    }

    return group;
}

// Append one quad (and its texture path) to a scene DTO.
static void loadQuadIntoScene(Project::SceneData& scene, const nlohmann::json& quadJson) {
    std::string texturePath;
    Quad quad = deserializeQuad(quadJson, texturePath);
    scene.quads.push_back(std::move(quad));
    scene.texturePaths.push_back(texturePath);
}

// Serialize one scene DTO to a v3 "scenes" element. A quad's mediaId is written only
// when it names an entry of the "sources" table (knownSourceIds).
static nlohmann::json serializeScene(const Project::SceneData& scene,
                                     const std::unordered_set<UUID>& knownSourceIds,
                                     const std::vector<Project::OutputData>& outputs) {
    nlohmann::json j;
    j["id"] = scene.id.toString();
    j["name"] = scene.name;
    j["canvas"] = nlohmann::json::array({scene.canvas.x, scene.canvas.y});
    if (scene.canvasLocked) j["canvasLocked"] = true;
    // For 3.0-3.3 readers, which route one output per scene (3.4 reads outputs[].sceneId).
    for (const auto& output : outputs) {
        if (output.sceneId == scene.id) {
            j["assignedOutputId"] = output.id.toString();
            break;
        }
    }
    j["view"] = {
        {"pan", serializeVec2(scene.view.pan)},
        {"zoom", scene.view.zoom},
        {"gridVisible", scene.view.gridVisible},
        {"snapToGrid", scene.view.snapToGrid},
    };

    nlohmann::json quadsJson = nlohmann::json::array();
    for (size_t i = 0; i < scene.quads.size(); ++i) {
        std::string texPath = (i < scene.texturePaths.size()) ? scene.texturePaths[i] : "";
        nlohmann::json quadJson = serializeQuad(scene.quads[i], texPath);
        const UUID& mediaId = scene.quads[i].getTexture().mediaId;
        if (!mediaId.isNull() && knownSourceIds.count(mediaId) > 0) {
            quadJson["textureBinding"]["mediaId"] = mediaId.toString();
        }
        quadsJson.push_back(std::move(quadJson));
    }
    j["quads"] = quadsJson;

    if (!scene.groups.empty()) {
        nlohmann::json groupsJson = nlohmann::json::array();
        for (const auto& group : scene.groups) {
            groupsJson.push_back(serializeGroup(group));
        }
        j["groups"] = groupsJson;
    }
    return j;
}

// Deserialize a v2/v3 "scenes" element into a scene DTO. v2 elements simply lack
// the v3-only fields (assignedOutputId/view/id), which fall back to defaults; a
// missing canvas is left for the pre-3.4 migration. `assignedOutputId` receives the
// pre-3.4 routing (null when absent or malformed).
static Project::SceneData deserializeScene(const nlohmann::json& j, UUID& assignedOutputId) {
    Project::SceneData scene;
    scene.id = parseIdOrNull(j);
    if (scene.id.isNull()) {
        scene.id = UUID::generate();  // generate a fresh id so every scene is addressable
    }
    scene.name = j.contains("name") ? j["name"].get<std::string>() : "Scene";
    assignedOutputId = j.contains("assignedOutputId") ? parseUuidOrNull(j["assignedOutputId"])
                                                      : UUID::null();
    if (j.contains("canvas")) {
        const auto& c = checkedArray(j["canvas"], 2);
        scene.canvas = IVec2(std::clamp(c[0].get<int>(), 1, MAX_CANVAS_DIMENSION),
                             std::clamp(c[1].get<int>(), 1, MAX_CANVAS_DIMENSION));
    }
    if (j.contains("canvasLocked") && j["canvasLocked"].is_boolean()) {
        scene.canvasLocked = j["canvasLocked"].get<bool>();
    }
    if (j.contains("view") && j["view"].is_object()) {
        const auto& v = j["view"];
        if (v.contains("pan") && v["pan"].is_array()) scene.view.pan = deserializeVec2(v["pan"]);
        if (v.contains("zoom")) scene.view.zoom = v["zoom"].get<float>();
        if (v.contains("gridVisible")) scene.view.gridVisible = v["gridVisible"].get<bool>();
        if (v.contains("snapToGrid")) scene.view.snapToGrid = v["snapToGrid"].get<bool>();
    }
    if (j.contains("quads") && j["quads"].is_array()) {
        for (const auto& quadJson : j["quads"]) {
            loadQuadIntoScene(scene, quadJson);
        }
    }
    if (j.contains("groups") && j["groups"].is_array()) {
        for (const auto& groupJson : j["groups"]) {
            scene.groups.push_back(deserializeGroup(groupJson));
        }
    }
    return scene;
}

// ---- Projector camera (.qmap v3.2, ADR-052) ----------------------------------
// rotation[9] is row-major (OpenCV's R), translation[3] as in pc = R * world + t.

static nlohmann::json serializeProjector(const Project::OutputData& output) {
    const ProjectorIntrinsics& k = output.projector.intrinsics;
    const glm::mat3& r = output.projector.pose.rotation;
    nlohmann::json rotation = nlohmann::json::array();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) rotation.push_back(r[col][row]);
    }
    nlohmann::json points = nlohmann::json::array();
    for (const CalibrationPoint& p : output.calibrationPoints) {
        points.push_back({{"world", serializeVec3(p.world)},
                          {"pixel", serializeVec2(p.pixel)},
                          {"enabled", p.enabled}});
    }
    nlohmann::json j;
    j["intrinsics"] = {{"fx", k.fx}, {"fy", k.fy}, {"cx", k.cx}, {"cy", k.cy},
                       {"width", k.width}, {"height", k.height}};
    j["pose"] = {{"rotation", rotation},
                 {"translation", serializeVec3(output.projector.pose.translation)}};
    j["calibrated"] = output.projector.calibrated;
    j["points"] = points;
    return j;
}

// Numbers are checked like every other vector; a lens or rotation that cannot be a
// real projector falls back to the uncalibrated default with a warning instead of
// failing the load.
static void deserializeProjector(const nlohmann::json& j, Project::OutputData& output) {
    if (!j.is_object()) throw std::runtime_error("Malformed project file: expected a projector object");
    ProjectorCamera camera;
    if (j.contains("intrinsics")) {
        const auto& k = j["intrinsics"];
        if (!k.is_object()) throw std::runtime_error("Malformed project file: expected intrinsics");
        ProjectorIntrinsics& in = camera.intrinsics;
        if (k.contains("width"))  in.width  = std::clamp(k["width"].get<int>(), 1, kMaxOutputDimension);
        if (k.contains("height")) in.height = std::clamp(k["height"].get<int>(), 1, kMaxOutputDimension);
        if (k.contains("fx")) in.fx = readFiniteFloat(k["fx"]);
        if (k.contains("fy")) in.fy = readFiniteFloat(k["fy"]);
        if (k.contains("cx")) in.cx = readFiniteFloat(k["cx"]);
        if (k.contains("cy")) in.cy = readFiniteFloat(k["cy"]);
    }
    if (j.contains("pose")) {
        const auto& pose = j["pose"];
        if (!pose.is_object()) throw std::runtime_error("Malformed project file: expected a pose");
        if (pose.contains("rotation")) {
            const auto& r = checkedArray(pose["rotation"], 9);
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    camera.pose.rotation[col][row] = readFiniteFloat(r[row * 3 + col]);
                }
            }
        }
        if (pose.contains("translation")) camera.pose.translation = deserializeVec3(pose["translation"]);
    }
    if (j.contains("calibrated")) camera.calibrated = j["calibrated"].get<bool>();

    const glm::mat3& r = camera.pose.rotation;
    const glm::mat3 drift = glm::transpose(r) * r - glm::mat3(1.0f);
    float worst = 0.0f;
    for (int c = 0; c < 3; ++c) {
        for (int i = 0; i < 3; ++i) worst = std::max(worst, std::abs(drift[c][i]));
    }
    const bool lensOk = camera.intrinsics.fx > 0.0f && camera.intrinsics.fy > 0.0f;
    const bool rotationOk = worst < 1e-2f && glm::determinant(r) > 0.0f;
    if (!lensOk || !rotationOk) {
        Logger::warning("Output '{}': projector {} is not valid; using the default lens",
                        output.name, lensOk ? "rotation" : "focal length");
        const int width = camera.intrinsics.width;
        const int height = camera.intrinsics.height;
        camera = ProjectorCamera();
        camera.intrinsics = ProjectorIntrinsics::fromThrowRatio(1.5f, width, height);
    }
    output.projector = camera;

    if (j.contains("points")) {
        const auto& points = j["points"];
        if (!points.is_array()) throw std::runtime_error("Malformed project file: expected a points array");
        if (points.size() > kMaxCalibrationPoints) {
            throw std::runtime_error("Malformed project file: too many calibration points");
        }
        for (const auto& p : points) {
            if (!p.is_object() || !p.contains("world") || !p.contains("pixel")) {
                throw std::runtime_error("Malformed project file: bad calibration point");
            }
            CalibrationPoint point;
            point.world = deserializeVec3(p["world"]);
            point.pixel = deserializeVec2(p["pixel"]);
            if (p.contains("enabled")) point.enabled = p["enabled"].get<bool>();
            output.calibrationPoints.push_back(point);
        }
    }
}

// ---- Output slices (.qmap v3.4, ADR-047) --------------------------------------

static const char* const kSliceEdgeNames[SLICE_EDGE_COUNT] = {"left", "right", "top", "bottom"};

static nlohmann::json serializeSlice(const Slice& slice) {
    nlohmann::json blend;
    for (int e = 0; e < SLICE_EDGE_COUNT; ++e) {
        const BlendEdge& edge = slice.blend[static_cast<size_t>(e)];
        blend[kSliceEdgeNames[e]] = {{"width", edge.width}, {"gamma", edge.gamma}, {"curve", edge.curve}};
    }
    nlohmann::json j;
    j["id"] = slice.id.toString();
    j["name"] = slice.name;
    j["input"] = nlohmann::json::array({slice.input.x, slice.input.y, slice.input.width,
                                        slice.input.height});
    j["warp"] = serializeQuadGeometry(slice.warp);
    j["blend"] = blend;
    j["blackLevel"] = slice.blackLevel;
    return j;
}

// Untrusted values are bounded like every other vector: a rectangle must have a
// positive size, a blend width cannot be negative, gamma and curve stay in range.
static Slice deserializeSlice(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("Malformed project file: expected a slice object");
    Slice slice;
    slice.id = parseIdOrNull(j);
    if (slice.id.isNull()) slice.id = UUID::generate();
    if (j.contains("name")) slice.name = j["name"].get<std::string>();
    if (j.contains("input")) {
        const auto& r = checkedArray(j["input"], 4);
        const float limit = static_cast<float>(kMaxOutputDimension);
        slice.input.x = std::clamp(readFiniteFloat(r[0]), -limit, limit);
        slice.input.y = std::clamp(readFiniteFloat(r[1]), -limit, limit);
        slice.input.width = std::clamp(readFiniteFloat(r[2]), 1.0f, limit);
        slice.input.height = std::clamp(readFiniteFloat(r[3]), 1.0f, limit);
    }
    if (j.contains("warp")) {
        if (!j["warp"].is_object()) throw std::runtime_error("Malformed project file: expected a slice warp");
        deserializeQuadGeometry(j["warp"], slice.warp);
    }
    if (j.contains("blend")) {
        const auto& blend = j["blend"];
        if (!blend.is_object()) throw std::runtime_error("Malformed project file: expected a blend object");
        for (int e = 0; e < SLICE_EDGE_COUNT; ++e) {
            const auto it = blend.find(kSliceEdgeNames[e]);
            if (it == blend.end()) continue;
            if (!it->is_object()) throw std::runtime_error("Malformed project file: bad blend edge");
            BlendEdge& edge = slice.blend[static_cast<size_t>(e)];
            if (it->contains("width")) {
                edge.width = std::clamp(readFiniteFloat((*it)["width"]), 0.0f,
                                        static_cast<float>(kMaxOutputDimension));
            }
            if (it->contains("gamma")) {
                edge.gamma = std::clamp(readFiniteFloat((*it)["gamma"]), MIN_BLEND_GAMMA, MAX_BLEND_GAMMA);
            }
            if (it->contains("curve")) {
                edge.curve = std::clamp(readFiniteFloat((*it)["curve"]), MIN_BLEND_CURVE, MAX_BLEND_CURVE);
            }
        }
    }
    if (j.contains("blackLevel")) {
        slice.blackLevel = std::clamp(readFiniteFloat(j["blackLevel"]), 0.0f, MAX_BLACK_LEVEL);
    }
    return slice;
}

static nlohmann::json serializeOutput(const Project::OutputData& output) {
    nlohmann::json j;
    j["id"] = output.id.toString();
    j["name"] = output.name;
    j["monitor"] = output.monitor;
    if (!output.monitorName.empty()) {
        j["monitorName"] = output.monitorName;
        j["monitorPosition"] = nlohmann::json::array({output.monitorPosition.x,
                                                      output.monitorPosition.y});
    }
    j["resolution"] = nlohmann::json::array({output.resolution.x, output.resolution.y});
    j["rotation"] = output.rotationDegrees;
    j["enabled"] = output.enabled;
    j["projector"] = serializeProjector(output);
    if (!output.sceneId.isNull()) {
        j["sceneId"] = output.sceneId.toString();
    }
    nlohmann::json slices = nlohmann::json::array();
    for (const Slice& slice : output.slices) {
        slices.push_back(serializeSlice(slice));
    }
    j["slices"] = slices;
    return j;
}

static Project::OutputData deserializeOutput(const nlohmann::json& j) {
    Project::OutputData output;
    output.id = parseIdOrNull(j);
    if (output.id.isNull()) {
        output.id = UUID::generate();
    }
    output.name = j.contains("name") ? j["name"].get<std::string>() : "Output";
    // Clamp hardware settings to sane ranges (ADR-046 §3): -1 = no monitor chosen.
    if (j.contains("monitor")) {
        output.monitor = std::clamp(j["monitor"].get<int>(), -1, kMaxMonitorIndex);
    }
    // Monitor identity (QM-087, additive): a malformed name or position is skipped
    // with a warning rather than failing the load, since the index is still a
    // usable fallback. A name is kept only with its position: a name alone would
    // match any display at (0, 0), usually the operator's own screen (QM-090).
    if (j.contains("monitorName") && j["monitorName"].is_string()) {
        const auto pos = j.find("monitorPosition");
        if (pos != j.end() && pos->is_array() && pos->size() == 2 &&
            (*pos)[0].is_number_integer() && (*pos)[1].is_number_integer()) {
            output.monitorName = j["monitorName"].get<std::string>();
            // Only ever compared for equality, so any integer is safe.
            output.monitorPosition = IVec2((*pos)[0].get<int>(), (*pos)[1].get<int>());
        } else {
            Logger::warning("Output '{}': missing or malformed monitorPosition; ignoring its monitor name",
                            output.name);
        }
    }
    if (j.contains("resolution")) {
        const auto& r = checkedArray(j["resolution"], 2);
        output.resolution = IVec2(std::clamp(r[0].get<int>(), 1, kMaxOutputDimension),
                                  std::clamp(r[1].get<int>(), 1, kMaxOutputDimension));
    }
    if (j.contains("rotation")) {
        // Snap to the nearest supported quarter turn in [0, 360).
        const int deg = ((j["rotation"].get<int>() % 360) + 360) % 360;
        output.rotationDegrees = ((deg + 45) / 90 % 4) * 90;
    }
    if (j.contains("enabled")) output.enabled = j["enabled"].get<bool>();
    if (j.contains("projector")) {
        deserializeProjector(j["projector"], output);
    } else {
        // Files before v3.2: an uncalibrated projector with the default lens for this output.
        output.projector.intrinsics =
            ProjectorIntrinsics::fromThrowRatio(1.5f, output.resolution.x, output.resolution.y);
    }
    // v3.4 routing (ADR-047); earlier files are migrated in loadProjectModel.
    if (j.contains("sceneId")) {
        output.sceneId = parseUuidOrNull(j["sceneId"]);  // bad id = no scene
    }
    if (j.contains("slices")) {
        const auto& slices = j["slices"];
        if (!slices.is_array()) throw std::runtime_error("Malformed project file: expected a slices array");
        if (slices.size() > kMaxSlicesPerOutput) {
            throw std::runtime_error("Malformed project file: too many slices");
        }
        for (const auto& sliceJson : slices) {
            output.slices.push_back(deserializeSlice(sliceJson));
        }
    }
    return output;
}

// ---- Sources table (.qmap v3.1, ADR-046) ------------------------------------

static bool isFileSourceKind(const std::string& kind) {
    return kind == "image" || kind == "video" || kind == "hap";
}

static bool isKnownSourceKind(const std::string& kind) {
    return isFileSourceKind(kind) || kind == "procedural" || kind == "camera";
}

static const char* loopModeName(LoopMode mode) {
    switch (mode) {
        case LoopMode::ONCE:      return "once";
        case LoopMode::PING_PONG: return "pingPong";
        default:                  return "loop";
    }
}

static LoopMode parseLoopMode(const std::string& name) {
    if (name == "once") return LoopMode::ONCE;
    if (name == "pingPong") return LoopMode::PING_PONG;
    return LoopMode::LOOP;
}

// Media paths are written relative to the project directory when they share a root
// (portable show folders), else absolute. baseDir empty = no project file: absolute.
// Paths are UTF-8 in the model and on disk.
static std::string toProjectRelative(const std::string& path, const fs::path& baseDir) {
    const fs::path p = pathFromUtf8(path);
    if (baseDir.empty() || !p.is_absolute()) return path;
    const fs::path rel = p.lexically_relative(baseDir);
    return rel.empty() ? path : rel.generic_u8string();
}

static nlohmann::json serializeSource(const Project::SourceData& s, const fs::path& baseDir) {
    nlohmann::json j;
    j["id"] = s.id.toString();
    j["kind"] = s.kind;
    if (isFileSourceKind(s.kind)) {
        j["path"] = toProjectRelative(s.path, baseDir);
        if (s.transport && s.kind != "image") {
            j["transport"] = {
                {"playing", s.transport->playing},
                {"loopMode", loopModeName(s.transport->loopMode)},
                {"speed", s.transport->speed},
                {"timelineLocked", s.transport->timelineLocked},
                {"volume", s.transport->volume},
                {"muted", s.transport->muted},
            };
        }
    } else if (s.kind == "procedural") {
        j["proceduralType"] = s.proceduralType;
        j["width"] = s.width;
        j["height"] = s.height;
    } else if (s.kind == "camera") {
        j["deviceName"] = s.deviceName;
    }
    return j;
}

// Parse one "sources" entry; entries with no usable id/kind/path are skipped (nullopt)
// so the quads that reference them fall back to texturePath.
static std::optional<Project::SourceData> deserializeSource(const nlohmann::json& j,
                                                            const fs::path& baseDir) {
    Project::SourceData s;
    s.id = parseIdOrNull(j);
    if (s.id.isNull() || !j.contains("kind") || !j["kind"].is_string()) return std::nullopt;
    s.kind = j["kind"].get<std::string>();
    if (!isKnownSourceKind(s.kind)) return std::nullopt;

    if (isFileSourceKind(s.kind)) {
        if (!j.contains("path") || !j["path"].is_string()) return std::nullopt;
        fs::path p = pathFromUtf8(j["path"].get<std::string>());
        if (p.empty()) return std::nullopt;
        if (p.is_relative() && !baseDir.empty()) p = (baseDir / p).lexically_normal();
        s.path = utf8FromPath(p);
        if (s.kind != "image" && j.contains("transport") && j["transport"].is_object()) {
            const auto& t = j["transport"];
            Project::TransportData transport;
            if (t.contains("playing")) transport.playing = t["playing"].get<bool>();
            if (t.contains("loopMode") && t["loopMode"].is_string()) {
                transport.loopMode = parseLoopMode(t["loopMode"].get<std::string>());
            }
            if (t.contains("speed")) {
                transport.speed = std::clamp(readFiniteFloat(t["speed"]), kMinPlaybackSpeed,
                                             kMaxPlaybackSpeed);
            }
            // Additive (ADR-050): older files have no lock and play on their own clocks.
            if (t.contains("timelineLocked") && t["timelineLocked"].is_boolean()) {
                transport.timelineLocked = t["timelineLocked"].get<bool>();
            }
            // Additive (ADR-050 §5): older files play at full volume, unmuted.
            if (t.contains("volume") && t["volume"].is_number()) {
                transport.volume = std::clamp(readFiniteFloat(t["volume"]), 0.0f, 1.0f);
            }
            if (t.contains("muted") && t["muted"].is_boolean()) {
                transport.muted = t["muted"].get<bool>();
            }
            s.transport = transport;
        }
    } else if (s.kind == "procedural") {
        if (!j.contains("proceduralType") || !j["proceduralType"].is_string()) return std::nullopt;
        s.proceduralType = j["proceduralType"].get<std::string>();
        if (j.contains("width"))  s.width  = std::clamp(j["width"].get<int>(), 1, kMaxOutputDimension);
        if (j.contains("height")) s.height = std::clamp(j["height"].get<int>(), 1, kMaxOutputDimension);
    } else if (j.contains("deviceName") && j["deviceName"].is_string()) {  // camera
        s.deviceName = j["deviceName"].get<std::string>();
    }
    return s;
}

// ---- 3D objects (.qmap v3.2, ADR-052) ------------------------------------------
// transform[16] is column-major, as glm stores it. The mesh is re-imported from path.
// A relative path gets its absolute form beside it as "absPath" (v3.3, TD-16), so a
// show file moved away from a mesh outside its folder still finds the mesh.

static nlohmann::json serializeObject(const Project::ObjectData& object,
                                      const std::unordered_set<UUID>& knownSourceIds,
                                      const fs::path& baseDir) {
    nlohmann::json transform = nlohmann::json::array();
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) transform.push_back(object.transform[col][row]);
    }
    nlohmann::json j;
    j["id"] = object.id.toString();
    j["name"] = object.name;
    const std::string written = toProjectRelative(object.path, baseDir);
    j["path"] = written;
    if (written != object.path) j["absPath"] = object.path;
    j["transform"] = transform;
    j["texture"] = serializeTextureBinding(object.texture);
    const UUID& mediaId = object.texture.mediaId;
    if (!mediaId.isNull() && knownSourceIds.count(mediaId) > 0) {
        j["texture"]["mediaId"] = mediaId.toString();
    }
    j["visible"] = object.visible;
    j["opacity"] = object.opacity;
    return j;
}

static Project::ObjectData deserializeObject(const nlohmann::json& j, const fs::path& baseDir) {
    if (!j.is_object()) throw std::runtime_error("Malformed project file: expected an object entry");
    Project::ObjectData object;
    object.id = parseIdOrNull(j);
    if (object.id.isNull()) object.id = UUID::generate();
    object.name = (j.contains("name") && j["name"].is_string()) ? j["name"].get<std::string>()
                                                                : "Object";
    // No usable path keeps the object (its transform and media) with an empty mesh.
    if (j.contains("path") && j["path"].is_string()) {
        fs::path p = pathFromUtf8(j["path"].get<std::string>());
        if (!p.empty() && p.is_relative() && !baseDir.empty()) p = (baseDir / p).lexically_normal();
        object.path = utf8FromPath(p);
    }
    // The absolute fallback is used only when "path" does not lead to a file and it
    // does (or "path" is empty). Otherwise "path" stays, for Missing Media to show.
    if (j.contains("absPath") && j["absPath"].is_string()) {
        const fs::path fallback = pathFromUtf8(j["absPath"].get<std::string>());
        std::error_code ec;
        if (!fallback.empty() && fallback.is_absolute() &&
            (object.path.empty() ||
             (!fs::exists(pathFromUtf8(object.path), ec) && fs::exists(fallback, ec)))) {
            object.path = utf8FromPath(fallback.lexically_normal());
        }
    }
    if (j.contains("transform")) {
        const auto& t = checkedArray(j["transform"], 16);
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                object.transform[col][row] = readFiniteFloat(t[col * 4 + row]);
            }
        }
    }
    if (j.contains("texture") && j["texture"].is_object()) {
        deserializeTextureBinding(j["texture"], object.texture);
    }
    if (j.contains("visible")) object.visible = j["visible"].get<bool>();
    if (j.contains("opacity")) object.opacity = std::clamp(readFiniteFloat(j["opacity"]), 0.0f, 1.0f);
    return object;
}

// ---- External control (ADR-048) -------------------------------------------------
// "control": {"midi": [{device, type, channel, number, target, mode[, bits]}, ...]}.
// It is show content, but never worth failing a load for: a malformed block or entry
// is skipped (logged), the rest of the show loads. "bits" is 7 (the default, not
// written) or 14, which only a cc 0-31 can be.

static nlohmann::json serializeMidiMapping(const Project::MidiMappingData& m) {
    nlohmann::json j;
    j["device"] = m.device;
    j["type"] = m.type;
    j["channel"] = m.channel;
    j["number"] = m.number;
    j["target"] = m.target;
    j["mode"] = m.mode;
    if (m.bits == 14) j["bits"] = 14;
    return j;
}

static std::optional<Project::MidiMappingData> deserializeMidiMapping(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    auto stringField = [&](const char* key) -> std::optional<std::string> {
        if (!j.contains(key) || !j[key].is_string()) return std::nullopt;
        return j[key].get<std::string>();
    };
    auto intField = [&](const char* key, int lo, int hi) -> std::optional<int> {
        if (!j.contains(key) || !j[key].is_number_integer()) return std::nullopt;
        const auto v = j[key].get<long long>();
        if (v < lo || v > hi) return std::nullopt;
        return static_cast<int>(v);
    };
    const auto type = stringField("type");
    const auto mode = stringField("mode");
    const auto target = stringField("target");
    const auto channel = intField("channel", 0, 16);
    const auto number = intField("number", 0, 127);
    if (!type || (*type != "note" && *type != "cc" && *type != "program")) return std::nullopt;
    if (!mode || (*mode != "trigger" && *mode != "toggle" && *mode != "momentary" && *mode != "value")) {
        return std::nullopt;
    }
    if (!target || target->empty() || (*target)[0] != '/' || !channel || !number) return std::nullopt;
    int bits = 7;
    if (j.contains("bits")) {
        const auto b = intField("bits", 7, 14);
        if (!b || (*b != 7 && *b != 14)) return std::nullopt;
        if (*b == 14 && (*type != "cc" || *number > 31)) return std::nullopt;
        bits = *b;
    }
    Project::MidiMappingData m;
    m.bits = bits;
    m.device = stringField("device").value_or("any");
    if (m.device.empty()) m.device = "any";
    m.type = *type;
    m.channel = *channel;
    m.number = *number;
    m.target = *target;
    m.mode = *mode;
    return m;
}

static void loadControl(const nlohmann::json& json, Project& project) {
    if (!json.contains("control")) return;
    const auto& control = json["control"];
    if (!control.is_object() || !control.contains("midi") || !control["midi"].is_array()) {
        Logger::warning("Project: ignoring a malformed \"control\" block");
        return;
    }
    size_t skipped = 0;
    for (const auto& entry : control["midi"]) {
        if (auto mapping = deserializeMidiMapping(entry)) {
            project.getMidiMappings().push_back(std::move(*mapping));
        } else {
            ++skipped;
        }
    }
    if (skipped > 0) Logger::warning("Project: skipped {} malformed MIDI mapping(s)", skipped);
}

// ---- Show sound (ADR-050 §5) -------------------------------------------------
// "audio": {"masterVolume": 0-1, "masterMuted": bool}. Missing or malformed fields keep
// the defaults (full volume, not muted), so older files play as they always did.

static nlohmann::json serializeAudio(const Project::AudioData& audio) {
    return {{"masterVolume", audio.masterVolume}, {"masterMuted", audio.masterMuted}};
}

static void loadAudio(const nlohmann::json& json, Project& project) {
    if (!json.contains("audio") || !json["audio"].is_object()) return;
    const auto& a = json["audio"];
    Project::AudioData& audio = project.getAudio();
    if (a.contains("masterVolume") && a["masterVolume"].is_number()) {
        audio.masterVolume = std::clamp(readFiniteFloat(a["masterVolume"]), 0.0f, 1.0f);
    }
    if (a.contains("masterMuted") && a["masterMuted"].is_boolean()) {
        audio.masterMuted = a["masterMuted"].get<bool>();
    }
}

// Resolve each quad's and object's mediaId against the sources table (ADR-046). A
// reference to an id the table does not hold is cleared, so a quad falls back to its
// absolute texturePath (the path every v1/v2/v3.0 file loads by) and an object shows
// untextured. A file source whose file is gone keeps its references and its path and
// is flagged missing, for the user to relocate (PRD §4.1.8); so is an object's mesh.
static void resolveQuadSources(Project& project) {
    std::unordered_map<UUID, const Project::SourceData*> byId;
    for (auto& s : project.getSources()) {
        if (isFileSourceKind(s.kind)) {
            std::error_code ec;
            s.missing = !fs::exists(pathFromUtf8(s.path), ec);
        }
        byId[s.id] = &s;
    }

    auto resolve = [&](UUID& mediaId) {
        if (!mediaId.isNull() && byId.count(mediaId) == 0) mediaId = UUID();
    };
    for (auto& scene : project.getScenes()) {
        for (auto& quad : scene.quads) resolve(quad.getTexture().mediaId);
        for (auto& object : scene.objects) {
            resolve(object.texture.mediaId);
            std::error_code ec;
            object.missing = !object.path.empty() && !fs::exists(pathFromUtf8(object.path), ec);
        }
    }
}

// True for a file of format 3.4 or later, which stores canvases, output routing and
// slices (ADR-047). The version was validated before the model is loaded.
static bool hasCanvasSlices(const nlohmann::json& json) {
    const std::string version = json["version"].get<std::string>();
    const char* end = version.data() + version.size();
    int major = 0, minor = 0;
    const auto [afterMajor, majorError] = std::from_chars(version.data(), end, major);
    if (majorError != std::errc()) return false;
    if (afterMajor != end && *afterMajor == '.') std::from_chars(afterMajor + 1, end, minor);
    return major > 3 || (major == 3 && minor >= 4);
}

static IVec2 effectiveResolution(const Project::OutputData& output) {
    const bool quarterTurn = output.rotationDegrees == 90 || output.rotationDegrees == 270;
    return quarterTurn ? IVec2(output.resolution.y, output.resolution.x) : output.resolution;
}

static IVec2 clampCanvas(IVec2 size) {
    return IVec2(std::clamp(size.x, 1, MAX_CANVAS_DIMENSION), std::clamp(size.y, 1, MAX_CANVAS_DIMENSION));
}

// Pre-3.4 files (ADR-047 §7): each output shows the scene whose assignedOutputId
// named it (the first such scene in list order, as the old routing drew), through one
// full-canvas slice with no warp and no blend. A scene's canvas is its output's
// effective resolution (rotated, as the editor laid the quads out on it). A scene no
// output shows gets the first output's effective resolution, the projector it is most
// likely taken to, else 1920x1080. Outputs no scene named show nothing and get no
// slice.
static void migrateToCanvasSlices(Project& project, const std::vector<UUID>& assignedOutputIds) {
    auto& scenes = project.getScenes();
    const auto& outputs = project.getOutputs();
    const IVec2 unshown = outputs.empty() ? IVec2(DEFAULT_CANVAS_WIDTH, DEFAULT_CANVAS_HEIGHT)
                                          : clampCanvas(effectiveResolution(outputs.front()));
    for (size_t i = 0; i < scenes.size(); ++i) {
        scenes[i].canvas = unshown;
    }
    for (auto& output : project.getOutputs()) {
        for (size_t i = 0; i < scenes.size() && i < assignedOutputIds.size(); ++i) {
            if (assignedOutputIds[i] != output.id) continue;
            scenes[i].canvas = clampCanvas(effectiveResolution(output));  // a scene named one output at most
            output.sceneId = scenes[i].id;
            output.slices.assign(1, makeFullCanvasSlice(scenes[i].canvas));
            break;
        }
    }
}

// A 3.4 output that names a scene but has no slices would show nothing: it gets one
// whole-canvas slice, as routing that scene to it would give.
static void sliceRoutedOutputs(Project& project) {
    for (auto& output : project.getOutputs()) {
        if (output.sceneId.isNull() || !output.slices.empty()) continue;
        for (const auto& scene : project.getScenes()) {
            if (scene.id == output.sceneId) {
                output.slices.assign(1, makeFullCanvasSlice(scene.canvas));
                break;
            }
        }
    }
}

// Rebuild the flat quads_/texturePaths_/groups_ as the union of every scene, so
// flat consumers (and the existing flat-API tests) keep working after a v3 load.
static void flattenScenesToFlat(Project& project) {
    project.clearQuads();
    project.getTexturePaths().clear();
    project.clearGroups();
    for (const auto& scene : project.getScenes()) {
        for (size_t i = 0; i < scene.quads.size(); ++i) {
            project.getQuads().push_back(scene.quads[i]);
            project.getTexturePaths().push_back(
                i < scene.texturePaths.size() ? scene.texturePaths[i] : std::string());
        }
        for (const auto& group : scene.groups) {
            project.getGroups().push_back(group);
        }
    }
}

// Load a JSON document into the Project model (ADR-032 / ADR-039). Every read path
// yields >= 1 scene: v1.x flat "quads" wrap into one scene; v2.x/v3.x "scenes" map
// directly; v3.x also restores the "outputs" array and v3.1 the "sources" table
// (relative paths resolve against baseDir, the project file's directory; empty =
// leave them as written). The flat quads_/groups_ are then derived from the scenes
// so flat consumers stay backward-compatible.
static void loadProjectModel(const nlohmann::json& json, Project& project,
                             const fs::path& baseDir) {
    if (json.contains("name")) {
        project.setName(json["name"].get<std::string>());
    }

    auto& scenes = project.getScenes();
    std::vector<UUID> assignedOutputIds;  // parallel to scenes: pre-3.4 routing
    if (json.contains("scenes") && json["scenes"].is_array()) {
        for (const auto& sceneJson : json["scenes"]) {
            UUID assignedOutputId;
            scenes.push_back(deserializeScene(sceneJson, assignedOutputId));
            assignedOutputIds.push_back(assignedOutputId);
            if (sceneJson.is_object() && sceneJson.contains("objects")) {
                const auto& objectsJson = sceneJson["objects"];
                if (!objectsJson.is_array()) {
                    throw std::runtime_error("Malformed project file: expected an objects array");
                }
                for (const auto& objectJson : objectsJson) {
                    scenes.back().objects.push_back(deserializeObject(objectJson, baseDir));
                }
            }
        }
    } else {
        // v1.x flat: wrap the top-level quads/groups into a single scene.
        Project::SceneData scene;
        scene.id = UUID::generate();
        scene.name = "Scene 1";
        if (json.contains("quads") && json["quads"].is_array()) {
            for (const auto& quadJson : json["quads"]) {
                loadQuadIntoScene(scene, quadJson);
            }
        }
        if (json.contains("groups") && json["groups"].is_array()) {
            for (const auto& groupJson : json["groups"]) {
                scene.groups.push_back(deserializeGroup(groupJson));
            }
        }
        scenes.push_back(std::move(scene));
    }

    // Guarantee at least one scene so the editor is never sceneless.
    if (scenes.empty()) {
        Project::SceneData scene;
        scene.id = UUID::generate();
        scene.name = "Scene 1";
        scenes.push_back(std::move(scene));
    }

    if (json.contains("outputs") && json["outputs"].is_array()) {
        for (const auto& outputJson : json["outputs"]) {
            project.getOutputs().push_back(deserializeOutput(outputJson));
        }
    }
    if (hasCanvasSlices(json)) {
        sliceRoutedOutputs(project);
    } else {
        migrateToCanvasSlices(project, assignedOutputIds);
    }

    if (json.contains("sources") && json["sources"].is_array()) {
        for (const auto& sourceJson : json["sources"]) {
            if (auto source = deserializeSource(sourceJson, baseDir)) {
                project.getSources().push_back(std::move(*source));
            }
        }
    }
    resolveQuadSources(project);
    loadControl(json, project);
    loadAudio(json, project);

    flattenScenesToFlat(project);
}

// Build the on-disk JSON for a project. Writes v3 (scenes + outputs) when the
// project carries scene DTOs; otherwise the legacy v1.2 flat format, so a flat-only
// Project (and the existing flat round-trip tests) is byte-compatible with before.
// baseDir is the project file's directory, used to write relative media paths.
static nlohmann::json buildProjectJson(const Project& project, const fs::path& baseDir) {
    nlohmann::json json;
    json["$schema"] = "quadmapper/project/1.0";

    if (!project.getScenes().empty()) {
        json["version"] = kSceneFormatVersion;
        json["name"] = project.getName();

        std::unordered_set<UUID> sourceIds;
        nlohmann::json sourcesJson = nlohmann::json::array();
        for (const auto& source : project.getSources()) {
            sourceIds.insert(source.id);
            sourcesJson.push_back(serializeSource(source, baseDir));
        }
        json["sources"] = sourcesJson;

        nlohmann::json scenesJson = nlohmann::json::array();
        for (const auto& scene : project.getScenes()) {
            nlohmann::json sceneJson = serializeScene(scene, sourceIds, project.getOutputs());
            if (!scene.objects.empty()) {
                nlohmann::json objectsJson = nlohmann::json::array();
                for (const auto& object : scene.objects) {
                    objectsJson.push_back(serializeObject(object, sourceIds, baseDir));
                }
                sceneJson["objects"] = objectsJson;
            }
            scenesJson.push_back(std::move(sceneJson));
        }
        json["scenes"] = scenesJson;

        if (!project.getOutputs().empty()) {
            nlohmann::json outputsJson = nlohmann::json::array();
            for (const auto& output : project.getOutputs()) {
                outputsJson.push_back(serializeOutput(output));
            }
            json["outputs"] = outputsJson;
        }

        if (!project.getMidiMappings().empty()) {
            nlohmann::json midiJson = nlohmann::json::array();
            for (const auto& mapping : project.getMidiMappings()) {
                midiJson.push_back(serializeMidiMapping(mapping));
            }
            json["control"] = {{"midi", midiJson}};
        }
        json["audio"] = serializeAudio(project.getAudio());
        return json;
    }

    json["version"] = "1.2.0";  // 1.2: additive non-uniform control grid (ADR-038)
    json["name"] = project.getName();

    const auto& quads = project.getQuads();
    const auto& texturePaths = project.getTexturePaths();
    nlohmann::json quadsJson = nlohmann::json::array();
    for (size_t i = 0; i < quads.size(); ++i) {
        std::string texPath = (i < texturePaths.size()) ? texturePaths[i] : "";
        quadsJson.push_back(serializeQuad(quads[i], texPath));
    }
    json["quads"] = quadsJson;

    const auto& groups = project.getGroups();
    if (!groups.empty()) {
        nlohmann::json groupsJson = nlohmann::json::array();
        for (const auto& group : groups) {
            groupsJson.push_back(serializeGroup(group));
        }
        json["groups"] = groupsJson;
    }
    return json;
}

// Absolute directory of a project file, for relative media paths ("" on failure).
static fs::path projectBaseDir(const fs::path& projectFile) {
    std::error_code ec;
    const fs::path abs = fs::absolute(projectFile, ec);
    return ec ? fs::path() : abs.parent_path();
}

Result<std::string, Error> ProjectSerializer::serializeFor(const Project& project,
                                                           const std::filesystem::path& path) {
    try {
        return Ok(buildProjectJson(project, projectBaseDir(path)).dump(2));
    } catch (const std::exception& e) {
        return Err<std::string>(std::string("Failed to serialize project: ") + e.what());
    }
}

Result<void, Error> ProjectSerializer::save(const Project& project, const std::filesystem::path& path) {
    Logger::info("Saving project to: {}", utf8FromPath(path));
    auto text = serializeFor(project, path);
    if (!text) return Err(text.error().message);
    return FileSystem::writeFileAtomic(path, text.value());
}

Result<Project, Error> ProjectSerializer::load(const std::filesystem::path& path) {
    Logger::info("Loading project from: {}", utf8FromPath(path));

    auto result = FileSystem::readFile(path);
    if (!result) {
        return Err<Project>(result.error().message);
    }

    Project project;
    try {
        auto json = nlohmann::json::parse(result.value());

        auto versionResult = validateVersion(json);
        if (versionResult.isError()) {
            return Err<Project>(versionResult.error());
        }

        loadProjectModel(json, project, projectBaseDir(path));

        project.setFilePath(utf8FromPath(path));

        Logger::info("Loaded project '{}' with {} scene(s), {} quads and {} groups",
                     project.getName(), project.getScenes().size(),
                     project.getQuads().size(), project.getGroups().size());

    } catch (const std::exception& e) {
        return Err<Project>(std::string("Failed to parse project: ") + e.what());
    }

    return Ok(std::move(project));
}

std::string ProjectSerializer::serialize(const Project& project) {
    return buildProjectJson(project, fs::path()).dump(2);
}

Result<void, std::string> ProjectSerializer::deserialize(const std::string& jsonStr, Project& project) {
    if (jsonStr.empty()) {
        return Result<void, std::string>::error("Empty JSON string");
    }

    try {
        auto json = nlohmann::json::parse(jsonStr);

        auto versionResult = validateVersion(json);
        if (versionResult.isError()) {
            return Result<void, std::string>::error(versionResult.error());
        }

        // Clear existing data, then load.
        project.clearQuads();
        project.getTexturePaths().clear();
        project.clearGroups();
        project.getScenes().clear();
        project.getOutputs().clear();
        project.getSources().clear();
        project.getMidiMappings().clear();

        loadProjectModel(json, project, fs::path());

        return Result<void, std::string>::success();
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(std::string("Parse error: ") + e.what());
    }
}

} // namespace quadmapper
