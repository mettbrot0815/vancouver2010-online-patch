#pragma once
#include <stdint.h>
#include <imgui.h>

// Lazily decoded ImGui textures for SPA images (achievement art, the title icon),
// created through whichever renderer backend is attached. Render-thread only.

// 0 until the image exists, decodes and uploads; then usable with ImGui::Image.
// A failed decode is cached so it is not retried every frame.
ImTextureID OverlayTextureForSpaImage(uint32_t image_id);
// Opens the per-frame decode budget. Call once per frame before anything asks for an image.
void OverlayTexturesBeginFrame();
// Releases every cached texture (device shutdown/recreation).
void OverlayTexturesReleaseAll();
