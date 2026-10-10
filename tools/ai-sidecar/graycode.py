"""Gray-code structured light (ADR-053 section 3).

The app shows the sequence fullscreen on one output and saves one camera frame per
pattern; this module writes the patterns and decodes the captures into a dense
camera-pixel -> projector-pixel map.

The sequence is OpenCV's ``cv2.structured_light.GrayCodePattern``: its patterns
(each followed by its inverse, columns then rows, most significant bit first), then
the white and the black frame of ``getImagesForShadowMasks``. Frame k is shown from
``pattern_{k:03d}.png`` and captured to ``capture_{k:03d}.png`` in the scan folder.
The count always comes from the generator.

The decode follows ``GrayCodePattern.getProjPixel`` exactly (a bit is set where the
pattern is brighter than its inverse; a pixel is rejected where any pair differs by
less than the white threshold or where white - black is under the black threshold),
but over the whole image at once with numpy: calling getProjPixel per pixel takes
minutes at 1080p. A test checks the two agree.
"""

from __future__ import annotations

import os
from typing import Optional

import numpy as np

PATTERN_NAME = "pattern_{:03d}.png"
CAPTURE_NAME = "capture_{:03d}.png"
MAP_NAME = "graymap.npz"

# OpenCV's defaults for GrayCodePattern (whiteThreshold 5, blackThreshold 40).
WHITE_THRESHOLD = 5
BLACK_THRESHOLD = 40

# Projector cells this size count toward coverage (a camera usually sees fewer pixels
# than the projector shows, so single projector pixels would undercount).
COVERAGE_CELL = 8
# A mapped point takes the median of the decoded pixels within this radius.
LOOKUP_RADIUS = 3


def available() -> bool:
    """cv2 with the contrib structured_light module is importable."""
    try:
        import cv2
    except Exception:  # noqa: BLE001 - no OpenCV at all
        return False
    return hasattr(cv2, "structured_light")


def _generator(width: int, height: int):
    import cv2
    return cv2.structured_light.GrayCodePattern.create(width, height)


def frame_count(width: int, height: int) -> int:
    """Frames the app shows: the generator's patterns plus white and black."""
    return _generator(width, height).getNumberOfPatternImages() + 2


def sequence(width: int, height: int) -> list:
    """Every frame of the sequence as uint8 grey images, in display order."""
    gen = _generator(width, height)
    ok, patterns = gen.generate()
    if not ok:
        raise RuntimeError("GrayCodePattern.generate failed")
    black, white = gen.getImagesForShadowMasks(
        np.zeros((height, width), np.uint8), np.zeros((height, width), np.uint8))
    return list(patterns) + [white, black]


def write_patterns(width: int, height: int, folder: str) -> int:
    """Write the sequence to `folder` as RGB PNGs; returns the frame count.

    RGB, not grey: the app's texture path shows a one-channel image as red.
    """
    import cv2
    frames = sequence(width, height)
    for k, frame in enumerate(frames):
        path = os.path.join(folder, PATTERN_NAME.format(k))
        if not cv2.imwrite(path, cv2.cvtColor(frame, cv2.COLOR_GRAY2BGR)):
            raise OSError(f"could not write {path}")
    return len(frames)


def decode_images(captures: list, width: int, height: int) -> tuple:
    """Decode grey captures (display order, white and black last).

    Returns (proj_x, proj_y): int32 arrays the size of the captures, holding the
    projector pixel each camera pixel sees, or -1 where it could not be decoded.
    """
    gen = _generator(width, height)
    n_patterns = gen.getNumberOfPatternImages()
    # The column patterns alone, from the generator: a 1-pixel-high sequence has no
    # row patterns.
    n_col_bits = _generator(width, 1).getNumberOfPatternImages() // 2
    n_row_bits = n_patterns // 2 - n_col_bits
    if len(captures) != n_patterns + 2:
        raise ValueError(f"expected {n_patterns + 2} captures, got {len(captures)}")

    imgs = [np.asarray(c, dtype=np.int16) for c in captures]
    white, black = imgs[n_patterns], imgs[n_patterns + 1]
    valid = (white - black) > BLACK_THRESHOLD

    def decode_axis(first: int, bits: int) -> np.ndarray:
        nonlocal valid
        binary_bit = np.zeros(white.shape, dtype=bool)
        value = np.zeros(white.shape, dtype=np.int32)
        for i in range(bits):
            pattern, inverse = imgs[first + 2 * i], imgs[first + 2 * i + 1]
            valid &= np.abs(pattern - inverse) >= WHITE_THRESHOLD
            gray_bit = pattern > inverse
            binary_bit = gray_bit if i == 0 else (binary_bit ^ gray_bit)
            value = (value << 1) | binary_bit.astype(np.int32)
        return value

    xs = decode_axis(0, n_col_bits)
    ys = decode_axis(2 * n_col_bits, n_row_bits)
    valid &= (xs < width) & (ys < height)
    proj_x = np.where(valid, xs, -1).astype(np.int32)
    proj_y = np.where(valid, ys, -1).astype(np.int32)
    return proj_x, proj_y


def coverage(proj_x: np.ndarray, proj_y: np.ndarray, width: int, height: int) -> float:
    """Share of the projector's area (in COVERAGE_CELL cells) some camera pixel sees."""
    valid = proj_x >= 0
    cells_x = -(-width // COVERAGE_CELL)
    cells_y = -(-height // COVERAGE_CELL)
    hit = np.zeros((cells_y, cells_x), dtype=bool)
    hit[proj_y[valid] // COVERAGE_CELL, proj_x[valid] // COVERAGE_CELL] = True
    return float(hit.mean())


def decode_error(proj_x: np.ndarray, proj_y: np.ndarray) -> Optional[float]:
    """Mean distance, in projector pixels, of each decoded pixel from the median of its
    5x5 neighbourhood: decode noise, since a real map is smooth. Only pixels whose whole
    neighbourhood decoded count; None when there are none.
    """
    import cv2
    valid = (proj_x >= 0).astype(np.uint8)
    inner = cv2.erode(valid, np.ones((5, 5), np.uint8), borderValue=0).astype(bool)
    if not inner.any():
        return None
    mx = cv2.medianBlur(proj_x.astype(np.float32), 5)
    my = cv2.medianBlur(proj_y.astype(np.float32), 5)
    dist = np.hypot(proj_x[inner] - mx[inner], proj_y[inner] - my[inner])
    return float(dist.mean())


def decode_folder(folder: str, width: int, height: int) -> dict:
    """Decode the captures in `folder` and write the map beside them.

    Raises FileNotFoundError when a capture is missing or unreadable, ValueError when
    they differ in size.
    """
    import cv2
    count = frame_count(width, height)
    captures = []
    for k in range(count):
        path = os.path.join(folder, CAPTURE_NAME.format(k))
        img = cv2.imread(path, cv2.IMREAD_GRAYSCALE) if os.path.isfile(path) else None
        if img is None:
            raise FileNotFoundError(f"capture {CAPTURE_NAME.format(k)} is missing or unreadable")
        if captures and img.shape != captures[0].shape:
            raise ValueError("the captures differ in size")
        captures.append(img)

    proj_x, proj_y = decode_images(captures, width, height)
    cam_h, cam_w = proj_x.shape
    map_path = os.path.join(folder, MAP_NAME)
    np.savez_compressed(map_path, proj_x=proj_x.astype(np.int16), proj_y=proj_y.astype(np.int16),
                        camera_size=np.array([cam_w, cam_h], np.int32),
                        projector_size=np.array([width, height], np.int32))
    return {
        "map_path": os.path.abspath(map_path),
        "coverage": round(coverage(proj_x, proj_y, width, height), 4),
        "decode_error_px": _rounded(decode_error(proj_x, proj_y)),
    }


def _rounded(value: Optional[float]) -> Optional[float]:
    return None if value is None else round(value, 3)


def load_map(map_path: str) -> dict:
    """Read a map written by decode_folder (no pickles)."""
    with np.load(map_path, allow_pickle=False) as data:
        out = {key: data[key] for key in ("proj_x", "proj_y", "camera_size", "projector_size")}
    proj_x = out["proj_x"]
    if proj_x.ndim != 2 or out["proj_y"].shape != proj_x.shape:
        raise ValueError("malformed map")
    return out


def map_points(gray_map: dict, points: list) -> tuple:
    """Normalized photo points -> normalized projector points through the map.

    Each point takes the median projector pixel of the decoded camera pixels within
    LOOKUP_RADIUS, read at that pixel's centre; a point with none is invalid
    ([-1, -1]). Returns (points, valid).
    """
    proj_x = gray_map["proj_x"].astype(np.int32)
    proj_y = gray_map["proj_y"].astype(np.int32)
    proj_w, proj_h = (int(v) for v in gray_map["projector_size"])
    cam_h, cam_w = proj_x.shape
    out, valid = [], []
    for x, y in points:
        cx = min(int(x * cam_w), cam_w - 1)
        cy = min(int(y * cam_h), cam_h - 1)
        x0, x1 = max(cx - LOOKUP_RADIUS, 0), min(cx + LOOKUP_RADIUS + 1, cam_w)
        y0, y1 = max(cy - LOOKUP_RADIUS, 0), min(cy + LOOKUP_RADIUS + 1, cam_h)
        wx, wy = proj_x[y0:y1, x0:x1], proj_y[y0:y1, x0:x1]
        ok = wx >= 0
        if not ok.any():
            out.append([-1.0, -1.0])
            valid.append(False)
            continue
        px = float(np.median(wx[ok])) + 0.5
        py = float(np.median(wy[ok])) + 0.5
        out.append([px / proj_w, py / proj_h])
        valid.append(True)
    return out, valid
