# stb - Single-file public domain libraries
# Repository: https://github.com/nothings/stb
#
# Setup instructions:
# 1. Download stb_image.h from https://github.com/nothings/stb/blob/master/stb_image.h
# 2. Download stb_dxt.h (v1.12) from https://github.com/nothings/stb/blob/master/stb_dxt.h
#    (BC1/BC3 compressor for Convert to HAP, QM-082)
# 3. Place them in this directory, unmodified
#
# This creates a static library with the implementations

add_library(stb STATIC
    stb_image.cpp
    stb_dxt.cpp
)

target_include_directories(stb PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
