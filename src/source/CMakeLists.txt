# Source library - Unified texture source system

add_library(quadmapper_source STATIC
    # Core infrastructure
    FrameData.hpp
    TextureSource.hpp
    SourceManager.hpp
    SourceManager.cpp

    # Static image source
    StaticImageSource.hpp
    StaticImageSource.cpp

    # Video file source
    VideoFileSource.hpp
    VideoFileSource.cpp

    # HAP video file source — demux-only, stages BCn frames (QM-080 A1)
    HapFileSource.hpp
    HapFileSource.cpp

    # Live camera / capture-device source (QM-055)
    CameraDevices.hpp
    CameraDevices.cpp
    CameraSource.hpp
    CameraSource.cpp

    # HAP frame decoder (QM-080 A2) — no GL, no FFmpeg
    hap/HapFormat.hpp
    hap/HapFrameParser.hpp
    hap/HapFrameParser.cpp
    hap/snappy/SnappyDecoder.hpp
    hap/snappy/SnappyDecoder.cpp

    # Procedural sources
    procedural/ProceduralSource.hpp
    procedural/ProceduralSource.cpp
    procedural/ProceduralFactory.hpp
    procedural/ProceduralFactory.cpp
    procedural/SolidColorSource.hpp
    procedural/SolidColorSource.cpp
    procedural/GradientSource.hpp
    procedural/GradientSource.cpp
    procedural/CheckerboardSource.hpp
    procedural/CheckerboardSource.cpp
    procedural/TestPatternSource.hpp
    procedural/TestPatternSource.cpp
    procedural/NoiseSource.hpp
    procedural/NoiseSource.cpp
    procedural/PlasmaSource.hpp
    procedural/PlasmaSource.cpp
)

target_include_directories(quadmapper_source
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_source
    PUBLIC
        quadmapper_core
        quadmapper_renderer
        quadmapper_media
        quadmapper_video
        glad
        glm::glm
)

# HapFileSource + the HAP codec probe in SourceManager demux with FFmpeg directly
# (no FFmpeg decoder — ADR-044). Mirror quadmapper_video's PRIVATE FFmpeg wiring so
# the libav* headers and QUADMAPPER_HAS_FFMPEG are visible to this lib's .cpp files.
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVFORMAT_LIBRARY avformat)
find_library(AVUTIL_LIBRARY avutil)
if(AVCODEC_LIBRARY AND AVFORMAT_LIBRARY AND AVUTIL_LIBRARY)
    target_include_directories(quadmapper_source PRIVATE ${AVCODEC_INCLUDE_DIR})
    target_link_libraries(quadmapper_source
        PRIVATE
            ${AVCODEC_LIBRARY}
            ${AVFORMAT_LIBRARY}
            ${AVUTIL_LIBRARY}
    )
    target_compile_definitions(quadmapper_source PRIVATE QUADMAPPER_HAS_FFMPEG=1)
endif()

set_target_properties(quadmapper_source PROPERTIES
    FOLDER "Libraries"
)
