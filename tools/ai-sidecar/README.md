# QUADmapper AI pre-show sidecar

Optional, out-of-process service for AI pre-show assist (ADR-035, ADR-053). The
C++ app talks to it over **loopback HTTP + JSON**. It is **never linked into
`QUADmapper.exe`**, and the app is fully usable without it: any feature whose
packages or model are missing answers with a stub, and `/health` says which
features are available.

## Setup

```powershell
powershell -ExecutionPolicy Bypass -File tools\ai-sidecar\setup.ps1
```

`setup.ps1`:

1. creates `tools\ai-sidecar\.venv` with Python 3.11 (`py -3.11`; git-ignored);
2. installs the pinned pip (`requirements-pip.lock`), then `requirements.lock` into
   it with `--require-hashes` (every package,
   transitive ones too, pinned by hash): torch 2.7.1 + torchvision
   (CUDA 12.6 builds from the PyTorch index), `transformers`,
   `opencv-contrib-python` (not `opencv-python`: both provide `cv2`, and only
   contrib has `aruco` and `structured_light`) and numpy; then `sam2` from
   `requirements-sam2.lock` (source-only: pinned by its sdist hash and built
   against the installed torch). The global Python is never touched. With `-Dev`
   it also installs `requirements-dev.lock` (pytest, for the tests below);
3. downloads the models from their official sources into
   `%LOCALAPPDATA%\QUADmapper\models\` (override with `QM_MODELS_DIR`), checking
   each file against a pinned SHA-256. A mismatch deletes the file and stops.

| Model | Source | Size | Licence |
|---|---|---|---|
| SAM 2.1 hiera small (default) | `dl.fbaipublicfiles.com/segment_anything_2/092824/sam2.1_hiera_small.pt` | 184 MB | Apache-2.0 |
| SAM 2.1 hiera tiny | `dl.fbaipublicfiles.com/segment_anything_2/092824/sam2.1_hiera_tiny.pt` | 156 MB | Apache-2.0 |
| Depth Anything V2 **Small** | `depth-anything/Depth-Anything-V2-Small-hf` @ `5426e4f` | 99 MB | Apache-2.0 |

Depth Anything V2 Base, Large and Giant are CC-BY-NC and are never used.

`requirements.txt` stays the human-readable list of direct packages; the lock is
what gets installed. To regenerate the lock (after changing `requirements.txt`),
in a throwaway venv, never the global Python:

```powershell
py -3.11 -m venv $env:TEMP\qm-lock; & $env:TEMP\qm-lock\Scripts\python.exe -m pip install packaging
# PINS.txt: `pip freeze` of the working .venv without sam2, plus setuptools==80.9.0 and wheel==0.45.1
& $env:TEMP\qm-lock\Scripts\python.exe tools\ai-sidecar\gen_lock.py PINS.txt tools\ai-sidecar\requirements.lock
```

`gen_lock.py` reads each file's SHA-256 from PyPI (and from the PyTorch cu126 index
for torch and torchvision) for the Windows cp311 wheels, and writes the lock.
The `--extra-index-url` line at the top of the lock is how pip finds torch.
`requirements-sam2.lock` holds sam2's sdist hash from PyPI; update it with the pin.
`requirements-dev.lock` is made by the same script from a pins file that holds pytest
and the packages only it needs (iniconfig, pluggy, Pygments); leave those four out of
`PINS.txt`, and put the file's own header comment back afterwards.

Before a release, check the lock for known vulnerabilities, in a throwaway venv:

```powershell
& $env:TEMP\qm-lock\Scripts\python.exe -m pip install pip-audit
& $env:TEMP\qm-lock\Scripts\python.exe -m pip_audit --require-hashes --disable-pip -r tools\ai-sidecar\requirements.lock
```

Re-running `setup.ps1` is safe: it reuses the venv and skips files that are
already present with the right hash. A developer build of the app runs the
sidecar on `.venv\Scripts\python.exe`, found by walking up from the folder of
`QUADmapper.exe` (never the current folder); without the venv it reports that
the AI features are not installed (ADR-035, amendment of 2026-11-14).

## Run

The app launches the sidecar itself, with a fresh token for each launch. To run
it by hand (to debug it, or to call it with curl), choose a token and give it to
both the sidecar and the app:

```powershell
$env:QM_SIDECAR_TOKEN = "my-dev-token"   # letters, digits, - and _
tools\ai-sidecar\.venv\Scripts\python.exe tools\ai-sidecar\sidecar.py --host 127.0.0.1 --port 8765
# --port 0 lets the OS assign a free port; it is reported on stdout as JSON.
curl.exe -H "X-QM-Token: my-dev-token" http://127.0.0.1:8765/health
```

On start it prints one JSON line to stdout, e.g.
`{"event": "listening", "host": "127.0.0.1", "port": 8765, "api_version": 1}`.
Without `QM_SIDECAR_TOKEN` it makes a token and prints it on the next line.

Start the app from a shell where `QM_SIDECAR_TOKEN` is set to the same value:
only then does it attach to a sidecar it did not launch. Otherwise a sidecar
already on the port shows "AI: port 8765 is in use by another program".

Every request must carry `X-QM-Token` (401 otherwise). A request with an
`Origin` header, or a `Host` other than `127.0.0.1:<port>` or `localhost:<port>`,
gets 403, and a POST whose `Content-Type` is not `application/json` gets 415, so
web pages cannot drive the sidecar.

Models load lazily on the first request that needs them (GPU when
`torch.cuda.is_available()`, else CPU) and stay loaded.

## Contract (API v1)

All coordinates are normalized `[x, y]` in `[0, 1]` photo space, origin top-left.
Corners are ordered TL, TR, BR, BL. Every POST answers 200 with `stub: true` and
`confidence: 0` (centered rectangle corners) when it cannot run for real; only a
malformed request gets 400.

| Method | Path | Request | Response |
|---|---|---|---|
| GET | `/health` | | `{status, api_version, service, capabilities}` |
| POST | `/surface` | `{image_path}` | `{api_version, corners[4], plane, confidence, stub}` |
| POST | `/oneflash` | `{pattern_image, target_corners?, surface_image?}` | `{api_version, corners[4], homography, confidence, stub}` |
| POST | `/segment` | `{image_path, point: [x, y], model?: "small"\|"tiny"}` | `{api_version, corners[4], outline, confidence, planarity, stub}` |
| POST | `/graycode/patterns` | `{folder, projector: [W, H]}` | `{api_version, count, stub}` |
| POST | `/graycode` | `{folder, projector: [W, H]}` | `{api_version, map_path, coverage, decode_error_px, stub}` |
| POST | `/graymap` | `{map_path, points: [[x, y], ...]}` | `{api_version, points: [[x, y], ...], valid: [bool, ...]}` |

`api_version` is bumped on any shape change; the C++ client refuses to attach to
a sidecar whose version it does not understand. Responses may carry extra
diagnostic fields (`timing_ms`, `debug`, `model`, `device`), which clients ignore.

### `/health` capabilities

Each name is listed only when its packages are importable and its model files
are present (checked without loading, so `/health` stays fast); a model whose
load fails is dropped for the rest of the run.

| Capability | Needs |
|---|---|
| `surface` | SAM 2.1 small + Depth Anything V2 Small |
| `oneflash` | `cv2.aruco` + `resources/patterns/charuco.png` |
| `segment` | SAM 2.1 small |
| `depth` | Depth Anything V2 Small |
| `graycode` | `cv2.structured_light` (opencv-contrib-python) |

### `/segment`: click-to-segment (ADR-053 section 2)

SAM 2.1 segments the surface under `point` (the best of its three masks). The
mask's largest contour gives:

- `corners`: the quadrilateral fitting the contour (`approxPolyDP` on its hull,
  tolerance grown until it reaches 4 points; else `minAreaRect`);
- `outline`: the contour, at most 256 points.

When Depth Anything V2 Small is present, `planarity` (0..1) comes from a
least-squares fit of `disparity = a*u + b*v + c` over the mask: relative inverse
depth is linear over a plane, so the residual (normalized by the photo's
disparity range) measures how curved or broken the surface is.
`confidence = SAM mask score * planarity`; without the depth model it is the SAM
score and `planarity` is null.

The image embedding and the disparity map are kept for the last photo, so a
second click on the same photo takes tens of milliseconds.

A request without `point`, with a point outside 0..1, an unknown `model` or a
non-string `image_path` gets 400. A missing or unreadable image, or no SAM 2
model, gives the stub.

### Gray-code scan (ADR-053 section 3)

The app drives the capture; the sidecar writes the patterns and decodes.

1. `/graycode/patterns` writes OpenCV's `GrayCodePattern` sequence for a `W x H`
   projector into `folder` as `pattern_000.png` ... : the patterns (each followed by
   its inverse, columns then rows), then a white and a black frame. `count` is the
   number of frames, read from the generator (46 at 1920x1080).
2. The app shows frame k fullscreen on the output and saves one camera frame as
   `capture_000.png` ... in the same folder.
3. `/graycode` decodes the captures and writes `graymap.npz` beside them: for each
   camera pixel, the projector pixel it sees, or -1. The decode is GrayCodePattern's
   `getProjPixel` rule over the whole image at once (a test checks they agree), with
   the white/black shadow mask. `coverage` is the share of the projector's area
   (8 px cells) the camera saw; `decode_error_px` is the mean distance of a decoded
   pixel from its 5x5 median, a noise estimate (null when nothing decoded).
4. `/graymap` maps normalized photo points (the same camera, same position) to
   normalized projector points: the median of the decoded pixels within 3 camera
   pixels, at the pixel centre. A point with none is `valid: false`.

Bad fields, a missing folder or capture, or a `map_path` that is not a map get
400. Without OpenCV contrib, `/graycode/patterns` and `/graycode` give the stub.

### `/surface`: automatic detection (QM-057, re-engined by ADR-053)

SAM 2.1's automatic mask generator proposes regions (2-95 % of the frame). Each
is scored from a disparity-plane fit: planarity, size, SAM's own score and how
light the region is (a white screen takes projection, a black curtain does not).
Two gates keep the QM-057 intent of "low, not confidently wrong":

- **verticality**: a floor or ceiling, whose disparity climbs steeply with image
  height, scores near 0;
- **ambiguity**: a separate region on a different plane that scores nearly as
  well collapses the confidence. Regions nested in the winner, or lying on its
  plane (other panels of one facade), are not rivals.

Relative depth has no metric scale, so `plane` is `null`. On the three spike
scenes: theatre screen 0.44 (the screen), oblique facade 0.88 (one panel of
it), gallery 0.11 (several equally good walls).

### Latency (RTX 3080 Laptop, 8 GB)

| | GPU | CPU |
|---|---|---|
| first request of a run (torch import, model loads) | ~12 s (40 s with a cold disk cache) | ~14 s |
| `/segment`, new photo | ~1.0 s | ~5.3 s |
| `/segment`, same photo again | ~50 ms | ~115 ms |
| `/surface` (256 SAM prompts) | ~1.0-1.3 s | ~22 s (33 s on the first call) |

On CPU, `/surface` can exceed the app's 30 s request timeout; a CUDA GPU is
recommended for Detect Surface.

## Test

pytest comes with `setup.ps1 -Dev`.

```powershell
tools\ai-sidecar\.venv\Scripts\python.exe -m pytest -q tools\ai-sidecar
```

- `test_sidecar.py`: the HTTP contract, including the stub paths and 400s
  (standard library only; models forced absent where it matters).
- `test_geometry.py`: mask to corners on synthetic polygons, the planarity fit
  on planar vs curved disparity, verticality (numpy + OpenCV only).
- `test_surface.py`: candidate scoring and gates on synthetic data, plus the
  three spike scenes when the models are present.
- `test_segment.py`: real SAM 2 on the theatre photo; skipped without models.
  Run with `-s` to see latencies.
- `test_oneflash.py`: One-Flash homography on a synthetic warp.
- `test_graycode.py`: Gray-code decode on patterns warped by a known homography
  and a known non-planar displacement (within 1 px), agreement with
  `getProjPixel`, and the `/graycode`, `/graycode/patterns`, `/graymap` contract.
