#include <winsock2.h>
#include <Windows.h>
#include "overlay-brandmark.hpp"
#include "../utils/image-decode.hpp"
#include "../xlln/xlln.hpp"
#include "../xlln/debug-log.hpp"
#include <stdlib.h>

// The compact XLLN brand mark (XLLN-Logo-Compact.svg by Glitchy Scripts, the project's own logo,
// CC BY-NC-SA 4.0) shipped as a PNG resource and decoded to a straight-alpha RGBA master on first
// use. Only the render thread reaches this, so the cache needs no lock, and the master is kept for
// the process lifetime because the texture is rebuilt whenever the band height changes.
static uint8_t* overlay_brandmark_rgba = 0;
static uint32_t overlay_brandmark_width = 0;
static uint32_t overlay_brandmark_height = 0;
static bool overlay_brandmark_decoded = false;

// Decodes the brand mark resource, once. A failure is remembered so a missing or corrupt resource
// costs one attempt rather than one per texture rebuild.
static bool OverlayBrandmarkMaster_()
{
	if (overlay_brandmark_decoded) {
		return overlay_brandmark_rgba != 0;
	}
	overlay_brandmark_decoded = true;

	HRSRC imageResHandle = FindResourceW(xlln_hModule, XLLN_LOGO_COMPACT_OVERLAY, RT_IMAGE);
	HGLOBAL imageResDataHandle = imageResHandle ? LoadResource(xlln_hModule, imageResHandle) : 0;
	// Locked resources do not need unlocking; the pages stay mapped for the module's lifetime.
	const void* imageData = imageResDataHandle ? LockResource(imageResDataHandle) : 0;
	DWORD imageDataSize = imageResHandle ? SizeofResource(xlln_hModule, imageResHandle) : 0;
	if (!imageData || !imageDataSize) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s brand mark resource is missing."
			, __func__
		);
		return false;
	}

	if (!XllnImageDecodeRgba((const uint8_t*)imageData, imageDataSize, &overlay_brandmark_width, &overlay_brandmark_height, &overlay_brandmark_rgba)
		|| !overlay_brandmark_width
		|| !overlay_brandmark_height) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s brand mark PNG did not decode."
			, __func__
		);
		overlay_brandmark_rgba = 0;
		return false;
	}

	return true;
}

unsigned char* OverlayBrandmarkScaled(int target_height, int* out_width, int* out_height)
{
	if (target_height < 1 || !OverlayBrandmarkMaster_()) {
		return 0;
	}
	const int masterWidth = (int)overlay_brandmark_width;
	const int masterHeight = (int)overlay_brandmark_height;
	if (target_height > masterHeight) {
		target_height = masterHeight;
	}
	int targetWidth = (int)((float)masterWidth * (float)target_height / (float)masterHeight + 0.5f);
	if (targetWidth < 1) {
		targetWidth = 1;
	}
	unsigned char* scaled = (unsigned char*)malloc((size_t)targetWidth * (size_t)target_height * 4);
	if (!scaled) {
		return 0;
	}
	for (int targetY = 0; targetY < target_height; targetY++) {
		int sourceY0 = targetY * masterHeight / target_height;
		int sourceY1 = (targetY + 1) * masterHeight / target_height;
		if (sourceY1 <= sourceY0) {
			sourceY1 = sourceY0 + 1;
		}
		for (int targetX = 0; targetX < targetWidth; targetX++) {
			int sourceX0 = targetX * masterWidth / targetWidth;
			int sourceX1 = (targetX + 1) * masterWidth / targetWidth;
			if (sourceX1 <= sourceX0) {
				sourceX1 = sourceX0 + 1;
			}
			unsigned int sumRedAlpha = 0;
			unsigned int sumGreenAlpha = 0;
			unsigned int sumBlueAlpha = 0;
			unsigned int sumAlpha = 0;
			unsigned int texelCount = 0;
			for (int sourceY = sourceY0; sourceY < sourceY1; sourceY++) {
				for (int sourceX = sourceX0; sourceX < sourceX1; sourceX++) {
					const unsigned char* texel = overlay_brandmark_rgba + (((size_t)sourceY * masterWidth) + sourceX) * 4;
					sumRedAlpha += (unsigned int)texel[0] * texel[3];
					sumGreenAlpha += (unsigned int)texel[1] * texel[3];
					sumBlueAlpha += (unsigned int)texel[2] * texel[3];
					sumAlpha += texel[3];
					texelCount++;
				}
			}
			unsigned char* destination = scaled + (((size_t)targetY * targetWidth) + targetX) * 4;
			if (sumAlpha) {
				destination[0] = (unsigned char)(sumRedAlpha / sumAlpha);
				destination[1] = (unsigned char)(sumGreenAlpha / sumAlpha);
				destination[2] = (unsigned char)(sumBlueAlpha / sumAlpha);
				destination[3] = (unsigned char)(sumAlpha / texelCount);
			}
			else {
				destination[0] = 0;
				destination[1] = 0;
				destination[2] = 0;
				destination[3] = 0;
			}
		}
	}
	if (out_width) {
		*out_width = targetWidth;
	}
	if (out_height) {
		*out_height = target_height;
	}
	return scaled;
}
