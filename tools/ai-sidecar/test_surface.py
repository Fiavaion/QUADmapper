"""Tests for /surface: candidate scoring (GPU-free) and the real engine on the
three spike scenes (skipped unless SAM 2 and Depth Anything V2 Small are present).

The scene assertions keep the QM-057 intent: the unambiguous screen and the
oblique facade come back with usable confidence, and the gallery, whose biggest
plane is the floor, comes back LOW rather than confidently wrong.
"""

import os

import numpy as np
import pytest

import surface

EVID = os.path.join(os.path.dirname(__file__), "..", "..",
                    "docs", "research", "snap-spike-evidence")
H, W = 240, 320


def _scene():
    """Synthetic disparity: a floor climbing toward the bottom, a wall behind."""
    v, u = np.mgrid[0:H, 0:W]
    disp = np.where(v > H // 2, 1.0 + 2.0 * (v - H // 2) / H, 0.5 + 0.3 * u / W)
    wall = np.zeros((H, W), bool)
    wall[10:H // 2, 20:300] = True
    floor = np.zeros((H, W), bool)
    floor[H // 2 + 5:, :] = True
    return disp, wall, floor


def test_scorer_prefers_the_wall_over_the_floor():
    disp, wall, floor = _scene()
    scored = surface.score_candidates([floor, wall], [0.95, 0.9], disp)
    assert scored[0]["index"] == 1
    assert scored[0]["verticality"] > 0.9
    floor_entry = next(c for c in scored if c["index"] == 0)
    assert floor_entry["verticality"] < 0.3


def test_scorer_ignores_tiny_and_whole_frame_masks():
    disp, _, _ = _scene()
    tiny = np.zeros((H, W), bool)
    tiny[:5, :5] = True
    whole = np.ones((H, W), bool)
    assert surface.score_candidates([tiny, whole], [0.9, 0.9], disp) == []


def _two_regions():
    """Left and right regions; the right one sits on its own, nearer plane."""
    v, u = np.mgrid[0:H, 0:W]
    left = np.zeros((H, W), bool)
    left[20:200, 10:150] = True
    right = np.zeros((H, W), bool)
    right[20:200, 170:310] = True
    coplanar = u / W
    split = np.where(right, 2.0 - u / W, u / W)
    return left, right, coplanar, split


def test_ambiguity_ignores_nested_regions():
    left, _, disp, _ = _two_regions()
    inner = np.zeros((H, W), bool)
    inner[60:160, 40:120] = True
    scored = surface.score_candidates([left, inner], [0.9, 0.9], disp)
    assert surface.ambiguity_gate(scored, [left, inner], disp) == 1.0


def test_ambiguity_ignores_regions_on_the_same_plane():
    left, right, disp, _ = _two_regions()
    scored = surface.score_candidates([left, right], [0.9, 0.9], disp)
    assert surface.ambiguity_gate(scored, [left, right], disp) == 1.0


def test_equal_rival_on_another_plane_collapses_confidence():
    left, right, _, disp = _two_regions()
    scored = surface.score_candidates([left, right], [0.9, 0.9], disp)
    assert surface.ambiguity_gate(scored, [left, right], disp) < 0.2


def test_light_region_beats_dark_rival():
    left, right, _, disp = _two_regions()
    luminance = np.where(right, 0.05, 0.9)  # the right one is a black curtain
    scored = surface.score_candidates([left, right], [0.9, 0.9], disp, luminance)
    assert scored[0]["index"] == 0
    assert surface.ambiguity_gate(scored, [left, right], disp) > 0.5


needs_models = pytest.mark.skipif(
    not surface.model_available(),
    reason="SAM 2.1 small and Depth Anything V2 Small not present (run setup.ps1)")


def _run(name):
    import cv2
    path = os.path.join(EVID, name + ".png")
    bgr = cv2.imread(path)
    assert bgr is not None, f"missing test image {path}"
    return surface.estimate_surface(bgr)


@pytest.fixture(scope="module")
def screen():
    return _run("test_screen")


@needs_models
def test_screen_shape_and_corners(screen):
    assert screen is not None
    assert screen["stub"] is False
    assert screen["plane"] is None
    assert len(screen["corners"]) == 4
    for x, y in screen["corners"]:
        assert 0.0 <= x <= 1.0 and 0.0 <= y <= 1.0


@needs_models
def test_screen_is_confident(screen):
    assert screen["confidence"] >= 0.35, screen["debug"]


@needs_models
def test_gallery_floor_trap_is_low_confidence(screen):
    gallery = _run("test_wall")
    assert gallery is None or gallery["confidence"] < 0.2, gallery["debug"]


@needs_models
def test_facade_is_confident(screen):
    facade = _run("test_facade")
    assert facade is not None
    assert facade["confidence"] >= 0.35, facade["debug"]
