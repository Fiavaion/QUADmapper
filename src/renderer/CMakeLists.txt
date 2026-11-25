# Renderer library - OpenGL rendering pipeline

add_library(quadmapper_renderer STATIC
    Renderer.hpp
    Renderer.cpp
    ShaderProgram.hpp
    ShaderProgram.cpp
    Texture.hpp
    Texture.cpp
    YuvConverter.hpp
    YuvConverter.cpp
    CompressedUpload.hpp
    CompressedUpload.cpp
    DxInterop.hpp
    DxInterop.cpp
    GpuCapabilities.hpp
    GpuCapabilities.cpp
    Framebuffer.hpp
    Framebuffer.cpp
    UvTestGrid.hpp
    UvTestGrid.cpp
)

target_include_directories(quadmapper_renderer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>
)

target_link_libraries(quadmapper_renderer
    PUBLIC
        quadmapper_core
        quadmapper_scene
        quadmapper_shaders
        glad
        glm::glm
    PRIVATE
        glfw
        stb
)

set_target_properties(quadmapper_renderer PROPERTIES
    FOLDER "Libraries"
)
