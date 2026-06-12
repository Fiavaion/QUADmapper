# Audio library - Mixing, decoding, and multi-output routing

add_library(quadmapper_audio STATIC
    AudioFrame.hpp
    AudioBuffer.hpp
    AudioBuffer.cpp
    AudioDecoder.hpp
    AudioDecoder.cpp
    AudioEngine.hpp
    AudioEngine.cpp
    AudioMixer.hpp
    AudioMixer.cpp
    AudioSource.hpp
    AudioSource.cpp
    AudioDevice.hpp
    AudioDevice.cpp
)

target_include_directories(quadmapper_audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_audio
    PUBLIC
        quadmapper_core
)

# Find miniaudio via vcpkg (header-only library)
find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
if(MINIAUDIO_INCLUDE_DIR)
    target_include_directories(quadmapper_audio PRIVATE ${MINIAUDIO_INCLUDE_DIR})
    message(STATUS "miniaudio found for audio library")
else()
    message(FATAL_ERROR "miniaudio not found - required for audio mixer")
endif()

# Find FFmpeg via find_path/find_library (works with vcpkg)
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
find_library(AVCODEC_LIBRARY avcodec)
find_library(AVFORMAT_LIBRARY avformat)
find_library(AVUTIL_LIBRARY avutil)
find_library(SWRESAMPLE_LIBRARY swresample)

# Link FFmpeg libraries if found (for AudioDecoder)
if(AVCODEC_LIBRARY AND AVFORMAT_LIBRARY AND AVUTIL_LIBRARY AND SWRESAMPLE_LIBRARY)
    target_include_directories(quadmapper_audio PRIVATE ${AVCODEC_INCLUDE_DIR})
    target_link_libraries(quadmapper_audio
        PRIVATE
            ${AVCODEC_LIBRARY}
            ${AVFORMAT_LIBRARY}
            ${AVUTIL_LIBRARY}
            ${SWRESAMPLE_LIBRARY}
    )
    target_compile_definitions(quadmapper_audio PRIVATE QUADMAPPER_HAS_FFMPEG=1)
    message(STATUS "FFmpeg found for audio library")
else()
    message(WARNING "FFmpeg not found - audio decoding features will be disabled")
endif()

set_target_properties(quadmapper_audio PROPERTIES
    FOLDER "Libraries"
)
