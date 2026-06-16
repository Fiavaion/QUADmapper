"""Surface detection for the AI pre-show sidecar (QM-057).

Turns a single photo of a projection surface into the dominant planar surface's
four image-space corners + plane + a calibrated confidence, using:

  image -> Metric3D (metric depth + normals) -> metric point cloud ->
  Open3D segment_plane (ms-scale RANSAC) -> largest planar component ->
  4-corner extraction -> confidence

Metric depth (not the spike's relative DA-V2) removes the affine bow on oblique
surfaces. The confidence folds in a verticality prior from the plane normal so a
cluttered scene whose biggest plane is the floor returns LOW confidence rather
than confidently warping onto the wrong surface (the spike's gallery failure).

The Metric3D model is loaded lazily from the local ComfyUI controlnet_aux install
(ADR-035; user-authorized). Paths are overridable via env vars so the heavy model
never loads in unit tests / on machines without it.
"""

from __future__ import annotations

import os
import sys
import time
from typing import Optional

import cv2
import numpy as np

# Local Metric3D install (overridable). Kept out of the repo; see ADR-035.
M3D_SRC = os.environ.get(
    "QM_M3D_SRC",
    r"D:\aiTools\ComfyUI_windows_portable\ComfyUI\custom_nodes\comfyui_controlnet_aux\src",
)
M3D_CKPT = os.environ.get(
    "QM_M3D_CKPT",
    r"D:\aiTools\ComfyUI_windows_portable\ComfyUI\custom_nodes\comfyui_controlnet_aux"
    r"\ckpts\JUGGHM\Metric3D\metric_depth_vit_small_800k.pth",
)
M3D_BACKBONE = os.environ.get("QM_M3D_BACKBONE", "vit-small")

# Lazily-initialised singletons (torch model + cfg).
_model = None
_cfg = None
_device = "cpu"


def model_available() -> bool:
    """True if the Metric3D source + checkpoint are present (no load attempted)."""
    return os.path.isdir(M3D_SRC) and os.path.isfile(M3D_CKPT)


def _ensure_model():
    """Load Metric3D once. Raises if unavailable (caller maps to a 503)."""
    global _model, _cfg, _device
    if _model is not None:
        return
    if not model_available():
        raise RuntimeError(f"Metric3D not found (src={M3D_SRC}, ckpt={M3D_CKPT})")

    # The vendored mmcv needs these set before import (matches the ComfyUI node).
    os.environ.setdefault("NPU_DEVICE_COUNT", "0")
    os.environ.setdefault("MMCV_WITH_OPS", "0")
    if M3D_SRC not in sys.path:
        sys.path.insert(0, M3D_SRC)

    import torch
    from custom_controlnet_aux.metric3d import load_model

    model, cfg = load_model(M3D_BACKBONE, M3D_CKPT)
    _device = "cuda" if torch.cuda.is_available() else "cpu"
    _model = model.to(_device)
    _cfg = cfg


def _infer_depth_normal(bgr: np.ndarray, fx: float, fy: float):
    """Run Metric3D. Returns (depth HxW float metres, normal HxWx3) at input size."""
    _ensure_model()
    import torch
    from custom_controlnet_aux.metric3d import predict_depth_normal

    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    state: dict = {}
    predict_depth_normal(_model, _cfg, rgb, fx=fx, fy=fy, state_cache=state)
    if _device == "cuda":
        torch.cuda.synchronize()

    H, W = bgr.shape[:2]
    depth = state["depth"].astype(np.float64)
    # Metric3D returns depth at its own internal (padded/resized) resolution;
    # resize back to the input frame so pixels line up for back-projection.
    if depth.shape != (H, W):
        depth = cv2.resize(depth, (W, H), interpolation=cv2.INTER_LINEAR)
    normal = state["normal"]
    if normal.shape[:2] != (H, W):
        normal = cv2.resize(normal, (W, H), interpolation=cv2.INTER_LINEAR)
    return depth, normal


def _backproject(depth: np.ndarray, fx: float, fy: float, step: int):
    """Pinhole back-projection on a subsampled grid. Returns (pts Nx3, gridshape)."""
    H, W = depth.shape
    cx, cy = W / 2.0, H / 2.0
    ys, xs = np.mgrid[0:H:step, 0:W:step]
    Z = depth[0:H:step, 0:W:step].astype(np.float64)
    X = (xs - cx) * Z / fx
    Y = (ys - cy) * Z / fy
    pts = np.stack([X.ravel(), Y.ravel(), Z.ravel()], axis=1)
    return pts, Z.shape


def _order_corners(pts: np.ndarray) -> np.ndarray:
    """Order 4 points as TL, TR, BR, BL using the sum/diff trick."""
    pts = pts.astype(np.float64)
    s = pts.sum(axis=1)
    d = np.diff(pts, axis=1).ravel()
    return np.array([
        pts[np.argmin(s)],   # TL: smallest x+y
        pts[np.argmin(d)],   # TR: smallest y-x
        pts[np.argmax(s)],   # BR: largest x+y
        pts[np.argmax(d)],   # BL: largest y-x
    ])


def _extract_corners(mask: np.ndarray):
    """Four corners (pixels) of the largest connected component of `mask`.

    Returns (corners 4x2 float, cc_fraction) or (None, 0.0) if nothing usable.
    cc_fraction is the largest component's share of all mask pixels — a
    compactness signal (scattered inliers -> low).
    """
    total = int(mask.sum() > 0 and np.count_nonzero(mask))
    if total == 0:
        return None, 0.0
    n, labels, stats, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8), 8)
    if n <= 1:
        return None, 0.0
    # Largest non-background component.
    areas = stats[1:, cv2.CC_STAT_AREA]
    largest = 1 + int(np.argmax(areas))
    cc = (labels == largest).astype(np.uint8)
    cc_fraction = float(areas.max()) / float(np.count_nonzero(mask))

    contours, _ = cv2.findContours(cc, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return None, 0.0
    cnt = max(contours, key=cv2.contourArea)

    # Prefer a clean 4-gon (perspective quad); fall back to the min-area rect.
    peri = cv2.arcLength(cnt, True)
    approx = cv2.approxPolyDP(cnt, 0.03 * peri, True)
    if len(approx) == 4:
        corners = approx.reshape(4, 2).astype(np.float64)
    else:
        corners = cv2.boxPoints(cv2.minAreaRect(cnt)).astype(np.float64)
    return _order_corners(corners), cc_fraction


def estimate_surface(bgr: np.ndarray, fx: float = 1000.0, fy: float = 1000.0,
                     dist_threshold: float = 0.04, step: int = 3,
                     debug: bool = False) -> dict:
    """Detect the dominant planar surface in `bgr`.

    Returns a dict with normalized corners[4] (TL,TR,BR,BL in [0,1]), the metric
    plane {normal,d}, a confidence in [0,1], and timing. `debug` adds the raw
    sub-signals + an inlier mask for visualization.
    """
    import open3d as o3d

    t_total = time.time()
    H, W = bgr.shape[:2]

    t = time.time()
    depth, normal = _infer_depth_normal(bgr, fx, fy)
    t_infer = time.time() - t

    pts, grid_shape = _backproject(depth, fx, fy, step)

    t = time.time()
    pcd = o3d.geometry.PointCloud()
    pcd.points = o3d.utility.Vector3dVector(pts)
    # Dominant plane.
    plane1, inl1 = pcd.segment_plane(dist_threshold, ransac_n=3, num_iterations=400)
    # Second plane on the remainder, to measure surface ambiguity.
    second_frac = 0.0
    if len(inl1) > 0 and len(pts) - len(inl1) > 100:
        rest = pcd.select_by_index(inl1, invert=True)
        _, inl2 = rest.segment_plane(dist_threshold, ransac_n=3, num_iterations=200)
        second_frac = len(inl2) / max(len(inl1), 1)
    t_fit = time.time() - t

    a, b, c, d = plane1
    pn = np.array([a, b, c], dtype=np.float64)
    pn = pn / (np.linalg.norm(pn) + 1e-9)

    # Inlier mask back in image space (grid resolution -> full frame).
    inl_grid = np.zeros(grid_shape[0] * grid_shape[1], dtype=np.uint8)
    inl_grid[np.asarray(inl1, dtype=np.int64)] = 1
    inl_grid = inl_grid.reshape(grid_shape)
    mask = cv2.resize(inl_grid * 255, (W, H), interpolation=cv2.INTER_NEAREST)

    corners_px, cc_fraction = _extract_corners(mask)

    # --- confidence ---------------------------------------------------------
    # Two parts: a "clean planar surface exists" quality score, then two
    # multiplicative gates that collapse it when the scene says the surface is
    # the WRONG kind (floor/ceiling) or ambiguous (cluttered). Multiplicative,
    # not additive, so the gallery floor-trap drops to ~0 (low, not confidently
    # wrong) instead of riding high on planarity alone (the spike's failure).
    inl_frac = len(inl1) / max(len(pts), 1)
    # Planarity: RMS point-plane distance of inliers, relative to depth span.
    inl_pts = pts[np.asarray(inl1, dtype=np.int64)]
    resid = np.abs(inl_pts @ pn + d)
    span = float(depth.max() - depth.min()) + 1e-9
    planarity = 1.0 - min(1.0, (np.sqrt((resid ** 2).mean()) / span) * 8.0)
    quality = (0.35 * min(1.0, inl_frac * 2.5)
               + 0.35 * planarity
               + 0.30 * cc_fraction)

    # Verticality gate: a projection surface is vertical, so its plane normal has
    # a small camera-up (Y) component. A floor/ceiling plane (the gallery trap)
    # has |normal_y| ~ 1 -> gate ~ 0.
    verticality = 1.0 - min(1.0, abs(pn[1]))
    # Ambiguity gate: a comparably-large second plane means a cluttered scene.
    ambiguity = 1.0 - min(1.0, second_frac)

    confidence = float(np.clip(quality * verticality * ambiguity, 0.0, 1.0))

    # Normalized corners (centered fallback if extraction failed).
    if corners_px is None:
        corners_norm = [[0.25, 0.25], [0.75, 0.25], [0.75, 0.75], [0.25, 0.75]]
        confidence = min(confidence, 0.1)
    else:
        corners_norm = [[float(x) / W, float(y) / H] for x, y in corners_px]

    result = {
        "corners": corners_norm,
        "plane": {"normal": [float(pn[0]), float(pn[1]), float(pn[2])],
                  "d": float(d)},
        "confidence": confidence,
        "stub": False,
        "timing_ms": {"infer": round(t_infer * 1000, 1),
                      "fit": round(t_fit * 1000, 1),
                      "total": round((time.time() - t_total) * 1000, 1)},
    }
    if debug:
        result["debug"] = {
            "inl_frac": round(inl_frac, 3),
            "planarity": round(planarity, 3),
            "cc_fraction": round(cc_fraction, 3),
            "verticality": round(verticality, 3),
            "second_frac": round(second_frac, 3),
            "ambiguity": round(ambiguity, 3),
            "normal": pn.tolist(),
            "depth_min": float(depth.min()),
            "depth_max": float(depth.max()),
            "_mask": mask,
            "_corners_px": None if corners_px is None else corners_px.tolist(),
        }
    return result
