#!/usr/bin/env python3
"""QUADmapper AI pre-show sidecar (ADR-035, ADR-053).

An optional, out-of-process service the C++ app talks to over loopback HTTP+JSON.

Contract (v1):
  GET  /health           -> 200 {status, api_version, service, capabilities}
  POST /surface {image_path}
                         -> 200 {api_version, corners[4], plane, confidence, stub}
  POST /oneflash {pattern_image, target_corners?, surface_image?}
                         -> 200 {api_version, corners[4], homography, confidence, stub}
  POST /segment {image_path, point[2], model?}
                         -> 200 {api_version, corners[4], outline, confidence,
                                 planarity, stub}
  POST /graycode/patterns {folder, projector[W, H]}
                         -> 200 {api_version, count, stub}
  POST /graycode {folder, projector[W, H]}
                         -> 200 {api_version, map_path, coverage, decode_error_px, stub}
  POST /graymap {map_path, points[[x, y]...]}
                         -> 200 {api_version, points[[x, y]...], valid[bool...]}

A request the client got wrong (bad fields, missing captures, no such map) answers 400.

Every request must carry `X-QM-Token: <token>` (401 otherwise). The app makes a fresh
token for each launch and passes it in the QM_SIDECAR_TOKEN environment variable; run
by hand without it, the server makes one and prints it. A request with an Origin header
(every browser cross-origin request has one) or a Host other than 127.0.0.1:<port> or
localhost:<port> (DNS rebinding) gets 403, and a POST that is not application/json 415
(ADR-035, amendment of 2026-11-14).

The server binds loopback-only (127.0.0.1) by default. The server itself needs
only the standard library; the CV modules are imported when a request needs them,
so /health, the stubs and the contract tests run without torch or a GPU.
"""

from __future__ import annotations

import argparse
import hmac
import importlib.util
import json
import math
import os
import secrets
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Optional

# Bump when the request/response shapes change. The C++ client refuses to attach
# to a sidecar whose api_version it does not understand (graceful version skew).
SIDECAR_API_VERSION = 1
SERVICE_NAME = "quadmapper-ai-sidecar"
# The shared secret: the environment variable the app sets, and the header it sends.
TOKEN_ENV = "QM_SIDECAR_TOKEN"
TOKEN_HEADER = "X-QM-Token"

# Max request body we will read for /surface. A path-only request is tiny; this
# is a sanity bound, not a real image-upload limit (images go by path for now).
MAX_BODY_BYTES = 1 << 20  # 1 MiB

# Placeholder corners every stub answers with: a centered rectangle, confidence 0.
STUB_CORNERS = [[0.25, 0.25], [0.75, 0.25], [0.75, 0.75], [0.25, 0.75]]


def _stub_surface(_request: dict) -> dict:
    """Return a placeholder surface response.

    A centered, axis-aligned rectangle in normalized [0,1] image coordinates with
    zero confidence and stub=True, so callers can prove the round-trip without
    mistaking it for a real detection. Returned whenever real detection cannot run
    (no model on this box, or no readable image_path) — the app degrades cleanly.
    """
    return {
        "api_version": SIDECAR_API_VERSION,
        "corners": [list(c) for c in STUB_CORNERS],
        "plane": {"normal": [0.0, 0.0, 1.0], "d": 0.0},
        "confidence": 0.0,
        "stub": True,
    }


def _real_surface(request: dict) -> Optional[dict]:
    """Run the SAM 2 + Depth Anything surface detector, or None to fall back.

    None (=> stub) when: no readable image_path, a model is absent on this
    machine, the CV stack is not importable, or no candidate region survives.
    """
    image_path = request.get("image_path")
    if not isinstance(image_path, str) or not os.path.isfile(image_path):
        return None

    try:
        import cv2
        import surface
    except Exception:  # noqa: BLE001 - any import failure => graceful stub
        return None
    if not surface.model_available():
        return None

    bgr = cv2.imread(image_path)
    if bgr is None:
        return None

    result = surface.estimate_surface(bgr)
    if result is None:
        return None
    result["api_version"] = SIDECAR_API_VERSION
    return result


def _surface_response(request: dict) -> dict:
    """Real detection when possible, else the stub. Never raises."""
    try:
        real = _real_surface(request)
    except Exception as exc:  # noqa: BLE001 - detection must never 500 the app
        sys.stderr.write(f"[sidecar] surface detection failed: {exc!r}\n")
        real = None
    return real if real is not None else _stub_surface(request)


def _stub_oneflash() -> dict:
    """Placeholder One-Flash response (pattern not detected / deps absent)."""
    return {
        "api_version": SIDECAR_API_VERSION,
        "corners": [list(c) for c in STUB_CORNERS],
        "homography": None,
        "confidence": 0.0,
        "stub": True,
    }


def _real_oneflash(request: dict) -> Optional[dict]:
    """Solve One-Flash (QM-060), or None to fall back to the stub.

    request: {pattern_image, target_corners?(photo-norm 4x2), surface_image?}.
    Without target_corners, the surface is detected (QM-057) on surface_image (or
    pattern_image); if the model is absent, the full frame is the target. Imports
    are deferred so /health and the contract tests stay light.
    """
    pattern_path = request.get("pattern_image") or request.get("image_path")
    if not pattern_path or not os.path.isfile(pattern_path):
        return None

    try:
        import cv2
        import oneflash
    except Exception:  # noqa: BLE001
        return None

    pattern_bgr = cv2.imread(pattern_path)
    if pattern_bgr is None:
        return None

    target = request.get("target_corners")
    if target is None:
        surface_path = request.get("surface_image", pattern_path)
        target = [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]]  # default: full frame
        try:
            import surface
            if surface.model_available() and os.path.isfile(surface_path):
                sbgr = cv2.imread(surface_path)
                found = surface.estimate_surface(sbgr) if sbgr is not None else None
                if found is not None:
                    target = found["corners"]
        except Exception:  # noqa: BLE001 - surface is best-effort; frame fallback
            pass

    result = oneflash.solve_oneflash(pattern_bgr, target)
    if result is None:
        return None
    result["api_version"] = SIDECAR_API_VERSION
    return result


def _oneflash_response(request: dict) -> dict:
    """Real solve when possible, else the stub. Never raises."""
    try:
        real = _real_oneflash(request)
    except Exception as exc:  # noqa: BLE001 - must never 500 the app
        sys.stderr.write(f"[sidecar] oneflash solve failed: {exc!r}\n")
        real = None
    return real if real is not None else _stub_oneflash()


SEGMENT_MODELS = ("small", "tiny")
# Gray-code limits, here so a bad request is refused without importing numpy.
GRAYCODE_MIN_PROJECTOR = 2
GRAYCODE_MAX_PROJECTOR_WIDTH = 7680   # the app's own output limit (8K UHD)
GRAYCODE_MAX_PROJECTOR_HEIGHT = 4320
GRAYMAP_MAX_POINTS = 64


class BadRequest(ValueError):
    """A request the client got wrong: answered with 400, never a crash."""


def _normalized_point(point, what: str) -> tuple:
    """[x, y] finite numbers in 0..1, else BadRequest naming `what`."""
    if (not isinstance(point, (list, tuple)) or len(point) != 2
            or not all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in point)):
        raise BadRequest(f"{what} must be [x, y] numbers")
    try:
        x, y = float(point[0]), float(point[1])  # a huge JSON integer overflows here
    except OverflowError:
        raise BadRequest(f"{what} must be [x, y] numbers") from None
    if not (math.isfinite(x) and math.isfinite(y)):
        raise BadRequest(f"{what} must be [x, y] numbers")
    if not (0.0 <= x <= 1.0 and 0.0 <= y <= 1.0):
        raise BadRequest(f"{what} must be normalized to 0..1")
    return x, y


def _parse_segment(request: dict) -> tuple:
    """Validate a /segment body. Returns (image_path or None, (x, y), model)."""
    x, y = _normalized_point(request.get("point"), "point")
    model = request.get("model", "small")
    if model not in SEGMENT_MODELS:
        raise BadRequest(f"model must be one of {list(SEGMENT_MODELS)}")
    image_path = request.get("image_path")
    if image_path is not None and not isinstance(image_path, str):
        raise BadRequest("image_path must be a string")
    return image_path, (x, y), model


def _stub_segment() -> dict:
    """Placeholder /segment response (no image, no model, or an empty mask)."""
    return {
        "api_version": SIDECAR_API_VERSION,
        "corners": [list(c) for c in STUB_CORNERS],
        "outline": [list(c) for c in STUB_CORNERS],
        "confidence": 0.0,
        "planarity": None,
        "stub": True,
    }


def _segment_response(request: dict) -> dict:
    """Real segmentation when possible, else the stub. Raises only BadRequest."""
    image_path, point, model = _parse_segment(request)
    real = None
    if image_path and os.path.isfile(image_path):
        try:
            import segment
            real = segment.segment(image_path, point, model)
        except Exception as exc:  # noqa: BLE001 - must never 500 the app
            sys.stderr.write(f"[sidecar] segment failed: {exc!r}\n")
            real = None
    if real is None:
        return _stub_segment()
    real["api_version"] = SIDECAR_API_VERSION
    return real


# ---- Gray-code structured light (ADR-053 section 3) ---------------------------------

def _graycode_available() -> bool:
    if importlib.util.find_spec("cv2") is None:
        return False
    try:
        import graycode
    except Exception:  # noqa: BLE001 - numpy missing
        return False
    return graycode.available()


def _parse_scan(request: dict) -> tuple:
    """Validate a /graycode or /graycode/patterns body. Returns (folder, width, height)."""
    folder = request.get("folder")
    if not isinstance(folder, str) or not folder:
        raise BadRequest("folder must be a path")
    if not os.path.isdir(folder):
        raise BadRequest("folder does not exist")
    projector = request.get("projector")
    if (not isinstance(projector, (list, tuple)) or len(projector) != 2
            or not all(isinstance(v, int) and not isinstance(v, bool) for v in projector)):
        raise BadRequest("projector must be [width, height] integers")
    width, height = projector
    if not (GRAYCODE_MIN_PROJECTOR <= width <= GRAYCODE_MAX_PROJECTOR_WIDTH
            and GRAYCODE_MIN_PROJECTOR <= height <= GRAYCODE_MAX_PROJECTOR_HEIGHT):
        raise BadRequest(f"projector size must be {GRAYCODE_MIN_PROJECTOR} to "
                         f"{GRAYCODE_MAX_PROJECTOR_WIDTH} x {GRAYCODE_MAX_PROJECTOR_HEIGHT} pixels")
    return folder, width, height


def _graycode_patterns_response(request: dict) -> dict:
    """Write the pattern PNGs. Raises only BadRequest; a stub without OpenCV contrib."""
    folder, width, height = _parse_scan(request)
    stub = {"api_version": SIDECAR_API_VERSION, "count": 0, "stub": True}
    if not _graycode_available():
        return stub
    try:
        import graycode
        count = graycode.write_patterns(width, height, folder)
    except Exception as exc:  # noqa: BLE001 - must never 500 the app
        sys.stderr.write(f"[sidecar] graycode patterns failed: {exc!r}\n")
        return stub
    return {"api_version": SIDECAR_API_VERSION, "count": count, "stub": False}


def _graycode_response(request: dict) -> dict:
    """Decode the captures into a map. Raises only BadRequest (missing captures too)."""
    folder, width, height = _parse_scan(request)
    stub = {"api_version": SIDECAR_API_VERSION, "map_path": None, "coverage": 0.0,
            "decode_error_px": None, "stub": True}
    if not _graycode_available():
        return stub
    import graycode
    try:
        result = graycode.decode_folder(folder, width, height)
    except (FileNotFoundError, ValueError) as exc:
        raise BadRequest(str(exc)) from None
    except Exception as exc:  # noqa: BLE001 - must never 500 the app
        sys.stderr.write(f"[sidecar] graycode decode failed: {exc!r}\n")
        return stub
    result["api_version"] = SIDECAR_API_VERSION
    result["stub"] = False
    return result


def _graymap_response(request: dict) -> dict:
    """Map photo points through a map. Raises only BadRequest."""
    map_path = request.get("map_path")
    if not isinstance(map_path, str) or not map_path.lower().endswith(".npz"):
        raise BadRequest("map_path must be a .npz path")
    if not os.path.isfile(map_path):
        raise BadRequest("map_path does not exist")
    points = request.get("points")
    if not isinstance(points, list) or not 1 <= len(points) <= GRAYMAP_MAX_POINTS:
        raise BadRequest(f"points must be a list of 1..{GRAYMAP_MAX_POINTS} [x, y]")
    parsed = [_normalized_point(p, "each point") for p in points]
    try:
        import graycode
        mapped, valid = graycode.map_points(graycode.load_map(map_path), parsed)
    except Exception as exc:  # noqa: BLE001 - not a map we wrote, or no numpy
        raise BadRequest(f"map_path is not a Gray-code map: {exc}") from None
    return {"api_version": SIDECAR_API_VERSION, "points": mapped, "valid": valid}


# The POST paths that validate their body (a bad one answers 400), and their handlers.
_CHECKED_ENDPOINTS = {
    "/segment": _segment_response,
    "/graycode/patterns": _graycode_patterns_response,
    "/graycode": _graycode_response,
    "/graymap": _graymap_response,
}


def capabilities() -> list:
    """Features whose packages and model files are present on this machine.

    Checked without loading anything (a model load takes seconds), and a model
    whose load has failed this run is dropped, so the list stays honest after a
    broken checkpoint is first touched.
    """
    caps = []
    try:
        import models
    except Exception:  # noqa: BLE001 - numpy missing: no CV features at all
        models = None
    if models is not None and models.sam2_available() and models.depth_available():
        caps.append("surface")
    if _oneflash_available():
        caps.append("oneflash")
    if models is not None and models.sam2_available():
        caps.append("segment")
    if models is not None and models.depth_available():
        caps.append("depth")
    if _graycode_available():
        caps.append("graycode")
    return caps


def _oneflash_available() -> bool:
    if importlib.util.find_spec("cv2") is None:
        return False
    try:
        import cv2
        import oneflash
    except Exception:  # noqa: BLE001
        return False
    return hasattr(cv2, "aruco") and os.path.isfile(
        os.path.join(oneflash._PATTERN_DIR, "charuco.png"))


class SidecarHandler(BaseHTTPRequestHandler):
    # Quiet by default; the app captures stdout/stderr for diagnostics.
    def log_message(self, fmt: str, *args) -> None:  # noqa: A003 - stdlib signature
        sys.stderr.write("[sidecar] " + (fmt % args) + "\n")

    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _refused(self) -> bool:
        """Answer and return True when the caller may not use the server (see module doc)."""
        if self.headers.get("Origin") is not None:
            self._send_json(403, {"error": "cross-origin requests are refused"})
            return True
        port = self.server.server_address[1]
        if self.headers.get("Host", "") not in (f"127.0.0.1:{port}", f"localhost:{port}"):
            self._send_json(403, {"error": "bad Host"})
            return True
        given = self.headers.get(TOKEN_HEADER, "").encode("utf-8", "replace")
        if not hmac.compare_digest(given, self.server.token.encode("utf-8")):
            self._send_json(401, {"error": f"missing or wrong {TOKEN_HEADER}"})
            return True
        return False

    def do_GET(self) -> None:  # noqa: N802 - stdlib signature
        if self._refused():
            return
        if self.path == "/health":
            self._send_json(200, {
                "status": "ok",
                "api_version": SIDECAR_API_VERSION,
                "service": SERVICE_NAME,
                "capabilities": capabilities(),
            })
            return
        self._send_json(404, {"error": "not found", "path": self.path})

    def do_POST(self) -> None:  # noqa: N802 - stdlib signature
        if self._refused():
            return
        if self.path not in ("/surface", "/oneflash") and self.path not in _CHECKED_ENDPOINTS:
            self._send_json(404, {"error": "not found", "path": self.path})
            return
        media_type = self.headers.get("Content-Type", "").split(";")[0].strip().lower()
        if media_type != "application/json":
            self._send_json(415, {"error": "Content-Type must be application/json"})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send_json(400, {"error": "bad Content-Length"})
            return
        if length < 0 or length > MAX_BODY_BYTES:
            self._send_json(400, {"error": "body too large", "limit": MAX_BODY_BYTES})
            return

        raw = self.rfile.read(length) if length else b"{}"
        try:
            request = json.loads(raw or b"{}")
            if not isinstance(request, dict):
                raise ValueError("body must be a JSON object")
        except (ValueError, json.JSONDecodeError) as exc:
            self._send_json(400, {"error": "invalid JSON", "detail": str(exc)})
            return

        if self.path == "/surface":
            self._send_json(200, _surface_response(request))
        elif self.path == "/oneflash":
            self._send_json(200, _oneflash_response(request))
        else:
            try:
                self._send_json(200, _CHECKED_ENDPOINTS[self.path](request))
            except BadRequest as exc:
                self._send_json(400, {"error": "bad request", "detail": str(exc)})


class SidecarServer(ThreadingHTTPServer):
    """The HTTP server, holding the token every request must carry.

    The port is bound exclusively: with SO_REUSEADDR (the stdlib default for
    HTTPServer) another program on Windows could bind the same port and take requests.
    """

    allow_reuse_address = False

    def __init__(self, address: tuple, token: str) -> None:
        self.token = token
        super().__init__(address, SidecarHandler)

    def server_bind(self) -> None:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):  # Windows only
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def make_server(host: str, port: int, token: str) -> SidecarServer:
    """Create (but do not serve) a sidecar HTTP server bound to host:port.

    Port 0 lets the OS pick a free port; read it back from
    ``server.server_address[1]``. Used by the unit tests for ephemeral servers.
    """
    if not token:
        raise ValueError("the sidecar never runs without a token")
    return SidecarServer((host, port), token)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="QUADmapper AI pre-show sidecar")
    parser.add_argument("--host", default="127.0.0.1",
                        help="bind address (loopback-only by default)")
    parser.add_argument("--port", type=int, default=8765,
                        help="TCP port (default 8765; 0 = OS-assigned)")
    args = parser.parse_args(argv)

    # From the app's environment, never the command line (other programs can read
    # that). Started by hand without one: make one and say it once.
    token = os.environ.get(TOKEN_ENV, "")
    generated = not token
    if generated:
        token = secrets.token_hex(16)

    server = make_server(args.host, args.port, token)
    bound_host, bound_port = server.server_address[:2]
    # Single line on stdout so a launching parent can confirm readiness and the
    # actual port (useful when --port 0 was requested).
    print(json.dumps({
        "event": "listening",
        "host": bound_host,
        "port": bound_port,
        "api_version": SIDECAR_API_VERSION,
    }), flush=True)
    if generated:
        print(f"{TOKEN_ENV} was not set: requests must send the header "
              f"{TOKEN_HEADER}: {token}", flush=True)
    try:
        import models
        models.warm_up_in_background()
    except Exception:  # noqa: BLE001 - numpy missing: no CV features to warm
        pass

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
