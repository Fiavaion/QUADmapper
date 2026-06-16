"""Unit tests for the AI sidecar skeleton (QM-056 AC3 — GPU-free).

Spins up the real stdlib server on an OS-assigned loopback port in a background
thread and exercises the v1 contract over HTTP. No torch/Open3D/GPU required.
"""

import http.client
import json
import threading

import pytest

import sidecar


@pytest.fixture()
def live_server():
    """Yield (host, port) for a running sidecar bound to an ephemeral port."""
    server = sidecar.make_server("127.0.0.1", 0)
    host, port = server.server_address[:2]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield host, port
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def _request(host, port, method, path, body=None):
    conn = http.client.HTTPConnection(host, port, timeout=5)
    try:
        payload = json.dumps(body).encode("utf-8") if body is not None else None
        headers = {"Content-Type": "application/json"} if payload else {}
        conn.request(method, path, body=payload, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        parsed = json.loads(data) if data else None
        return resp.status, parsed
    finally:
        conn.close()


def test_health_reports_version(live_server):
    status, body = _request(*live_server, "GET", "/health")
    assert status == 200
    assert body["status"] == "ok"
    assert body["api_version"] == sidecar.SIDECAR_API_VERSION
    assert body["service"] == sidecar.SERVICE_NAME


def test_surface_round_trips_stub(live_server):
    status, body = _request(*live_server, "POST", "/surface",
                            {"image_path": "C:/tmp/frame.png"})
    assert status == 200
    assert body["api_version"] == sidecar.SIDECAR_API_VERSION
    assert body["stub"] is True
    assert len(body["corners"]) == 4
    assert all(len(c) == 2 for c in body["corners"])
    assert "plane" in body
    assert 0.0 <= body["confidence"] <= 1.0


def test_surface_accepts_empty_body(live_server):
    status, body = _request(*live_server, "POST", "/surface")
    assert status == 200
    assert body["stub"] is True


def test_surface_rejects_invalid_json(live_server):
    conn = http.client.HTTPConnection(*live_server, timeout=5)
    try:
        conn.request("POST", "/surface", body=b"{not json",
                     headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        assert resp.status == 400
    finally:
        conn.close()


def test_unknown_path_is_404(live_server):
    status, _ = _request(*live_server, "GET", "/nope")
    assert status == 404


def test_surface_rejects_get(live_server):
    status, _ = _request(*live_server, "GET", "/surface")
    assert status == 404
