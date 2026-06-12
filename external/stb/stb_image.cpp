/*
 * stb_image implementation
 *
 * This file creates the implementation of stb_image.
 * Replace stb_image.h with the actual header from:
 * https://github.com/nothings/stb/blob/master/stb_image.h
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Placeholder implementations - will be replaced by actual stb_image
#ifndef STBI_INCLUDE_STB_IMAGE_H

unsigned char* stbi_load(const char*, int* x, int* y, int* channels, int) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (channels) *channels = 0;
    return nullptr;
}

unsigned char* stbi_load_from_memory(const unsigned char*, int, int* x, int* y, int* channels, int) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (channels) *channels = 0;
    return nullptr;
}

void stbi_image_free(void*) {}

const char* stbi_failure_reason(void) {
    return "stb_image placeholder - download actual stb_image.h";
}

void stbi_set_flip_vertically_on_load(int) {}

#endif
