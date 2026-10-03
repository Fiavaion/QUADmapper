"""Model locations, availability and lazy, cached loading (ADR-053 section 1).

Models live outside the repo, in %LOCALAPPDATA%\\QUADmapper\\models (or
$QM_MODELS_DIR), put there by setup.ps1 with pinned hashes:

  sam2.1_hiera_small.pt, sam2.1_hiera_tiny.pt     SAM 2.1 (Apache-2.0)
  depth-anything-v2-small/                         Depth Anything V2 Small (Apache-2.0)

Each model is loaded once, on first use, onto the GPU when torch sees one and
the CPU otherwise. torch, sam2 and transformers are imported only here and only
when a model is actually needed, so /health and the stub paths stay light.
"""

from __future__ import annotations

import importlib.util
import os
import threading

import numpy as np

SAM2_MODELS = {
    # name: (checkpoint file, config inside the sam2 package)
    "small": ("sam2.1_hiera_small.pt", "configs/sam2.1/sam2.1_hiera_s.yaml"),
    "tiny": ("sam2.1_hiera_tiny.pt", "configs/sam2.1/sam2.1_hiera_t.yaml"),
}
DEFAULT_SAM2 = "small"
DEPTH_DIR_NAME = "depth-anything-v2-small"
DEPTH_FILES = ("config.json", "preprocessor_config.json", "model.safetensors")

# One lock for loading and inference: the HTTP server is threaded, the models are
# not re-entrant, and an 8 GB GPU should not hold two copies.
LOCK = threading.RLock()

_cache: dict = {}
_failed: set = set()  # model keys whose load raised; dropped from capabilities


def models_dir() -> str:
    override = os.environ.get("QM_MODELS_DIR")
    if override:
        return override
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    return os.path.join(base, "QUADmapper", "models")


def _has_packages(*names: str) -> bool:
    return all(importlib.util.find_spec(n) is not None for n in names)


def sam2_available(name: str = DEFAULT_SAM2) -> bool:
    """SAM 2 package importable and the checkpoint present (nothing loaded)."""
    if name not in SAM2_MODELS or f"sam2:{name}" in _failed:
        return False
    ckpt = os.path.join(models_dir(), SAM2_MODELS[name][0])
    return os.path.isfile(ckpt) and _has_packages("torch", "sam2", "cv2")


def depth_available() -> bool:
    """transformers importable and the Depth Anything V2 Small files present."""
    if "depth" in _failed:
        return False
    folder = os.path.join(models_dir(), DEPTH_DIR_NAME)
    return (all(os.path.isfile(os.path.join(folder, f)) for f in DEPTH_FILES)
            and _has_packages("torch", "transformers"))


def device() -> str:
    import torch
    usable = torch.cuda.is_available() and torch.cuda.device_count() > 0
    return "cuda" if usable else "cpu"


def _load_once(key: str, loader):
    with LOCK:
        if key in _cache:
            return _cache[key]
        try:
            _cache[key] = loader()
        except Exception:
            _failed.add(key)
            raise
        return _cache[key]


def sam2_model(name: str = DEFAULT_SAM2):
    if name not in SAM2_MODELS:
        raise ValueError(f"unknown SAM 2 model {name!r}")

    def load():
        from sam2.build_sam import build_sam2
        ckpt_file, config = SAM2_MODELS[name]
        return build_sam2(config, os.path.join(models_dir(), ckpt_file), device=device())

    return _load_once(f"sam2:{name}", load)


def sam2_predictor(name: str = DEFAULT_SAM2):
    def load():
        from sam2.sam2_image_predictor import SAM2ImagePredictor
        return SAM2ImagePredictor(sam2_model(name))

    return _load_once(f"sam2-predictor:{name}", load)


def sam2_mask_generator():
    """Automatic mask generator on SAM 2.1 small, tuned for a few large regions."""
    def load():
        from sam2.automatic_mask_generator import SAM2AutomaticMaskGenerator
        return SAM2AutomaticMaskGenerator(
            sam2_model(DEFAULT_SAM2),
            points_per_side=16,        # 256 prompts: surfaces are large, speed matters
            points_per_batch=64,
            pred_iou_thresh=0.8,
            stability_score_thresh=0.9,
            min_mask_region_area=0,    # needs the optional CUDA op; small regions are filtered later
        )

    return _load_once("sam2-amg", load)


def autocast():
    """bfloat16 autocast on CUDA (as SAM 2 recommends), a no-op context on CPU."""
    import contextlib
    import torch
    if device() == "cuda":
        return torch.autocast("cuda", dtype=torch.bfloat16)
    return contextlib.nullcontext()


def _depth_parts():
    def load():
        from transformers import AutoImageProcessor, AutoModelForDepthEstimation
        folder = os.path.join(models_dir(), DEPTH_DIR_NAME)
        processor = AutoImageProcessor.from_pretrained(folder, local_files_only=True)
        model = AutoModelForDepthEstimation.from_pretrained(folder, local_files_only=True)
        return processor, model.to(device()).eval()

    return _load_once("depth", load)


def relative_disparity(rgb: np.ndarray) -> np.ndarray:
    """Depth Anything V2 Small relative inverse depth, HxW float32 at input size.

    Larger means closer. The scale and shift are unknown (affine-invariant), which
    is all the planarity test needs.
    """
    import torch
    with LOCK:
        processor, model = _depth_parts()
        inputs = processor(images=rgb, return_tensors="pt").to(device())
        with torch.inference_mode():
            out = model(**inputs).predicted_depth  # (1, h, w)
            disp = torch.nn.functional.interpolate(
                out.unsqueeze(1).float(), size=rgb.shape[:2],
                mode="bilinear", align_corners=False)
        return disp[0, 0].cpu().numpy().astype(np.float32)
