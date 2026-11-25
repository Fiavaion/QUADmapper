# Unit tests - mirror src/ structure

# Core module tests
add_quadmapper_test(test_core
    core/UUIDTest.cpp
    core/ResultTest.cpp
    core/LoggerTest.cpp
    core/CommandHistoryTest.cpp
    core/FileSystemTest.cpp
)
target_link_libraries(test_core PRIVATE quadmapper_core)

# Scene module tests
add_quadmapper_test(test_scene
    scene/QuadTest.cpp
    scene/QuadGeometryTest.cpp
    scene/SceneTest.cpp
    scene/MeshSubdivisionTest.cpp
    scene/QuadManagerTest.cpp
    scene/QuadGroupTest.cpp
    scene/CommandsTest.cpp
    scene/ProjectorCameraTest.cpp
    scene/ObjLoaderTest.cpp
    scene/ProjectorCalibrationTest.cpp
    scene/RayPickTest.cpp
    scene/CalibrationSessionTest.cpp
    scene/CropInsetsTest.cpp
    scene/SliceTest.cpp
)
target_link_libraries(test_scene PRIVATE quadmapper_scene quadmapper_core)

# Project module tests
add_quadmapper_test(test_project
    project/ProjectTest.cpp
    project/ProjectSerializerTest.cpp
    project/ProjectPersistenceTest.cpp
)
target_link_libraries(test_project PRIVATE quadmapper_project quadmapper_scene quadmapper_core)
# Historical .qmap fixtures (ADR-046): every shipped format version must still load.
target_compile_definitions(test_project PRIVATE
    QM_PROJECT_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/projects"
)

# Output module tests
add_quadmapper_test(test_output
    output/OutputWindowTest.cpp
    output/OutputManagerTest.cpp
)
target_link_libraries(test_output PRIVATE quadmapper_output quadmapper_core)

# Audio module tests
add_quadmapper_test(test_audio
    audio/AudioFrameTest.cpp
    audio/AudioBufferTest.cpp
    audio/AudioDecoderTest.cpp
    audio/AudioSourceTest.cpp
    audio/AudioMixerTest.cpp
    audio/AudioDeviceTest.cpp
    audio/AudioDriftControllerTest.cpp
    audio/SyncedAudioSourceTest.cpp
    audio/OfflineAudioMixTest.cpp  # export sound; fixtures written with video/Mp4Writer
)
target_link_libraries(test_audio PRIVATE quadmapper_audio quadmapper_video quadmapper_core)

# Input module tests
add_quadmapper_test(test_input
    input/VertexKeyboardTest.cpp
)
target_link_libraries(test_input PRIVATE quadmapper_input quadmapper_core imgui)

# Video module tests (QM-088) - pure frame pacing / decode backoff, no FFmpeg file I/O
add_quadmapper_test(test_video
    video/FrameSelectionTest.cpp
    video/DecodeBackoffTest.cpp
    video/PlaybackClockTest.cpp
    video/PlaybackTransportTest.cpp
)
target_link_libraries(test_video PRIVATE quadmapper_video quadmapper_core)

# Decoding and playback tests (QM-091, ADR-050) - encode tiny clips with the linked
# FFmpeg at test time (TestClips.hpp), so they need its headers and libraries directly.
if(AVCODEC_LIBRARY AND AVFORMAT_LIBRARY AND AVUTIL_LIBRARY)
    add_quadmapper_test(test_video_decoder
        video/VideoDecoderSeekTest.cpp
        video/VideoPlayerPlaybackTest.cpp
        video/LoopSpliceSafetyTest.cpp    # TD-30/TD-31 review: splice-safety regressions
        source/HapPlaybackTest.cpp
        source/AsyncSourceLifecycleTest.cpp  # TD-35: opening and freeing sources off the main thread
        video/HapExportTest.cpp          # QM-082 Convert to HAP, end to end
        video/Mp4WriterTest.cpp          # Export video: H.264 MP4 writer
        video/RealMediaPlaybackBench.cpp  # opt-in (DISABLED_), needs local media
        video/HardwareDecodeTest.cpp      # ADR-055; needs local media + a D3D11 video decoder
        video/ColourBarsPlaybackTest.cpp  # ADR-055 colour gate; needs a GL 3.3 context
        video/ZeroCopyPlaybackTest.cpp    # ADR-056; needs local media + WGL_NV_DX_interop2
    )
    target_include_directories(test_video_decoder PRIVATE ${AVCODEC_INCLUDE_DIR})
    target_link_libraries(test_video_decoder PRIVATE quadmapper_source quadmapper_video quadmapper_core glfw glad
        ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY})
    # Pre-made open-GOP/CRA fixtures (no x264/x265 encoder in the linked FFmpeg build):
    # the loop-splice safety checks only branch on real H.264/HEVC NAL headers.
    target_compile_definitions(test_video_decoder PRIVATE
        QM_VIDEO_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/videos"
    )
endif()

# Media module tests
add_quadmapper_test(test_media
    media/MediaLibraryTest.cpp
    media/ImageLoaderTest.cpp
    media/ThumbnailCacheTest.cpp
)
target_link_libraries(test_media PRIVATE quadmapper_media quadmapper_core glad)

# Source module tests (QM-055 camera)
add_quadmapper_test(test_source
    source/CameraSourceTest.cpp
    source/SourceReleaseTest.cpp
    source/CompositionPlayheadTest.cpp
)
target_link_libraries(test_source PRIVATE quadmapper_source quadmapper_core)

# HAP frame parser tests (QM-080 A2) — pure CPU, no GL context required
add_quadmapper_test(test_hap
    source/HapFrameParserTest.cpp
    video/HapFrameEncoderTest.cpp  # QM-082 encoder, checked through the parser
)
target_link_libraries(test_hap PRIVATE quadmapper_source quadmapper_video quadmapper_core)

# Source GL pipeline tests — real GL 3.3 context (skip-if-absent). Proves a video
# file decodes/uploads through SourceManager (ADR-040 P3). Separate target so the
# context-free test_source stays light. Links glfw for the hidden-window context.
add_quadmapper_test(test_source_gl
    source/VideoSourcePipelineTest.cpp
    source/ProceduralPatternRenderTest.cpp
)
target_link_libraries(test_source_gl PRIVATE quadmapper_source quadmapper_core glfw glad)

# Renderer module tests (pure helpers only — no GL context needed)
add_quadmapper_test(test_renderer
    renderer/UvTestGridTest.cpp
)
target_link_libraries(test_renderer PRIVATE quadmapper_renderer quadmapper_core)

# Renderer GL tests — require a real GL 3.3 context (skip-if-absent). Kept in a
# separate target so the pure test_renderer stays context-free. Links glfw for
# the hidden-window context (ADR-040 P1 crop readback).
add_quadmapper_test(test_render_gl
    renderer/CropRenderTest.cpp
    renderer/HapUploadTest.cpp
    renderer/MeshObjectRenderTest.cpp
    renderer/OutputCompositeRenderTest.cpp
    renderer/TextureUploadTest.cpp
    renderer/YuvConverterTest.cpp
)
if(WIN32)
    target_sources(test_render_gl PRIVATE
        renderer/DxInteropTest.cpp  # ADR-056 zero-copy colour gate and fallbacks; needs WGL_NV_DX_interop2
    )
endif()
target_link_libraries(test_render_gl PRIVATE quadmapper_renderer quadmapper_source quadmapper_core glfw glad)

# AI sidecar tests (QM-056). Live round-trip launches the real Python sidecar;
# script path passed in so the test can find it. Skip-if-absent keeps it CI-safe.
# Windows only: macOS has a deliberate stand-in client (src/ai/SidecarClient_mac.cpp).
if(WIN32)
    add_quadmapper_test(test_ai
        ai/SidecarClientTest.cpp
    )
    target_link_libraries(test_ai PRIVATE quadmapper_ai quadmapper_core)
    target_compile_definitions(test_ai PRIVATE
        QM_SIDECAR_SCRIPT="${CMAKE_SOURCE_DIR}/tools/ai-sidecar/sidecar.py"
    )
endif()

# External control tests (ADR-048): OSC parser (with fuzzing), UDP server on loopback,
# MIDI decode and WinMM device handling (no hardware needed), registry, dispatch, learn;
# OSC writer, address patterns, feedback (coalescing, and end to end over loopback),
# 14-bit CC.
add_quadmapper_test(test_control
    control/OscParserTest.cpp
    control/OscServerTest.cpp
    control/MidiTest.cpp
    control/ControlEventQueueTest.cpp
    control/ControlDispatchTest.cpp
    control/OscWriterTest.cpp
    control/OscPatternTest.cpp
    control/OscFeedbackTest.cpp
    control/OscFeedbackLoopbackTest.cpp
    control/Midi14BitTest.cpp
)
target_link_libraries(test_control PRIVATE quadmapper_control quadmapper_core)
if(WIN32)
    target_link_libraries(test_control PRIVATE ws2_32)
endif()

# The editor library (ADR-051 E4), linked alone: no panel, no app code.
# The control targets (editor/ControlBindings.hpp) on real scenes, quads and sources, the
# preferences file, missing media and relocation, replace in show, the mesh-edit
# keyboard workflow, scene takes and slice editing, grid undo, and the live<->DTO
# mapping (editor/ProjectMapping.cpp). Also the editor tests the lower modules' targets
# held until E8: output persistence, the "(modified)" label, and audio from video kept
# on the playback clock (editor/SourceAudio.hpp; real decode and mixer, media tests skip
# without D:\testing Videos).
add_quadmapper_test(test_editor
    editor/ActionRegistryTest.cpp
    editor/EditActionsTest.cpp
    editor/ControlBindingsTest.cpp
    editor/PreferencesTest.cpp
    editor/MissingMediaTest.cpp
    editor/ReplaceMediaTest.cpp
    editor/MeshEditKeysTest.cpp
    editor/SceneTakeRoutingTest.cpp
    editor/GridUndoTest.cpp
    editor/ProjectMappingTest.cpp
    editor/VideoExportPlanTest.cpp
    editor/PerformLockTest.cpp
    editor/OutputPersistenceTest.cpp
    editor/UnsavedIndicatorTest.cpp
    editor/AudioSyncTest.cpp
)
target_link_libraries(test_editor PRIVATE quadmapper_editor)
target_compile_definitions(test_editor PRIVATE
    QM_PROJECT_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/projects"
)

# The ui library: the PanelHost, the Preferences window, the shortcut router, and the
# editor frame's order with the menu's action items against
# headless ImGui, and the calibration view on the canvas (ViewportEditor in a hidden GL
# window; skips without GL).
add_quadmapper_test(test_ui
    ui/PanelHostTest.cpp
    ui/PreferencesWindowTest.cpp
    ui/CalibrationViewEditorTest.cpp
    ui/ShortcutRouterTest.cpp
    ui/EditorUiFrameTest.cpp
    ui/PerformLockUiTest.cpp
    ui/ListPanelHelpersTest.cpp
)
target_link_libraries(test_ui PRIVATE quadmapper_ui glfw)

# The project session (app/ProjectSession.cpp, compiled in here because the app is an
# executable, TD-14). It opens files through ui/AsyncFileDialog. No GL context needed.
# The perform lock sweep (TD-34) drives the real EditorUi with the session in a hidden
# GL window (skips without GL).
add_quadmapper_test(test_app
    app/ProjectSessionTest.cpp
    app/PerformLockSweepTest.cpp
    ${CMAKE_SOURCE_DIR}/src/app/ProjectSession.cpp
)
target_link_libraries(test_app PRIVATE quadmapper_ui quadmapper_editor glfw)

# The AI assist's status paths (app/AiAssist.cpp, compiled in here because the app is an
# executable, TD-14) without a sidecar, camera or GL context.
add_quadmapper_test(test_ai_assist
    app/AiAssistTest.cpp
    app/GrayCodeScanTest.cpp
    ${CMAKE_SOURCE_DIR}/src/app/AiAssist.cpp
    ${CMAKE_SOURCE_DIR}/src/app/GrayCodeScan.cpp
)
target_link_libraries(test_ai_assist PRIVATE quadmapper_editor quadmapper_ai)
