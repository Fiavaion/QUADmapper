"""Click-to-segment surface proposals, POST /segment (ADR-053 section 2).

photo + one click -> SAM 2.1 mask -> largest contour -> four corners + outline,
with Depth Anything V2 Small checking that the masked surface is planar.

The image embedding (the expensive SAM step) and the disparity map are kept for
the last image, so a second click on the same photo only runs the mask decoder.
"""

from __future__ import annotations

import os
import time
from typing import Optional

import cv2
import numpy as np

import geometry
import models

_last_image: dict = {}  # {"key": (path, mtime, size), "rgb": ndarray}
_embedded: dict = {}    # model name -> image key whose embedding the predictor holds
_disparity: dict = {}   # {"key": image key, "disp": ndarray}


def _load_rgb(image_path: str):
    st = os.stat(image_path)
    key = (os.path.abspath(image_path), st.st_mtime_ns, st.st_size)
    if _last_image.get("key") != key:
        bgr = cv2.imread(image_path)
        if bgr is None:
            return None, None
        _last_image.clear()
        _last_image.update(key=key, rgb=cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB))
    return _last_image["key"], _last_image["rgb"]


def disparity_for(key, rgb: np.ndarray) -> Optional[np.ndarray]:
    """Cached relative disparity for an image, or None without the depth model."""
    if not models.depth_available():
        return None
    if _disparity.get("key") != key:
        _disparity.clear()
        _disparity.update(key=key, disp=models.relative_disparity(rgb))
    return _disparity["disp"]


def segment(image_path: str, point, model: str = models.DEFAULT_SAM2) -> Optional[dict]:
    """Segment the surface under `point` (normalized, origin top-left).

    Returns the /segment response body without api_version, or None when the
    image is unreadable, SAM 2 is unavailable, or the mask is empty (the caller
    then answers with the stub).
    """
    if not os.path.isfile(image_path) or not models.sam2_available(model):
        return None

    import torch

    t0 = time.perf_counter()
    with models.LOCK:
        key, rgb = _load_rgb(image_path)
        if rgb is None:
            return None
        h, w = rgb.shape[:2]
        predictor = models.sam2_predictor(model)
        click = np.array([[min(point[0] * w, w - 1), min(point[1] * h, h - 1)]],
                         dtype=np.float32)
        with torch.inference_mode(), models.autocast():
            if _embedded.get(model) != key:
                predictor.set_image(rgb)
                _embedded[model] = key
            masks, scores, _ = predictor.predict(
                point_coords=click, point_labels=np.array([1]), multimask_output=True)
        best = int(np.argmax(scores))
        mask = masks[best] > 0
        sam_score = float(np.clip(scores[best], 0.0, 1.0))

        quad = geometry.mask_to_quad(mask)
        if quad is None:
            return None
        corners_px, outline_px = quad

        planarity = None
        disp = disparity_for(key, rgb)
        if disp is not None:
            fit = geometry.fit_disparity_plane(disp, mask)
            if fit is not None:
                planarity = fit["planarity"]

    confidence = sam_score if planarity is None else sam_score * planarity
    return {
        "corners": geometry.normalize_points(corners_px, w, h),
        "outline": geometry.normalize_points(outline_px, w, h),
        "confidence": float(np.clip(confidence, 0.0, 1.0)),
        "planarity": planarity,
        "stub": False,
        "model": model,
        "device": models.device(),
        "timing_ms": round((time.perf_counter() - t0) * 1000.0, 1),
    }
