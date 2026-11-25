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
        # Windows Imaging Component for TIFF decoding (OS libraries, no new dependency)
        windowscodecs
        ole32
)

set_target_properties(quadmapper_media PROPERTIES
    FOLDER "Libraries"
)
