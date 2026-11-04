#include "image-decode.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h>

bool XllnImageDecodeRgba(const uint8_t* image_data, size_t image_data_size, uint32_t* result_width, uint32_t* result_height, uint8_t** result_rgba)
{
	if (!image_data || !image_data_size || !result_width || !result_height || !result_rgba) {
		return false;
	}
	int width = 0;
	int height = 0;
	stbi_uc* rgba = stbi_load_from_memory(image_data, (int)image_data_size, &width, &height, NULL, 4);
	if (!rgba || width <= 0 || height <= 0) {
		if (rgba) {
			stbi_image_free(rgba);
		}
		return false;
	}
	*result_width = (uint32_t)width;
	*result_height = (uint32_t)height;
	*result_rgba = rgba;
	return true;
}

void XllnImageFree(uint8_t* rgba)
{
	if (rgba) {
		stbi_image_free(rgba);
	}
}
