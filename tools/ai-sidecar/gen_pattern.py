"""Generate the One-Flash calibration pattern (QM-059) + its spec.

A ChArUco board is robust to partial occlusion and gives many sub-pixel
correspondences for the QM-060 homography solve. The board is displayed fullscreen
on the projector via an identity quad, so its outer extent maps to projector
normalized [0,1]; the per-corner board coordinates in the spec let QM-060 recover
the projector<->photo homography.

Run once to (re)generate resources/patterns/charuco.png + charuco.json.
"""

import json
import os

import cv2
import cv2.aruco as aruco

HERE = os.path.dirname(__file__)
OUT_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "resources", "patterns"))
os.makedirs(OUT_DIR, exist_ok=True)

DICT_NAME = "DICT_5X5_100"
SQUARES_X = 7
SQUARES_Y = 4
SQUARE_LEN = 0.04   # metres (relative; only ratios matter for homography)
MARKER_LEN = 0.03
IMG_W, IMG_H = 1920, 1080
MARGIN_PX = 40

dictionary = aruco.getPredefinedDictionary(getattr(aruco, DICT_NAME))
board = aruco.CharucoBoard((SQUARES_X, SQUARES_Y), SQUARE_LEN, MARKER_LEN, dictionary)
img = board.generateImage((IMG_W, IMG_H), marginSize=MARGIN_PX, borderBits=1)

# generateImage() is single-channel; write it as 3-channel BGR so the app's
# texture loader gives white-on-black (a 1-channel PNG loads as GL_RED -> red).
img = cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)

png_path = os.path.join(OUT_DIR, "charuco.png")
cv2.imwrite(png_path, img)

spec = {
    "type": "charuco",
    "dictionary": DICT_NAME,
    "squares_x": SQUARES_X,
    "squares_y": SQUARES_Y,
    "square_length": SQUARE_LEN,
    "marker_length": MARKER_LEN,
    "image_size": [IMG_W, IMG_H],
    "margin_px": MARGIN_PX,
    # The board is shown via an identity fullscreen quad, so its image extent maps
    # to the projector's normalized [0,1] output space (TL,TR,BR,BL).
    "projector_corners_norm": [[0, 0], [1, 0], [1, 1], [0, 1]],
}
with open(os.path.join(OUT_DIR, "charuco.json"), "w") as f:
    json.dump(spec, f, indent=2)

print("wrote", png_path, img.shape)
print("wrote", os.path.join(OUT_DIR, "charuco.json"))
