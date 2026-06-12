#pragma once

#include <glm/glm.hpp>
#include <string>
#include <cstdint>

namespace quadmapper {

// Forward declarations
class UUID;

// Common type aliases
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using Mat4 = glm::mat4;

// Color type (RGBA, 0.0-1.0)
using Color = glm::vec4;

// Rectangle structure
struct Rectangle {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    Rectangle() = default;
    Rectangle(int x, int y, int w, int h) : x(x), y(y), width(w), height(h) {}
};

// Normalized rectangle (0.0-1.0 range)
struct NormalizedRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
};

// Rotation enum for outputs
enum class Rotation {
    R0 = 0,
    R90 = 90,
    R180 = 180,
    R270 = 270
};

// Media type enum
enum class MediaType {
    IMAGE,
    VIDEO,
    AUDIO,
    PROCEDURAL
};

// Texture source type enum
enum class SourceType {
    STATIC_IMAGE,
    VIDEO_FILE,
    WEBCAM,
    NDI,
    CAPTURE_CARD,
    PROCEDURAL_SOLID,
    PROCEDURAL_GRADIENT,
    PROCEDURAL_CHECKERBOARD,
    PROCEDURAL_TEST_PATTERN,
    PROCEDURAL_NOISE,
    PROCEDURAL_PLASMA,
    PROCEDURAL_GRID,
    SPOUT,
    SYPHON
};

// Procedural texture type enum
enum class ProceduralType {
    SOLID_COLOR,
    GRADIENT,
    CHECKERBOARD,
    TEST_PATTERN,
    NOISE,
    PLASMA,
    GRID
};

// Mesh edit mode
enum class MeshMode {
    CORNERS_ONLY,
    MESH_EDIT
};

// Playback state
enum class PlaybackState {
    STOPPED,
    PLAYING,
    PAUSED,
    FINISHED
};

// Loop mode
enum class LoopMode {
    ONCE,
    LOOP,
    PING_PONG
};

} // namespace quadmapper
