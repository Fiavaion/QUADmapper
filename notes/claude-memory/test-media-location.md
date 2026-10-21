---
name: test-media-location
description: Where to find real test images/videos for QUADmapper media verification
metadata: 
  node_type: memory
  type: reference
  originSessionId: 6bed2b8c-604b-4e2b-b8f8-2f6a2d2e2a50
---

Real test media for verifying the media-assignment workflow lives in `D:\testing Videos\`.
Contains videos (`greenCrystals_15Sec_4x3.mp4`, `4k_10Sec_Quads.mp4`, `5sec.mp4`, etc.),
audio (`.mp3`), and at least one image (`knowledge-graph.png`, 1800×1200).

HAP test clips live in `D:\testing Videos\Hap\` (3 `.mov` files; `5sec_hap.mov` is 1920×1080
**Hap Q** / BC3_YCoCg — used to verify QM-080 HAP playback end-to-end). See [[hap-playback]].

Used by the media smoke harnesses `tests/smoke/drive_media_smoke.ps1` (video, `-VideoPath`)
and `tests/smoke/drive_image_smoke.ps1` (image, `-ImagePath`). Verified end-to-end in
Session 12: load → assign to quad → warp → F11 projector, working in editor + on monitor 2.
