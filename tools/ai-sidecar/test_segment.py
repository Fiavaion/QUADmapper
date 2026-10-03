"""Real-model tests for /segment (SAM 2.1 + Depth Anything V2 Small).

Skipped unless the models from setup.ps1 are present. The theatre spike photo
has a known screen: a click on it must come back as that screen's quad, planar
and confident. Latency is printed (run with -s to see it).
"""

import glob
import os
import time

import numpy as np
import pytest

import models
import segment

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
SCREEN = os.path.join(ROOT, "docs", "research", "snap-spike-evidence", "test_screen.png")
# The screen in test_screen.png, measured by hand (normalized TL, TR, BR, BL).
SCREEN_QUAD = [[0.08, 0.23], [0.54, 0.37], [0.54, 0.78], [0.07, 0.86]]
CLICK = [0.3, 0.5]

pytestmark = pytest.mark.skipif(
    not (models.sam2_available("small") and models.depth_available()),
    reason="SAM 2.1 small and Depth Anything V2 Small not present (run setup.ps1)")


def _timed(path, point, model="small"):
    t0 = time.perf_counter()
    result = segment.segment(path, point, model)
    return result, (time.perf_counter() - t0) * 1000.0


def test_click_on_screen_returns_the_screen():
    result, ms = _timed(SCREEN, CLICK)
    assert result is not None and result["stub"] is False
    got = np.asarray(result["corners"])
    err = np.linalg.norm(got - np.asarray(SCREEN_QUAD), axis=1)
    assert err.max() < 0.06, f"corners {got.tolist()}"
    assert result["planarity"] > 0.8
    assert result["confidence"] > 0.6
    assert 4 <= len(result["outline"]) <= 256
    print(f"\n/segment small, first call (load + embed): {ms:.0f} ms on {models.device()}")


def test_second_click_reuses_the_embedding():
    segment.segment(SCREEN, CLICK)
    _, ms = _timed(SCREEN, [0.32, 0.55])
    print(f"\n/segment small, same image again: {ms:.0f} ms on {models.device()}")
    assert ms < 2000.0


@pytest.mark.skipif(not models.sam2_available("tiny"), reason="SAM 2.1 tiny absent")
def test_tiny_model_also_finds_the_screen():
    result, ms = _timed(SCREEN, CLICK, "tiny")
    assert result is not None and result["stub"] is False
    got = np.asarray(result["corners"])
    assert np.linalg.norm(got - np.asarray(SCREEN_QUAD), axis=1).max() < 0.08
    print(f"\n/segment tiny, first call: {ms:.0f} ms on {models.device()}")


def test_smoke_on_a_test_media_image():
    images = sorted(glob.glob("D:/testing Videos/Media/Images/*.jpeg"))
    if not images:
        pytest.skip("no test media on this machine")
    result, ms = _timed(images[0], [0.5, 0.5])
    assert result is not None
    assert len(result["corners"]) == 4
    assert 0.0 <= result["confidence"] <= 1.0
    print(f"\n/segment small, new image {os.path.basename(images[0])}: "
          f"{ms:.0f} ms on {models.device()}")
