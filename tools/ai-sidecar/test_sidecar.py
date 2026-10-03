"""Contract tests for the AI sidecar (QM-056 AC3, ADR-053) — GPU-free.

Spins up the real stdlib server on an OS-assigned loopback port in a background
thread and exercises the v1 contract over HTTP. No torch or GPU required: the
/segment stub path is forced by pointing QM_MODELS_DIR at an empty folder.
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


@pytest.fixture()
def no_models(tmp_path, monkeypatch):
    """Point the model folder at an empty directory: every model is absent."""
    monkeypatch.setenv("QM_MODELS_DIR", str(tmp_path))
    return tmp_path


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
    assert isinstance(body["capabilities"], list)
    assert set(body["capabilities"]) <= {"surface", "oneflash", "segment", "depth"}


def test_health_drops_model_capabilities_without_models(live_server, no_models):
    _, body = _request(*live_server, "GET", "/health")
    assert not {"surface", "segment", "depth"} & set(body["capabilities"])


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


def _assert_segment_shape(body):
    assert body["api_version"] == sidecar.SIDECAR_API_VERSION
    assert len(body["corners"]) == 4
    for x, y in body["corners"] + body["outline"]:
        assert 0.0 <= x <= 1.0 and 0.0 <= y <= 1.0
    assert 3 <= len(body["outline"]) <= 256
    assert 0.0 <= body["confidence"] <= 1.0
    assert body["planarity"] is None or 0.0 <= body["planarity"] <= 1.0
    assert isinstance(body["stub"], bool)


def test_segment_stub_without_image(live_server):
    status, body = _request(*live_server, "POST", "/segment",
                            {"image_path": "C:/no/such/photo.png", "point": [0.5, 0.5]})
    assert status == 200
    _assert_segment_shape(body)
    assert body["stub"] is True
    assert body["confidence"] == 0.0
    assert body["corners"] == [[0.25, 0.25], [0.75, 0.25], [0.75, 0.75], [0.25, 0.75]]
    assert body["planarity"] is None


def test_segment_stub_without_models(live_server, no_models, tmp_path):
    photo = tmp_path / "photo.png"
    photo.write_bytes(b"not decoded: the model check comes first")
    status, body = _request(*live_server, "POST", "/segment",
                            {"image_path": str(photo), "point": [0.2, 0.8], "model": "tiny"})
    assert status == 200
    _assert_segment_shape(body)
    assert body["stub"] is True


def test_segment_stub_without_image_path(live_server):
    status, body = _request(*live_server, "POST", "/segment", {"point": [0.5, 0.5]})
    assert status == 200
    assert body["stub"] is True


@pytest.mark.parametrize("body", [
    {},
    {"image_path": "x.png"},
    {"image_path": "x.png", "point": [0.5]},
    {"image_path": "x.png", "point": ["a", 0.5]},
    {"image_path": "x.png", "point": [True, 0.5]},
    {"image_path": "x.png", "point": [1.5, 0.5]},
    {"image_path": "x.png", "point": [0.5, -0.1]},
    {"image_path": "x.png", "point": [0.5, 0.5], "model": "large"},
    {"image_path": 42, "point": [0.5, 0.5]},
])
def test_segment_bad_request_is_400(live_server, body):
    status, reply = _request(*live_server, "POST", "/segment", body)
    assert status == 400
    assert "error" in reply


def test_segment_invalid_json_is_400(live_server):
    conn = http.client.HTTPConnection(*live_server, timeout=5)
    try:
        conn.request("POST", "/segment", body=b"[1, 2]",
                     headers={"Content-Type": "application/json"})
        assert conn.getresponse().status == 400
    finally:
        conn.close()
