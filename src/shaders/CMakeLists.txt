# Shaders library - GLSL shader sources

# Read shader source files
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/quad.vert QUAD_VERT_SOURCE)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/quad.frag QUAD_FRAG_SOURCE)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/mesh3d.vert MESH3D_VERT_SOURCE)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/outline.vert OUTLINE_VERT_SOURCE)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/outline.frag OUTLINE_FRAG_SOURCE)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/handle.vert HANDLE_VERT_SOURCE)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/handle.frag HANDLE_FRAG_SOURCE)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/slice.vert SLICE_VERT_SOURCE)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/slice.frag SLICE_FRAG_SOURCE)

# Configure shader source header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/ShaderSources.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/ShaderSources.hpp
    @ONLY
)

add_library(quadmapper_shaders INTERFACE)

target_include_directories(quadmapper_shaders
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

# Shader files for IDE visibility
set(SHADER_FILES
    quad.vert
    quad.frag
    mesh3d.vert
    outline.vert
    outline.frag
    handle.vert
    handle.frag
    slice.vert
    slice.frag
)

# Add shader files to a custom target for IDE visibility
add_custom_target(quadmapper_shader_files SOURCES ${SHADER_FILES})
set_target_properties(quadmapper_shader_files PROPERTIES FOLDER "Shaders")
