# Release Review

## 2026-11-20: second `/release` for 1.0.0 (interactive, whole codebase, on Opus)

**Verdict: PASS.** No critical or high findings. Two mediums need the owner (below); every other medium is fixed.

### How it ran
- **Stage 0:** debug build at `57c6844`, green.
- **Scans:** three `scan-sonnet` scans, same split as 2026-11-19; each was told what was already fixed or parked. The engine-half scan was sent back once to cover the files it skipped (VideoDecoder, OutputManager, DxInterop, YuvConverter, HapExport, scene).
- **Humanize:** pattern greps by the lead.
- **Fixes:** applied by the lead. One fresh-context `verify` pass gave PASS with 4 MINORs, fixed in one pass.
- **Stage 5:** one full `ctest`.

### Gate records
- Full suite: `ctest --test-dir build/debug -C Debug -j 8 | uncommitted (3cace9d..f13c9cd content before the verify fixes) | 1772 passed, 1 failed of 1773`.
  - The failure was `AsyncSourceLifecycle.AReopenedZeroCopyClipTakesOverTheKeptSharedPlanes`: no frame within the wait, under parallel GPU load (19.5 s).
  - It passed alone straight after (9.5 s). Treated as a flaky test, not a regression; it never reaches the changed code.
- After the verify fixes (three comments and `AiAssist` `tempFolder`): build green, `test_ai_assist` 57 passed, `test_video_decoder *Seek*` 34 passed, at `f13c9cd`.

### Fixed
| Commit | Severity | What |
|---|---|---|
| `3cace9d` | MEDIUM | Video, HAP playback and Convert to HAP had no size cap; a crafted file could force a multi-GB allocation. Now they use the image limit (32768 px a side, 134 MP), shared in `core/MediaLimits.hpp`. |
| `3cace9d` | MEDIUM | `VideoDecoder::seek()` moved the demuxer while the decode thread could be using it (test callers only), and two comments said otherwise. It now joins the thread first. |
| `86b73e5` | MEDIUM | Open/Save dialogs had a MAX_PATH buffer; a longer path made the dialog do nothing, silently. |
| `9c9841d` | MEDIUM | The three `QM_AI_TEST_IMAGE` lookups are now one helper. |
| `9c9841d` | LOW | A save or file action could delete the recovery files while an autosave was still flushing, and the write then put them back (a stale recovery prompt next start). A flushing write also cost a full autosave interval. |
| `9c9841d` | LOW | The AI engine was launched even if its kill-on-close job failed to set up, so a crash could orphan it. |
| `9c9841d`, `1f1ddbc` | LOW | `temp_directory_path()` (throwing) in fallbacks is replaced by the non-throwing overload. |
| `1f1ddbc` | LOW | `bad_alloc` in the OBJ worker thread ended the app; now it is an import error. A failed thumbnail cache write leaves no partial file; the buffer is a vector. |
| `f13c9cd`, `1f1ddbc` | style | Six comments that restated the code (one was wrong: "device index" over a device id). |

### Needs the owner
- **Network paths in a `.qmap` (MEDIUM).** A project can name `\\host\share\clip.mp4`. Opening it makes Windows connect to that host and offer the user's NTLM hash.
  - Blocking UNC paths would also block media kept on a NAS.
  - Options: warn and ask once per host on load; or leave it and document it ("open projects only from people you trust").
  - The check would sit in the load mapping (`editor/ProjectMapping.cpp`), not `ProjectSerializer`.
- **FFmpeg protocol whitelist (LOW, same class).** Media files are opened with every FFmpeg protocol allowed, so a crafted "video" that is really an HLS playlist can fetch from a web server.
  - The app never opens URLs.
  - Passing `protocol_whitelist=file,crypto,data` at the five `avformat_open_input` file opens would close this.
- **`VideoDecoder::seekFrame` has no callers.** Removing it changes a public header (a Change Boundary).

### Already decided, not reopened
- OSC listens on `0.0.0.0` when enabled, with no sender allowlist (owner, 2026-11-19).
- The `QM_AI_SELFTEST` hook stays (it is the beta's verification path).

### Parked (low, not blocking)
- Thumbnails are made once at the slider size at that moment; moving the size slider up shows them slightly soft. A fix must also deal with thumbnails already cached on disk.
- `AudioMixer::playing_` is a plain bool read by `renderOffline()`. It has no shipped caller that overlaps `start()`.
- Thumbnail GL textures bypass `Texture`/`GlObjectStats`. They are released correctly, but the leak harness cannot see them.
- HapConversion worker thread has no exception guard. The size cap now bounds its buffer.
- OSC reply-mode feedback goes to the last sender, which can be spoofed. It is off by default and rate-capped.
- AI engine: 400 replies include `str(exc)`, behind the token.
- `.qmap`:
  - `view.zoom`, crop, colour and opacity floats are not checked for finite values;
  - there is no cap on scene, quad or group counts.
  - Both fixes would go in `ProjectSerializer`, a change boundary.
- Unknown procedural type on load is only logged.
- `ShellExecute` results in the Help menu and Reveal in Explorer are not checked.
- Dismissed: `X-Frame-Options`/`Referrer-Policy` on the AI engine's JSON-only loopback API.

### Checked clean (from the scans)
- **VideoDecoder threading:**
  - stop flag set under the mutex;
  - every wait has a predicate;
  - replan is re-checked before a push.
- **GPU paths:**
  - OutputManager fence and frame deletion;
  - DxInterop unregisters before release;
  - YuvConverter RAII.
- **HapExport:** the `.part` file is cleaned up.
- **Audio callback:** no lock or allocation.
- **Parsers:** HapFrameParser and Snappy caps, the OSC parser bounds.
- **AI engine:**
  - SidecarClient token handling;
  - the sidecar's token, Origin, Host, body cap, `allow_pickle=False` and `local_files_only`.

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
