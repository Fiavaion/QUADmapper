---
paths:
  - "src/renderer/**"
  - "src/shaders/**"
  - "src/output/**"
---
# Rendering & OpenGL Rules (loaded when touching renderer/shaders/output)

- **OpenGL 3.3 core**, GLSL 3.30 — the verified runtime target (NVIDIA path tested). Do not
  use features above 3.3 without an ADR; many install machines cap here.
- **Texture uploads on the main thread only** (OpenGL context constraint). Video decode runs
  on a background thread and stages frames; the upload itself stays on the render thread.
- **Minimize per-frame GL state changes**; cache textures aggressively (`TextureCache`).
  Target 60 FPS with multiple 1080p outputs.
- **The frame loop** is `Application::run()` → `pollFrame()` (delta time, `glfwPollEvents`,
  frame-boundary tasks: file actions, external control, source release, projector fit,
  autosave) then `presentFrame()` (input → update → render → editor swap → projector
  outputs), vsync on. The bench (`--bench`) drives the same two halves. Don't restructure it
  without a measured reason.
- Every GL object (VAO/VBO/FBO/texture/shader) is RAII-wrapped — keep it that way; no raw
  `glGen*`/`glDelete*` leaking outside a wrapper.
- Shaders live in `src/shaders/*.{vert,frag}` and are copied to the build dir as resources.
