---
name: build-stale-relink
description: "cmake --build --target QUADmapper sometimes doesn't relink the exe after a source/generated-header change; touch the .cpp to force it"
metadata: 
  node_type: memory
  type: project
  originSessionId: 634949cc-4953-42c2-a9f8-791a0993e6da
---

On this MSVC/CMake project, `cmake --build build/debug --target QUADmapper` occasionally
reports success but leaves a **stale `QUADmapper.exe`** — it skips the relink after a changed
source or a regenerated header (e.g. `ShaderSources.hpp` from a `.vert` edit, or an
`Application.cpp` edit). Symptom: the running app doesn't reflect the change even though the
build "succeeded."

**How to detect:** check the exe timestamp vs your edit, or grep the binary for a known new
string (`grep -c "MY_NEW_STRING" build/debug/src/app/Debug/QUADmapper.exe`).

**Fix:** `touch` the relevant `.cpp` (e.g. `src/app/Application.cpp` or `src/renderer/Renderer.cpp`)
then rebuild — that forces the object recompile + relink. After editing a `src/shaders/*.vert/frag`,
also re-run `cmake --preset debug` (configure) so `file(READ ...)` regenerates `ShaderSources.hpp`,
then touch a renderer source.

Verified twice in Session 16 (handle shader fix; the P2 test hook).
