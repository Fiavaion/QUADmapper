#include "ProjectSerializer.hpp"
#include "core/FileSystem.hpp"
#include "core/Logger.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
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
// Readers accept any major <= kMaxSupportedMajor; v1/v2 are wrapped/mapped into the
// scene model so every read path yields >= 1 scene.
static constexpr int kMaxSupportedMajor = 3;
static constexpr const char* kSceneFormatVersion = "3.1.0";

// Load-time clamps for untrusted values (ADR-046 §3).
static constexpr int kMaxMonitorIndex = 63;
static constexpr int kMaxOutputDimension = 16384;
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

    // Serialize mesh vertex offsets if any exist
    if (geometry.hasMeshModifications()) {
        const auto& meshVertices = geometry.getMeshVertices();
        nlohmann::json meshOffsetsJson = nlohmann::json::array();
        for (const auto& offset : meshVertices) {
            meshOffsetsJson.push_back(serializeVec2(offset));
        }
        j["meshOffsets"] = meshOffsetsJson;
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

    // Deserialize mesh vertex offsets if present
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
                                     const std::unordered_set<UUID>& knownSourceIds) {
    nlohmann::json j;
    j["id"] = scene.id.toString();
    j["name"] = scene.name;
    if (!scene.assignedOutputId.isNull()) {
        j["assignedOutputId"] = scene.assignedOutputId.toString();
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
// the v3-only fields (assignedOutputId/view/id), which fall back to defaults.
static Project::SceneData deserializeScene(const nlohmann::json& j) {
    Project::SceneData scene;
    scene.id = parseIdOrNull(j);
    if (scene.id.isNull()) {
        scene.id = UUID::generate();  // generate a fresh id so every scene is addressable
    }
    scene.name = j.contains("name") ? j["name"].get<std::string>() : "Scene";
    if (j.contains("assignedOutputId")) {
        scene.assignedOutputId = parseUuidOrNull(j["assignedOutputId"]);  // bad id = unassigned
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

static nlohmann::json serializeOutput(const Project::OutputData& output) {
    nlohmann::json j;
    j["id"] = output.id.toString();
    j["name"] = output.name;
    j["monitor"] = output.monitor;
    j["resolution"] = nlohmann::json::array({output.resolution.x, output.resolution.y});
    j["rotation"] = output.rotationDegrees;
    j["enabled"] = output.enabled;
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
static std::string toProjectRelative(const std::string& path, const fs::path& baseDir) {
    const fs::path p(path);
    if (baseDir.empty() || !p.is_absolute()) return path;
    const fs::path rel = p.lexically_relative(baseDir);
    return rel.empty() ? path : rel.generic_string();
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
        fs::path p(j["path"].get<std::string>());
        if (p.empty()) return std::nullopt;
        if (p.is_relative() && !baseDir.empty()) p = (baseDir / p).lexically_normal();
        s.path = p.string();
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

// Resolve each quad's mediaId against the sources table (ADR-046): a reference to a
// missing entry, or to a file source whose file is gone, is cleared so the quad falls
// back to its absolute texturePath - the same path every v1/v2/v3.0 file loads by.
static void resolveQuadSources(Project& project) {
    std::unordered_map<UUID, const Project::SourceData*> byId;
    for (const auto& s : project.getSources()) byId[s.id] = &s;

    for (auto& scene : project.getScenes()) {
        for (auto& quad : scene.quads) {
            UUID& mediaId = quad.getTexture().mediaId;
            if (mediaId.isNull()) continue;
            auto it = byId.find(mediaId);
            bool usable = (it != byId.end());
            if (usable && isFileSourceKind(it->second->kind)) {
                std::error_code ec;
                usable = fs::exists(it->second->path, ec);
            }
            if (!usable) mediaId = UUID();
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
    if (json.contains("scenes") && json["scenes"].is_array()) {
        for (const auto& sceneJson : json["scenes"]) {
            scenes.push_back(deserializeScene(sceneJson));
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

    if (json.contains("sources") && json["sources"].is_array()) {
        for (const auto& sourceJson : json["sources"]) {
            if (auto source = deserializeSource(sourceJson, baseDir)) {
                project.getSources().push_back(std::move(*source));
            }
        }
    }
    resolveQuadSources(project);

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
            scenesJson.push_back(serializeScene(scene, sourceIds));
        }
        json["scenes"] = scenesJson;

        if (!project.getOutputs().empty()) {
            nlohmann::json outputsJson = nlohmann::json::array();
            for (const auto& output : project.getOutputs()) {
                outputsJson.push_back(serializeOutput(output));
            }
            json["outputs"] = outputsJson;
        }
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
    Logger::info("Saving project to: {}", path.string());
    auto text = serializeFor(project, path);
    if (!text) return Err(text.error().message);
    return FileSystem::writeFileAtomic(path, text.value());
}

Result<Project, Error> ProjectSerializer::load(const std::filesystem::path& path) {
    Logger::info("Loading project from: {}", path.string());

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

        project.setFilePath(path.string());
        project.setModified(false);

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

        loadProjectModel(json, project, fs::path());

        project.setModified(false);

        return Result<void, std::string>::success();
    } catch (const std::exception& e) {
        return Result<void, std::string>::error(std::string("Parse error: ") + e.what());
    }
}

} // namespace quadmapper
