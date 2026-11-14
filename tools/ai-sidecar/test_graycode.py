"""Gray-code structured light (ADR-053 section 3) on synthetic captures — GPU-free.

The patterns are warped into "camera" images once by a known homography and once by
a known non-planar displacement, decoded, and the recovered map is checked against
the known one: within 1 px wherever it decoded. Also the /graycode, /graycode/patterns
and /graymap contract, and their 400s.
"""

import http.client
import json
import os
import threading

import pytest

np = pytest.importorskip("numpy")
cv2 = pytest.importorskip("cv2")
if not hasattr(cv2, "structured_light"):
    pytest.skip("needs opencv-contrib-python (cv2.structured_light)", allow_module_level=True)

import graycode  # noqa: E402
import sidecar  # noqa: E402

PROJ_W, PROJ_H = 320, 200
CAM_W, CAM_H = 480, 360

# Projector pixel -> camera pixel: the projector frame seen at an angle, wholly in view.
H_PROJ_TO_CAM = cv2.getPerspectiveTransform(
    np.float32([[0, 0], [PROJ_W - 1, 0], [PROJ_W - 1, PROJ_H - 1], [0, PROJ_H - 1]]),
    np.float32([[40, 30], [440, 60], [420, 330], [60, 300]]))


def _camera(frame, map_x, map_y, rng):
    """What a camera sees: the frame resampled, with gain, black level and noise."""
    seen = cv2.remap(frame.astype(np.float32), map_x, map_y, cv2.INTER_LINEAR,
                     borderMode=cv2.BORDER_CONSTANT, borderValue=0)
    seen = 20.0 + 0.7 * seen + rng.normal(0.0, 1.5, seen.shape)
    return np.clip(seen, 0, 255).astype(np.uint8)


def _homography_maps():
    """For each camera pixel, the projector position it sees (pixel centres at integers)."""
    u, v = np.meshgrid(np.arange(CAM_W, dtype=np.float32), np.arange(CAM_H, dtype=np.float32))
    pts = np.stack([u.ravel(), v.ravel()], axis=1).reshape(-1, 1, 2)
    src = cv2.perspectiveTransform(pts, np.linalg.inv(H_PROJ_TO_CAM)).reshape(CAM_H, CAM_W, 2)
    return src[..., 0].astype(np.float32), src[..., 1].astype(np.float32)


def _curved_maps():
    """A non-planar surface: an affine view plus a smooth bulge no homography fits."""
    u, v = np.meshgrid(np.arange(CAM_W, dtype=np.float32), np.arange(CAM_H, dtype=np.float32))
    map_x = 0.62 * u - 10.0 + 9.0 * np.sin(v / 37.0) + 5.0 * np.sin(u / 23.0)
    map_y = 0.52 * v + 2.0 + 7.0 * np.cos(u / 41.0)
    return map_x.astype(np.float32), map_y.astype(np.float32)


def _captures(map_x, map_y, seed=1):
    rng = np.random.default_rng(seed)
    return [_camera(f, map_x, map_y, rng) for f in graycode.sequence(PROJ_W, PROJ_H)]


def _check_map(map_x, map_y, min_coverage):
    proj_x, proj_y = graycode.decode_images(_captures(map_x, map_y), PROJ_W, PROJ_H)
    valid = proj_x >= 0
    # Where the camera sees the projector frame well inside its edge, nearly all decode.
    inside = ((map_x > 1) & (map_x < PROJ_W - 2) & (map_y > 1) & (map_y < PROJ_H - 2))
    assert valid[inside].mean() > 0.9
    err = np.hypot(proj_x[valid] - map_x[valid], proj_y[valid] - map_y[valid])
    assert err.max() <= 1.0, f"max error {err.max():.2f} px"
    assert graycode.coverage(proj_x, proj_y, PROJ_W, PROJ_H) >= min_coverage
    noise = graycode.decode_error(proj_x, proj_y)
    assert noise is not None and noise < 1.0
    return proj_x, proj_y


def test_the_count_comes_from_the_generator():
    gen = cv2.structured_light.GrayCodePattern.create(1920, 1080)
    assert graycode.frame_count(1920, 1080) == gen.getNumberOfPatternImages() + 2 == 46
    frames = graycode.sequence(PROJ_W, PROJ_H)
    assert len(frames) == graycode.frame_count(PROJ_W, PROJ_H)
    assert frames[-2].min() == 255 and frames[-1].max() == 0  # white, then black


def test_a_homography_is_recovered_within_a_pixel():
    map_x, map_y = _homography_maps()
    _check_map(map_x, map_y, min_coverage=0.95)


def test_a_non_planar_displacement_is_recovered_within_a_pixel():
    map_x, map_y = _curved_maps()
    _check_map(map_x, map_y, min_coverage=0.8)


def test_the_decode_agrees_with_opencv_get_proj_pixel():
    map_x, map_y = _homography_maps()
    caps = _captures(map_x, map_y)
    proj_x, proj_y = graycode.decode_images(caps, PROJ_W, PROJ_H)
    gen = cv2.structured_light.GrayCodePattern.create(PROJ_W, PROJ_H)
    n = gen.getNumberOfPatternImages()
    white, black = caps[n].astype(int), caps[n + 1].astype(int)
    rng = np.random.default_rng(7)
    checked = 0
    for _ in range(400):
        x, y = int(rng.integers(0, CAM_W)), int(rng.integers(0, CAM_H))
        if white[y, x] - black[y, x] <= graycode.BLACK_THRESHOLD:
            assert proj_x[y, x] == -1  # shadowed: never decoded
            continue
        error, (px, py) = gen.getProjPixel(caps[:n], x, y)
        in_range = 0 <= px < PROJ_W and 0 <= py < PROJ_H
        if not error and in_range:
            assert (proj_x[y, x], proj_y[y, x]) == (px, py)
            checked += 1
        else:
            assert proj_x[y, x] == -1
    assert checked > 100


def test_points_map_through_a_written_map(tmp_path):
    map_x, map_y = _curved_maps()
    for k, img in enumerate(_captures(map_x, map_y)):
        cv2.imwrite(str(tmp_path / graycode.CAPTURE_NAME.format(k)), img)
    result = graycode.decode_folder(str(tmp_path), PROJ_W, PROJ_H)
    assert os.path.isfile(result["map_path"])
    gray_map = graycode.load_map(result["map_path"])

    cam = [(240, 180), (100, 90), (400, 300)]
    points = [[(x + 0.5) / CAM_W, (y + 0.5) / CAM_H] for x, y in cam]
    mapped, valid = graycode.map_points(gray_map, points + [[0.001, 0.001]])
    assert valid == [True, True, True, False]  # the top-left corner sees no projector
    for (x, y), (px, py) in zip(cam, mapped):
        assert abs(px * PROJ_W - (map_x[y, x] + 0.5)) <= 1.5
        assert abs(py * PROJ_H - (map_y[y, x] + 0.5)) <= 1.5


# ---- HTTP contract -------------------------------------------------------------------

AUTH = {sidecar.TOKEN_HEADER: "graycode-test-token"}


@pytest.fixture()
def live_server():
    server = sidecar.make_server("127.0.0.1", 0, AUTH[sidecar.TOKEN_HEADER])
    host, port = server.server_address[:2]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield host, port
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


def _post(server, path, body):
    conn = http.client.HTTPConnection(*server, timeout=30)
    try:
        conn.request("POST", path, body=json.dumps(body).encode("utf-8"),
                     headers={"Content-Type": "application/json", **AUTH})
        resp = conn.getresponse()
        return resp.status, json.loads(resp.read() or b"null")
    finally:
        conn.close()


def test_health_advertises_graycode(live_server):
    conn = http.client.HTTPConnection(*live_server, timeout=5)
    conn.request("GET", "/health", headers=AUTH)
    body = json.loads(conn.getresponse().read())
    conn.close()
    assert "graycode" in body["capabilities"]


def test_scan_round_trip_over_http(live_server, tmp_path):
    folder = str(tmp_path)
    status, body = _post(live_server, "/graycode/patterns",
                         {"folder": folder, "projector": [PROJ_W, PROJ_H]})
    assert status == 200 and body["stub"] is False
    assert body["api_version"] == sidecar.SIDECAR_API_VERSION
    count = body["count"]
    assert count == graycode.frame_count(PROJ_W, PROJ_H)

    # The app's side: show each written pattern, photograph it.
    map_x, map_y = _homography_maps()
    rng = np.random.default_rng(3)
    for k in range(count):
        shown = cv2.imread(os.path.join(folder, graycode.PATTERN_NAME.format(k)))
        assert shown is not None and shown.shape == (PROJ_H, PROJ_W, 3)
        seen = _camera(cv2.cvtColor(shown, cv2.COLOR_BGR2GRAY), map_x, map_y, rng)
        cv2.imwrite(os.path.join(folder, graycode.CAPTURE_NAME.format(k)),
                    cv2.cvtColor(seen, cv2.COLOR_GRAY2BGR))

    status, body = _post(live_server, "/graycode", {"folder": folder, "projector": [PROJ_W, PROJ_H]})
    assert status == 200 and body["stub"] is False
    assert set(body) == {"api_version", "map_path", "coverage", "decode_error_px", "stub"}
    assert body["coverage"] > 0.95 and 0.0 <= body["decode_error_px"] < 1.0

    # The projector's centre, seen through the homography, maps back to the centre.
    centre = cv2.perspectiveTransform(
        np.float32([[[(PROJ_W - 1) / 2, (PROJ_H - 1) / 2]]]), H_PROJ_TO_CAM)[0, 0]
    photo = [float((centre[0] + 0.5) / CAM_W), float((centre[1] + 0.5) / CAM_H)]
    status, body = _post(live_server, "/graymap", {"map_path": body["map_path"], "points": [photo]})
    assert status == 200
    assert body["valid"] == [True]
    assert abs(body["points"][0][0] - 0.5) * PROJ_W <= 1.5
    assert abs(body["points"][0][1] - 0.5) * PROJ_H <= 1.5


def test_missing_captures_are_a_400(live_server, tmp_path):
    status, body = _post(live_server, "/graycode", {"folder": str(tmp_path), "projector": [64, 32]})
    assert status == 400
    assert "capture_000.png" in body["detail"]


@pytest.mark.parametrize("path", ["/graycode", "/graycode/patterns"])
@pytest.mark.parametrize("body", [
    {},
    {"projector": [64, 32]},
    {"folder": 7, "projector": [64, 32]},
    {"folder": "Z:/no/such/folder/qm", "projector": [64, 32]},
    {"folder": ".", "projector": [64]},
    {"folder": ".", "projector": [64.0, 32]},
    {"folder": ".", "projector": [True, 32]},
    {"folder": ".", "projector": [1, 32]},
    {"folder": ".", "projector": [64, 100000]},
    {"folder": ".", "projector": [7681, 32]},
])
def test_bad_scan_requests_are_400(live_server, path, body):
    status, _ = _post(live_server, path, body)
    assert status == 400


@pytest.mark.parametrize("body", [
    {},
    {"map_path": "map.txt", "points": [[0.5, 0.5]]},
    {"map_path": "Z:/no/such/graymap.npz", "points": [[0.5, 0.5]]},
])
def test_bad_map_paths_are_400(live_server, body):
    status, _ = _post(live_server, "/graymap", body)
    assert status == 400


@pytest.mark.parametrize("points", [None, [], [[0.5]], [[0.5, 2.0]], [["a", 0.5]],
                                    [[0.5, 0.5]] * (sidecar.GRAYMAP_MAX_POINTS + 1)])
def test_bad_map_points_are_400(live_server, tmp_path, points):
    path = tmp_path / "graymap.npz"
    np.savez(path, proj_x=np.zeros((4, 4), np.int16), proj_y=np.zeros((4, 4), np.int16),
             camera_size=np.array([4, 4]), projector_size=np.array([8, 8]))
    status, _ = _post(live_server, "/graymap", {"map_path": str(path), "points": points})
    assert status == 400


def test_a_file_that_is_not_a_map_is_400(live_server, tmp_path):
    path = tmp_path / "graymap.npz"
    path.write_bytes(b"not a zip")
    status, _ = _post(live_server, "/graymap", {"map_path": str(path), "points": [[0.5, 0.5]]})
    assert status == 400
