#pragma once
#include <d3d9.h>
#include <stdint.h>

struct ImDrawData;

// Derived from Dear ImGui's imgui_impl_dx9 backend (MIT).
// A plain D3D9 device holds every overlay resource in D3DPOOL_MANAGED, so the title's
// own Reset never fails because of a default-pool resource the overlay still holds.
// A D3D9Ex device has no managed pool and does not lose resources on reset, so it uses
// D3DPOOL_DEFAULT with dynamic buffers.

bool OverlayDX9Init(IDirect3DDevice9* d3d_device, bool is_d3d9ex);
void OverlayDX9Shutdown();
// Releases GPU objects only (buffers/font texture). Safe to call any time on the render thread.
void OverlayDX9InvalidateDeviceObjects();
// Drop just the font texture so it is rebuilt from the current atlas on the next frame.
void OverlayDX9MarkFontsDirty();
// Returns false when the frame must be skipped (device lost / not reset yet).
bool OverlayDX9BeginFrame();
// Opens a scene of its own, because a title can call XLiveRender either inside or outside its own.
void OverlayDX9RenderDrawData(ImDrawData* draw_data);
void* OverlayDX9CreateTextureRgba(uint32_t width, uint32_t height, const uint8_t* rgba);
void OverlayDX9DestroyTexture(void* texture);
