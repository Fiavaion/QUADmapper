"""Synthetic validation of the One-Flash solve (QM-060) — GPU-free.

We can't project + photograph in this environment (that's the QM-061 hardware
gate), so we simulate it: warp the reference ChArUco by a KNOWN homography H_true
(a projector-on-tilted-surface look) into a photo canvas, then assert the solve
recovers projector corners that match H_true^-1(target) within sub-percent
tolerance. This pins the detection + homography + inverse-mapping math.
"""

import os

import cv2
import numpy as np
import pytest

import oneflash

PATTERN_DIR = os.path.join(os.path.dirname(__file__), "..", "..", "resources", "patterns")
REF_PNG = os.path.join(PATTERN_DIR, "charuco.png")

pytestmark = pytest.mark.skipif(
    not os.path.isfile(REF_PNG), reason="reference pattern not generated")

PHOTO_W, PHOTO_H = 1600, 1200
# A tilted trapezoid in the photo = where the projected board lands on the surface.
DST_QUAD = np.array([[220, 170], [1380, 250], [1320, 1040], [320, 960]], np.float32)


def _make_synthetic_photo():
    """Warp the reference board by a known H_true into a photo. Returns (photo, H_true)."""
    ref = cv2.imread(REF_PNG)
    rh, rw = ref.shape[:2]
    # H_true: projector-norm [0,1] -> photo px.
    unit = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], np.float32)
    H_true = cv2.getPerspectiveTransform(unit, DST_QUAD)
    # Warp the reference *pixels* -> photo: M maps ref_px -> photo_px.
    S = np.array([[1.0 / rw, 0, 0], [0, 1.0 / rh, 0], [0, 0, 1]], np.float64)
    M = H_true.astype(np.float64) @ S
    photo = cv2.warpPerspective(ref, M, (PHOTO_W, PHOTO_H),
                                borderValue=(40, 40, 40))
    return photo, H_true


def test_recovers_known_homography():
    photo, H_true = _make_synthetic_photo()
    # Target surface region in photo-normalized coords (TL,TR,BR,BL).
    target_norm = np.array([[0.30, 0.30], [0.70, 0.28],
                            [0.72, 0.70], [0.28, 0.68]], np.float64)

    result = oneflash.solve_oneflash(photo, target_norm, pattern_dir=os.path.normpath(PATTERN_DIR))
    assert result is not None, "solve returned None (detection failed)"
    assert result["n_corr"] >= oneflash.MIN_CORRESPONDENCES
    assert result["stub"] is False

    # Expected projector-norm corners = H_true^-1 applied to target photo pixels.
    target_px = (target_norm * [PHOTO_W, PHOTO_H]).astype(np.float64)
    Hinv_true = np.linalg.inv(H_true.astype(np.float64))
    expected = cv2.perspectiveTransform(target_px.reshape(1, -1, 2), Hinv_true).reshape(-1, 2)

    got = np.asarray(result["corners"], np.float64)
    err = np.linalg.norm(got - expected, axis=1)
    assert err.max() < 0.02, f"corner error too large: {err} (got {got}, exp {expected})"
    assert result["confidence"] > 0.5


def test_blank_image_returns_none():
    blank = np.full((PHOTO_H, PHOTO_W, 3), 30, np.uint8)
    result = oneflash.solve_oneflash(
        blank, [[0, 0], [1, 0], [1, 1], [0, 1]],
        pattern_dir=os.path.normpath(PATTERN_DIR))
    assert result is None


def test_corners_in_reasonable_range():
    photo, _ = _make_synthetic_photo()
    # Full-frame target -> projector corners that fill the camera view.
    result = oneflash.solve_oneflash(
        photo, [[0, 0], [1, 0], [1, 1], [0, 1]],
        pattern_dir=os.path.normpath(PATTERN_DIR))
    assert result is not None
    got = np.asarray(result["corners"])
    # The full photo frame maps well outside the board, but should stay finite
    # and not wildly diverge (sanity, not tight).
    assert np.all(np.isfinite(got))
    assert got.min() > -2.0 and got.max() < 3.0
