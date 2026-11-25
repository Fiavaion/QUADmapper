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


TOKEN = "0123456789abcdef0123456789abcdef"
AUTH = {sidecar.TOKEN_HEADER: TOKEN}


@pytest.fixture()
def live_server():
    """Yield (host, port) for a running sidecar bound to an ephemeral port."""
    server = sidecar.make_server("127.0.0.1", 0, TOKEN)
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


def _request(host, port, method, path, body=None, headers=None):
    """Ask as the app does: the token, and JSON on a POST. `headers` adds to that, and a
    None value leaves that header out."""
    conn = http.client.HTTPConnection(host, port, timeout=5)
    try:
        payload = json.dumps(body).encode("utf-8") if body is not None else None
        sent = dict(AUTH)
        if method == "POST":
            sent["Content-Type"] = "application/json"
        sent.update(headers or {})
        skip_host = "Host" in sent
        conn.putrequest(method, path, skip_host=skip_host)
        for name, value in sent.items():
            if value is not None:
                conn.putheader(name, value)
        conn.putheader("Content-Length", str(len(payload or b"")))
        conn.endheaders(payload)
        resp = conn.getresponse()
        data = resp.read()
        parsed = json.loads(data) if data else None
        return resp.status, parsed
    finally:
        conn.close()


def test_the_hugging_face_libraries_are_offline():
    assert sidecar.os.environ["HF_HUB_OFFLINE"] == "1"


def test_health_reports_version(live_server):
    status, body = _request(*live_server, "GET", "/health")
    assert status == 200
    assert body["status"] == "ok"
    assert body["api_version"] == sidecar.SIDECAR_API_VERSION
    assert body["service"] == sidecar.SERVICE_NAME
    assert isinstance(body["capabilities"], list)
    assert set(body["capabilities"]) <= {"surface", "oneflash", "segment", "depth", "graycode"}


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
                     headers={"Content-Type": "application/json", **AUTH})
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
    {"image_path": "x.png", "point": [10 ** 400, 0.5]},  # overflows float(): still a 400
])
def test_segment_bad_request_is_400(live_server, body):
    status, reply = _request(*live_server, "POST", "/segment", body)
    assert status == 400
    assert "error" in reply


def test_json_nested_too_deeply_is_400(live_server):
    conn = http.client.HTTPConnection(*live_server, timeout=5)
    try:
        conn.request("POST", "/segment", body=b"[" * 100000,
                     headers={"Content-Type": "application/json", **AUTH})
        assert conn.getresponse().status == 400
    finally:
        conn.close()


@pytest.mark.parametrize("path,local", [
    ("C:\\shots\\a.png", True),
    ("d:/shots/a.png", True),
    ("a.png", False),
    ("C:a.png", False),
    ("\\shots\\a.png", False),
    ("\\\\server\\share\\a.png", False),
    ("//server/share/a.png", False),
    ("\\\\.\\PhysicalDrive0", False),
    ("\\\\?\\C:\\shots\\a.png", False),
    ("C:\\shots\\a\x00.png", False),
    ("", False),
    (None, False),
    (7, False),
])
def test_only_a_path_on_a_local_drive_is_used(path, local):
    assert sidecar._is_local_path(path) is local


def test_a_folder_on_a_network_share_is_400(live_server):
    status, _ = _request(*live_server, "POST", "/graycode/patterns",
                         {"folder": "\\\\server\\share\\scan", "projector": [64, 32]})
    assert status == 400


def test_segment_invalid_json_is_400(live_server):
    conn = http.client.HTTPConnection(*live_server, timeout=5)
    try:
        conn.request("POST", "/segment", body=b"[1, 2]",
                     headers={"Content-Type": "application/json", **AUTH})
        assert conn.getresponse().status == 400
    finally:
        conn.close()


# ---- Who may call (ADR-035, amendment of 2026-11-14) -------------------------------

@pytest.mark.parametrize("method,path", [("GET", "/health"), ("POST", "/surface"),
                                         ("POST", "/graycode/patterns"), ("GET", "/nope")])
def test_a_request_without_the_token_is_401(live_server, method, path):
    status, body = _request(*live_server, method, path, {} if method == "POST" else None,
                            headers={sidecar.TOKEN_HEADER: None})
    assert status == 401
    assert "error" in body


@pytest.mark.parametrize("token", ["", "wrong", TOKEN[:-1], TOKEN + "0", TOKEN.upper()])
def test_a_wrong_token_is_401(live_server, token):
    status, _ = _request(*live_server, "GET", "/health", headers={sidecar.TOKEN_HEADER: token})
    assert status == 401


def test_a_request_with_an_origin_is_403_even_with_the_token(live_server):
    # A browser adds Origin to every cross-origin POST, no-cors text/plain ones too.
    for origin in ("https://evil.example", "null", "http://127.0.0.1"):
        status, _ = _request(*live_server, "POST", "/graycode/patterns",
                             {"folder": ".", "projector": [64, 32]}, headers={"Origin": origin})
        assert status == 403


@pytest.mark.parametrize("host", ["evil.example", "evil.example:{port}", "127.0.0.1",
                                  "127.0.0.1:1", "localhost", "[::1]:{port}", "0.0.0.0:{port}"])
def test_a_foreign_host_is_403(live_server, host):
    # DNS rebinding: the page's own name is in Host.
    status, _ = _request(*live_server, "GET", "/health",
                         headers={"Host": host.format(port=live_server[1])})
    assert status == 403


@pytest.mark.parametrize("host", ["127.0.0.1:{port}", "localhost:{port}"])
def test_loopback_hosts_are_accepted(live_server, host):
    status, _ = _request(*live_server, "GET", "/health",
                         headers={"Host": host.format(port=live_server[1])})
    assert status == 200


@pytest.mark.parametrize("content_type", [None, "text/plain", "application/x-www-form-urlencoded",
                                          "multipart/form-data; boundary=x"])
def test_a_post_that_is_not_json_is_415(live_server, content_type):
    status, _ = _request(*live_server, "POST", "/surface", {"image_path": "x.png"},
                         headers={"Content-Type": content_type})
    assert status == 415


def test_json_with_a_charset_is_accepted(live_server):
    status, _ = _request(*live_server, "POST", "/surface", {"image_path": "x.png"},
                         headers={"Content-Type": "application/json; charset=utf-8"})
    assert status == 200


@pytest.mark.parametrize("projector", [[7681, 4320], [7680, 4321], [8192, 8192]])
def test_a_projector_beyond_the_apps_output_limit_is_400(live_server, tmp_path, projector):
    status, body = _request(*live_server, "POST", "/graycode/patterns",
                            {"folder": str(tmp_path), "projector": projector})
    assert status == 400
    assert "7680 x 4320" in body["detail"]
    assert not list(tmp_path.iterdir())


def test_the_server_never_runs_without_a_token():
    with pytest.raises(ValueError):
        sidecar.make_server("127.0.0.1", 0, "")
