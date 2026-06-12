# stb - Single-file public domain libraries
# Repository: https://github.com/nothings/stb
#
# Setup instructions:
# 1. Download stb_image.h from https://github.com/nothings/stb/blob/master/stb_image.h
# 2. Place it in this directory
#
# This creates a static library with the implementation

add_library(stb STATIC
    stb_image.cpp
)

target_include_directories(stb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
