# Unit tests - mirror src/ structure

# Core module tests
add_quadmapper_test(test_core
    core/UUIDTest.cpp
    core/ResultTest.cpp
    core/LoggerTest.cpp
    core/TimerTest.cpp
    core/CommandHistoryTest.cpp
    core/PerformanceMonitorTest.cpp
    core/ErrorNotificationTest.cpp
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
)
target_link_libraries(test_audio PRIVATE quadmapper_audio quadmapper_core)

# Input module tests
add_quadmapper_test(test_input
    input/KeyboardShortcutsTest.cpp
    input/VertexKeyboardTest.cpp
)
target_link_libraries(test_input PRIVATE quadmapper_input quadmapper_core imgui)

# Video module tests (QM-088) - pure frame pacing / decode backoff, no FFmpeg file I/O
add_quadmapper_test(test_video
    video/FrameSelectionTest.cpp
    video/DecodeBackoffTest.cpp
)
target_link_libraries(test_video PRIVATE quadmapper_video quadmapper_core)

# VideoDecoder seek/drain tests (QM-091) - encode a tiny B-frame clip with the linked
# FFmpeg at test time, so they need its headers and libraries directly.
if(AVCODEC_LIBRARY AND AVFORMAT_LIBRARY AND AVUTIL_LIBRARY)
    add_quadmapper_test(test_video_decoder
        video/VideoDecoderSeekTest.cpp
    )
    target_include_directories(test_video_decoder PRIVATE ${AVCODEC_INCLUDE_DIR})
    target_link_libraries(test_video_decoder PRIVATE quadmapper_video quadmapper_core
        ${AVCODEC_LIBRARY} ${AVFORMAT_LIBRARY} ${AVUTIL_LIBRARY})
endif()

# Media module tests
add_quadmapper_test(test_media
    media/MediaLibraryTest.cpp
)
target_link_libraries(test_media PRIVATE quadmapper_media quadmapper_core)

# Source module tests (QM-055 camera)
add_quadmapper_test(test_source
    source/CameraSourceTest.cpp
    source/SourceReleaseTest.cpp
)
target_link_libraries(test_source PRIVATE quadmapper_source quadmapper_core)

# HAP frame parser tests (QM-080 A2) — pure CPU, no GL context required
add_quadmapper_test(test_hap
    source/HapFrameParserTest.cpp
)
target_link_libraries(test_hap PRIVATE quadmapper_source quadmapper_core)

# Source GL pipeline tests — real GL 3.3 context (skip-if-absent). Proves a video
# file decodes/uploads through SourceManager (ADR-040 P3). Separate target so the
# context-free test_source stays light. Links glfw for the hidden-window context.
add_quadmapper_test(test_source_gl
    source/VideoSourcePipelineTest.cpp
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
)
target_link_libraries(test_render_gl PRIVATE quadmapper_renderer quadmapper_source quadmapper_core glfw glad)

# AI sidecar tests (QM-056). Live round-trip launches the real Python sidecar;
# script path passed in so the test can find it. Skip-if-absent keeps it CI-safe.
add_quadmapper_test(test_ai
    ai/SidecarClientTest.cpp
)
target_link_libraries(test_ai PRIVATE quadmapper_ai quadmapper_core)
target_compile_definitions(test_ai PRIVATE
    QM_SIDECAR_SCRIPT="${CMAKE_SOURCE_DIR}/tools/ai-sidecar/sidecar.py"
)
