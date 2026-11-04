#pragma once
#include <winsock2.h>
#include <Windows.h>
#include <stdint.h>
#include <imgui.h>

struct ImDrawData;

// One entry point per drawing operation the overlay needs, so that everything above this line is
// free of the graphics API. The backend behind it is chosen once, when the Title hands over its
// device. Every call runs under xlln_critsec_overlay_frame, on the thread the Title calls
// XLiveRender from or on the thread of a device callback.

// device_api is one of XLLN_OVERLAY_DEVICE_API_* from xlln/xlln-overlay-api.hpp.
// presentation_parameters is a D3DPRESENT_PARAMETERS* for Direct3D 9 and 9Ex, and a
// DXGI_SWAP_CHAIN_DESC* for Direct3D 10 and 11.
bool OverlayRendererInit(uint32_t device_api, IUnknown* d3d_device, const void* presentation_parameters);
void OverlayRendererShutdown();
// The attached API, or 0 when no device is attached.
uint32_t OverlayRendererDeviceApi();
// Releases GPU objects only. Safe to call any time on the render thread.
void OverlayRendererInvalidateDeviceObjects();
// Drop just the font texture so it is rebuilt from the current atlas on the next frame.
void OverlayRendererMarkFontsDirty();
void OverlayRendererOnDeviceReset(const void* presentation_parameters);
// Returns false when the frame must be skipped, which each backend decides for its own reasons.
bool OverlayRendererBeginFrame();
void OverlayRendererRenderDrawData(ImDrawData* draw_data);
// False when the backend refused the Title's bound render target on the frame just drawn, so
// nothing reached the screen. Always true on Direct3D 9.
bool OverlayRendererTargetAccepted();
// Cached per height and rebuilt when target_height changes. Null until the device can build the
// texture; callers then use the text wordmark.
ImTextureID OverlayRendererBrandmarkTexture(int target_height, int* out_width, int* out_height);
// Straight-alpha RGBA, four bytes per texel and no padding between rows. 0 when the device
// refuses the texture.
ImTextureID OverlayRendererCreateTextureRgba(uint32_t width, uint32_t height, const uint8_t* rgba);
void OverlayRendererDestroyTexture(ImTextureID texture);
