---
paths:
  - "src/**/*.{cpp,hpp}"
  - "tests/**/*.cpp"
---
# C++ Conventions (loaded when touching C++ sources)

- **Standard:** C++17, MSVC. `#pragma once` in headers. Include order: corresponding
  header → project headers → third-party → stdlib. Forward-declare to cut compile time.
- **Naming:** PascalCase types/files, camelCase functions/vars, SCREAMING_SNAKE constants,
  lowercase namespaces (`quadmapper::scene`).
- **Errors:** return `Result<T, Error>` for fallible functions; avoid exceptions in hot
  paths; log with context via `Logger`.
- **Memory/RAII:** `std::unique_ptr` for ownership, `shared_ptr` only when truly shared,
  RAII for every GL object / file handle.
- **Tests mirror sources:** `src/scene/Quad.cpp` → `tests/unit/scene/QuadTest.cpp`,
  GoogleTest fixtures. Run `ctest --preset debug` after a change batch.
- Keep diffs surgical; match the file you are editing. Delete replaced code, don't comment it out.
