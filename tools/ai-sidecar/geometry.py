"""Mask and disparity geometry for the AI sidecar (ADR-053 section 2).

Pure numpy + OpenCV, no torch, so everything here is testable without a GPU or a
model: a binary mask becomes four quad corners and an outline, and a relative
disparity map over a mask becomes a planarity score and a tilt estimate.
"""

from __future__ import annotations

from typing import Optional

import cv2
import numpy as np

MAX_OUTLINE_POINTS = 256


def order_corners(pts: np.ndarray) -> np.ndarray:
    """Order 4 points as TL, TR, BR, BL (image space, y down).

    Sorted by angle around the centroid, then rotated so the point with the
    smallest x+y comes first. Unlike the sum/diff trick this never picks the same
    point twice, even for a 45-degree rotated square.
    """
    pts = np.asarray(pts, dtype=np.float64).reshape(4, 2)
    centre = pts.mean(axis=0)
    # atan2 with y down: increasing angle runs clockwise on screen, i.e. TL->TR->BR->BL.
    angles = np.arctan2(pts[:, 1] - centre[1], pts[:, 0] - centre[0])
    pts = pts[np.argsort(angles)]
    start = int(np.argmin(pts.sum(axis=1)))
    return np.roll(pts, -start, axis=0)


def _quad_from_contour(contour: np.ndarray) -> np.ndarray:
    """Four corners for a contour: approxPolyDP toward 4 points, else minAreaRect.

    The convex hull is simplified with a growing tolerance until it has at most
    four vertices. Exactly four is a perspective quad and wins; anything else (a
    triangle, or a blob that never reduces cleanly) falls back to the rotated
    minimum-area rectangle.
    """
    hull = cv2.convexHull(contour)
    peri = cv2.arcLength(hull, True)
    for frac in (0.01, 0.02, 0.03, 0.04, 0.05, 0.065, 0.08, 0.1):
        approx = cv2.approxPolyDP(hull, frac * peri, True)
        if len(approx) <= 4:
            if len(approx) == 4:
                return approx.reshape(4, 2).astype(np.float64)
            break
    return cv2.boxPoints(cv2.minAreaRect(contour)).astype(np.float64)


def _outline_from_contour(contour: np.ndarray,
                          max_points: int = MAX_OUTLINE_POINTS) -> np.ndarray:
    """The contour as at most `max_points` vertices, shape kept as well as possible."""
    pts = contour.reshape(-1, 2)
    if len(pts) <= max_points:
        return pts.astype(np.float64)
    peri = cv2.arcLength(contour, True)
    eps = 0.0005 * peri
    approx = cv2.approxPolyDP(contour, eps, True)
    while len(approx) > max_points:
        eps *= 1.5
        approx = cv2.approxPolyDP(contour, eps, True)
    return approx.reshape(-1, 2).astype(np.float64)


def largest_contour(mask: np.ndarray) -> Optional[np.ndarray]:
    """The largest external contour of a binary mask, or None if it is empty."""
    m = (np.asarray(mask) > 0).astype(np.uint8)
    if not m.any():
        return None
    contours, _ = cv2.findContours(m, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    if not contours:
        return None
    cnt = max(contours, key=cv2.contourArea)
    if cv2.contourArea(cnt) <= 0.0:
        return None
    return cnt


def mask_to_quad(mask: np.ndarray):
    """Corners and outline of a mask's largest contour, both in pixels.

    Returns (corners 4x2 ordered TL,TR,BR,BL, outline Nx2 with N <= 256) or
    None when the mask has no usable region.
    """
    cnt = largest_contour(mask)
    if cnt is None:
        return None
    return order_corners(_quad_from_contour(cnt)), _outline_from_contour(cnt)


def normalize_points(pts_px: np.ndarray, width: int, height: int) -> list:
    """Pixel points -> normalized [x, y] lists in [0, 1]."""
    pts = np.asarray(pts_px, dtype=np.float64).reshape(-1, 2)
    scale = np.array([width, height], dtype=np.float64)
    pts = np.clip(pts / scale, 0.0, 1.0)
    return [[float(x), float(y)] for x, y in pts]


def disparity_range(disparity: np.ndarray) -> float:
    """Robust spread of a disparity map (2nd to 98th percentile)."""
    lo, hi = np.percentile(disparity, [2.0, 98.0])
    return float(hi - lo)


# Planarity = 1 - PLANARITY_GAIN * rms_residual / scene_disparity_range, clipped.
# A planar surface leaves only sensor/model noise (a few percent of the range); a
# curved one leaves a residual of 10-20 percent, which this maps to ~0.2-0.6.
PLANARITY_GAIN = 4.0


def fit_disparity_plane(disparity: np.ndarray, mask: np.ndarray,
                        max_samples: int = 20000, seed: int = 0) -> Optional[dict]:
    """Least-squares fit of d = a*u + b*v + c over the mask.

    u and v are normalized image coordinates in [0, 1] (v down). Relative inverse
    depth is linear in (u, v) over any plane, so the fit's residual measures how
    far the masked surface is from planar. The residual is normalized by the
    whole image's disparity range, so a frontal wall (nearly constant disparity)
    still reads as planar rather than dividing noise by noise.

    Returns {a, b, c, rms, planarity, a_n, b_n} or None if the mask has fewer
    than three pixels. `a_n` and `b_n` are `a` and `b` in units of the scene
    range per image width/height, which `verticality` uses.
    """
    disparity = np.asarray(disparity, dtype=np.float64)
    samples = _mask_samples(disparity, mask, max_samples, seed)
    if samples is None:
        return None
    A, d = samples
    coeffs, *_ = np.linalg.lstsq(A, d, rcond=None)
    rms = float(np.sqrt(np.mean((A @ coeffs - d) ** 2)))

    scene = max(disparity_range(disparity), 1e-6)
    a, b, c = (float(x) for x in coeffs)
    return {"a": a, "b": b, "c": c, "rms": rms, "planarity": _planarity(rms, scene),
            "a_n": a / scene, "b_n": b / scene}


def plane_agreement(disparity: np.ndarray, mask: np.ndarray, fit: dict,
                    max_samples: int = 20000, seed: int = 0) -> float:
    """Planarity of another region measured against an existing plane fit.

    Near 1 when the masked pixels lie on `fit`'s plane (two panels of one
    facade), low when they belong to a different surface. 0 for an empty mask.
    """
    disparity = np.asarray(disparity, dtype=np.float64)
    samples = _mask_samples(disparity, mask, max_samples, seed)
    if samples is None:
        return 0.0
    A, d = samples
    pred = A @ np.array([fit["a"], fit["b"], fit["c"]])
    rms = float(np.sqrt(np.mean((pred - d) ** 2)))
    return _planarity(rms, max(disparity_range(disparity), 1e-6))


def _planarity(rms: float, scene: float) -> float:
    return float(np.clip(1.0 - PLANARITY_GAIN * rms / scene, 0.0, 1.0))


def _mask_samples(disparity: np.ndarray, mask: np.ndarray, max_samples: int, seed: int):
    """Design matrix [u, v, 1] and disparities for (a sample of) the mask's pixels."""
    m = np.asarray(mask) > 0
    if m.shape != disparity.shape:
        raise ValueError(f"mask {m.shape} and disparity {disparity.shape} differ")
    ys, xs = np.nonzero(m)
    if len(xs) < 3:
        return None
    if len(xs) > max_samples:
        idx = np.random.default_rng(seed).choice(len(xs), max_samples, replace=False)
        ys, xs = ys[idx], xs[idx]
    h, w = disparity.shape
    u = xs / max(w - 1, 1)
    v = ys / max(h - 1, 1)
    A = np.stack([u, v, np.ones_like(u)], axis=1)
    return A, disparity[ys, xs]


# Tolerance (scene range per image height) of vertical disparity gradient that
# still counts as a wall seen by a slightly pitched camera.
VERTICAL_TOLERANCE = 0.35


def verticality(fit: dict) -> float:
    """1 for a vertical surface, toward 0 for a floor or ceiling.

    Over a plane, inverse depth changes with image height in proportion to the
    plane normal's vertical component. A floor's disparity climbs steeply toward
    the bottom of the frame (large |b|); a wall, frontal or turned sideways,
    changes mostly or only along u. Scale-free, because relative disparity has an
    unknown scale and shift.
    """
    a, b = fit["a_n"], fit["b_n"]
    tilt = b * b / (a * a + b * b + VERTICAL_TOLERANCE ** 2)
    return float(np.clip(1.0 - tilt, 0.0, 1.0))
