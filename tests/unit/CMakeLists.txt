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
)
target_link_libraries(test_scene PRIVATE quadmapper_scene quadmapper_core)

# Project module tests
add_quadmapper_test(test_project
    project/ProjectTest.cpp
    project/ProjectSerializerTest.cpp
)
target_link_libraries(test_project PRIVATE quadmapper_project quadmapper_scene quadmapper_core)

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
)
target_link_libraries(test_input PRIVATE quadmapper_input quadmapper_core)

# Media module tests
add_quadmapper_test(test_media
    media/MediaLibraryTest.cpp
)
target_link_libraries(test_media PRIVATE quadmapper_media quadmapper_core)

# Source module tests (QM-055 camera)
add_quadmapper_test(test_source
    source/CameraSourceTest.cpp
)
target_link_libraries(test_source PRIVATE quadmapper_source quadmapper_core)

# AI sidecar tests (QM-056). Live round-trip launches the real Python sidecar;
# script path passed in so the test can find it. Skip-if-absent keeps it CI-safe.
add_quadmapper_test(test_ai
    ai/SidecarClientTest.cpp
)
target_link_libraries(test_ai PRIVATE quadmapper_ai quadmapper_core)
target_compile_definitions(test_ai PRIVATE
    QM_SIDECAR_SCRIPT="${CMAKE_SOURCE_DIR}/tools/ai-sidecar/sidecar.py"
)
