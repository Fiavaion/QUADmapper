# Dear ImGui - Immediate Mode GUI Library
# Repository: https://github.com/ocornut/imgui
# Branch: docking (for docking/multi-viewport support)
#
# Setup instructions:
# 1. Clone imgui: git clone -b docking https://github.com/ocornut/imgui.git
# 2. Copy the following files to this directory:
#    - imgui.h, imgui.cpp
#    - imgui_demo.cpp, imgui_draw.cpp, imgui_tables.cpp, imgui_widgets.cpp
#    - imgui_internal.h, imstb_rectpack.h, imstb_textedit.h, imstb_truetype.h
#    - backends/imgui_impl_glfw.h, backends/imgui_impl_glfw.cpp
#    - backends/imgui_impl_opengl3.h, backends/imgui_impl_opengl3.cpp
#    - backends/imgui_impl_opengl3_loader.h

find_package(glfw3 CONFIG REQUIRED)

add_library(imgui STATIC
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    imgui_impl_glfw.cpp
    imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(imgui PUBLIC
    glfw
    glad
)

# ImGui configuration
target_compile_definitions(imgui PUBLIC
    IMGUI_IMPL_OPENGL_LOADER_GLAD
)

# Enable docking
target_compile_definitions(imgui PUBLIC
    IMGUI_ENABLE_DOCKING
)
