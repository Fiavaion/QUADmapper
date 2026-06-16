# QUADmapper AI pre-show sidecar

Optional, out-of-process service for AI pre-show assist (ADR-035). The C++ app
talks to it over **loopback HTTP + JSON**; it is **never linked into
`QUADmapper.exe`** and the app is fully usable without it.

It holds the **QM-056 boundary** (launch / health-check / versioned
request-response / teardown) and the **QM-057 surface service** (real Metric3D →
plane → 4-corner detection). The heavy CV stack loads **lazily**, only when a real
`/surface` request arrives — so `/health` and the contract tests stay light.

## Run

```powershell
python sidecar.py --host 127.0.0.1 --port 8765
# --port 0 lets the OS assign a free port; it is reported on stdout as JSON.
```

On start it prints one JSON line to stdout, e.g.
`{"event": "listening", "host": "127.0.0.1", "port": 8765, "api_version": 1}`.

## Contract (API v1)

| Method | Path       | Request                     | Response |
|--------|------------|-----------------------------|----------|
| GET    | `/health`  | —                           | `{status, api_version, service}` |
| POST   | `/surface` | `{image_path}` or `{frame}` | `{api_version, corners[4], plane, confidence, stub}` |

`corners` are normalized `[x, y]` in `[0,1]` image space. In the skeleton,
`/surface` returns a centered placeholder rectangle with `confidence: 0.0` and
`stub: true`.

`api_version` is bumped on any shape change; the C++ client refuses to attach to
a sidecar whose version it does not understand.

### `/surface` — real detection (QM-057)

When the request's `image_path` is readable **and** the Metric3D model is present,
`/surface` runs the real pipeline (`stub: false`):

  image → Metric3D (metric depth + normals) → metric point cloud →
  Open3D `segment_plane` → largest planar component → 4 corners → confidence

`confidence` folds in a **verticality prior**: a cluttered scene whose biggest
plane is the floor/ceiling returns ~0 rather than confidently warping onto the
wrong surface. On the 3 spike scenes: theatre screen ≈0.58 (encloses the screen),
oblique facade ≈0.59, gallery floor-trap ≈0.0.

Latency: **~350ms** steady state on GPU (RTX 3080); the **first** request pays a
one-time ~7–8s model load. CPU works but is slower. If `image_path` is missing or
the model is absent, `/surface` falls back to the stub and the app is unaffected.

**Model location** (not pip-installed) — defaults to the local ComfyUI
controlnet_aux tree; override via env vars before launch:

```
QM_M3D_SRC=...\comfyui_controlnet_aux\src
QM_M3D_CKPT=...\Metric3D\metric_depth_vit_small_800k.pth
QM_M3D_BACKBONE=vit-small   # or vit-large / vit-giant2
```

## Test (GPU-free)

```powershell
python -m pytest -q
```

`test_sidecar.py` (the boundary/contract) uses the standard library only — no
torch / Open3D / GPU. `test_surface.py` (the QM-057 service) is **skipped** unless
the Metric3D model is present, so the contract stays testable without a GPU; when
the model is on the box it asserts the AC2 behaviour on the 3 spike scenes.
