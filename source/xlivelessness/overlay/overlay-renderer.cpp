#include <winsock2.h>
#include <Windows.h>
#include "overlay-renderer.hpp"
#include "overlay-dx9.hpp"
#include "overlay-dx10.hpp"
#include "overlay-dx11.hpp"
#include "overlay-brandmark.hpp"
#include "overlay-textures.hpp"
#include "../xlln/xlln-overlay-api.hpp"
#include "../xlln/debug-log.hpp"
#include <d3d9.h>
#include <stdlib.h>

static uint32_t overlay_renderer_device_api = 0;
// The brand mark sits here rather than in a backend, because it is the same picture whichever
// graphics API draws it.
static ImTextureID overlay_renderer_brandmark_texture = 0;
static int overlay_renderer_brandmark_built_height = 0;
static int overlay_renderer_brandmark_width = 0;
static int overlay_renderer_brandmark_height = 0;

uint32_t OverlayRendererDeviceApi()
{
	return overlay_renderer_device_api;
}

ImTextureID OverlayRendererCreateTextureRgba(uint32_t width, uint32_t height, const uint8_t* rgba)
{
	switch (overlay_renderer_device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D9:
		case XLLN_OVERLAY_DEVICE_API_D3D9EX:
			return (ImTextureID)OverlayDX9CreateTextureRgba(width, height, rgba);
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			return (ImTextureID)OverlayDX10CreateTextureRgba(width, height, rgba);
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			return (ImTextureID)OverlayDX11CreateTextureRgba(width, height, rgba);
	}
	return 0;
}

void OverlayRendererDestroyTexture(ImTextureID texture)
{
	if (!texture) {
		return;
	}
	switch (overlay_renderer_device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D9:
		case XLLN_OVERLAY_DEVICE_API_D3D9EX:
			OverlayDX9DestroyTexture((void*)texture);
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			OverlayDX10DestroyTexture((void*)texture);
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			OverlayDX11DestroyTexture((void*)texture);
			break;
	}
}

static void OverlayRendererReleaseBrandmark()
{
	if (overlay_renderer_brandmark_texture) {
		OverlayRendererDestroyTexture(overlay_renderer_brandmark_texture);
		overlay_renderer_brandmark_texture = 0;
	}
	overlay_renderer_brandmark_built_height = 0;
	overlay_renderer_brandmark_width = 0;
	overlay_renderer_brandmark_height = 0;
}

bool OverlayRendererInit(uint32_t device_api, IUnknown* d3d_device, const void* presentation_parameters)
{
	OverlayRendererShutdown();
	if (!d3d_device || !presentation_parameters) {
		return false;
	}

	bool attached = false;
	switch (device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D9:
		case XLLN_OVERLAY_DEVICE_API_D3D9EX:
			attached = OverlayDX9Init((IDirect3DDevice9*)d3d_device, device_api == XLLN_OVERLAY_DEVICE_API_D3D9EX);
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			attached = OverlayDX10Init((ID3D10Device*)d3d_device, (const DXGI_SWAP_CHAIN_DESC*)presentation_parameters);
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			attached = OverlayDX11Init((ID3D11Device*)d3d_device, (const DXGI_SWAP_CHAIN_DESC*)presentation_parameters);
			break;
		default:
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
				, "%s device_api (%u) has no overlay renderer."
				, __func__
				, device_api
			);
			return false;
	}
	if (!attached) {
		return false;
	}

	overlay_renderer_device_api = device_api;
	return true;
}

void OverlayRendererShutdown()
{
	// Both caches hand their textures back through the backend that made them, so they have to go
	// before the backend does.
	OverlayTexturesReleaseAll();
	OverlayRendererReleaseBrandmark();

	switch (overlay_renderer_device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D9:
		case XLLN_OVERLAY_DEVICE_API_D3D9EX:
			OverlayDX9Shutdown();
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			OverlayDX10Shutdown();
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			OverlayDX11Shutdown();
			break;
	}
	overlay_renderer_device_api = 0;
}

void OverlayRendererInvalidateDeviceObjects()
{
	OverlayRendererReleaseBrandmark();
	switch (overlay_renderer_device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D9:
		case XLLN_OVERLAY_DEVICE_API_D3D9EX:
			OverlayDX9InvalidateDeviceObjects();
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			OverlayDX10InvalidateDeviceObjects();
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			OverlayDX11InvalidateDeviceObjects();
			break;
	}
}

void OverlayRendererMarkFontsDirty()
{
	switch (overlay_renderer_device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D9:
		case XLLN_OVERLAY_DEVICE_API_D3D9EX:
			OverlayDX9MarkFontsDirty();
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			OverlayDX10MarkFontsDirty();
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			OverlayDX11MarkFontsDirty();
			break;
	}
}

void OverlayRendererOnDeviceReset(const void* presentation_parameters)
{
	OverlayRendererReleaseBrandmark();
	switch (overlay_renderer_device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D9:
		case XLLN_OVERLAY_DEVICE_API_D3D9EX:
			OverlayDX9InvalidateDeviceObjects();
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			OverlayDX10OnDeviceReset((const DXGI_SWAP_CHAIN_DESC*)presentation_parameters);
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			OverlayDX11OnDeviceReset((const DXGI_SWAP_CHAIN_DESC*)presentation_parameters);
			break;
	}
}

bool OverlayRendererBeginFrame()
{
	switch (overlay_renderer_device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D9:
		case XLLN_OVERLAY_DEVICE_API_D3D9EX:
			return OverlayDX9BeginFrame();
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			return OverlayDX10BeginFrame();
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			return OverlayDX11BeginFrame();
	}
	return false;
}

bool OverlayRendererTargetAccepted()
{
	switch (overlay_renderer_device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			return OverlayDX10TargetAccepted();
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			return OverlayDX11TargetAccepted();
	}
	// A Direct3D 9 backend takes the back buffer itself, so there is no target to disagree with.
	return true;
}

void OverlayRendererRenderDrawData(ImDrawData* draw_data)
{
	switch (overlay_renderer_device_api) {
		case XLLN_OVERLAY_DEVICE_API_D3D9:
		case XLLN_OVERLAY_DEVICE_API_D3D9EX:
			OverlayDX9RenderDrawData(draw_data);
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D10:
			OverlayDX10RenderDrawData(draw_data);
			break;
		case XLLN_OVERLAY_DEVICE_API_D3D11:
			OverlayDX11RenderDrawData(draw_data);
			break;
	}
}

ImTextureID OverlayRendererBrandmarkTexture(int target_height, int* out_width, int* out_height)
{
	if (!overlay_renderer_device_api || target_height < 1) {
		return 0;
	}
	if (!overlay_renderer_brandmark_texture || overlay_renderer_brandmark_built_height != target_height) {
		OverlayRendererReleaseBrandmark();

		int scaledWidth = 0;
		int scaledHeight = 0;
		unsigned char* scaledPixels = OverlayBrandmarkScaled(target_height, &scaledWidth, &scaledHeight);
		if (!scaledPixels) {
			return 0;
		}
		overlay_renderer_brandmark_texture = OverlayRendererCreateTextureRgba((uint32_t)scaledWidth, (uint32_t)scaledHeight, scaledPixels);
		free(scaledPixels);
		if (!overlay_renderer_brandmark_texture) {
			return 0;
		}
		overlay_renderer_brandmark_built_height = target_height;
		overlay_renderer_brandmark_width = scaledWidth;
		overlay_renderer_brandmark_height = scaledHeight;
	}

	if (out_width) {
		*out_width = overlay_renderer_brandmark_width;
	}
	if (out_height) {
		*out_height = overlay_renderer_brandmark_height;
	}
	return overlay_renderer_brandmark_texture;
}
