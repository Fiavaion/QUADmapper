# QuadMapper MVP - Comprehensive Specification

**Document Version**: 1.0
**Date**: 2025-11-23
**Status**: SPECIFICATION COMPLETE
**Target Platform**: Windows 11

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Application Architecture](#2-application-architecture)
3. [Core Data Model](#3-core-data-model)
4. [User Interface Design](#4-user-interface-design)
5. [Quad Warping System](#5-quad-warping-system)
6. [Texture & Media System](#6-texture--media-system)
7. [Video Playback Engine](#7-video-playback-engine)
8. [Audio System](#8-audio-system)
9. [Multi-Output System](#9-multi-output-system)
10. [Scene Management](#10-scene-management)
11. [Keyboard Shortcuts](#11-keyboard-shortcuts)
12. [Project File Format](#12-project-file-format)
13. [Performance Targets](#13-performance-targets)
14. [MVP Feature Matrix](#14-mvp-feature-matrix)
15. [Implementation Phases](#15-implementation-phases)
16. [Technical Stack](#16-technical-stack)

---

## 1. Executive Summary

### 1.1 Product Vision

**QuadMapper** is a professional quad-based projection mapping application for Windows 11. It enables users to warp video and image content onto physical surfaces using perspective-corrected quads with mesh subdivision, manage multiple output windows for multi-projector setups, and organize content into switchable scenes.

### 1.2 Target Users

- Live event technicians
- Installation artists
- VJs and visual performers
- Architectural projection designers
- Theater and stage designers

### 1.3 Core Value Proposition

| Feature | Benefit |
|---------|---------|
| Multi-quad warping | Map content to complex surfaces |
| Mesh subdivision | Smooth curved surface mapping |
| Multiple outputs | Drive unlimited projectors |
| Scene system | Instant content switching |
| Independent video timelines | Flexible playback synchronization |
| Perform Mode | Professional fullscreen output |

### 1.4 MVP Scope

The MVP delivers a complete, usable projection mapping tool with:
- Quad creation, warping, and mesh editing
- Image and video texture support
- Multiple scenes with keyboard switching
- Multiple output windows with resolution control
- Video playback with timeline sync options
- Audio routing with multi-output support

---

## 2. Application Architecture

### 2.1 High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      QuadMapper Application                      │
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
│  ├─────────┬─────────┬─────────┬─────────────────┤              │
│  │Inspector│Media Bin│ Preview │  Scene Panel    │              │
│  └─────────┴─────────┴─────────┴─────────────────┘              │
└─────────────────────────────────────────────────────────────────┘
         │                    │                    │
         ▼                    ▼                    ▼
┌─────────────┐      ┌─────────────┐      ┌─────────────┐
│  Output 1   │      │  Output 2   │      │  Output N   │
│  (Window)   │      │  (Window)   │      │  (Window)   │
└─────────────┘      └─────────────┘      └─────────────┘
```

### 2.2 Component Overview

| Component | Responsibility |
|-----------|----------------|
| **Project Manager** | Load/save projects, manage project state |
| **Scene Manager** | Create/switch scenes, manage quad hierarchy |
| **Output Manager** | Create/configure output windows, handle Perform Mode |
| **Renderer** | OpenGL rendering, quad warping, mesh subdivision |
| **Media Decoder** | FFmpeg video decoding, image loading |
| **Audio Mixer** | Audio routing, volume control, multi-output |
| **UI Manager** | ImGui interface, panels, inspector |

### 2.3 Threading Model

```
┌─────────────────────────────────────────────────────────────┐
│                        Main Thread                           │
│  - UI rendering (ImGui)                                      │
│  - Input handling                                            │
│  - Scene management                                          │
│  - OpenGL rendering                                          │
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

---

## 3. Core Data Model

### 3.1 Project Structure

```
Project
├── name: string
├── version: string
├── created: datetime
├── modified: datetime
├── settings: ProjectSettings
├── media: MediaLibrary
├── scenes: Scene[]
├── outputs: OutputWindow[]
└── audioConfig: AudioConfiguration
```

### 3.2 Scene Structure

```
Scene
├── id: uuid
├── name: string
├── isActive: boolean
├── groups: QuadGroup[]
├── quads: Quad[]
└── outputAssignments: OutputAssignment[]
```

### 3.3 Quad Structure

```
Quad
├── id: uuid
├── name: string
├── parentGroup: uuid | null
├── zOrder: integer
├── visible: boolean
├── locked: boolean
├── opacity: float [0.0-1.0]
│
├── geometry: QuadGeometry
│   ├── corners: Vector2[4]           // TL, TR, BR, BL in normalized [0,1]
│   ├── meshResolution: int           // 2-32 (default 16)
│   ├── meshVertices: Vector2[][]     // 2D array of mesh points
│   └── meshMode: enum                // CORNERS_ONLY | MESH_EDIT
│
├── texture: TextureBinding
│   ├── mediaId: uuid
│   ├── uvOffset: Vector2             // Pan (0,0 = top-left)
│   ├── uvScale: Vector2              // Zoom (1,1 = full texture)
│   ├── cropRegion: Rectangle | null  // Pixel-based crop (optional)
│   └── flipH: boolean
│   └── flipV: boolean
│
└── outputAssignments: uuid[]         // Which outputs show this quad
```

### 3.4 Quad Group Structure

```
QuadGroup
├── id: uuid
├── name: string
├── parentGroup: uuid | null
├── children: uuid[]                  // Quad or QuadGroup IDs
├── collapsed: boolean                // UI state
└── visible: boolean                  // Affects all children
```

### 3.5 Output Window Structure

```
OutputWindow
├── id: uuid
├── name: string
├── monitor: integer                  // Display index (-1 = floating)
├── position: Vector2                 // Window position (windowed mode)
├── resolution: Vector2               // Output resolution
├── rotation: enum                    // 0 | 90 | 180 | 270
├── isFullscreen: boolean             // Perform Mode state
├── sceneAssignments: uuid[]          // Which scenes render here
└── backgroundColor: Color
```

### 3.6 Media Item Structure

```
MediaItem
├── id: uuid
├── name: string
├── path: string                      // File path
├── type: enum                        // IMAGE | VIDEO | AUDIO
├── thumbnailId: integer              // Cached thumbnail texture
│
├── imageData: ImageData | null
│   ├── width: integer
│   ├── height: integer
│   └── format: enum                  // PNG | JPG | TIF | BMP
│
├── videoData: VideoData | null
│   ├── width: integer
│   ├── height: integer
│   ├── duration: float               // Seconds
│   ├── frameRate: float
│   ├── codec: string
│   └── hasAudio: boolean
│
└── audioData: AudioData | null
    ├── duration: float
    ├── sampleRate: integer
    └── channels: integer
```

### 3.7 Video Playback State

```
VideoPlayback
├── mediaId: uuid
├── state: enum                       // STOPPED | PLAYING | PAUSED
├── position: float                   // Current time in seconds
├── speed: float                      // Playback rate (0.1-4.0)
├── loopMode: enum                    // ONCE | LOOP | PING_PONG
├── volume: float [0.0-1.0]
├── muted: boolean
├── audioEnabled: boolean             // Contribute to audio mix
└── timelineLocked: boolean           // Sync with other locked videos
```

---

## 4. User Interface Design

### 4.1 Main Window Layout

```
┌────────────────────────────────────────────────────────────────────┐
│ File  Edit  View  Scene  Output  Window  Help                      │
├────────────────────────────────────────────────────────────────────┤
│ ┌──────────────┐ ┌────────────────────────────┐ ┌────────────────┐ │
│ │              │ │                            │ │                │ │
│ │   INSPECTOR  │ │                            │ │    SCENES      │ │
│ │              │ │                            │ │                │ │
│ │  [Quad]      │ │         PREVIEW            │ │  ► Scene 1     │ │
│ │  Name: ___   │ │                            │ │    Scene 2     │ │
│ │              │ │    ┌──────────────┐        │ │    Scene 3     │ │
│ │  Position    │ │    │              │        │ │                │ │
│ │  TL: x,y     │ │    │    QUAD      │        │ ├────────────────┤ │
│ │  TR: x,y     │ │    │              │        │ │   OUTPUTS      │ │
│ │  BR: x,y     │ │    └──────────────┘        │ │                │ │
│ │  BL: x,y     │ │                            │ │  ■ Output 1    │ │
│ │              │ │                            │ │  ■ Output 2    │ │
│ │  Mesh: 16x16 │ │                            │ │                │ │
│ │              │ │                            │ │  [+ New Output]│ │
│ │  Texture     │ │                            │ │                │ │
│ │  [image.png] │ │                            │ │                │ │
│ │  UV: 0,0     │ │                            │ │                │ │
│ │  Scale: 1,1  │ │                            │ │                │ │
│ │              │ └────────────────────────────┘ └────────────────┘ │
│ │              │ ┌────────────────────────────────────────────────┐ │
│ │  Video       │ │                                                │ │
│ │  ▶ ❚❚ ■     │ │                  MEDIA BIN                     │ │
│ │  ─●──────    │ │                                                │ │
│ │  Speed: 1.0x │ │  [img1] [img2] [vid1] [vid2] [vid3] [aud1]    │ │
│ │  Loop: ✓     │ │                                                │ │
│ │  🔒 Timeline │ │  [+ Import Media]                              │ │
│ └──────────────┘ └────────────────────────────────────────────────┘ │
├────────────────────────────────────────────────────────────────────┤
│ Ready | Scene: Scene 1 | Quads: 4 | FPS: 60 | Output: 1920x1080   │
└────────────────────────────────────────────────────────────────────┘
```

### 4.2 Panel Descriptions

#### 4.2.1 Inspector Panel (Left)

**Quad Properties Section:**
```
┌─────────────────────────────┐
│ ▼ Quad Properties           │
├─────────────────────────────┤
│ Name: [Quad 1          ]    │
│ Visible: [✓]  Locked: [ ]   │
│ Opacity: [────●────] 100%   │
│ Z-Order: [▲] 1 [▼]          │
├─────────────────────────────┤
│ ▼ Corners                   │
├─────────────────────────────┤
│ TL: [0.10] [0.10]           │
│ TR: [0.90] [0.10]           │
│ BR: [0.90] [0.90]           │
│ BL: [0.10] [0.90]           │
│ [Reset Corners]             │
├─────────────────────────────┤
│ ▼ Mesh                      │
├─────────────────────────────┤
│ Resolution: [──●──] 16      │
│ Mode: (●) Corners ( ) Mesh  │
│ [Reset Mesh]                │
├─────────────────────────────┤
│ ▼ Texture                   │
├─────────────────────────────┤
│ Source: [video.mp4    ] [×] │
│ UV Offset: [0.00] [0.00]    │
│ UV Scale:  [1.00] [1.00]    │
│ Flip: [H] [V]               │
│ ▼ Crop Region               │
│   X: [0] Y: [0]             │
│   W: [1920] H: [1080]       │
│   [Use Full Texture]        │
├─────────────────────────────┤
│ ▼ Video Playback            │
├─────────────────────────────┤
│ [▶] [❚❚] [■] [↻]           │
│ [═══════●═══════] 00:30     │
│ Speed: [──●──] 1.0x         │
│ Volume: [────●────] 80%     │
│ [🔒 Lock Timeline]          │
│ [🔊 Audio to Mix]           │
└─────────────────────────────┘
```

#### 4.2.2 Preview Panel (Center)

**Features:**
- Real-time preview of selected scene
- Quad handles for corner dragging
- Mesh vertex editing mode
- Grid overlay toggle
- Zoom/pan controls
- Output window preview toggle

**Interaction Modes:**
1. **Select Mode (V)**: Click to select quads
2. **Corner Mode (C)**: Drag corner handles
3. **Mesh Mode (M)**: Drag individual mesh vertices
4. **Pan Mode (H)**: Pan the preview canvas

#### 4.2.3 Scene Panel (Right-Top)

```
┌─────────────────────────────┐
│ SCENES                [+]   │
├─────────────────────────────┤
│ ► Scene 1              [1]  │ ← Active (highlighted)
│   Scene 2              [2]  │
│   Scene 3              [3]  │
│   Scene 4              [4]  │
├─────────────────────────────┤
│ [Rename] [Duplicate] [Delete]│
└─────────────────────────────┘
```

**Behavior:**
- Click to switch scenes (instant)
- Number keys 1-9 for quick switching
- Right-click for context menu
- Drag to reorder

#### 4.2.4 Output Panel (Right-Bottom)

```
┌─────────────────────────────┐
│ OUTPUTS               [+]   │
├─────────────────────────────┤
│ ■ Output 1 - 1920x1080 [⛶] │
│ ■ Output 2 - 3840x2160 [⛶] │
│ □ Output 3 - 1280x720  [⛶] │ ← Disabled
├─────────────────────────────┤
│ [🎭 PERFORM MODE]           │
├─────────────────────────────┤
│ Selected: Output 1          │
│ Resolution: [1920] x [1080] │
│ Rotation: [0°  ▼]           │
│ Monitor: [Display 2 ▼]      │
│ [Configure...]              │
└─────────────────────────────┘
```

#### 4.2.5 Media Bin Panel (Bottom)

```
┌────────────────────────────────────────────────────────────────┐
│ MEDIA BIN                                      [+ Import] [🔍] │
├────────────────────────────────────────────────────────────────┤
│ ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐  │
│ │ ▶     │ │       │ │ ▶     │ │ ▶     │ │       │ │ ♪     │  │
│ │[thumb]│ │[thumb]│ │[thumb]│ │[thumb]│ │[thumb]│ │[audio]│  │
│ │       │ │       │ │       │ │       │ │       │ │       │  │
│ ├───────┤ ├───────┤ ├───────┤ ├───────┤ ├───────┤ ├───────┤  │
│ │vid1.mp│ │img1.pn│ │vid2.mo│ │vid3.mp│ │img2.ti│ │music.w│  │
│ └───────┘ └───────┘ └───────┘ └───────┘ └───────┘ └───────┘  │
│                                                                │
│ Drag media to quad or preview to assign                        │
└────────────────────────────────────────────────────────────────┘
```

**Features:**
- Thumbnail preview for all media types
- Video indicator (▶) for video files
- Audio indicator (♪) for audio files
- Drag-and-drop onto quads
- Right-click for context menu
- Search/filter bar

### 4.3 Output Window Design

**Windowed Mode:**
```
┌─────────────────────────────────────┐
│ Output 1 - Scene 1            _ □ × │
├─────────────────────────────────────┤
│                                     │
│     ┌───────────────────────┐       │
│     │                       │       │
│     │       QUAD 1          │       │
│     │                       │       │
│     └───────────────────────┘       │
│                                     │
│            ┌─────────────┐          │
│            │   QUAD 2    │          │
│            └─────────────┘          │
│                                     │
└─────────────────────────────────────┘
```

**Perform Mode (Fullscreen):**
- Borderless fullscreen on selected monitor
- No title bar, no borders
- Black letterboxing if aspect ratio differs
- Alt+Tab brings control window to front

---

## 5. Quad Warping System

### 5.1 Coordinate System

**Normalized Coordinates [0,1]:**
```
(0,0) ────────────────── (1,0)
  │                        │
  │      Quad Space        │
  │                        │
(0,1) ────────────────── (1,1)
```

- All quad corners use normalized [0,1] coordinates
- (0,0) = top-left of output window
- (1,1) = bottom-right of output window
- Coordinates can exceed [0,1] for off-screen quads

### 5.2 Corner-Pin Warping

**4-Point Perspective Transform:**

```cpp
// Quad corners define perspective transformation
struct QuadCorners {
    Vector2 topLeft;      // TL
    Vector2 topRight;     // TR
    Vector2 bottomRight;  // BR
    Vector2 bottomLeft;   // BL
};

// Transform UV to screen position via bilinear interpolation
Vector2 TransformUVToScreen(Vector2 uv, QuadCorners corners) {
    Vector2 top = Lerp(corners.topLeft, corners.topRight, uv.x);
    Vector2 bottom = Lerp(corners.bottomLeft, corners.bottomRight, uv.x);
    return Lerp(top, bottom, uv.y);
}
```

### 5.3 Mesh Subdivision

**Mesh Grid Generation:**

```cpp
// Generate NxN mesh grid
void GenerateMesh(int resolution) {
    // resolution = 2 to 32 (default 16)
    for (int row = 0; row <= resolution; row++) {
        for (int col = 0; col <= resolution; col++) {
            float u = col / (float)resolution;
            float v = row / (float)resolution;

            // Initial position via bilinear interpolation
            meshVertices[row][col] = TransformUVToScreen({u, v}, corners);
        }
    }
}
```

**Mesh Editing Mode:**
- Individual vertex dragging
- Vertex snapping to grid (optional)
- Reset to bilinear (recalculate from corners)
- Smooth tool (average with neighbors)

### 5.4 Rendering Pipeline

```
┌──────────────────────────────────────────────────────────┐
│                  Per-Frame Rendering                      │
├──────────────────────────────────────────────────────────┤
│                                                          │
│  1. Clear output framebuffer                             │
│                                                          │
│  2. For each visible quad (sorted by z-order):           │
│     a. Get current texture frame (video/image)           │
│     b. Apply UV offset/scale/crop                        │
│     c. Generate mesh vertices from corners + subdivision │
│     d. Render textured mesh with perspective correction  │
│     e. Apply opacity blending                            │
│                                                          │
│  3. Present to output window                             │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### 5.5 Shader Design

**Vertex Shader:**
```glsl
#version 330 core

layout(location = 0) in vec2 a_Position;  // Mesh vertex position [0,1]
layout(location = 1) in vec2 a_TexCoord;  // UV coordinate [0,1]

out vec2 v_TexCoord;

void main() {
    // Convert [0,1] to NDC [-1,1]
    vec2 ndc = a_Position * 2.0 - 1.0;
    ndc.y = -ndc.y;  // Flip Y for top-down coords

    gl_Position = vec4(ndc, 0.0, 1.0);
    v_TexCoord = a_TexCoord;
}
```

**Fragment Shader:**
```glsl
#version 330 core

in vec2 v_TexCoord;
out vec4 FragColor;

uniform sampler2D u_Texture;
uniform float u_Opacity;
uniform vec2 u_UVOffset;
uniform vec2 u_UVScale;

void main() {
    // Apply UV transformation
    vec2 uv = v_TexCoord * u_UVScale + u_UVOffset;

    // Sample texture
    vec4 color = texture(u_Texture, uv);

    // Apply opacity
    color.a *= u_Opacity;

    FragColor = color;
}
```

---

## 6. Texture & Media System

### 6.1 Supported Formats

**Images:**
| Format | Extension | Notes |
|--------|-----------|-------|
| PNG | .png | With alpha channel support |
| JPEG | .jpg, .jpeg | No alpha |
| TIFF | .tif, .tiff | High bit depth support |
| BMP | .bmp | Legacy support |
| TGA | .tga | With alpha |

**Videos:**
| Format | Extension | Codec Support |
|--------|-----------|---------------|
| MP4 | .mp4 | H.264, H.265 |
| MOV | .mov | ProRes, H.264 |
| AVI | .avi | Various |
| MKV | .mkv | Various |
| WEBM | .webm | VP8, VP9 |

**Audio:**
| Format | Extension | Notes |
|--------|-----------|-------|
| WAV | .wav | Uncompressed |
| MP3 | .mp3 | Compressed |
| AAC | .aac | From video tracks |
| FLAC | .flac | Lossless |

### 6.2 Media Loading Pipeline

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│  File Drop  │────▶│  Importer   │────▶│   Media     │
│  or Dialog  │     │  (FFmpeg)   │     │   Library   │
└─────────────┘     └─────────────┘     └─────────────┘
                           │
                           ▼
                    ┌─────────────┐
                    │  Thumbnail  │
                    │  Generator  │
                    └─────────────┘
```

### 6.3 UV Mapping & Cropping

**UV Offset/Scale (Pan & Zoom):**
```
UV Offset: (0.25, 0.25)  →  Start at 25% from top-left
UV Scale:  (0.5, 0.5)    →  Show 50% of texture (zoom in 2x)

Result: Shows center quarter of texture
```

**Pixel Crop Region:**
```cpp
struct CropRegion {
    int x;      // Left edge (pixels)
    int y;      // Top edge (pixels)
    int width;  // Crop width (pixels)
    int height; // Crop height (pixels)
};

// Convert crop to UV
vec2 uvMin = vec2(crop.x / texWidth, crop.y / texHeight);
vec2 uvMax = vec2((crop.x + crop.width) / texWidth,
                  (crop.y + crop.height) / texHeight);
```

**Texture Splitting Across Quads:**

Example: 1920x1080 video split across 2 quads horizontally
```
Quad 1 (left half):
  - Crop: x=0, y=0, w=960, h=1080
  - OR UV: offset=(0,0), scale=(0.5, 1.0)

Quad 2 (right half):
  - Crop: x=960, y=0, w=960, h=1080
  - OR UV: offset=(0.5, 0), scale=(0.5, 1.0)
```

---

## 7. Video Playback Engine

### 7.1 Playback Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                     Video Playback System                     │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐      │
│  │   Video 1   │    │   Video 2   │    │   Video N   │      │
│  │   Decoder   │    │   Decoder   │    │   Decoder   │      │
│  │   Thread    │    │   Thread    │    │   Thread    │      │
│  └──────┬──────┘    └──────┬──────┘    └──────┬──────┘      │
│         │                  │                  │              │
│         ▼                  ▼                  ▼              │
│  ┌─────────────────────────────────────────────────────┐    │
│  │              Frame Ring Buffer (per video)           │    │
│  │         [Frame N-2] [Frame N-1] [Frame N]           │    │
│  └─────────────────────────────────────────────────────┘    │
│                            │                                 │
│                            ▼                                 │
│  ┌─────────────────────────────────────────────────────┐    │
│  │            GPU Texture Upload (main thread)          │    │
│  └─────────────────────────────────────────────────────┘    │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 7.2 Playback States

```cpp
enum class PlaybackState {
    STOPPED,    // At beginning, not playing
    PLAYING,    // Active playback
    PAUSED,     // Frozen at current frame
    FINISHED    // Reached end (non-loop mode)
};
```

### 7.3 Loop Modes

```cpp
enum class LoopMode {
    ONCE,       // Play once, then stop
    LOOP,       // Restart from beginning
    PING_PONG   // Reverse at end, play backward, repeat
};
```

### 7.4 Timeline Locking System

**Purpose:** Synchronize multiple videos so they restart together.

**Behavior:**
1. When a video with `timelineLocked = true` reaches its end:
   - Freeze on last frame
   - Wait for all other locked videos to finish
2. When ALL locked videos have finished:
   - All restart simultaneously from frame 0
3. Unlocked videos loop independently

**Implementation:**
```cpp
class TimelineSyncManager {
    std::vector<VideoPlayback*> lockedVideos;

    void OnVideoFinished(VideoPlayback* video) {
        if (!video->isTimelineLocked()) return;

        video->freezeOnLastFrame();

        // Check if all locked videos finished
        bool allFinished = std::all_of(
            lockedVideos.begin(),
            lockedVideos.end(),
            [](auto* v) { return v->hasReachedEnd(); }
        );

        if (allFinished) {
            // Restart all locked videos together
            for (auto* v : lockedVideos) {
                v->seekToStart();
                v->play();
            }
        }
    }
};
```

### 7.5 Playback Controls

| Control | Action | Shortcut |
|---------|--------|----------|
| Play | Start/resume playback | Space |
| Pause | Freeze current frame | Space |
| Stop | Return to start | . |
| Speed Up | Increase playback rate | ] |
| Speed Down | Decrease playback rate | [ |
| Jump Forward | Seek +5 seconds | Right |
| Jump Backward | Seek -5 seconds | Left |
| Toggle Loop | Cycle loop mode | L |
| Lock Timeline | Toggle sync lock | Ctrl+L |

### 7.6 Speed Control

**Supported Speeds:**
- 0.1x (10% speed)
- 0.25x
- 0.5x
- 1.0x (normal)
- 1.5x
- 2.0x
- 4.0x

**Frame Rate Handling:**
```cpp
// Target native frame rate when speed = 1.0x
double targetFPS = video.nativeFrameRate * playbackSpeed;

// Calculate frame delta
double frameDelta = 1.0 / targetFPS;

// Seek to frame based on elapsed time
currentTime += deltaTime * playbackSpeed;
currentFrame = (int)(currentTime * video.nativeFrameRate);
```

---

## 8. Audio System

### 8.1 Audio Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                      Audio System                             │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌───────────┐   │
│  │  Video 1  │ │  Video 2  │ │ Audio File│ │  Video N  │   │
│  │   Audio   │ │   Audio   │ │           │ │   Audio   │   │
│  └─────┬─────┘ └─────┬─────┘ └─────┬─────┘ └─────┬─────┘   │
│        │             │             │             │          │
│        │ vol: 80%    │ vol: 100%   │ vol: 60%    │ muted    │
│        │             │             │             │          │
│        ▼             ▼             ▼             ▼          │
│  ┌─────────────────────────────────────────────────────┐    │
│  │                    AUDIO MIXER                       │    │
│  │    ┌─────┐ ┌─────┐ ┌─────┐ ┌─────┐                 │    │
│  │    │ Ch1 │ │ Ch2 │ │ Ch3 │ │ Ch4 │ ... (N inputs) │    │
│  │    └──┬──┘ └──┬──┘ └──┬──┘ └──┬──┘                 │    │
│  │       │       │       │       │                     │    │
│  │       └───────┴───────┴───────┘                     │    │
│  │                   │                                  │    │
│  │                   ▼                                  │    │
│  │             Master Volume                            │    │
│  │              [────●────] 100%                        │    │
│  └─────────────────────────────────────────────────────┘    │
│                          │                                   │
│         ┌────────────────┼────────────────┐                 │
│         ▼                ▼                ▼                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐         │
│  │  Output 1   │  │  Output 2   │  │  Output N   │         │
│  │ (Speakers)  │  │ (HDMI Out)  │  │ (USB Audio) │         │
│  └─────────────┘  └─────────────┘  └─────────────┘         │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 8.2 Audio Source Configuration

```cpp
struct AudioSource {
    uuid sourceId;           // MediaItem ID (video or audio file)
    float volume;            // 0.0 - 1.0
    bool muted;
    bool enabled;            // Contribute to mix

    // Output routing
    std::vector<int> outputChannels;  // Which hardware channels
};
```

### 8.3 Audio Routing Options

**Single Output (Default):**
- All audio sources mix to stereo
- Output to default audio device

**Multi-Output:**
- User selects which hardware outputs to use
- Each audio source can be routed to specific outputs
- Example: Video 1 → HDMI, Video 2 → Speakers

**Channel Mapping:**
```cpp
// Example: 5.1 surround routing
audioSource.outputChannels = {
    0,  // Front Left
    1,  // Front Right
    // Excludes center, LFE, surrounds
};
```

### 8.4 Audio Mixing Rules

1. **Volume Summation:**
   - Each source volume is applied
   - Sources are summed together
   - Master volume applied to final mix
   - Clipping prevention via soft limiting

2. **Sync with Video:**
   - Audio follows video playback position
   - Seek operations sync both streams
   - Speed changes affect audio pitch (optional)

---

## 9. Multi-Output System

### 9.1 Output Window Management

**Creating Output Windows:**
```
Menu: Window → New Output Window

Creates new OutputWindow with defaults:
- Resolution: 1920x1080
- Rotation: 0°
- Position: Cascaded from last window
- Monitor: Primary display
- Scene: Current active scene
```

### 9.2 Output Window Properties

| Property | Type | Range | Default |
|----------|------|-------|---------|
| Name | string | - | "Output N" |
| Resolution | Vector2 | 640x480 - 8192x8192 | 1920x1080 |
| Rotation | enum | 0, 90, 180, 270 | 0 |
| Monitor | int | -1 (floating) to N | -1 |
| Fullscreen | bool | - | false |
| Background | Color | RGBA | Black |
| Enabled | bool | - | true |

### 9.3 Scene-to-Output Assignment

**Multiple Scenes → One Output:**
- Only active scene renders
- Scene switching changes output content

**One Scene → Multiple Outputs:**
- Same scene renders to all assigned outputs
- Each output can have different:
  - Resolution
  - Rotation
  - Cropping (future: output regions)

**Independent Output Content:**
- Different scenes can be active on different outputs
- Achieved via per-output scene assignment

### 9.4 Perform Mode

**Activation:**
- Button: "PERFORM MODE" in Output Panel
- Shortcut: F11 or Ctrl+Shift+F

**Behavior:**
1. All enabled output windows go fullscreen
2. Each window targets its assigned monitor
3. Main control window remains accessible via Alt+Tab
4. Borderless fullscreen (no Windows decorations)

**Exit:**
- Press F11 or Ctrl+Shift+F again
- Press Escape on any output window
- Click button in control window (via Alt+Tab)

### 9.5 Multi-GPU Support

**Detection:**
```cpp
// Enumerate available GPUs
std::vector<GPU> availableGPUs = EnumerateGPUs();

// Each output window can target specific GPU
outputWindow.preferredGPU = gpuIndex;
```

**Considerations:**
- OpenGL context sharing across GPUs
- Texture transfer overhead
- Recommended: One GPU per output group

---

## 10. Scene Management

### 10.1 Scene Structure

```
Scene
├── Unique ID (uuid)
├── Name (user-editable)
├── Quad hierarchy (groups + quads)
├── Output assignments
└── Active state
```

### 10.2 Scene Operations

| Operation | UI | Shortcut |
|-----------|-----|----------|
| New Scene | Scene → New | Ctrl+Shift+N |
| Duplicate Scene | Scene → Duplicate | Ctrl+D |
| Rename Scene | Double-click name | F2 |
| Delete Scene | Scene → Delete | Delete |
| Switch to Scene | Click / Number key | 1-9 |
| Reorder Scene | Drag in list | - |

### 10.3 Scene Switching Logic

**Instant Switching:**
```cpp
void SwitchToScene(Scene* newScene) {
    // 1. Deactivate current scene
    if (activeScene) {
        activeScene->isActive = false;
    }

    // 2. Activate new scene
    newScene->isActive = true;
    activeScene = newScene;

    // 3. Update all outputs to render new scene
    for (auto& output : outputs) {
        if (output.isAssignedToScene(newScene)) {
            output.setActiveScene(newScene);
        }
    }

    // 4. Video playback continues (scenes share media)
    // No interruption to video playback
}
```

### 10.4 Scene Isolation

**What's Per-Scene:**
- Quad instances (position, grouping, z-order)
- Quad visibility states
- Output assignments

**What's Shared Across Scenes:**
- Media library (all scenes access same media)
- Video playback state (continues across scene switches)
- Audio mix

### 10.5 Default Scene Setup

**On New Project:**
1. Create "Scene 1" as default
2. Empty scene (no quads)
3. All outputs assigned to Scene 1

---

## 11. Keyboard Shortcuts

### 11.1 Default Shortcut Scheme (Resolume-Style)

#### 11.1.1 Global Shortcuts

| Action | Shortcut | Category |
|--------|----------|----------|
| New Project | Ctrl+N | File |
| Open Project | Ctrl+O | File |
| Save Project | Ctrl+S | File |
| Save As | Ctrl+Shift+S | File |
| Quit | Alt+F4 | File |
| Undo | Ctrl+Z | Edit |
| Redo | Ctrl+Shift+Z | Edit |
| Preferences | Ctrl+, | Edit |

#### 11.1.2 View/Mode Shortcuts

| Action | Shortcut | Category |
|--------|----------|----------|
| Select Tool | V | Tool |
| Corner Edit | C | Tool |
| Mesh Edit | M | Tool |
| Pan View | H | Tool |
| Toggle Grid | G | View |
| Toggle Guides | ; | View |
| Zoom In | = / Scroll Up | View |
| Zoom Out | - / Scroll Down | View |
| Fit to Window | F | View |
| Actual Size | 1 | View |

#### 11.1.3 Scene Shortcuts

| Action | Shortcut | Category |
|--------|----------|----------|
| Scene 1-9 | 1-9 | Scene |
| Next Scene | Page Down | Scene |
| Previous Scene | Page Up | Scene |
| New Scene | Ctrl+Shift+N | Scene |
| Duplicate Scene | Ctrl+D | Scene |

#### 11.1.4 Quad Shortcuts

| Action | Shortcut | Category |
|--------|----------|----------|
| New Quad | Q | Quad |
| Delete Quad | Delete / Backspace | Quad |
| Duplicate Quad | Ctrl+D | Quad |
| Group Quads | Ctrl+G | Quad |
| Ungroup | Ctrl+Shift+G | Quad |
| Bring Forward | Ctrl+] | Quad |
| Send Backward | Ctrl+[ | Quad |
| Bring to Front | Ctrl+Shift+] | Quad |
| Send to Back | Ctrl+Shift+[ | Quad |
| Select All | Ctrl+A | Quad |
| Deselect | Escape | Quad |

#### 11.1.5 Playback Shortcuts

| Action | Shortcut | Category |
|--------|----------|----------|
| Play/Pause | Space | Playback |
| Stop | . | Playback |
| Speed Up | ] | Playback |
| Speed Down | [ | Playback |
| Seek Forward | Right Arrow | Playback |
| Seek Backward | Left Arrow | Playback |
| Loop Toggle | L | Playback |
| Timeline Lock | Ctrl+L | Playback |

#### 11.1.6 Output Shortcuts

| Action | Shortcut | Category |
|--------|----------|----------|
| Perform Mode | F11 | Output |
| New Output | Ctrl+Shift+O | Output |
| Toggle Output 1-4 | Ctrl+1-4 | Output |

### 11.2 Shortcut Customization

**Preferences → Keyboard Shortcuts:**
```
┌────────────────────────────────────────────────────────────┐
│ Keyboard Shortcuts                                          │
├────────────────────────────────────────────────────────────┤
│ Search: [_______________]                                   │
│                                                            │
│ Category: [All ▼]                                          │
│                                                            │
│ ┌──────────────────────────────────────────────────────┐  │
│ │ Action              │ Shortcut    │ Default          │  │
│ ├──────────────────────────────────────────────────────┤  │
│ │ New Project         │ Ctrl+N      │ Ctrl+N           │  │
│ │ Open Project        │ Ctrl+O      │ Ctrl+O           │  │
│ │ Select Tool         │ V           │ V                │  │
│ │ Play/Pause          │ Space       │ Space            │  │
│ │ Scene 1             │ 1           │ 1                │  │
│ │ ...                 │ ...         │ ...              │  │
│ └──────────────────────────────────────────────────────┘  │
│                                                            │
│ [Reset All to Default]    [Import...] [Export...]         │
└────────────────────────────────────────────────────────────┘
```

---

## 12. Project File Format

### 12.1 File Structure

**Extension:** `.qmap`
**Format:** JSON (human-readable, version-controllable)

### 12.2 File Schema

```json
{
  "$schema": "quadmapper/project/1.0",
  "version": "1.0.0",
  "name": "My Project",
  "created": "2025-11-23T10:30:00Z",
  "modified": "2025-11-23T14:45:00Z",

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
    },
    {
      "id": "uuid-2",
      "name": "logo.png",
      "path": "media/logo.png",
      "type": "IMAGE",
      "width": 512,
      "height": 512
    }
  ],

  "scenes": [
    {
      "id": "scene-uuid-1",
      "name": "Scene 1",
      "groups": [...],
      "quads": [
        {
          "id": "quad-uuid-1",
          "name": "Background",
          "zOrder": 0,
          "visible": true,
          "locked": false,
          "opacity": 1.0,
          "geometry": {
            "corners": [
              [0.0, 0.0],
              [1.0, 0.0],
              [1.0, 1.0],
              [0.0, 1.0]
            ],
            "meshResolution": 16,
            "meshMode": "CORNERS_ONLY"
          },
          "texture": {
            "mediaId": "uuid-1",
            "uvOffset": [0.0, 0.0],
            "uvScale": [1.0, 1.0],
            "flipH": false,
            "flipV": false
          }
        }
      ]
    }
  ],

  "outputs": [
    {
      "id": "output-uuid-1",
      "name": "Output 1",
      "resolution": [1920, 1080],
      "rotation": 0,
      "monitor": 1,
      "sceneAssignments": ["scene-uuid-1"]
    }
  ],

  "audioConfig": {
    "masterVolume": 1.0,
    "outputs": [
      {
        "deviceId": "default",
        "channels": [0, 1]
      }
    ],
    "sources": [
      {
        "mediaId": "uuid-1",
        "volume": 0.8,
        "muted": false,
        "outputRouting": [0]
      }
    ]
  },

  "playbackStates": [
    {
      "mediaId": "uuid-1",
      "state": "PLAYING",
      "position": 45.2,
      "speed": 1.0,
      "loopMode": "LOOP",
      "timelineLocked": false
    }
  ]
}
```

### 12.3 Media Path Resolution

**Relative Paths (Preferred):**
```
Project: C:/Projects/MyShow/show.qmap
Media:   C:/Projects/MyShow/media/video.mp4
Stored:  "media/video.mp4" (relative)
```

**Absolute Paths (Fallback):**
- If file not found at relative path
- Store original absolute path as backup
- Prompt user to relocate missing media

---

## 13. Performance Targets

### 13.1 Frame Rate Targets

| Scenario | Target FPS | Minimum FPS |
|----------|------------|-------------|
| Single 1080p output | 60 | 30 |
| Single 4K output | 60 | 30 |
| Dual 1080p outputs | 60 | 30 |
| Quad 1080p outputs | 60 | 24 |
| 8+ outputs | 30+ | 24 |

### 13.2 Latency Targets

| Metric | Target | Maximum |
|--------|--------|---------|
| Input → Display | < 50ms | 100ms |
| Scene switch | < 16ms | 33ms |
| Video decode | < 16ms | 33ms |

### 13.3 Resource Limits

| Resource | Limit | Notes |
|----------|-------|-------|
| Simultaneous videos | 16 | Per GPU |
| Max texture size | 8192x8192 | GPU dependent |
| Max quads per scene | 256 | Soft limit |
| Max mesh resolution | 64x64 | Per quad |
| Max scenes | 99 | UI limit |
| Max outputs | 16 | Per GPU |

### 13.4 Memory Management

**Texture Memory:**
- Unload unused textures after 30 seconds
- Keep recently used in VRAM
- Thumbnail cache in RAM (limited to 256 MB)

**Video Buffer:**
- Ring buffer per video (8 frames ahead)
- Release buffers when video stops
- GPU texture upload on main thread

---

## 14. MVP Feature Matrix

### 14.1 MVP Features (Must Have)

| Category | Feature | Priority |
|----------|---------|----------|
| **Rendering** | 4-corner quad warping | P0 |
| **Rendering** | Mesh subdivision (16x16) | P0 |
| **Rendering** | Individual mesh vertex editing | P0 |
| **Rendering** | Z-order rendering | P0 |
| **Texture** | Image textures (PNG, JPG, TIF) | P0 |
| **Texture** | Video textures (MP4, MOV) | P0 |
| **Texture** | UV offset/scale | P0 |
| **Texture** | Pixel crop region | P0 |
| **Video** | Play/Pause/Stop | P0 |
| **Video** | Speed control | P0 |
| **Video** | Loop modes | P0 |
| **Video** | Timeline lock/sync | P0 |
| **Audio** | Video audio playback | P0 |
| **Audio** | Volume per source | P0 |
| **Audio** | Global audio mix | P0 |
| **Scene** | Multiple scenes | P0 |
| **Scene** | Scene switching (1-9 keys) | P0 |
| **Scene** | Scene rename/reorder | P0 |
| **Output** | Multiple output windows | P0 |
| **Output** | Resolution control | P0 |
| **Output** | Rotation (0/90/180/270) | P0 |
| **Output** | Perform Mode (fullscreen) | P0 |
| **Project** | Save/Load projects (.qmap) | P0 |
| **UI** | Inspector panel | P0 |
| **UI** | Media bin | P0 |
| **UI** | Preview panel | P0 |
| **UI** | Drag-drop media assignment | P0 |

### 14.2 Post-MVP Features (Future)

| Category | Feature | Phase |
|----------|---------|-------|
| **Rendering** | Soft-edge blending | v1.1 |
| **Rendering** | Bezier mesh warping | v1.2 |
| **Control** | MIDI input | v1.1 |
| **Control** | OSC input/output | v1.1 |
| **Input** | NDI receiver | v1.2 |
| **Input** | Spout receiver | v1.2 |
| **Input** | Webcam capture | v1.2 |
| **Effects** | Color correction per quad | v1.1 |
| **Effects** | Brightness/contrast | v1.1 |
| **Transitions** | Scene crossfade | v1.2 |
| **Advanced** | 3D model import | v2.0 |
| **Advanced** | Multi-GPU optimization | v1.2 |

---

## 15. Implementation Phases

### Phase 1: Core Foundation (2-3 weeks)

**Deliverables:**
- [ ] Application window with ImGui
- [ ] OpenGL rendering context
- [ ] Single quad with 4-corner warping
- [ ] Image texture loading (PNG, JPG)
- [ ] Basic inspector panel
- [ ] Project save/load (minimal)

**Validation:**
- Load image, create quad, warp corners, save project

---

### Phase 2: Mesh & Multi-Quad (2 weeks)

**Deliverables:**
- [ ] Mesh subdivision system
- [ ] Mesh vertex editing mode
- [ ] Multiple quads
- [ ] Quad groups
- [ ] Z-order management
- [ ] UV offset/scale controls

**Validation:**
- Create 4 quads, group 2, edit mesh vertices, adjust UV

---

### Phase 3: Video Playback (2 weeks)

**Deliverables:**
- [ ] FFmpeg video decoder
- [ ] Video texture binding
- [ ] Playback controls (play/pause/stop)
- [ ] Speed control
- [ ] Loop modes
- [ ] Timeline lock system

**Validation:**
- Load 2 videos, play simultaneously, lock timelines, verify sync

---

### Phase 4: Multi-Output (1-2 weeks)

**Deliverables:**
- [ ] Output window creation
- [ ] Resolution/rotation controls
- [ ] Monitor assignment
- [ ] Perform Mode (fullscreen)
- [ ] Scene-to-output assignment

**Validation:**
- Create 2 outputs on different monitors, enter Perform Mode

---

### Phase 5: Scene System (1 week)

**Deliverables:**
- [ ] Scene creation/deletion
- [ ] Scene switching (keyboard 1-9)
- [ ] Scene panel UI
- [ ] Scene rename/reorder
- [ ] Per-scene quad instances

**Validation:**
- Create 3 scenes with different quads, switch between them

---

### Phase 6: Audio System (1 week)

**Deliverables:**
- [ ] Audio decoding from video
- [ ] Audio mixer
- [ ] Volume controls
- [ ] Multi-output routing
- [ ] Audio source selection

**Validation:**
- Play 2 videos with audio, mix volumes, route to different outputs

---

### Phase 7: Polish & Testing (1-2 weeks)

**Deliverables:**
- [ ] Keyboard shortcut system
- [ ] Preferences dialog
- [ ] Performance optimization
- [ ] Error handling
- [ ] User testing
- [ ] Bug fixes

**Validation:**
- Full workflow test, performance benchmarks, user feedback

---

## 16. Technical Stack

### 16.1 Core Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| **GLFW** | 3.x | Window/context management |
| **GLAD** | 2.x | OpenGL loader |
| **GLM** | 1.x | Mathematics |
| **Dear ImGui** | Docking | User interface |
| **FFmpeg** | 6.x | Video/audio decoding |
| **stb_image** | Latest | Image loading |
| **nlohmann/json** | Latest | Project serialization |
| **miniaudio** | Latest | Audio playback |

### 16.2 Build System

- **CMake** 3.20+
- **Visual Studio 2022** (MSVC 17)
- **C++17** standard

### 16.3 Minimum System Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| OS | Windows 10 (1903) | Windows 11 |
| CPU | 4 cores, 2.5 GHz | 8 cores, 3.5 GHz |
| RAM | 8 GB | 16 GB |
| GPU | OpenGL 3.3, 2 GB | OpenGL 4.5, 8 GB |
| Storage | SSD recommended | NVMe SSD |

---

## Appendix A: UI Mockup - Main Window

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ QuadMapper - My Project.qmap                                      _ □ ×    │
├─────────────────────────────────────────────────────────────────────────────┤
│ File  Edit  View  Scene  Output  Window  Help                               │
├─────────────────────────────────────────────────────────────────────────────┤
│ ┌──────────────────┐┌─────────────────────────────────────┐┌──────────────┐│
│ │ INSPECTOR        ││                                     ││ SCENES   [+] ││
│ │                  ││                                     ││              ││
│ │ ▼ Quad: BG Video ││                                     ││ ► Scene 1 [1]││
│ │   Visible [✓]    ││        ┌─────────────────┐          ││   Scene 2 [2]││
│ │   Locked [ ]     ││        │                 │          ││   Scene 3 [3]││
│ │   Opacity: 100%  ││        │   ┌─────────┐   │          ││              ││
│ │                  ││        │   │  QUAD   │   │          │├──────────────┤│
│ │ ▼ Position       ││        │   │         │   │          ││ OUTPUTS  [+] ││
│ │   TL: 0.1, 0.1   ││        │   └─────────┘   │          ││              ││
│ │   TR: 0.9, 0.1   ││        │                 │          ││ ■ Output 1   ││
│ │   BR: 0.9, 0.9   ││        │        ┌────────┤          ││ ■ Output 2   ││
│ │   BL: 0.1, 0.9   ││        │        │ QUAD 2 │          ││              ││
│ │   [Reset]        ││        └────────┴────────┘          ││ [PERFORM ▶]  ││
│ │                  ││                                     ││              ││
│ │ ▼ Mesh           ││         PREVIEW                     │├──────────────┤│
│ │   Resolution: 16 ││                                     ││ Properties   ││
│ │   Mode: Corners  ││                                     ││              ││
│ │   [Reset Mesh]   ││                                     ││ Resolution:  ││
│ │                  ││                                     ││ 1920 x 1080  ││
│ │ ▼ Texture        ││                                     ││ Rotation: 0° ││
│ │   [video.mp4  ×] ││                                     ││ Monitor: 2   ││
│ │   Offset: 0, 0   │└─────────────────────────────────────┘└──────────────┘│
│ │   Scale: 1, 1    │┌─────────────────────────────────────────────────────┐│
│ │   Flip: [H] [V]  ││ MEDIA BIN                                   [+ Add] ││
│ │                  ││                                                      ││
│ │ ▼ Video          ││ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐   ││
│ │   [▶][❚❚][■][↻] ││ │ ▶  │ │    │ │ ▶  │ │ ▶  │ │    │ │ ♪  │ │ ▶  │   ││
│ │   ═══●════ 1:30  ││ │    │ │    │ │    │ │    │ │    │ │    │ │    │   ││
│ │   Speed: 1.0x    ││ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘ └────┘   ││
│ │   Vol: ████░ 80% ││ vid1   img1   vid2   vid3   img2   music  vid4     ││
│ │   [🔒 Lock]      │└─────────────────────────────────────────────────────┘│
│ └──────────────────┘                                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│ Scene: Scene 1 | Quads: 2 | FPS: 60.0 | Output: 1920x1080 @ Display 2      │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Appendix B: Keyboard Shortcut Quick Reference Card

```
┌─────────────────────────────────────────────────────────────┐
│                QUADMAPPER SHORTCUTS                         │
├─────────────────────────────────────────────────────────────┤
│ FILE                          TOOLS                         │
│ Ctrl+N    New Project         V         Select              │
│ Ctrl+O    Open Project        C         Corner Edit         │
│ Ctrl+S    Save Project        M         Mesh Edit           │
│ Ctrl+Shift+S  Save As         H         Pan                 │
├─────────────────────────────────────────────────────────────┤
│ SCENE                         VIEW                          │
│ 1-9       Switch Scene        G         Toggle Grid         │
│ PgUp/Dn   Prev/Next Scene     F         Fit to Window       │
│ Ctrl+Shift+N  New Scene       +/-       Zoom In/Out         │
├─────────────────────────────────────────────────────────────┤
│ QUAD                          PLAYBACK                      │
│ Q         New Quad            Space     Play/Pause          │
│ Delete    Delete Quad         .         Stop                │
│ Ctrl+D    Duplicate           [/]       Speed Down/Up       │
│ Ctrl+G    Group               L         Toggle Loop         │
│ Ctrl+[/]  Send Back/Front     Ctrl+L    Lock Timeline       │
├─────────────────────────────────────────────────────────────┤
│ OUTPUT                                                      │
│ F11       Perform Mode                                      │
│ Ctrl+1-4  Toggle Output 1-4                                 │
│ Alt+Tab   Switch to Control Window (in Perform Mode)        │
└─────────────────────────────────────────────────────────────┘
```

---

## Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-11-23 | Claude | Initial specification |

---

**End of Specification**
