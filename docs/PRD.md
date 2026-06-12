# QUADmapper Product Requirements Document (PRD)

**Document Version:** 1.0
**Date:** 2025-12-19
**Status:** Approved
**Target Platform:** Windows 11

---

## 1. Executive Summary

### 1.1 Product Vision

QUADmapper is a professional quad-based projection mapping application that enables users to warp video and image content onto physical surfaces using perspective-corrected quads with mesh subdivision, manage multiple output windows for multi-projector setups, and organize content into switchable scenes.

### 1.2 Problem Statement

Live event technicians, installation artists, and VJs need a reliable, performant tool to map video content onto irregular surfaces during performances and installations. Existing solutions are either prohibitively expensive, overly complex, or lack the real-time performance required for live events.

### 1.3 Product Goals

1. **Accessibility:** Provide professional-grade projection mapping at an accessible price point
2. **Performance:** Maintain 60 FPS output with multiple simultaneous video sources
3. **Usability:** Intuitive interface that allows quick setup during live events
4. **Reliability:** Stable operation for multi-hour events without crashes or frame drops

---

## 2. Target Users

### 2.1 Primary Users

| User Type | Description | Key Needs |
|-----------|-------------|-----------|
| **Live Event Technicians** | Professionals operating video for concerts, corporate events | Quick setup, reliable performance, multi-output |
| **Installation Artists** | Artists creating permanent or temporary visual installations | Precise mapping, scene scheduling, unattended operation |
| **VJs** | Visual performers at clubs and events | Real-time control, scene switching, video playback |

### 2.2 Secondary Users

| User Type | Description | Key Needs |
|-----------|-------------|-----------|
| **Architectural Designers** | Mapping onto building facades | High resolution output, precise geometry |
| **Theater Technicians** | Stage and set projection | Multi-scene shows, cue-based switching |

### 2.3 User Personas

**Persona 1: Alex - Live Event Tech**
- 5+ years experience with video equipment
- Works 10-15 events per month
- Needs: Fast setup, reliable playback, easy troubleshooting
- Pain points: Software crashes during shows, complex licensing

**Persona 2: Maya - Installation Artist**
- Creates interactive visual installations
- Installations run 8-12 hours daily
- Needs: Precise mapping, long-term stability, remote monitoring
- Pain points: Drift over time, difficult calibration

---

## 3. User Stories

### 3.1 Core User Stories (MVP)

| ID | As a... | I want to... | So that... | Priority |
|----|---------|--------------|------------|----------|
| US-001 | User | Create a quad and position its 4 corners | I can map content to a rectangular surface | P0 |
| US-002 | User | Edit mesh vertices within a quad | I can map content to curved surfaces | P0 |
| US-003 | User | Load and display an image on a quad | I can map static content | P0 |
| US-004 | User | Load and play a video on a quad | I can map dynamic content | P0 |
| US-005 | User | Control video playback (play/pause/stop) | I can manage content timing | P0 |
| US-006 | User | Adjust video speed and loop mode | I can create seamless loops | P0 |
| US-007 | User | Create multiple scenes | I can organize different looks | P0 |
| US-008 | User | Switch scenes instantly with keyboard | I can perform live shows | P0 |
| US-009 | User | Create multiple output windows | I can drive multiple projectors | P0 |
| US-010 | User | Enter fullscreen perform mode | I can display clean output | P0 |
| US-011 | User | Save and load projects | I can resume work across sessions | P0 |
| US-012 | User | Adjust video volume per source | I can control audio mix | P0 |

### 3.2 Post-MVP User Stories

| ID | As a... | I want to... | Priority | Phase |
|----|---------|--------------|----------|-------|
| US-101 | User | Blend edges between adjacent quads | P1 | v1.1 |
| US-102 | User | Control the app via MIDI | P1 | v1.1 |
| US-103 | User | Receive NDI video streams | P2 | v1.2 |
| US-104 | User | Add color correction per quad | P1 | v1.1 |
| US-105 | User | Crossfade between scenes | P2 | v1.2 |

---

## 4. Feature Requirements

### 4.1 MVP Features (Must Have)

#### 4.1.1 Quad Warping System
- 4-corner perspective transform for each quad
- Mesh subdivision (16x16 default, up to 64x64)
- Individual mesh vertex editing
- Z-order control for overlapping quads
- Quad grouping and hierarchy

#### 4.1.2 Texture System
- Image formats: PNG, JPG, TIF, BMP, TGA
- Video formats: MP4, MOV, AVI, MKV, WEBM
- UV offset and scale (pan/zoom within texture)
- Pixel-based crop regions
- Horizontal and vertical flip

#### 4.1.3 Video Playback
- Play, pause, stop controls
- Speed control (0.1x to 4.0x)
- Loop modes: Once, Loop, Ping-Pong
- Timeline lock for multi-video synchronization
- Background decoding threads

#### 4.1.4 Audio System
- Audio playback from video files
- Per-source volume control
- Master volume
- Mute per source
- Multi-output routing (future)

#### 4.1.5 Output System
- Multiple output windows
- Per-output resolution control
- Rotation: 0, 90, 180, 270 degrees
- Perform mode (borderless fullscreen)
- Monitor selection per output

#### 4.1.6 Scene Management
- Create, rename, delete scenes
- Keyboard switching (1-9 keys)
- Scene duplication
- Scene reordering
- Per-scene quad visibility

#### 4.1.7 User Interface
- Inspector panel with all quad properties
- Media bin with thumbnails
- Preview panel with interactive editing
- Scene panel with scene list
- Output panel with output configuration

#### 4.1.8 Project Management
- Save/load projects (.qmap JSON format)
- Relative media paths
- Media relocation prompts
- Auto-save (optional)

### 4.2 Post-MVP Features

| Feature | Description | Target Version |
|---------|-------------|----------------|
| Soft-edge blending | Smooth transitions between adjacent quads | v1.1 |
| MIDI control | External control via MIDI devices | v1.1 |
| OSC support | Network control protocol | v1.1 |
| Color correction | Per-quad brightness, contrast, gamma | v1.1 |
| NDI input | Receive network video streams | v1.2 |
| Spout input | Texture sharing between applications | v1.2 |
| Scene transitions | Crossfade and other transition effects | v1.2 |

---

## 5. Success Metrics

### 5.1 Performance Metrics

| Metric | Target | Minimum Acceptable |
|--------|--------|-------------------|
| Frame rate (single 1080p) | 60 FPS | 30 FPS |
| Frame rate (dual 1080p) | 60 FPS | 30 FPS |
| Input-to-display latency | < 50ms | < 100ms |
| Scene switch time | < 16ms | < 33ms |
| Simultaneous videos | 16 | 8 |
| Max quads per scene | 256 | 64 |

### 5.2 Quality Metrics

| Metric | Target |
|--------|--------|
| Crash rate | < 1 per 100 hours of operation |
| Memory leaks | None |
| Video sync drift | < 1 frame over 1 hour |

### 5.3 User Experience Metrics

| Metric | Target |
|--------|--------|
| Time to first output | < 5 minutes for new users |
| Time to map single surface | < 2 minutes |
| Time to create 4-quad setup | < 10 minutes |

---

## 6. Constraints and Assumptions

### 6.1 Technical Constraints

- **Platform:** Windows 10/11 only (64-bit)
- **Graphics:** OpenGL 3.3+ required
- **Memory:** Minimum 8GB RAM, 16GB recommended
- **GPU:** Dedicated GPU with 2GB+ VRAM recommended

### 6.2 Business Constraints

- MVP must be completed within planned development phases
- Single developer (AI-assisted) development model
- Open source or permissive licensing for dependencies

### 6.3 Assumptions

- Users have basic familiarity with video playback software
- Users understand projection mapping concepts
- Target machines will have dedicated GPUs
- Video files are accessible on local or network storage

---

## 7. Out of Scope (MVP)

The following features are explicitly out of scope for the MVP:

- 3D object mapping
- Bezier curve warping
- Built-in video effects (blur, glow, etc.)
- Timeline-based animation
- DMX or ArtNet integration
- macOS or Linux support
- Real-time camera input
- Recording/capture functionality

---

## 8. Dependencies

### 8.1 External Dependencies

| Dependency | Purpose | License |
|------------|---------|---------|
| GLFW | Window management | zlib |
| GLAD | OpenGL loading | MIT |
| GLM | Mathematics | MIT |
| Dear ImGui | User interface | MIT |
| FFmpeg | Video decoding | LGPL/GPL |
| nlohmann/json | Serialization | MIT |
| stb_image | Image loading | Public Domain |
| miniaudio | Audio playback | Public Domain |

### 8.2 Development Dependencies

| Dependency | Purpose |
|------------|---------|
| CMake 3.20+ | Build system |
| Visual Studio 2022 | Compiler |
| vcpkg | Package management |
| GoogleTest | Unit testing |

---

## 9. Risks and Mitigations

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| FFmpeg integration complexity | High | Medium | Use proven integration patterns, comprehensive testing |
| Multi-window OpenGL issues | High | Medium | Careful context management, single-context design |
| Performance with multiple videos | High | Medium | Background decoding, texture caching, profiling |
| Cross-GPU compatibility | Medium | Low | Stick to OpenGL 3.3 core, avoid vendor extensions |

---

## 10. Glossary

| Term | Definition |
|------|------------|
| **Quad** | A four-cornered surface used for projection mapping |
| **Mesh** | A grid of vertices within a quad for curved surface mapping |
| **Scene** | A collection of quads with their textures and settings |
| **Output** | A window displaying rendered content for a projector |
| **Perform Mode** | Fullscreen output mode without window decorations |
| **Timeline Lock** | Synchronization of multiple video playback positions |
| **UV Coordinates** | Texture mapping coordinates (0-1 range) |

---

## Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2025-12-19 | Claude | Initial PRD from specification |
