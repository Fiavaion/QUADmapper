# GLAD - OpenGL Loader Generator
# Generate at: https://glad.dav1d.de/
# Settings: OpenGL 3.3, Core Profile, Generate a loader

add_library(glad STATIC
    src/glad.c
)

target_include_directories(glad PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# GLAD needs to link against OpenGL
if(WIN32)
    target_link_libraries(glad PUBLIC opengl32)
endif()
