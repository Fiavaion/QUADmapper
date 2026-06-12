# Lessons Ledger

Capture lessons **as they happen**, never retrospectively. Tag the spot in code with an
inline comment `LESSON-{ARCH|BUG|PERF|API|UI|TEST|BUILD}-NNN: <one line>`, then `/lessons`
extracts the tags here. Each lesson should change a durable layer (a rule, CLAUDE.md, or a
skill) so the mistake can't recur — fix the process, not just the instance.

| ID | Tag | Lesson | Where it changed the process |
|----|-----|--------|------------------------------|
| LESSON-BUILD-001 | BUILD | This repo had no git history despite a "v1.0.0" — 25K LOC was unversioned. Always `git init` + baseline before any other work. | git initialized this session (baseline `627481d`). |
| LESSON-BUG-002 | BUG | "Stop-start" was the *dev process* (fix→regress loops), not app runtime. Clarify whether a complaint is about runtime or workflow before chasing a perf fix. | Bootstrapped agent team + scoped rules to cut the looping. |

<!-- New entries appended by /lessons from LESSON-* tags in source. -->
