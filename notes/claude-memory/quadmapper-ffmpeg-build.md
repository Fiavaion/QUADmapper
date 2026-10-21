---
name: quadmapper-ffmpeg-build
description: "Non-obvious facts about the vcpkg FFmpeg build — libavdevice is present, but there is NO PNG encoder."
metadata: 
  node_type: memory
  type: reference
  originSessionId: 031ce117-379a-489f-8f21-7b4795918542
---

The vcpkg FFmpeg in this project is `ffmpeg[avcodec,avdevice,avfilter,avformat,core,swresample,swscale]`
(x64-windows). Two facts that cost/saved real time in Session 13 (QM-055):

- **libavdevice IS installed** (`avdevice.lib` + `libavdevice/avdevice.h`), so webcam capture via
  the dshow input works with NO new dependency — but `src/video/CMakeLists.txt` must explicitly
  `find_library(AVDEVICE_LIBRARY avdevice)` and link it (it wasn't, originally). `VideoDecoder::open`
  passes `nullptr` input format (file); the camera path uses `av_find_input_format("dshow")` +
  `avformat_open_input("video=<name>", ...)` after `avdevice_register_all()`.
- **There is NO PNG encoder** — `avcodec_find_encoder(AV_CODEC_ID_PNG)` returns null (PNG needs zlib,
  omitted from this build). Don't rely on FFmpeg to write PNGs. `src/video/PngWriter.cpp` is a
  dependency-free PNG writer (stored-DEFLATE) instead; output is OpenCV/libpng-readable.

Enabling the PNG encoder would require a `vcpkg.json` ffmpeg-feature change (Change Boundary →
approval + ffmpeg rebuild) — avoided. Webcam device names come from DirectShow `FriendlyName`
(see `CameraDevices.cpp`), which is exactly what dshow's `video=<name>` expects. See
[[ai-preshow-initiative]].
