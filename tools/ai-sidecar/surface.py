"""Automatic surface detection, POST /surface (QM-057, re-engined by ADR-053).

  photo -> SAM 2.1 automatic masks (candidate regions)
        -> Depth Anything V2 Small relative disparity
        -> per candidate: planarity + verticality from a disparity-plane fit
        -> best candidate's four corners + confidence

The confidence keeps the QM-057 intent: a quality score for "a clean planar
region exists", then two multiplicative gates that collapse it when the winner
is the wrong kind of surface (a floor or ceiling: verticality) or when another,
separate region scores about as well (a cluttered scene: ambiguity). Low, not
confidently wrong.

Relative depth has no metric scale, so no plane equation is returned
(`plane: null`).
"""

from __future__ import annotations

import time
from typing import Optional

import numpy as np

import geometry
import models

MIN_AREA = 0.02  # candidates smaller than this share of the frame are ignored
MAX_AREA = 0.95  # ...and one that is the whole frame says nothing
FULL_SIZE_AREA = 0.15  # a region this big earns the full size term


def model_available() -> bool:
    return models.sam2_available(models.DEFAULT_SAM2) and models.depth_available()


def score_candidates(masks, sam_scores, disparity: np.ndarray,
                     luminance: Optional[np.ndarray] = None) -> list:
    """Score candidate masks. Pure numpy; returns dicts sorted best first.

    quality mixes planarity, size, SAM's own mask score and, when `luminance`
    (HxW in 0..1) is given, how light the region is: a white screen or wall
    takes projection, a black curtain hardly does. score = quality * verticality.
    """
    h, w = disparity.shape
    total = float(h * w)
    scored = []
    for i, (mask, sam) in enumerate(zip(masks, sam_scores)):
        area = float(np.count_nonzero(mask)) / total
        if not MIN_AREA <= area <= MAX_AREA:
            continue
        fit = geometry.fit_disparity_plane(disparity, mask, seed=i)
        if fit is None:
            continue
        vert = geometry.verticality(fit)
        size = min(1.0, area / FULL_SIZE_AREA)
        sam = float(np.clip(sam, 0.0, 1.0))
        if luminance is None:
            quality = 0.5 * fit["planarity"] + 0.25 * size + 0.25 * sam
        else:
            light = float(np.clip(luminance[mask].mean(), 0.0, 1.0))
            quality = 0.4 * fit["planarity"] + 0.2 * size + 0.2 * sam + 0.2 * light
        scored.append({"index": i, "area": area, "planarity": fit["planarity"],
                       "verticality": vert, "quality": quality,
                       "score": quality * vert, "fit": fit})
    scored.sort(key=lambda c: c["score"], reverse=True)
    return scored


def _overlap(a: np.ndarray, b: np.ndarray) -> float:
    """Intersection over the smaller mask: 1 when one region contains the other."""
    inter = np.count_nonzero(a & b)
    return inter / max(1, min(np.count_nonzero(a), np.count_nonzero(b)))


COPLANAR = 0.8  # plane_agreement at or above this: the same surface, not a rival


def ambiguity_gate(scored: list, masks, disparity: np.ndarray) -> float:
    """1 when the winner has no rival, falling to 0 as a different surface matches it.

    Not rivals: a region nested in the winner (a screen inside its wall) and a
    region on the winner's plane (another panel of the same facade). The best
    remaining region is the rival: at two thirds of the winner's score or less it
    costs nothing, and an equal rival collapses the gate.
    """
    if len(scored) < 2 or scored[0]["score"] <= 0.0:
        return 1.0
    best = scored[0]
    winner = masks[best["index"]]
    for rival in scored[1:]:
        other = masks[rival["index"]]
        if _overlap(winner, other) >= 0.2:
            continue
        if geometry.plane_agreement(disparity, other, best["fit"]) >= COPLANAR:
            continue
        ratio = rival["score"] / best["score"]
        return float(np.clip(3.0 * (1.0 - ratio), 0.0, 1.0))
    return 1.0


def estimate_surface(bgr: np.ndarray) -> Optional[dict]:
    """Detect the best projection surface in a BGR photo.

    Returns the /surface body without api_version, or None when no candidate
    region survives (the caller answers with the stub).
    """
    import cv2
    import torch

    t0 = time.perf_counter()
    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
    h, w = rgb.shape[:2]
    with models.LOCK:
        generator = models.sam2_mask_generator()
        with torch.inference_mode(), models.autocast():
            proposals = generator.generate(rgb)
        t_sam = time.perf_counter()
        disparity = models.relative_disparity(rgb)
    t_depth = time.perf_counter()

    masks = [p["segmentation"].astype(bool) for p in proposals]
    luminance = cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY).astype(np.float32) / 255.0
    scored = score_candidates(masks, [p["predicted_iou"] for p in proposals],
                              disparity, luminance)
    if not scored:
        return None
    best = scored[0]
    quad = geometry.mask_to_quad(masks[best["index"]])
    if quad is None:
        return None
    corners_px, _ = quad

    ambiguity = ambiguity_gate(scored, masks, disparity)
    confidence = float(np.clip(best["score"] * ambiguity, 0.0, 1.0))
    return {
        "corners": geometry.normalize_points(corners_px, w, h),
        "plane": None,
        "confidence": confidence,
        "stub": False,
        "timing_ms": {"sam": round((t_sam - t0) * 1000, 1),
                      "depth": round((t_depth - t_sam) * 1000, 1),
                      "total": round((time.perf_counter() - t0) * 1000, 1)},
        "debug": {"candidates": len(scored), "planarity": round(best["planarity"], 3),
                  "verticality": round(best["verticality"], 3),
                  "area": round(best["area"], 3), "ambiguity": round(ambiguity, 3)},
    }
