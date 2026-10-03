"""Mask -> corners and the disparity planarity fit on synthetic data (GPU-free)."""

import cv2
import numpy as np
import pytest

import geometry

W, H = 640, 480


def _poly_mask(pts):
    mask = np.zeros((H, W), np.uint8)
    cv2.fillPoly(mask, [np.asarray(pts, np.int32)], 1)
    return mask


def _match(got, expected, tol):
    got = np.asarray(got, np.float64)
    expected = np.asarray(expected, np.float64)
    err = np.linalg.norm(got - expected, axis=1)
    assert err.max() < tol, f"corner error {err} (got {got.tolist()})"


def test_order_corners_handles_45_degree_square():
    diamond = np.array([[100, 50], [150, 100], [100, 150], [50, 100]], np.float64)
    ordered = geometry.order_corners(diamond[[2, 0, 3, 1]])
    assert len({tuple(p) for p in ordered}) == 4
    # Clockwise on screen from the smallest x+y point.
    assert ordered.sum(axis=1).argmin() == 0
    e1, e2 = ordered[1] - ordered[0], ordered[2] - ordered[1]
    cross = e1[0] * e2[1] - e1[1] * e2[0]
    assert cross > 0  # clockwise with y down


def test_rotated_rectangle():
    rect = ((320, 240), (300, 160), 25.0)
    box = cv2.boxPoints(rect)
    corners, outline = geometry.mask_to_quad(_poly_mask(box))
    _match(corners, geometry.order_corners(box), tol=3.0)
    assert 4 <= len(outline) <= geometry.MAX_OUTLINE_POINTS


def test_perspective_trapezoid():
    trap = np.array([[180, 90], [470, 120], [520, 400], [120, 380]], np.float64)
    corners, _ = geometry.mask_to_quad(_poly_mask(trap))
    _match(corners, trap, tol=3.0)


def test_noisy_blob_falls_back_to_a_sane_quad():
    rng = np.random.default_rng(1)
    angles = np.linspace(0, 2 * np.pi, 90, endpoint=False)
    radius = 140 + rng.normal(0, 12, angles.size)
    blob = np.stack([320 + radius * np.cos(angles) * 1.3,
                     240 + radius * np.sin(angles)], axis=1)
    mask = _poly_mask(blob)
    corners, outline = geometry.mask_to_quad(mask)
    assert corners.shape == (4, 2)
    assert len({tuple(np.round(p, 3)) for p in corners}) == 4
    # A convex quad covering most of the blob, not a sliver.
    quad_area = cv2.contourArea(corners.astype(np.float32))
    assert quad_area > 0.6 * mask.sum()
    assert len(outline) <= geometry.MAX_OUTLINE_POINTS


def test_long_outline_is_capped():
    # A wiggly boundary has far more than 256 contour pixels.
    t = np.linspace(0, 2 * np.pi, 2000, endpoint=False)
    r = 180 + 10 * np.sin(40 * t)
    pts = np.stack([320 + r * np.cos(t), 240 + r * np.sin(t)], axis=1)
    _, outline = geometry.mask_to_quad(_poly_mask(pts))
    assert 16 <= len(outline) <= geometry.MAX_OUTLINE_POINTS


def test_empty_mask_returns_none():
    assert geometry.mask_to_quad(np.zeros((H, W), np.uint8)) is None


def test_normalize_points_clips_to_unit_square():
    pts = geometry.normalize_points([[0, 0], [W, H], [W * 2, -5]], W, H)
    assert pts == [[0.0, 0.0], [1.0, 1.0], [1.0, 0.0]]


# --- planarity ---------------------------------------------------------------

def _grid():
    v, u = np.mgrid[0:H, 0:W]
    return u / (W - 1), v / (H - 1)


def _centre_mask():
    mask = np.zeros((H, W), bool)
    mask[60:420, 80:560] = True
    return mask


def test_planar_disparity_scores_high():
    u, v = _grid()
    disp = 0.8 * u + 0.1 * v + 2.0
    fit = geometry.fit_disparity_plane(disp, _centre_mask())
    assert fit["planarity"] > 0.98
    assert fit["a"] == pytest.approx(0.8, abs=1e-6)
    assert fit["b"] == pytest.approx(0.1, abs=1e-6)


def test_planar_disparity_with_noise_still_planar():
    u, v = _grid()
    rng = np.random.default_rng(0)
    disp = 0.8 * u + 0.1 * v + rng.normal(0, 0.01, u.shape)
    assert geometry.fit_disparity_plane(disp, _centre_mask())["planarity"] > 0.85


def test_curved_disparity_scores_low():
    u, v = _grid()
    disp = (u - 0.5) ** 2 + (v - 0.5) ** 2  # a bowl
    planar = geometry.fit_disparity_plane(0.8 * u + 0.1 * v, _centre_mask())
    curved = geometry.fit_disparity_plane(disp, _centre_mask())
    assert curved["planarity"] < 0.5
    assert curved["planarity"] < planar["planarity"] - 0.4


def test_frontal_wall_reads_planar():
    # A frontal wall: near-constant disparity inside, the room varies outside.
    u, v = _grid()
    disp = 1.0 + 2.0 * v
    mask = _centre_mask()
    disp[mask] = 1.5
    fit = geometry.fit_disparity_plane(disp, mask)
    assert fit["planarity"] > 0.98


def test_verticality_wall_vs_floor():
    u, v = _grid()
    mask = _centre_mask()
    scene = np.zeros_like(u)
    scene[0, 0], scene[-1, -1] = 0.0, 1.0  # fix the scene range at ~1
    wall = geometry.fit_disparity_plane(np.where(mask, 0.6 * u, scene), mask)
    frontal = geometry.fit_disparity_plane(np.where(mask, 0.5, u), mask)
    floor = geometry.fit_disparity_plane(np.where(mask, 1.0 * v, scene), mask)
    assert geometry.verticality(wall) > 0.95
    assert geometry.verticality(frontal) > 0.95
    assert geometry.verticality(floor) < 0.2


def test_plane_agreement_same_plane_vs_another():
    u, v = _grid()
    left = np.zeros((H, W), bool)
    left[:, :300] = True
    right = ~left
    one_plane = 0.8 * u + 0.1 * v
    fit = geometry.fit_disparity_plane(one_plane, left)
    assert geometry.plane_agreement(one_plane, right, fit) > 0.98
    two_planes = np.where(right, 2.0 - u, one_plane)
    fit = geometry.fit_disparity_plane(two_planes, left)
    assert geometry.plane_agreement(two_planes, right, fit) < 0.3


def test_tiny_mask_returns_none():
    mask = np.zeros((H, W), bool)
    mask[0, :2] = True
    assert geometry.fit_disparity_plane(np.ones((H, W)), mask) is None
