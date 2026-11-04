#include <winsock2.h>
#include <Windows.h>
#include "overlay-textures.hpp"
#include "overlay-renderer.hpp"
#include "../xlln/xlln.hpp"
#include "../utils/image-decode.hpp"
#include <map>

// A null texture marks a permanent decode/create failure so it is not retried.
static std::map<uint32_t, ImTextureID> overlay_spa_textures;

// Decoding is a PNG decode plus a texture upload, on the Title's render thread. One is not felt;
// a panel of achievement art asking for all of them on the frame it opens is. Whatever is over the
// budget is left uncached, so the next frames pick it up.
#define OVERLAY_TEXTURE_DECODES_PER_FRAME 2
static uint32_t overlay_texture_decodes_this_frame = 0;

void OverlayTexturesBeginFrame()
{
	overlay_texture_decodes_this_frame = 0;
}

ImTextureID OverlayTextureForSpaImage(uint32_t image_id)
{
	auto itrTexture = overlay_spa_textures.find(image_id);
	if (itrTexture != overlay_spa_textures.end()) {
		return itrTexture->second;
	}

	if (!OverlayRendererDeviceApi()) {
		// Not cached: retried once a device exists.
		return 0;
	}
	if (overlay_texture_decodes_this_frame >= OVERLAY_TEXTURE_DECODES_PER_FRAME) {
		// Not cached: retried on the next frame.
		return 0;
	}
	overlay_texture_decodes_this_frame++;

	const uint8_t* imageData = 0;
	size_t imageDataSize = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint8_t* rgba = 0;
	ImTextureID texture = 0;
	if (XllnSpaGetImage(image_id, &imageData, &imageDataSize)
		&& XllnImageDecodeRgba(imageData, imageDataSize, &width, &height, &rgba)
	) {
		texture = OverlayRendererCreateTextureRgba(width, height, rgba);
	}
	if (rgba) {
		XllnImageFree(rgba);
	}

	overlay_spa_textures[image_id] = texture;
	return texture;
}

void OverlayTexturesReleaseAll()
{
	for (auto &itrTexture : overlay_spa_textures) {
		if (itrTexture.second) {
			OverlayRendererDestroyTexture(itrTexture.second);
		}
	}
	overlay_spa_textures.clear();
}
