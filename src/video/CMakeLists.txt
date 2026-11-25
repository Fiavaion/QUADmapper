# Video library - FFmpeg decoding and playback

add_library(quadmapper_video STATIC
    VideoDecoder.hpp
    VideoDecoder.cpp
    PngWriter.hpp
    PngWriter.cpp
    HapFrameEncoder.hpp
    HapFrameEncoder.cpp
    HapExport.hpp
    HapExport.cpp
    Mp4Writer.hpp
    Mp4Writer.cpp
    VideoPlayer.hpp
    VideoPlayer.cpp
    VideoPlaybackState.hpp
    VideoPlaybackState.cpp
    PlaybackClock.hpp
    PlaybackClock.cpp
    PlaybackTransport.hpp
    PlaybackTransport.cpp
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
find_library(AVDEVICE_LIBRARY avdevice)  # webcam capture (dshow) — QM-055
find_library(AVUTIL_LIBRARY avutil)
find_library(SWSCALE_LIBRARY swscale)
find_library(SWRESAMPLE_LIBRARY swresample)

target_link_libraries(quadmapper_video
    PUBLIC
        quadmapper_core
        quadmapper_media
    PRIVATE
        stb  # stb_dxt: BC1/BC3 blocks for Convert to HAP
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
    # libavdevice is optional: enables webcam capture (dshow). Linked when present.
    if(AVDEVICE_LIBRARY)
        target_link_libraries(quadmapper_video PRIVATE ${AVDEVICE_LIBRARY})
        target_compile_definitions(quadmapper_video PRIVATE QUADMAPPER_HAS_AVDEVICE=1)
        message(STATUS "FFmpeg avdevice found - webcam capture enabled")
    else()
        message(STATUS "FFmpeg avdevice NOT found - webcam capture disabled")
    endif()
    target_compile_definitions(quadmapper_video PRIVATE QUADMAPPER_HAS_FFMPEG=1)
    if(TARGET quadmapper_ffmpeg_frameworks)
        target_link_libraries(quadmapper_video PUBLIC quadmapper_ffmpeg_frameworks)
    endif()
    message(STATUS "FFmpeg found and linked")
else()
    message(WARNING "FFmpeg not found - video features will be disabled")
endif()

set_target_properties(quadmapper_video PROPERTIES
    FOLDER "Libraries"
)
