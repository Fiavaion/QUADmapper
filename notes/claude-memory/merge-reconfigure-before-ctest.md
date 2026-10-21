---
name: merge-reconfigure-before-ctest
description: "After merging an agent branch into the main tree, reconfigure and do a full build before ctest, and check the test count matches the agent's"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 69336402-ac3a-466d-a51f-a3815abe3b02
  modified: 2026-09-29T20:32:48.638Z
---

After merging an agent branch into the main checkout (fast-forward or patch apply), always run `cmake --preset debug -DVCPKG_MANIFEST_INSTALL=OFF` and a full `cmake --build build/debug --parallel 8` before `ctest`. Then compare the test count with the count the agent reported.

**Why:** on 2026-09-29 I fast-forwarded the ADR-051 E6 wiring, rebuilt only the `QUADmapper` target and ran ctest. It reported "100% of 1551". But the branch had added test files to `tests/unit/CMakeLists.txt`, so the test list and the test executables were stale; the agent's count was 1558. A green run on stale binaries proves nothing. A semantic merge conflict (an E3 test using a moved member) was also only caught because a full build failed.

**How to apply:** treat a count lower than the branch's reported count as a stale build, not a pass. Shader edits also need the reconfigure (shaders embed at configure time). Related: [[build-stale-relink]], [[merge-conflict-guard]].
