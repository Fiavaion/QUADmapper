"""One-Flash homography solve (QM-060).

Recovers the projector->camera mapping from a single photo of the projected
ChArUco pattern (QM-059), then maps a target surface region back through it to
the projector corners that land content on that surface — no projector pose, one
photo (the spike's One-Flash insight).

Pipeline:
  detect ChArUco in the reference charuco.png  -> {id: projector_px}
  detect ChArUco in the photo (pattern on surface) -> {id: photo_px}
  match by id -> H: projector-norm [0,1] -> photo px   (cv2.findHomography)
  target surface corners (photo-norm) -> H^-1 -> projector-norm quad corners

Only cv2.aruco is needed (no torch), so the core solve + its test are GPU-free.
Surface targets can come from the QM-057 service or be passed in directly.
"""

from __future__ import annotations

import json
import os
from typing import Optional

import cv2
import numpy as np

_HERE = os.path.dirname(__file__)
_PATTERN_DIR = os.path.normpath(os.path.join(_HERE, "..", "..", "resources", "patterns"))

_ref_cache: dict = {}


def _load_board(spec: dict):
    dictionary = cv2.aruco.getPredefinedDictionary(getattr(cv2.aruco, spec["dictionary"]))
    board = cv2.aruco.CharucoBoard(
        (spec["squares_x"], spec["squares_y"]),
        spec["square_length"], spec["marker_length"], dictionary)
    return board


def _detect(bgr, board) -> dict:
    """Return {charuco_id: (x, y) pixel} for the board detected in bgr."""
    detector = cv2.aruco.CharucoDetector(board)
    corners, ids, _, _ = detector.detectBoard(bgr)
    out = {}
    if ids is not None and corners is not None:
        for cid, pt in zip(ids.flatten(), corners.reshape(-1, 2)):
            out[int(cid)] = (float(pt[0]), float(pt[1]))
    return out


def _reference(pattern_dir: str = _PATTERN_DIR):
    """Detected reference board (cached): {id: projector_norm [0,1]} + size."""
    if "ids" in _ref_cache:
        return _ref_cache
    spec = json.load(open(os.path.join(pattern_dir, "charuco.json")))
    ref_img = cv2.imread(os.path.join(pattern_dir, "charuco.png"))
    if ref_img is None:
        raise RuntimeError("reference charuco.png not found")
    board = _load_board(spec)
    h, w = ref_img.shape[:2]
    detected = _detect(ref_img, board)
    _ref_cache.update(
        board=board,
        size=(w, h),
        # projector-normalized [0,1] (the board is shown via an identity quad).
        ids={cid: (x / w, y / h) for cid, (x, y) in detected.items()},
    )
    return _ref_cache


# Minimum id-matched corners for a trustworthy homography.
MIN_CORRESPONDENCES = 6


def solve_oneflash(pattern_bgr: np.ndarray,
                   target_corners_norm,
                   pattern_dir: str = _PATTERN_DIR) -> Optional[dict]:
    """Solve projector->surface and return projector-norm quad corners.

    pattern_bgr: photo of the projected ChArUco (on the surface).
    target_corners_norm: 4x2 surface corners in photo-normalized [0,1] (TL,TR,BR,BL)
        — typically from the QM-057 surface service on the same viewpoint.
    Returns {corners[4] projector-norm, homography 3x3, confidence, n_corr} or
    None if too few correspondences were found.
    """
    ref = _reference(pattern_dir)
    photo = _detect(pattern_bgr, ref["board"])

    proj_norm, photo_px = [], []
    for cid, ppx in photo.items():
        if cid in ref["ids"]:
            proj_norm.append(ref["ids"][cid])
            photo_px.append(ppx)
    if len(proj_norm) < MIN_CORRESPONDENCES:
        return None

    proj_norm = np.asarray(proj_norm, dtype=np.float64)
    photo_px = np.asarray(photo_px, dtype=np.float64)

    # H maps projector-norm [0,1] -> photo pixels.
    H, mask = cv2.findHomography(proj_norm, photo_px, cv2.RANSAC, 3.0)
    if H is None:
        return None
    inliers = int(mask.sum()) if mask is not None else len(proj_norm)

    ph, pw = pattern_bgr.shape[:2]
    target_px = np.asarray(target_corners_norm, dtype=np.float64) * [pw, ph]

    # Map the surface region back to projector-norm via H^-1.
    Hinv = np.linalg.inv(H)
    proj_corners = cv2.perspectiveTransform(
        target_px.reshape(1, -1, 2), Hinv).reshape(-1, 2)

    # Reprojection error of inliers (projector-norm -> photo px), as a confidence.
    reproj = cv2.perspectiveTransform(proj_norm.reshape(1, -1, 2), H).reshape(-1, 2)
    err = np.linalg.norm(reproj - photo_px, axis=1)
    rms_px = float(np.sqrt((err ** 2).mean()))
    diag = float(np.hypot(pw, ph))
    confidence = float(np.clip(
        (inliers / max(len(proj_norm), 1)) * (1.0 - min(1.0, rms_px / (0.01 * diag))),
        0.0, 1.0))

    return {
        "corners": [[float(x), float(y)] for x, y in proj_corners],
        "homography": [[float(v) for v in row] for row in H],
        "confidence": confidence,
        "n_corr": len(proj_norm),
        "inliers": inliers,
        "rms_px": rms_px,
        "stub": False,
    }
