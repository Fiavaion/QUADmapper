---
name: verification-budget
description: "Owner decisions 2026-10-02/03 — QUADmapper testing is proportionate to risk: full ctest only near release, targeted tests per change, reviews only for risky changes, no routine mutation/stress loops"
metadata:
  node_type: memory
  type: feedback
  originSessionId: 69336402-ac3a-466d-a51f-a3815abe3b02
  modified: 2026-10-02T05:57:51.724Z
---

For QUADmapper, keep verification proportionate (written into CLAUDE.md "Verification budget"):

1. No full `ctest` per change (owner, 2026-10-03: "test suite is not necessary... when we are nearing release we will run the full test suite"). Build, and run only the test executables for the code changed. The full suite runs once, near release (part of `/release`).
2. A fresh-context `verify` review only for risky changes: threading, the `.qmap` format, the perform lock, the render loop. Otherwise the lead reads the diff.
3. No mutation runs except to settle one specific doubt (at most 1–2).
4. Stress loops at most 5 rounds with no added load, unless chasing a reported failure.
5. Benchmarks only for changes meant to affect speed.
6. No speed work below what a user would notice unless the owner reports a stall.
7. The owner's hands-on testing comes before more automated proof.

**Why:** in late September 2026 each slice got author, reviewer, fixer and merge full-suite runs, plus 20–100-round stress loops, 3–20 mutant rebuilds and benches. Single agents ran 2–4.5 h, and the owner said "this application will never be done at this rate". The project's own success metric is the running app, not test volume.

**How to apply:** write the budget into every agent prompt (no "100 rounds", no mutation gates by default, no full-suite runs). After a merge that changes CMake lists or shaders, reconfigure and build; run only the affected tests. When in doubt, get the build into the owner's hands. Related: [[merge-reconfigure-before-ctest]].
