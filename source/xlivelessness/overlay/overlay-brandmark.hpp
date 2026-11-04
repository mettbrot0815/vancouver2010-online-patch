#pragma once

// Box-downscales the XLLN brand mark to target_height (aspect preserved, never upscaled beyond
// the master), averaging colour weighted by alpha so transparent texels cannot darken the edges.
// Returns malloc'd straight-alpha RGBA the caller frees with free(), or null when the mark
// resource is missing or will not decode.
unsigned char* OverlayBrandmarkScaled(int target_height, int* out_width, int* out_height);
