#pragma once
// d3d11.h includes d3d10_1.h, and d3d10_1.h refuses to compile once d3d10.h is already in.
// Taking d3d10_1.h first lets both DXGI backends sit in one translation unit.
#include <d3d10_1.h>
#include <d3d10.h>
#include <stdint.h>

struct ImDrawData;

// The Direct3D 10 counterpart of overlay-dx11. Direct3D 10 has no separate device context, so the
// device takes every call the Direct3D 11 backend makes on its context. Everything else, including
// how the render target is taken from the Title and checked, is the same.

bool OverlayDX10Init(ID3D10Device* d3d_device, const DXGI_SWAP_CHAIN_DESC* swap_chain_description);
void OverlayDX10Shutdown();
void OverlayDX10OnDeviceReset(const DXGI_SWAP_CHAIN_DESC* swap_chain_description);
void OverlayDX10InvalidateDeviceObjects();
void OverlayDX10MarkFontsDirty();
bool OverlayDX10BeginFrame();
// Whether the last drawn frame went onto the target the Title presents.
bool OverlayDX10TargetAccepted();
void OverlayDX10RenderDrawData(ImDrawData* draw_data);
void* OverlayDX10CreateTextureRgba(uint32_t width, uint32_t height, const uint8_t* rgba);
void OverlayDX10DestroyTexture(void* texture);
