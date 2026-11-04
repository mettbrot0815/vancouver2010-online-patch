#pragma once
#include <d3d11.h>
#include <stdint.h>

struct ImDrawData;

// Derived from Dear ImGui's imgui_impl_dx11 backend (MIT), with the shaders precompiled and with
// the two additions this integration needs. A DXGI Title gives XLLN its device and a description
// of its swap chain, never the swap chain, so there is no back buffer to take a render target
// from. Instead the render target already bound by the Title is read at frame time, and its size
// is checked against the swap chain description before anything is drawn.

bool OverlayDX11Init(ID3D11Device* d3d_device, const DXGI_SWAP_CHAIN_DESC* swap_chain_description);
void OverlayDX11Shutdown();
void OverlayDX11OnDeviceReset(const DXGI_SWAP_CHAIN_DESC* swap_chain_description);
void OverlayDX11InvalidateDeviceObjects();
void OverlayDX11MarkFontsDirty();
bool OverlayDX11BeginFrame();
// Whether the last drawn frame went onto the target the Title presents.
bool OverlayDX11TargetAccepted();
void OverlayDX11RenderDrawData(ImDrawData* draw_data);
// The shader resource view is the texture identity; it holds the only reference to the texture.
void* OverlayDX11CreateTextureRgba(uint32_t width, uint32_t height, const uint8_t* rgba);
void OverlayDX11DestroyTexture(void* texture);
