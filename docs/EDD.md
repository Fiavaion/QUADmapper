# QUADmapper Engineering Design Document (EDD)

**Document Version:** 1.0
**Date:** 2025-12-19
**Status:** Approved

---

## 1. System Architecture

### 1.1 High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      QUADmapper Application                      │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐              │
│  │   Project   │  │    Scene    │  │   Output    │              │
│  │   Manager   │  │   Manager   │  │   Manager   │              │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘              │
│         │                │                │                      │
│  ┌──────┴────────────────┴────────────────┴──────┐              │
│  │                 Core Engine                    │              │
│  ├───────────────────────────────────────────────┤              │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐       │              │
│  │  │ Renderer│  │  Media  │  │  Audio  │       │              │
│  │  │  (GPU)  │  │ Decoder │  │  Mixer  │       │              │
│  │  └─────────┘  └─────────┘  └─────────┘       │              │
│  └───────────────────────────────────────────────┘              │
│                                                                  │
│  ┌───────────────────────────────────────────────┐              │
│  │              User Interface (ImGui)            │              │
│  └───────────────────────────────────────────────┘              │
└─────────────────────────────────────────────────────────────────┘
         │                    │                    │
         ▼                    ▼                    ▼
┌─────────────┐      ┌─────────────┐      ┌─────────────┐
│  Output 1   │      │  Output 2   │      │  Output N   │
│  (Window)   │      │  (Window)   │      │  (Window)   │
└─────────────┘      └─────────────┘      └─────────────┘
```

### 1.2 Component Responsibilities

| Component | Responsibility |
|-----------|----------------|
| **Project Manager** | Load/save projects, manage project state |
| **Scene Manager** | Create/switch scenes, manage quad hierarchy |
| **Output Manager** | Create/configure output windows, handle Perform Mode |
| **Renderer** | OpenGL rendering, quad warping, mesh subdivision |
| **Media Decoder** | FFmpeg video decoding, image loading |
| **Audio Mixer** | Audio routing, volume control, multi-output |
| **UI Manager** | ImGui interface, panels, inspector |

### 1.3 Threading Model

```
┌─────────────────────────────────────────────────────────────┐
│                        Main Thread                           │
│  - UI rendering (ImGui)                                      │
│  - Input handling                                            │
│  - Scene management                                          │
│  - OpenGL rendering (all GL calls)                          │
└─────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│ Video Decoder │    │ Video Decoder │    │ Audio Mixer   │
│   Thread 1    │    │   Thread N    │    │    Thread     │
│ (per video)   │    │ (per video)   │    │ (single)      │
└───────────────┘    └───────────────┘    └───────────────┘
```

**Thread Safety Rules:**
- All OpenGL calls must occur on main thread
- Video decoder threads produce frames to ring buffers
- Main thread consumes frames and uploads to GPU textures
- Audio mixer thread has dedicated output buffer

---

## 2. Core Data Models

### 2.1 Project Structure

```cpp
struct Project {
    std::string name;
    std::string version;
    DateTime created;
    DateTime modified;
    ProjectSettings settings;
    MediaLibrary media;
    std::vector<Scene> scenes;
    std::vector<OutputWindow> outputs;
    AudioConfiguration audioConfig;
};
```

### 2.2 Scene Structure

```cpp
struct Scene {
    UUID id;
    std::string name;
    bool isActive;
    std::vector<QuadGroup> groups;
    std::vector<Quad> quads;
    std::vector<OutputAssignment> outputAssignments;
};
```

### 2.3 Quad Structure

```cpp
struct Quad {
    UUID id;
    std::string name;
    std::optional<UUID> parentGroup;
    int zOrder;
    bool visible;
    bool locked;
    float opacity;  // 0.0 - 1.0

    QuadGeometry geometry;
    TextureBinding texture;
    std::vector<UUID> outputAssignments;
};

struct QuadGeometry {
    glm::vec2 corners[4];      // TL, TR, BR, BL in normalized [0,1]
    int meshResolution;         // 2-64 (default 16)
    std::vector<glm::vec2> meshVertices;  // (res+1) x (res+1)
    MeshMode meshMode;          // CORNERS_ONLY | MESH_EDIT
};

struct TextureBinding {
    UUID mediaId;
    glm::vec2 uvOffset;         // Pan (0,0 = top-left)
    glm::vec2 uvScale;          // Zoom (1,1 = full texture)
    std::optional<Rectangle> cropRegion;
    bool flipH;
    bool flipV;
};
```

### 2.4 Media Item Structure

```cpp
struct MediaItem {
    UUID id;
    std::string name;
    std::filesystem::path path;
    MediaType type;             // IMAGE | VIDEO | AUDIO
    GLuint thumbnailTexture;

    std::optional<ImageData> imageData;
    std::optional<VideoData> videoData;
    std::optional<AudioData> audioData;
};

struct VideoData {
    int width;
    int height;
    double duration;            // Seconds
    double frameRate;
    std::string codec;
    bool hasAudio;
};
```

### 2.5 Video Playback State

```cpp
struct VideoPlayback {
    UUID mediaId;
    PlaybackState state;        // STOPPED | PLAYING | PAUSED
    double position;            // Current time in seconds
    float speed;                // 0.1 - 4.0
    LoopMode loopMode;          // ONCE | LOOP | PING_PONG
    float volume;               // 0.0 - 1.0
    bool muted;
    bool audioEnabled;
    bool timelineLocked;
};
```

### 2.6 Output Window Structure

```cpp
struct OutputWindow {
    UUID id;
    std::string name;
    int monitor;                // Display index (-1 = floating)
    glm::ivec2 position;
    glm::ivec2 resolution;
    Rotation rotation;          // R0 | R90 | R180 | R270
    bool isFullscreen;
    std::vector<UUID> sceneAssignments;
    glm::vec4 backgroundColor;

    // Runtime state
    GLFWwindow* window;
    GLuint framebuffer;
};
```

---

## 3. Rendering Pipeline

### 3.1 Per-Frame Rendering

```
┌──────────────────────────────────────────────────────────┐
│                  Per-Frame Rendering                      │
├──────────────────────────────────────────────────────────┤
│                                                          │
│  1. Update video textures from decoder ring buffers      │
│                                                          │
│  2. For each enabled output window:                      │
│     a. Bind output framebuffer                           │
│     b. Clear with background color                       │
│     c. For each visible quad (sorted by z-order):        │
│        i.   Get current texture (video frame or image)   │
│        ii.  Apply UV offset/scale/crop                   │
│        iii. Generate/update mesh vertices                │
│        iv.  Render textured mesh with blending           │
│     d. Present to window                                 │
│                                                          │
│  3. Render UI to main window                             │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### 3.2 Coordinate System

```
Normalized Output Coordinates [0,1]:

(0,0) ────────────────── (1,0)
  │                        │
  │      Output Space      │
  │                        │
(0,1) ────────────────── (1,1)

- All quad corners use normalized [0,1] coordinates
- (0,0) = top-left of output window
- (1,1) = bottom-right of output window
- Coordinates can exceed [0,1] for off-screen quads
```

### 3.3 Mesh Subdivision Algorithm

```cpp
void generateMesh(const QuadGeometry& geometry, std::vector<Vertex>& vertices) {
    int res = geometry.meshResolution;

    for (int row = 0; row <= res; row++) {
        for (int col = 0; col <= res; col++) {
            float u = col / static_cast<float>(res);
            float v = row / static_cast<float>(res);

            // Bilinear interpolation from corners
            glm::vec2 top = glm::mix(
                geometry.corners[0],  // TL
                geometry.corners[1],  // TR
                u
            );
            glm::vec2 bottom = glm::mix(
                geometry.corners[3],  // BL
                geometry.corners[2],  // BR
                u
            );
            glm::vec2 position = glm::mix(top, bottom, v);

            // Apply mesh vertex offset if in edit mode
            if (geometry.meshMode == MeshMode::MESH_EDIT) {
                position += geometry.meshVertices[row * (res + 1) + col];
            }

            vertices.push_back({position, {u, v}});
        }
    }
}
```

### 3.4 Shader Design

**Vertex Shader (quad.vert):**
```glsl
#version 330 core

layout(location = 0) in vec2 a_Position;
layout(location = 1) in vec2 a_TexCoord;

out vec2 v_TexCoord;

void main() {
    // Convert [0,1] to NDC [-1,1]
    vec2 ndc = a_Position * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y for top-down coords

    gl_Position = vec4(ndc, 0.0, 1.0);
    v_TexCoord = a_TexCoord;
}
```

**Fragment Shader (quad.frag):**
```glsl
#version 330 core

in vec2 v_TexCoord;
out vec4 FragColor;

uniform sampler2D u_Texture;
uniform float u_Opacity;
uniform vec2 u_UVOffset;
uniform vec2 u_UVScale;

void main() {
    vec2 uv = v_TexCoord * u_UVScale + u_UVOffset;
    vec4 color = texture(u_Texture, uv);
    color.a *= u_Opacity;
    FragColor = color;
}
```

---

## 4. Video Playback Engine

### 4.1 Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                     Video Playback System                     │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌───────────┐                    ┌─────────────────────┐   │
│  │  Video 1  │──decode thread───▶│   Ring Buffer       │   │
│  │  File     │                    │  [Frame N-2]        │   │
│  └───────────┘                    │  [Frame N-1]        │   │
│                                   │  [Frame N  ] ◀──────┼───┤
│  ┌───────────┐                    └─────────────────────┘   │
│  │  Video 2  │──decode thread───▶ [Ring Buffer]             │
│  │  File     │                                              │
│  └───────────┘                                              │
│                                                              │
│                         Main Thread                          │
│                              │                               │
│                              ▼                               │
│                    ┌──────────────────┐                     │
│                    │  GPU Texture     │                     │
│                    │  Upload          │                     │
│                    └──────────────────┘                     │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 Playback States

```cpp
enum class PlaybackState {
    STOPPED,    // At beginning, decoder idle
    PLAYING,    // Active decoding and playback
    PAUSED,     // Frozen at current frame, decoder paused
    FINISHED    // Reached end (non-loop mode)
};
```

### 4.3 Timeline Lock System

```cpp
class TimelineSyncManager {
    std::vector<VideoPlayback*> lockedVideos;

    void onVideoFinished(VideoPlayback* video) {
        if (!video->isTimelineLocked()) return;

        video->freezeOnLastFrame();

        bool allFinished = std::all_of(
            lockedVideos.begin(),
            lockedVideos.end(),
            [](auto* v) { return v->hasReachedEnd(); }
        );

        if (allFinished) {
            for (auto* v : lockedVideos) {
                v->seekToStart();
                v->play();
            }
        }
    }
};
```

---

## 5. Audio System

### 5.1 Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                      Audio System                             │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────┐   │
│  │  Video 1  │ │  Video 2  │ │ Audio File│ │  Video N  │   │
│  │   Audio   │ │   Audio   │ │           │ │   Audio   │   │
│  └─────┬─────┘ └─────┬─────┘ └─────┬─────┘ └─────┬─────┘   │
│        │ vol: 80%    │ vol: 100%   │ vol: 60%    │ muted    │
│        ▼             ▼             ▼             ▼          │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                    AUDIO MIXER                       │    │
│  │         Sum all sources → Apply master volume        │    │
│  └─────────────────────────────────────────────────────┘    │
│                          │                                   │
│                          ▼                                   │
│                  ┌─────────────┐                            │
│                  │  miniaudio  │                            │
│                  │   Output    │                            │
│                  └─────────────┘                            │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 5.2 Audio Source Configuration

```cpp
struct AudioSource {
    UUID sourceId;           // MediaItem ID
    float volume;            // 0.0 - 1.0
    bool muted;
    bool enabled;            // Contribute to mix
    std::vector<int> outputChannels;  // Future: per-output routing
};
```

---

## 6. File Format

### 6.1 Project File (.qmap)

**Format:** JSON
**Schema Version:** 1.0.0

```json
{
  "$schema": "quadmapper/project/1.0",
  "version": "1.0.0",
  "name": "My Project",
  "created": "2025-12-19T10:30:00Z",
  "modified": "2025-12-19T14:45:00Z",

  "settings": {
    "defaultResolution": [1920, 1080],
    "gridSize": 32,
    "snapToGrid": true
  },

  "media": [
    {
      "id": "uuid-1",
      "name": "background.mp4",
      "path": "media/background.mp4",
      "type": "VIDEO",
      "width": 1920,
      "height": 1080,
      "duration": 120.5,
      "frameRate": 29.97
    }
  ],

  "scenes": [
    {
      "id": "scene-uuid-1",
      "name": "Scene 1",
      "quads": [...]
    }
  ],

  "outputs": [...],
  "audioConfig": {...},
  "playbackStates": [...]
}
```

### 6.2 Media Path Resolution

1. **Relative paths (preferred):** Stored relative to project file
2. **Absolute paths (fallback):** Used if relative path not found
3. **Missing media:** Prompt user to relocate

---

## 7. Performance Requirements

### 7.1 Frame Rate Targets

| Scenario | Target FPS | Minimum FPS |
|----------|------------|-------------|
| Single 1080p output | 60 | 30 |
| Single 4K output | 60 | 30 |
| Dual 1080p outputs | 60 | 30 |
| Quad 1080p outputs | 60 | 24 |

### 7.2 Resource Limits

| Resource | Limit | Notes |
|----------|-------|-------|
| Simultaneous videos | 16 | Per GPU |
| Max texture size | 8192x8192 | GPU dependent |
| Max quads per scene | 256 | Soft limit |
| Max mesh resolution | 64x64 | Per quad |
| Max scenes | 99 | UI limit |
| Max outputs | 16 | Per GPU |

### 7.3 Memory Management

- **Texture caching:** LRU cache with configurable size
- **Video buffers:** 8-frame ring buffer per video
- **Thumbnail cache:** 256 MB RAM limit

---

## 8. API Contracts

### 8.1 Renderer Interface

```cpp
class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;

    virtual void setOutputTarget(OutputWindow* output) = 0;
    virtual void clearOutput(const glm::vec4& color) = 0;

    virtual void renderQuad(const Quad& quad, GLuint texture) = 0;
    virtual void renderMesh(const Mesh& mesh, GLuint texture,
                           const glm::mat4& transform) = 0;

    virtual GLuint createTexture(int width, int height,
                                 const void* data) = 0;
    virtual void updateTexture(GLuint texture, int width, int height,
                              const void* data) = 0;
    virtual void deleteTexture(GLuint texture) = 0;
};
```

### 8.2 Video Decoder Interface

```cpp
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual bool open(const std::filesystem::path& path) = 0;
    virtual void close() = 0;

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(double time) = 0;

    virtual bool hasNewFrame() const = 0;
    virtual VideoFrame getFrame() = 0;

    virtual double getDuration() const = 0;
    virtual double getPosition() const = 0;
    virtual glm::ivec2 getResolution() const = 0;
};
```

### 8.3 Scene Manager Interface

```cpp
class ISceneManager {
public:
    virtual ~ISceneManager() = default;

    virtual Scene* createScene(const std::string& name) = 0;
    virtual void deleteScene(UUID id) = 0;
    virtual Scene* getScene(UUID id) = 0;
    virtual Scene* getActiveScene() = 0;
    virtual void setActiveScene(UUID id) = 0;

    virtual Quad* createQuad(Scene* scene) = 0;
    virtual void deleteQuad(UUID id) = 0;
    virtual Quad* getQuad(UUID id) = 0;

    virtual std::vector<Quad*> getQuadsByZOrder(Scene* scene) = 0;
};
```

---

## 9. Implementation Phases

### Phase 1: Core Foundation
- Application window with ImGui
- OpenGL rendering context
- Single quad with 4-corner warping
- Image texture loading (PNG, JPG)
- Basic inspector panel
- Project save/load (minimal)

### Phase 2: Mesh & Multi-Quad
- Mesh subdivision system
- Mesh vertex editing mode
- Multiple quads
- Quad groups
- Z-order management
- UV offset/scale controls

### Phase 3: Video Playback
- FFmpeg video decoder
- Video texture binding
- Playback controls
- Speed control
- Loop modes
- Timeline lock system

### Phase 4: Multi-Output
- Output window creation
- Resolution/rotation controls
- Monitor assignment
- Perform Mode (fullscreen)

### Phase 5: Scene System
- Scene creation/deletion
- Scene switching (keyboard 1-9)
- Scene panel UI
- Per-scene quad instances

### Phase 6: Audio System
- Audio decoding from video
- Audio mixer
- Volume controls
- Multi-output routing

### Phase 7: Polish
- Keyboard shortcut system
- Preferences dialog
- Performance optimization
- Error handling
- Testing and bug fixes

---

## 10. Technology Stack

| Component | Technology | Version |
|-----------|------------|---------|
| Build System | CMake | 3.20+ |
| Compiler | MSVC | 17 (VS 2022) |
| Language | C++ | 17 |
| Window Management | GLFW | 3.x |
| OpenGL Loading | GLAD | 2.x |
| Mathematics | GLM | 1.x |
| User Interface | Dear ImGui | Docking |
| Video Decoding | FFmpeg | 6.x |
| Image Loading | stb_image | Latest |
| JSON | nlohmann/json | Latest |
| Audio | miniaudio | Latest |
| Testing | GoogleTest | Latest |

---

## Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-12-19 | Claude | Initial EDD from specification |
