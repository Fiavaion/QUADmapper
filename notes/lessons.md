# Lessons Ledger

Capture lessons **as they happen**, never retrospectively. Tag the spot in code with an
inline comment `LESSON-{ARCH|BUG|PERF|API|UI|TEST|BUILD}-NNN: <one line>`, then `/lessons`
extracts the tags here. Each lesson should change a durable layer (a rule, CLAUDE.md, or a
skill) so the mistake can't recur — fix the process, not just the instance.

| ID | Tag | Lesson | Where it changed the process |
|----|-----|--------|------------------------------|
| LESSON-BUILD-001 | BUILD | This repo had no git history despite a "v1.0.0" — 25K LOC was unversioned. Always `git init` + baseline before any other work. | git initialized this session (baseline `627481d`). |
| LESSON-BUG-002 | BUG | "Stop-start" was the *dev process* (fix→regress loops), not app runtime. Clarify whether a complaint is about runtime or workflow before chasing a perf fix. | Bootstrapped agent team + scoped rules to cut the looping. |
| LESSON-TEST-003 | TEST | A UI smoke harness that drives the *real* app caught two things 624 unit tests couldn't: (a) projector output is unimplemented (TD-01), (b) the vertex-warp UX genuinely works (exact corner values verified). "Build the harness first" paid off immediately. | Added `tests/smoke/drive_smoke.ps1` as the verification gate for the ADR-031 refactor. |
| LESSON-TEST-004 | TEST | Driving a GLFW/OpenGL window: SendKeys mis-delivers extended arrow keys, and `keybd_event` with `bScan=0` makes GLFW map arrows to `GLFW_KEY_UNKNOWN` (GLFW translates by SCANCODE, not VK). Must send real scancodes via `MapVirtualKey` + `KEYEVENTF_SCANCODE` (+`EXTENDEDKEY` for arrows). Tab/Shift survived bScan=0; arrows did not — a misleading partial symptom. | Documented in `drive_smoke.ps1`; this lesson row is the durable record. |
| LESSON-BUG-005 | BUG | Don't accuse the app before instrumenting. Two wrong guesses (NavEnableKeyboard, then key-delivery method) each "felt right"; a 12-line diagnostic log (`WantCaptureKeyboard`/`shift`/`IsKeyDown`) settled it in one run — the app was correct, the harness was wrong. Per discipline: add logging before reading lots of code; if not diagnosed in ~5 min, change strategy. | Reinforces the "add logging/visual feedback first" rule in CLAUDE.md. |

| LESSON-ARCH-006 | ARCH | Safe way to de-static a giant god-function: move the function-statics into an owned struct (`EditorState`), then at the top of the function alias each field back to a same-named local reference (`auto& scene = es.scene;`). The 2000-line body stays textually unchanged, the compiler flags any missed reference, and a behavioral harness confirms parity. Surfaced one dead field (`currentSelectionId`) for free. | `src/app/EditorState.hpp` + `Application::render()` (TD-02 step 1). |

| LESSON-BUILD-007 | BUILD | `ctest` after a PARTIAL build (`cmake --build --target QUADmapper`) runs STALE test binaries: it reported 624/624 green across TD-01/TD-03 while `test_output` had a signature-mismatch compile error and `test_audio` had a real regression (AudioBuffer rounded capacity up to a power of two, breaking the exact-capacity contract). Both only surfaced on the next FULL `cmake --build`. **Always full-build before ctest** so test exes recompile against changed headers. | Adopted full-build-before-ctest in this workflow; both regressions fixed. |

<!-- New entries appended by /lessons from LESSON-* tags in source. -->
