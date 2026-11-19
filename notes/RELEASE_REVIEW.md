# Release Review

## 2026-11-19: `/release` for 1.0.0 (interactive, whole codebase)

**Verdict: PASS.** No critical or high findings. Every medium is fixed.

### How it ran
- **Stage 0:** a full debug build, green.
- **Scans:** three parallel read-only `scan-sonnet` scans.
  - Security: AI engine HTTP server, SidecarClient, OSC/MIDI, `.qmap` load, OBJ and image parsing, secrets.
  - Slop and prod-ready on app/ui/editor/ai/control/project/input.
  - Slop and prod-ready on video/source/audio/renderer/output/scene/media/core.
- **Humanize:** pattern greps, run by the lead.
- **Fixes:** applied by the lead, then one fresh-context `verify` pass (FAIL on two MAJORs, both fixed in one pass).
- **Stage 5:** one full `ctest`.

### Gate records
- Full suite: `ctest --test-dir build/debug -C Debug -j 8 | uncommitted = d34821d..3b86825 before the review fixes | 1773 passed, 0 failed (8 disabled)`.
- After the review fixes (ImageLoader size cap and TIFF path, one sidecar line, one comment): `test_media ImageLoader*` 21, `test_scene *Obj*` 36, `pytest test_sidecar.py` 49 passed, at `d34821d`.
- `pytest tools/ai-sidecar | before the review fixes | 119 passed`.

### Fixed
| Commit | Severity | What |
|---|---|---|
| `d34821d` | MEDIUM | Images were decoded in full before any size check. Now the header is read first; over 32768 px a side or 134 MP is refused (stb and TIFF paths). Test added. |
| `d34821d` | MEDIUM | OBJ import cap lowered to 2^24 elements per list. |
| `d34821d` | MEDIUM | AI engine: a stalled client held a server thread for good. Now there is a 30 s socket timeout. |
| `d34821d` | LOW | AI engine: nosniff and no-store headers; the 404 no longer echoes the path. |
| `10e0157` | MEDIUM | An unreadable `preferences.json` was silently replaced by the defaults on the next save. Now it is kept as `.bad`, logged and named in the status bar. |
| `10e0157` | MEDIUM | A failed autosave was only logged. Now the status bar says it once per run of failures. |
| `18bee43` | LOW | `sws_scale` failures showed or exported stale pixels. Now the frame is skipped, or the export stops. |
| `d34821d`, `3b86825` | style | Five comments that restated the code. |

### Parked (low, not blocking)
- The AI engine reads and writes any path a request names. Only the app can reach it (token plus Origin/Host checks). Sandbox it to the capture folder if wanted.
- `tools/ai-sidecar/test_segment.py:64` has a machine path (`D:/testing Videos`). It is a dev test; keep `test_*.py` out of the AI engine bundle (TD-44).
- A `.qmap` file is read whole with no size cap. The fix belongs in `ProjectSerializer`, which is a change boundary.
- `UUID::fromString` try/catch is duplicated (`ProjectSerializer.cpp:354`, `ControlBindings.hpp:96`). A `UUID::tryParse` would need a core header change.
- Per-frame warnings with no rate limit if a pixel-buffer map or a GPU frame download keeps failing (`CompressedUpload.cpp:189`, `YuvConverter.cpp:413`, `VideoDecoder.cpp:1034/1472`).
- The startup "Preferences could not be read" status lasts 4 s and a later status can replace it; the log keeps it. A failed autosave shows one interval late.
- The section banners in Theme, InspectorPanel and HapFrameParser match those files' style and were kept. The restating doc comments in core headers were left so public headers stay unchanged.

### Checked clean (from the scans)
- AI engine: loopback bind, the token on every route, Origin/Host refusal, a 1 MiB body cap, an exclusive port.
- SidecarClient: suspended launch into a job, the token only in the environment, WinHTTP timeouts, an 8 MB reply cap.
- OSC: bounded parser (depth 8, 1024 messages, 64 args), DP pattern matching, a bounded queue. MIDI is masked to 7 bits.
- `.qmap` load: checked readers with bounds on every count.
- No secrets or personal paths in shipped code.
- The audio callback is lock- and allocation-free. Every condition-variable wait has a predicate. Shutdown waits are bounded.
