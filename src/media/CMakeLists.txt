# Media library - Image loading and media management

add_library(quadmapper_media STATIC
    MediaLibrary.hpp
    MediaLibrary.cpp
    MediaItem.hpp
    MediaItem.cpp
    ImageLoader.hpp
    ImageLoader.cpp
    FolderWatcher.hpp
    FolderWatcher.cpp
    ThumbnailCache.hpp
    ThumbnailCache.cpp
)

target_include_directories(quadmapper_media
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_media
    PUBLIC
        quadmapper_core
    PRIVATE
        stb
        glad
)
if(WIN32)
    # Windows Imaging Component for TIFF decoding (OS libraries, no new dependency)
    target_link_libraries(quadmapper_media PRIVATE windowscodecs ole32)
elseif(APPLE)
    # ImageIO for TIFF decoding (system frameworks, no new dependency)
    target_sources(quadmapper_media PRIVATE ImageLoaderTiffMac.hpp ImageLoaderTiff_mac.cpp)
    target_link_libraries(quadmapper_media PRIVATE
        "-framework ImageIO" "-framework CoreGraphics" "-framework CoreFoundation")
endif()

set_target_properties(quadmapper_media PROPERTIES
    FOLDER "Libraries"
)
