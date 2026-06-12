# Video library - FFmpeg decoding and playback

add_library(quadmapper_video STATIC
    VideoDecoder.hpp
    VideoDecoder.cpp
    VideoPlayer.hpp
    VideoPlayer.cpp
    VideoPlaybackState.hpp
    VideoPlaybackState.cpp
    FrameBuffer.hpp
    FrameBuffer.cpp
    TimelineSyncManager.hpp
    TimelineSyncManager.cpp
)

target_include_directories(quadmapper_video
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

# Find FFmpeg via find_path/find_library (works with vcpkg)
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVFORMAT_LIBRARY avformat)
find_library(AVUTIL_LIBRARY avutil)
find_library(SWSCALE_LIBRARY swscale)
find_library(SWRESAMPLE_LIBRARY swresample)

target_link_libraries(quadmapper_video
    PUBLIC
        quadmapper_core
        quadmapper_media
)

# Link FFmpeg libraries if found
if(AVCODEC_LIBRARY AND AVFORMAT_LIBRARY AND AVUTIL_LIBRARY AND SWSCALE_LIBRARY)
    target_include_directories(quadmapper_video PRIVATE ${AVCODEC_INCLUDE_DIR})
    target_link_libraries(quadmapper_video
        PRIVATE
            ${AVCODEC_LIBRARY}
            ${AVFORMAT_LIBRARY}
            ${AVUTIL_LIBRARY}
            ${SWSCALE_LIBRARY}
            ${SWRESAMPLE_LIBRARY}
    )
    target_compile_definitions(quadmapper_video PRIVATE QUADMAPPER_HAS_FFMPEG=1)
    message(STATUS "FFmpeg found and linked")
else()
    message(WARNING "FFmpeg not found - video features will be disabled")
endif()

set_target_properties(quadmapper_video PROPERTIES
    FOLDER "Libraries"
)
