#!/usr/bin/env python3
"""QUADmapper AI pre-show sidecar (QM-056 skeleton).

An optional, out-of-process service the C++ app talks to over loopback HTTP+JSON
(ADR-035). This is the *skeleton*: it proves the integration boundary — launch,
health-check, versioned request/response, teardown — with **nothing intelligent
behind it yet**. The surface/one-flash CV lands in QM-057+.

Contract (v1):
  GET  /health           -> 200 {status, api_version, service}
  POST /surface {image_path|frame}
                         -> 200 {api_version, corners[4], plane, confidence, stub}

The server binds loopback-only (127.0.0.1) by default. It depends on the Python
standard library alone, so the skeleton and its unit tests run GPU-free and
without installing torch/Open3D (those arrive with the real service).
"""

from __future__ import annotations

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Bump when the request/response shapes change. The C++ client refuses to attach
# to a sidecar whose api_version it does not understand (graceful version skew).
SIDECAR_API_VERSION = 1
SERVICE_NAME = "quadmapper-ai-sidecar"

# Max request body we will read for /surface. A path-only request is tiny; this
# is a sanity bound, not a real image-upload limit (images go by path for now).
MAX_BODY_BYTES = 1 << 20  # 1 MiB


def _stub_surface(_request: dict) -> dict:
    """Return a placeholder surface response.

    A centered, axis-aligned rectangle in normalized [0,1] image coordinates with
    zero confidence and stub=True, so callers can prove the round-trip without
    mistaking it for a real detection. QM-057 replaces this with Metric3D + a
    plane fit.
    """
    return {
        "api_version": SIDECAR_API_VERSION,
        "corners": [
            [0.25, 0.25],
            [0.75, 0.25],
            [0.75, 0.75],
            [0.25, 0.75],
        ],
        "plane": {"normal": [0.0, 0.0, 1.0], "d": 0.0},
        "confidence": 0.0,
        "stub": True,
    }


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

    def do_GET(self) -> None:  # noqa: N802 - stdlib signature
        if self.path == "/health":
            self._send_json(200, {
                "status": "ok",
                "api_version": SIDECAR_API_VERSION,
                "service": SERVICE_NAME,
            })
            return
        self._send_json(404, {"error": "not found", "path": self.path})

    def do_POST(self) -> None:  # noqa: N802 - stdlib signature
        if self.path != "/surface":
            self._send_json(404, {"error": "not found", "path": self.path})
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

        self._send_json(200, _stub_surface(request))


def make_server(host: str, port: int) -> ThreadingHTTPServer:
    """Create (but do not serve) a sidecar HTTP server bound to host:port.

    Port 0 lets the OS pick a free port; read it back from
    ``server.server_address[1]``. Used by the unit tests for ephemeral servers.
    """
    return ThreadingHTTPServer((host, port), SidecarHandler)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="QUADmapper AI pre-show sidecar")
    parser.add_argument("--host", default="127.0.0.1",
                        help="bind address (loopback-only by default)")
    parser.add_argument("--port", type=int, default=8765,
                        help="TCP port (default 8765; 0 = OS-assigned)")
    args = parser.parse_args(argv)

    server = make_server(args.host, args.port)
    bound_host, bound_port = server.server_address[:2]
    # Single line on stdout so a launching parent can confirm readiness and the
    # actual port (useful when --port 0 was requested).
    print(json.dumps({
        "event": "listening",
        "host": bound_host,
        "port": bound_port,
        "api_version": SIDECAR_API_VERSION,
    }), flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
