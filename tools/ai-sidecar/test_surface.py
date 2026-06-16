"""Tests for the real Metric3D surface service (QM-057).

Skipped wholesale when the Metric3D model is absent (CI / machines without the
local ComfyUI install), so the rest of the suite stays GPU-free. When the model
is present these assert the AC2 behaviour on the three spike scenes — the
unambiguous screen detects with usable confidence, the cluttered gallery
(floor-trap) returns LOW confidence rather than confidently wrong — and the AC3
latency budget (<3s on GPU, steady state).
"""

import os

import cv2
import pytest

import surface

EVID = os.path.join(os.path.dirname(__file__), "..", "..",
                    "docs", "research", "snap-spike-evidence")

pytestmark = pytest.mark.skipif(
    not surface.model_available(),
    reason="Metric3D model not present on this machine",
)


def _run(name):
    path = os.path.join(EVID, name + ".png")
    bgr = cv2.imread(path)
    assert bgr is not None, f"missing test image {path}"
    return surface.estimate_surface(bgr, fx=1000, fy=1000)


@pytest.fixture(scope="module")
def screen():
    return _run("test_screen")


def test_screen_shape_and_corners(screen):
    assert screen["stub"] is False
    assert len(screen["corners"]) == 4
    for x, y in screen["corners"]:
        assert -0.05 <= x <= 1.05
        assert -0.05 <= y <= 1.05
    assert "normal" in screen["plane"]


def test_screen_is_confident(screen):
    # Unambiguous vertical surface -> usable confidence (propose-then-confirm).
    assert screen["confidence"] >= 0.35


def test_screen_latency_under_budget(screen):
    # Steady state (model already loaded by the fixture's first call).
    again = _run("test_screen")
    assert again["timing_ms"]["total"] < 3000.0


def test_gallery_floor_trap_is_low_confidence(screen):
    # The gallery's biggest plane is the floor; verticality gate must collapse
    # confidence so we never confidently warp onto the wrong surface.
    gallery = _run("test_wall")
    assert gallery["confidence"] < 0.2


def test_facade_metric_depth_no_collapse(screen):
    # Oblique facade: metric depth should keep it a confident vertical surface.
    facade = _run("test_facade")
    assert facade["confidence"] >= 0.35
    assert facade["stub"] is False
