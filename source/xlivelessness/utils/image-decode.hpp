#pragma once
#include <stdint.h>
#include <cstddef>

// Decodes a PNG (the format SPA images use) into tightly packed RGBA8.
// The result is heap-allocated; release it with XllnImageFree.
bool XllnImageDecodeRgba(const uint8_t* image_data, size_t image_data_size, uint32_t* result_width, uint32_t* result_height, uint8_t** result_rgba);
void XllnImageFree(uint8_t* rgba);
