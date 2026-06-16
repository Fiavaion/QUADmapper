# QUADmapper AI pre-show sidecar

Optional, out-of-process service for AI pre-show assist (ADR-035). The C++ app
talks to it over **loopback HTTP + JSON**; it is **never linked into
`QUADmapper.exe`** and the app is fully usable without it.

This directory currently holds the **QM-056 skeleton**: the integration boundary
(launch / health-check / versioned request-response / teardown) with no CV/ML
behind it yet. Real surface detection (Metric3D → plane → corners) lands in
QM-057+.

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

## Test (GPU-free)

```powershell
python -m pytest -q
```

The skeleton and its tests use the Python standard library only — no torch /
Open3D / GPU. CV/ML dependencies (added with QM-057) are pinned in
`requirements.txt` and kept out of the skeleton path so the contract stays
testable without a GPU.
