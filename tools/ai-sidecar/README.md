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
2. installs the pinned `requirements.txt` into it: torch 2.7.1 + torchvision
   (CUDA 12.6 builds from the PyTorch index), `sam2`, `transformers`,
   `opencv-contrib-python` (not `opencv-python`: both provide `cv2`, and only
   contrib has `aruco` and `structured_light`) and numpy. The global Python is
   never touched;
3. downloads the models from their official sources into
   `%LOCALAPPDATA%\QUADmapper\models\` (override with `QM_MODELS_DIR`), checking
   each file against a pinned SHA-256. A mismatch deletes the file and stops.

| Model | Source | Size | Licence |
|---|---|---|---|
| SAM 2.1 hiera small (default) | `dl.fbaipublicfiles.com/segment_anything_2/092824/sam2.1_hiera_small.pt` | 184 MB | Apache-2.0 |
| SAM 2.1 hiera tiny | `dl.fbaipublicfiles.com/segment_anything_2/092824/sam2.1_hiera_tiny.pt` | 156 MB | Apache-2.0 |
| Depth Anything V2 **Small** | `depth-anything/Depth-Anything-V2-Small-hf` @ `5426e4f` | 99 MB | Apache-2.0 |

Depth Anything V2 Base, Large and Giant are CC-BY-NC and are never used.

Re-running `setup.ps1` is safe: it reuses the venv and skips files that are
already present with the right hash. The app prefers `.venv\Scripts\python.exe`
when it exists (ADR-053 section 1).

## Run

```powershell
tools\ai-sidecar\.venv\Scripts\python.exe tools\ai-sidecar\sidecar.py --host 127.0.0.1 --port 8765
# --port 0 lets the OS assign a free port; it is reported on stdout as JSON.
```

On start it prints one JSON line to stdout, e.g.
`{"event": "listening", "host": "127.0.0.1", "port": 8765, "api_version": 1}`.

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
