#include "overlay-widgets.hpp"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <float.h>

float overlay_widget_scale = 1.f;
float overlay_widget_alpha = 1.f;
OVERLAY_FONT_SET overlay_fonts;

// Gradient pairs for per-name avatars: the top-left color, then the bottom-right.
// Amber is not in the set because it marks system identities.
static const ImU32 overlay_avatar_gradients[3][2] = {
	{ IM_COL32(91, 141, 239, 255), IM_COL32(59, 91, 219, 255) },
	{ IM_COL32(242, 120, 159, 255), IM_COL32(180, 67, 123, 255) },
	{ IM_COL32(67, 185, 127, 255), IM_COL32(31, 122, 80, 255) },
};
#define OVERLAY_COL_AVATAR_AMBER_TOP IM_COL32(255, 177, 67, 255)
#define OVERLAY_COL_AVATAR_AMBER_BOTTOM IM_COL32(224, 122, 18, 255)

// --- primitives -----------------------------------------------------------------

ImU32 OverlayAlpha(ImU32 color)
{
	if (overlay_widget_alpha >= 1.f) {
		return color;
	}
	uint32_t alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
	alpha = (uint32_t)(alpha * (overlay_widget_alpha < 0.f ? 0.f : overlay_widget_alpha));
	return (color & ~((ImU32)0xFF << IM_COL32_A_SHIFT)) | (alpha << IM_COL32_A_SHIFT);
}

ImU32 OverlayDimColor(float strength)
{
	uint32_t fullAlpha = (overlay_palette.dim >> IM_COL32_A_SHIFT) & 0xFF;
	if (strength < 0.f) {
		strength = 0.f;
	}
	if (strength > 1.f) {
		strength = 1.f;
	}
	return OverlayColorWithAlpha(overlay_palette.dim, (uint32_t)(fullAlpha * strength));
}

ImU32 OverlayColorLerp(ImU32 color_a, ImU32 color_b, float t)
{
	if (t <= 0.f) {
		return color_a;
	}
	if (t >= 1.f) {
		return color_b;
	}
	uint32_t redA = (color_a >> IM_COL32_R_SHIFT) & 0xFF;
	uint32_t greenA = (color_a >> IM_COL32_G_SHIFT) & 0xFF;
	uint32_t blueA = (color_a >> IM_COL32_B_SHIFT) & 0xFF;
	uint32_t alphaA = (color_a >> IM_COL32_A_SHIFT) & 0xFF;
	uint32_t redB = (color_b >> IM_COL32_R_SHIFT) & 0xFF;
	uint32_t greenB = (color_b >> IM_COL32_G_SHIFT) & 0xFF;
	uint32_t blueB = (color_b >> IM_COL32_B_SHIFT) & 0xFF;
	uint32_t alphaB = (color_b >> IM_COL32_A_SHIFT) & 0xFF;
	return IM_COL32(
		(uint32_t)(redA + (redB - (float)redA) * t)
		, (uint32_t)(greenA + (greenB - (float)greenA) * t)
		, (uint32_t)(blueA + (blueB - (float)blueA) * t)
		, (uint32_t)(alphaA + (alphaB - (float)alphaA) * t)
	);
}

ImU32 OverlayColorDesaturate(ImU32 color)
{
	float red = (float)((color >> IM_COL32_R_SHIFT) & 0xFF);
	float green = (float)((color >> IM_COL32_G_SHIFT) & 0xFF);
	float blue = (float)((color >> IM_COL32_B_SHIFT) & 0xFF);
	uint32_t alpha = (color >> IM_COL32_A_SHIFT) & 0xFF;
	float luma = red * 0.3f + green * 0.59f + blue * 0.11f;
	red = (red + (luma - red) * 0.85f) * 0.7f;
	green = (green + (luma - green) * 0.85f) * 0.7f;
	blue = (blue + (luma - blue) * 0.85f) * 0.7f;
	return IM_COL32((uint32_t)red, (uint32_t)green, (uint32_t)blue, alpha);
}

// Quarter-disc corner pie, flat-shaded (the flat span is at most `rounding` px).
static void OverlayDrawCornerPie(ImDrawList* draw_list, ImVec2 corner_center, float radius, float angle_start, ImU32 color)
{
	draw_list->PathArcTo(corner_center, radius, angle_start, angle_start + 3.14159265f * 0.5f, 8);
	draw_list->PathLineTo(corner_center);
	draw_list->PathFillConvex(color);
}

// Vertex-color mesh: an anti-aliased rounded base fill for a clean silhouette,
// with the gradient mesh (three slabs + corner pies) inset one pixel inside it.
// No clip rects, so no scissor-rounding seams on any backend (D3D9 included).
void OverlayDrawRectVGradient(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, ImU32 color_top, ImU32 color_bottom, float rounding, ImDrawFlags draw_flags)
{
	float width = rect_max.x - rect_min.x;
	float height = rect_max.y - rect_min.y;
	if (width <= 0.f || height <= 0.f) {
		return;
	}
	float radius = rounding;
	float halfMinimum = (width < height ? width : height) * 0.5f;
	if (radius > halfMinimum) {
		radius = halfMinimum;
	}
	// AA silhouette in the mid color; the hard-edged mesh stays inside it.
	draw_list->AddRectFilled(rect_min, rect_max, OverlayAlpha(OverlayColorLerp(color_top, color_bottom, 0.5f)), radius, draw_flags);
	rect_min = ImVec2(rect_min.x + 1.f, rect_min.y + 1.f);
	rect_max = ImVec2(rect_max.x - 1.f, rect_max.y - 1.f);
	height = rect_max.y - rect_min.y;
	radius -= 1.f;
	if (rect_max.x <= rect_min.x || height <= 0.f) {
		return;
	}
	#define OVERLAY_VGRAD_SAMPLE(sample_y) OverlayAlpha(OverlayColorLerp(color_top, color_bottom, ((sample_y) - rect_min.y) / height))
	if (radius <= 0.5f) {
		draw_list->AddRectFilledMultiColor(rect_min, rect_max, OVERLAY_VGRAD_SAMPLE(rect_min.y), OVERLAY_VGRAD_SAMPLE(rect_min.y), OVERLAY_VGRAD_SAMPLE(rect_max.y), OVERLAY_VGRAD_SAMPLE(rect_max.y));
		return;
	}
	bool roundTop = draw_flags == 0 || (draw_flags & ImDrawFlags_RoundCornersTop) != 0;
	bool roundBottom = draw_flags == 0 || (draw_flags & ImDrawFlags_RoundCornersBottom) != 0;
	const float pi = 3.14159265f;

	// Top slab.
	float topSlabBottom = rect_min.y + (roundTop ? radius : 0.f);
	if (roundTop) {
		ImU32 cornerColor = OVERLAY_VGRAD_SAMPLE(rect_min.y + radius * 0.5f);
		OverlayDrawCornerPie(draw_list, ImVec2(rect_min.x + radius, rect_min.y + radius), radius, pi, cornerColor);
		OverlayDrawCornerPie(draw_list, ImVec2(rect_max.x - radius, rect_min.y + radius), radius, pi * 1.5f, cornerColor);
		draw_list->AddRectFilledMultiColor(
			ImVec2(rect_min.x + radius, rect_min.y)
			, ImVec2(rect_max.x - radius, topSlabBottom)
			, OVERLAY_VGRAD_SAMPLE(rect_min.y), OVERLAY_VGRAD_SAMPLE(rect_min.y)
			, OVERLAY_VGRAD_SAMPLE(topSlabBottom), OVERLAY_VGRAD_SAMPLE(topSlabBottom)
		);
	}
	// Bottom slab.
	float bottomSlabTop = rect_max.y - (roundBottom ? radius : 0.f);
	if (roundBottom) {
		ImU32 cornerColor = OVERLAY_VGRAD_SAMPLE(rect_max.y - radius * 0.5f);
		OverlayDrawCornerPie(draw_list, ImVec2(rect_max.x - radius, rect_max.y - radius), radius, 0.f, cornerColor);
		OverlayDrawCornerPie(draw_list, ImVec2(rect_min.x + radius, rect_max.y - radius), radius, pi * 0.5f, cornerColor);
		draw_list->AddRectFilledMultiColor(
			ImVec2(rect_min.x + radius, bottomSlabTop)
			, ImVec2(rect_max.x - radius, rect_max.y)
			, OVERLAY_VGRAD_SAMPLE(bottomSlabTop), OVERLAY_VGRAD_SAMPLE(bottomSlabTop)
			, OVERLAY_VGRAD_SAMPLE(rect_max.y), OVERLAY_VGRAD_SAMPLE(rect_max.y)
		);
	}
	// Middle slab, full width.
	draw_list->AddRectFilledMultiColor(
		ImVec2(rect_min.x, topSlabBottom)
		, ImVec2(rect_max.x, bottomSlabTop)
		, OVERLAY_VGRAD_SAMPLE(topSlabBottom), OVERLAY_VGRAD_SAMPLE(topSlabBottom)
		, OVERLAY_VGRAD_SAMPLE(bottomSlabTop), OVERLAY_VGRAD_SAMPLE(bottomSlabTop)
	);
	#undef OVERLAY_VGRAD_SAMPLE
}

// No silhouette under the mesh, unlike OverlayDrawRectVGradient: it would put a hard edge at half
// alpha along the bottom, where the sheen must end at zero.
void OverlayDrawSheen(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, float rounding, uint32_t alpha_top, uint32_t alpha_bottom)
{
	float width = rect_max.x - rect_min.x;
	float height = rect_max.y - rect_min.y;
	if (width <= 0.f || height <= 0.f) {
		return;
	}
	float radius = rounding;
	float halfMinimum = (width < height ? width : height) * 0.5f;
	if (radius > halfMinimum) {
		radius = halfMinimum;
	}
	if (radius < 0.f) {
		radius = 0.f;
	}
	#define OVERLAY_SHEEN_SAMPLE(sample_y) OverlayAlpha(IM_COL32(255, 255, 255, (uint32_t)(alpha_top + ((float)alpha_bottom - (float)alpha_top) * (((sample_y) - rect_min.y) / height))))
	if (radius > 0.5f) {
		const float pi = 3.14159265f;
		ImU32 cornerColor = OVERLAY_SHEEN_SAMPLE(rect_min.y + radius * 0.5f);
		OverlayDrawCornerPie(draw_list, ImVec2(rect_min.x + radius, rect_min.y + radius), radius, pi, cornerColor);
		OverlayDrawCornerPie(draw_list, ImVec2(rect_max.x - radius, rect_min.y + radius), radius, pi * 1.5f, cornerColor);
		draw_list->AddRectFilledMultiColor(
			ImVec2(rect_min.x + radius, rect_min.y)
			, ImVec2(rect_max.x - radius, rect_min.y + radius)
			, OVERLAY_SHEEN_SAMPLE(rect_min.y), OVERLAY_SHEEN_SAMPLE(rect_min.y)
			, OVERLAY_SHEEN_SAMPLE(rect_min.y + radius), OVERLAY_SHEEN_SAMPLE(rect_min.y + radius)
		);
	}
	draw_list->AddRectFilledMultiColor(
		ImVec2(rect_min.x, rect_min.y + radius)
		, ImVec2(rect_max.x, rect_max.y)
		, OVERLAY_SHEEN_SAMPLE(rect_min.y + radius), OVERLAY_SHEEN_SAMPLE(rect_min.y + radius)
		, OVERLAY_SHEEN_SAMPLE(rect_max.y), OVERLAY_SHEEN_SAMPLE(rect_max.y)
	);
	#undef OVERLAY_SHEEN_SAMPLE
}

void OverlayDrawAmberFace(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, float rounding, ImU32 color_top, ImU32 color_bottom)
{
	OverlayDrawRectVGradient(draw_list, rect_min, rect_max, color_top, color_bottom, rounding);
	if (overlay_palette.amberGloss) {
		float middle = rect_min.y + (rect_max.y - rect_min.y) * 0.5f;
		OverlayDrawSheen(
			draw_list
			, ImVec2(rect_min.x + 1.f, rect_min.y + 1.f)
			, ImVec2(rect_max.x - 1.f, middle)
			, rounding - 1.f
			, overlay_palette.amberGloss
			, overlay_palette.amberGloss * 2 / 5
		);
		draw_list->AddLine(
			ImVec2(rect_min.x + rounding, rect_max.y - 2.f)
			, ImVec2(rect_max.x - rounding, rect_max.y - 2.f)
			, OverlayAlpha(IM_COL32(255, 255, 255, overlay_palette.amberGloss))
			, OverlayPx(1.5f)
		);
	}
	draw_list->AddRect(rect_min, rect_max, OverlayAlpha(OVERLAY_COL_AMBER_RIM), rounding);
}

// Horizontal three-stop gradient as a mesh (invite modal art). Only the top
// corners honour rounding (the shape the modal art needs).
void OverlayDrawRectHGradient3(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, ImU32 color_0, ImU32 color_1, ImU32 color_2, float middle_stop, float rounding, ImDrawFlags draw_flags)
{
	(void)draw_flags;
	float width = rect_max.x - rect_min.x;
	float height = rect_max.y - rect_min.y;
	if (width <= 0.f || height <= 0.f) {
		return;
	}
	float radius = rounding;
	float halfMinimum = (width < height ? width : height) * 0.5f;
	if (radius > halfMinimum) {
		radius = halfMinimum;
	}
	// AA silhouette first, hard-edged gradient mesh inset inside it.
	draw_list->AddRectFilled(rect_min, rect_max, OverlayAlpha(color_1), radius, ImDrawFlags_RoundCornersTop);
	rect_min = ImVec2(rect_min.x + 1.f, rect_min.y + 1.f);
	rect_max = ImVec2(rect_max.x - 1.f, rect_max.y - 1.f);
	width = rect_max.x - rect_min.x;
	radius -= 1.f;
	if (width <= 0.f || rect_max.y <= rect_min.y || radius < 1.f) {
		return;
	}
	float stopX = rect_min.x + width * middle_stop;
	#define OVERLAY_HGRAD_SAMPLE(sample_x) OverlayAlpha( \
		(((sample_x) - rect_min.x) / width) < middle_stop \
		? OverlayColorLerp(color_0, color_1, (((sample_x) - rect_min.x) / width) / middle_stop) \
		: OverlayColorLerp(color_1, color_2, (((((sample_x) - rect_min.x) / width)) - middle_stop) / (1.f - middle_stop)))
	const float pi = 3.14159265f;
	// Top slab with corner pies.
	OverlayDrawCornerPie(draw_list, ImVec2(rect_min.x + radius, rect_min.y + radius), radius, pi, OVERLAY_HGRAD_SAMPLE(rect_min.x + radius * 0.5f));
	OverlayDrawCornerPie(draw_list, ImVec2(rect_max.x - radius, rect_min.y + radius), radius, pi * 1.5f, OVERLAY_HGRAD_SAMPLE(rect_max.x - radius * 0.5f));
	float slabSplits[3][2] = {
		{ rect_min.x + radius, stopX },
		{ stopX, rect_max.x - radius },
		{ 0.f, 0.f },
	};
	for (int iSegment = 0; iSegment < 2; iSegment++) {
		float left = slabSplits[iSegment][0];
		float right = slabSplits[iSegment][1];
		if (right <= left) {
			continue;
		}
		draw_list->AddRectFilledMultiColor(
			ImVec2(left, rect_min.y)
			, ImVec2(right, rect_min.y + radius)
			, OVERLAY_HGRAD_SAMPLE(left), OVERLAY_HGRAD_SAMPLE(right)
			, OVERLAY_HGRAD_SAMPLE(right), OVERLAY_HGRAD_SAMPLE(left)
		);
	}
	// Main slab, full width, split at the middle stop.
	float mainSplits[2][2] = {
		{ rect_min.x, stopX },
		{ stopX, rect_max.x },
	};
	for (int iSegment = 0; iSegment < 2; iSegment++) {
		float left = mainSplits[iSegment][0];
		float right = mainSplits[iSegment][1];
		if (right <= left) {
			continue;
		}
		draw_list->AddRectFilledMultiColor(
			ImVec2(left, rect_min.y + radius)
			, ImVec2(right, rect_max.y)
			, OVERLAY_HGRAD_SAMPLE(left), OVERLAY_HGRAD_SAMPLE(right)
			, OVERLAY_HGRAD_SAMPLE(right), OVERLAY_HGRAD_SAMPLE(left)
		);
	}
	#undef OVERLAY_HGRAD_SAMPLE
}

void OverlayDrawRectDiagGradient(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, ImU32 color_top_left, ImU32 color_bottom_right, float rounding)
{
	float width = rect_max.x - rect_min.x;
	float height = rect_max.y - rect_min.y;
	if (width <= 0.f || height <= 0.f) {
		return;
	}
	float radius = rounding;
	float halfMinimum = (width < height ? width : height) * 0.5f;
	if (radius > halfMinimum) {
		radius = halfMinimum;
	}
	// AA silhouette first, hard-edged gradient mesh inset inside it.
	draw_list->AddRectFilled(rect_min, rect_max, OverlayAlpha(OverlayColorLerp(color_top_left, color_bottom_right, 0.5f)), radius);
	rect_min = ImVec2(rect_min.x + 1.f, rect_min.y + 1.f);
	rect_max = ImVec2(rect_max.x - 1.f, rect_max.y - 1.f);
	width = rect_max.x - rect_min.x;
	height = rect_max.y - rect_min.y;
	radius -= 1.f;
	if (width <= 0.f || height <= 0.f || radius < 1.f) {
		return;
	}
	float diagonalSpan = width + height;
	// t along the 135-degree axis at a given point.
	#define OVERLAY_DIAG_SAMPLE(sample_x, sample_y) OverlayAlpha(OverlayColorLerp(color_top_left, color_bottom_right, (((sample_x) - rect_min.x) + ((sample_y) - rect_min.y)) / diagonalSpan))

	// Center column.
	draw_list->AddRectFilledMultiColor(
		ImVec2(rect_min.x + radius, rect_min.y)
		, ImVec2(rect_max.x - radius, rect_max.y)
		, OVERLAY_DIAG_SAMPLE(rect_min.x + radius, rect_min.y)
		, OVERLAY_DIAG_SAMPLE(rect_max.x - radius, rect_min.y)
		, OVERLAY_DIAG_SAMPLE(rect_max.x - radius, rect_max.y)
		, OVERLAY_DIAG_SAMPLE(rect_min.x + radius, rect_max.y)
	);
	// Side columns between the corner arcs.
	draw_list->AddRectFilledMultiColor(
		ImVec2(rect_min.x, rect_min.y + radius)
		, ImVec2(rect_min.x + radius, rect_max.y - radius)
		, OVERLAY_DIAG_SAMPLE(rect_min.x, rect_min.y + radius)
		, OVERLAY_DIAG_SAMPLE(rect_min.x + radius, rect_min.y + radius)
		, OVERLAY_DIAG_SAMPLE(rect_min.x + radius, rect_max.y - radius)
		, OVERLAY_DIAG_SAMPLE(rect_min.x, rect_max.y - radius)
	);
	draw_list->AddRectFilledMultiColor(
		ImVec2(rect_max.x - radius, rect_min.y + radius)
		, ImVec2(rect_max.x, rect_max.y - radius)
		, OVERLAY_DIAG_SAMPLE(rect_max.x - radius, rect_min.y + radius)
		, OVERLAY_DIAG_SAMPLE(rect_max.x, rect_min.y + radius)
		, OVERLAY_DIAG_SAMPLE(rect_max.x, rect_max.y - radius)
		, OVERLAY_DIAG_SAMPLE(rect_max.x - radius, rect_max.y - radius)
	);
	// Corner pies, flat-shaded with the local gradient value.
	const float pi = 3.14159265f;
	struct {
		ImVec2 center;
		float angleStart;
	} corners[4] = {
		{ ImVec2(rect_min.x + radius, rect_min.y + radius), pi },
		{ ImVec2(rect_max.x - radius, rect_min.y + radius), pi * 1.5f },
		{ ImVec2(rect_max.x - radius, rect_max.y - radius), 0.f },
		{ ImVec2(rect_min.x + radius, rect_max.y - radius), pi * 0.5f },
	};
	for (int iCorner = 0; iCorner < 4; iCorner++) {
		draw_list->PathArcTo(corners[iCorner].center, radius, corners[iCorner].angleStart, corners[iCorner].angleStart + pi * 0.5f, 8);
		draw_list->PathLineTo(corners[iCorner].center);
		draw_list->PathFillConvex(OVERLAY_DIAG_SAMPLE(corners[iCorner].center.x, corners[iCorner].center.y));
	}
	#undef OVERLAY_DIAG_SAMPLE
}

void OverlayDrawSoftShadow(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, float rounding, float offset_y, float spread, int layer_alpha)
{
	const int layerCount = 5;
	for (int iLayer = 0; iLayer < layerCount; iLayer++) {
		float t = (iLayer + 1.f) / layerCount;
		float expand = spread * t;
		float falloff = 1.f - t * 0.85f;
		int alpha = (int)(layer_alpha * falloff * falloff);
		if (alpha <= 0) {
			continue;
		}
		draw_list->AddRectFilled(
			ImVec2(rect_min.x - expand, rect_min.y - expand + offset_y)
			, ImVec2(rect_max.x + expand, rect_max.y + expand + offset_y)
			, OverlayAlpha(IM_COL32(0, 0, 0, alpha))
			, rounding + expand
		);
	}
}

void OverlayDrawGlow(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, float rounding, ImU32 color, float spread, int layer_alpha)
{
	ImU32 colorNoAlpha = color & ~((ImU32)0xFF << IM_COL32_A_SHIFT);
	const int layerCount = 4;
	for (int iLayer = 0; iLayer < layerCount; iLayer++) {
		float t = (iLayer + 1.f) / layerCount;
		float expand = spread * t;
		float falloff = 1.f - t * 0.8f;
		int alpha = (int)(layer_alpha * falloff * falloff);
		if (alpha <= 0) {
			continue;
		}
		draw_list->AddRectFilled(
			ImVec2(rect_min.x - expand, rect_min.y - expand + OverlayPx(2.f))
			, ImVec2(rect_max.x + expand, rect_max.y + expand + OverlayPx(2.f))
			, OverlayAlpha(colorNoAlpha | ((ImU32)alpha << IM_COL32_A_SHIFT))
			, rounding + expand
		);
	}
}

ImVec2 OverlayTrackedTextSize(ImFont* font, const char* text, float tracking)
{
	float width = 0.f;
	int characterCount = 0;
	for (const char* character = text; *character; character++) {
		width += font->GetCharAdvance((ImWchar)(unsigned char)*character);
		characterCount++;
	}
	if (characterCount > 1) {
		width += tracking * (characterCount - 1);
	}
	return ImVec2(width, font->FontSize);
}

void OverlayDrawTrackedText(ImDrawList* draw_list, ImFont* font, ImVec2 position, ImU32 color, const char* text, float tracking)
{
	ImU32 finalColor = OverlayAlpha(color);
	float x = position.x;
	char characterBuffer[2] = {0, 0};
	for (const char* character = text; *character; character++) {
		characterBuffer[0] = *character;
		draw_list->AddText(font, font->FontSize, ImVec2(x, position.y), finalColor, characterBuffer);
		x += font->GetCharAdvance((ImWchar)(unsigned char)*character) + tracking;
	}
}

// --- keycaps --------------------------------------------------------------------

float OverlayKeycapHeight(bool standalone)
{
	return overlay_fonts.semi11->FontSize + OverlayPx(standalone ? 10.f : 5.f);
}

float OverlayKeycapWidth(const char* label, bool standalone)
{
	float textWidth = overlay_fonts.semi11->CalcTextSizeA(overlay_fonts.semi11->FontSize, FLT_MAX, 0.f, label).x;
	float width = textWidth + OverlayPx(standalone ? 18.f : 14.f);
	float minimumWidth = OverlayPx(standalone ? 34.f : 0.f);
	return width < minimumWidth ? minimumWidth : width;
}

void OverlayDrawKeycap(ImDrawList* draw_list, ImVec2 position, const char* label, bool standalone)
{
	float width = OverlayKeycapWidth(label, standalone);
	float height = OverlayKeycapHeight(standalone);
	ImVec2 rectMax(position.x + width, position.y + height);
	float rounding = OverlayPx(6.f);
	OverlayDrawRectVGradient(draw_list, position, rectMax, OVERLAY_COL_KEYCAP_TOP, OVERLAY_COL_KEYCAP_BOTTOM, rounding);
	draw_list->AddRect(position, rectMax, OverlayAlpha(OVERLAY_COL_BORDER_HI), rounding);
	// The darker bottom lip that makes it read as a physical key.
	// One pixel above the border, or it covers the bottom border.
	draw_list->AddLine(
		ImVec2(position.x + rounding, rectMax.y - 2.f)
		, ImVec2(rectMax.x - rounding, rectMax.y - 2.f)
		, OverlayAlpha(overlay_palette.keycapLip)
	);
	float textWidth = overlay_fonts.semi11->CalcTextSizeA(overlay_fonts.semi11->FontSize, FLT_MAX, 0.f, label).x;
	draw_list->AddText(
		overlay_fonts.semi11
		, overlay_fonts.semi11->FontSize
		, ImVec2(position.x + (width - textWidth) * 0.5f, position.y + (height - overlay_fonts.semi11->FontSize) * 0.5f)
		, OverlayAlpha(overlay_palette.keycapText)
		, label
	);
}

void OverlayKeycapItem(const char* label, bool standalone)
{
	ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(OverlayKeycapWidth(label, standalone), OverlayKeycapHeight(standalone)));
	OverlayDrawKeycap(ImGui::GetWindowDrawList(), position, label, standalone);
}

// --- avatars --------------------------------------------------------------------

static uint32_t OverlayNameHash(const char* seed_name)
{
	uint32_t hash = 2166136261u;
	for (const char* character = seed_name; character && *character; character++) {
		hash = (hash ^ (uint8_t)*character) * 16777619u;
	}
	// Final avalanche so short gamertags spread evenly across the small palette.
	hash ^= hash >> 15;
	hash *= 0x2C1B3C6Du;
	hash ^= hash >> 12;
	return hash;
}

static void OverlayDrawAvatarDot(ImDrawList* draw_list, ImVec2 position, float size_px)
{
	ImVec2 dotCenter(position.x + size_px * 0.93f, position.y + size_px * 0.93f);
	float dotRadius = size_px * 0.153f;
	draw_list->AddCircleFilled(dotCenter, dotRadius + OverlayPx(2.2f), OverlayAlpha(OVERLAY_COL_AVATAR_RING));
	draw_list->AddCircleFilled(dotCenter, dotRadius, OverlayAlpha(OVERLAY_COL_GREEN));
}

void OverlayDrawAvatar(ImDrawList* draw_list, ImVec2 position, float size_px, const char* seed_name, bool amber, bool online_dot, bool dimmed)
{
	ImU32 gradientTop;
	ImU32 gradientBottom;
	if (amber) {
		gradientTop = OVERLAY_COL_AVATAR_AMBER_TOP;
		gradientBottom = OVERLAY_COL_AVATAR_AMBER_BOTTOM;
	}
	else {
		const ImU32* gradient = overlay_avatar_gradients[OverlayNameHash(seed_name) % 3];
		gradientTop = gradient[0];
		gradientBottom = gradient[1];
	}
	if (dimmed) {
		gradientTop = OverlayColorDesaturate(gradientTop);
		gradientBottom = OverlayColorDesaturate(gradientBottom);
	}
	ImVec2 rectMax(position.x + size_px, position.y + size_px);
	OverlayDrawRectDiagGradient(draw_list, position, rectMax, gradientTop, gradientBottom, size_px * 0.28f);

	char initial[2] = { '?', 0 };
	if (seed_name && seed_name[0] && !((uint8_t)seed_name[0] & 0x80)) {
		initial[0] = (char)toupper((unsigned char)seed_name[0]);
	}
	ImFont* initialFont = overlay_fonts.bold13;
	if (size_px >= OverlayPx(40.f)) {
		initialFont = overlay_fonts.bold17;
	}
	else if (size_px >= OverlayPx(32.f)) {
		initialFont = overlay_fonts.bold16;
	}
	ImVec2 initialSize = initialFont->CalcTextSizeA(initialFont->FontSize, FLT_MAX, 0.f, initial);
	ImU32 initialColor = amber ? OVERLAY_COL_AMBER_INK : (dimmed ? IM_COL32(255, 255, 255, 200) : IM_COL32(255, 255, 255, 255));
	draw_list->AddText(
		initialFont
		, initialFont->FontSize
		, ImVec2(position.x + (size_px - initialSize.x) * 0.5f, position.y + (size_px - initialSize.y) * 0.5f)
		, OverlayAlpha(initialColor)
		, initial
	);
	if (online_dot && !dimmed) {
		OverlayDrawAvatarDot(draw_list, position, size_px);
	}
}

void OverlayDrawAvatarCheck(ImDrawList* draw_list, ImVec2 position, float size_px)
{
	ImVec2 rectMax(position.x + size_px, position.y + size_px);
	OverlayDrawRectDiagGradient(draw_list, position, rectMax, overlay_avatar_gradients[2][0], overlay_avatar_gradients[2][1], size_px * 0.28f);
	ImVec2 checkPoints[3] = {
		ImVec2(position.x + size_px * 0.27f, position.y + size_px * 0.53f)
		, ImVec2(position.x + size_px * 0.44f, position.y + size_px * 0.70f)
		, ImVec2(position.x + size_px * 0.74f, position.y + size_px * 0.33f)
	};
	draw_list->AddPolyline(checkPoints, 3, OverlayAlpha(IM_COL32(255, 255, 255, 255)), 0, size_px * 0.09f);
}

void OverlayAvatarItem(const char* seed_name, float design_size, bool amber, bool online_dot, bool dimmed)
{
	float sizePx = OverlayPx(design_size);
	ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(sizePx, sizePx));
	OverlayDrawAvatar(ImGui::GetWindowDrawList(), position, sizePx, seed_name, amber, online_dot, dimmed);
}

// --- badge ----------------------------------------------------------------------

static void OverlayBadgeLabel(int count, char* buffer, size_t buffer_size)
{
	if (count > 9) {
		snprintf(buffer, buffer_size, "9+");
	}
	else {
		snprintf(buffer, buffer_size, "%d", count);
	}
}

float OverlayBadgeWidth(int count)
{
	char label[8];
	OverlayBadgeLabel(count, label, sizeof(label));
	float textWidth = overlay_fonts.semi11->CalcTextSizeA(overlay_fonts.semi11->FontSize, FLT_MAX, 0.f, label).x;
	float width = textWidth + OverlayPx(8.f);
	float minimumWidth = OverlayPx(16.f);
	return width < minimumWidth ? minimumWidth : width;
}

void OverlayDrawBadge(ImDrawList* draw_list, ImVec2 position, int count, bool on_amber)
{
	char label[8];
	OverlayBadgeLabel(count, label, sizeof(label));
	float width = OverlayBadgeWidth(count);
	float height = OverlayPx(16.f);
	ImVec2 rectMax(position.x + width, position.y + height);
	ImU32 backgroundColor = on_amber ? IM_COL32(35, 19, 3, 217) : OVERLAY_COL_RED;
	ImU32 textColor = on_amber ? IM_COL32(255, 217, 160, 255) : IM_COL32(255, 255, 255, 255);
	draw_list->AddRectFilled(position, rectMax, OverlayAlpha(backgroundColor), height * 0.5f);
	float textWidth = overlay_fonts.semi11->CalcTextSizeA(overlay_fonts.semi11->FontSize, FLT_MAX, 0.f, label).x;
	draw_list->AddText(
		overlay_fonts.semi11
		, overlay_fonts.semi11->FontSize
		, ImVec2(position.x + (width - textWidth) * 0.5f, position.y + (height - overlay_fonts.semi11->FontSize) * 0.5f)
		, OverlayAlpha(textColor)
		, label
	);
}

// --- focus ring -----------------------------------------------------------------

void OverlayFocusRingForLastItem(float rounding)
{
	if (!ImGui::IsItemFocused()) {
		return;
	}
	ImVec2 rectMin = ImGui::GetItemRectMin();
	ImVec2 rectMax = ImGui::GetItemRectMax();
	float offset = OverlayPx(2.f);
	ImGui::GetWindowDrawList()->AddRect(
		ImVec2(rectMin.x - offset, rectMin.y - offset)
		, ImVec2(rectMax.x + offset, rectMax.y + offset)
		, OverlayAlpha(OVERLAY_COL_AMBER)
		, rounding + offset
		, 0
		, OverlayPx(1.6f)
	);
}

// --- buttons --------------------------------------------------------------------

float OverlayButtonHeight()
{
	return OverlayPx(30.f);
}

float OverlayButtonWidth(const char* label)
{
	float textWidth = overlay_fonts.semi12->CalcTextSizeA(overlay_fonts.semi12->FontSize, FLT_MAX, 0.f, label).x;
	return textWidth + OverlayPx(26.f);
}

bool OverlayButton(const char* str_id, const char* label, OverlayButtonKind kind, ImVec2 size_px, const char* trailing_hint)
{
	float width = size_px.x > 0.f ? size_px.x : OverlayButtonWidth(label);
	float height = size_px.y > 0.f ? size_px.y : OverlayButtonHeight();
	ImVec2 position = ImGui::GetCursorScreenPos();
	// InvisibleButton needs EnableNav or directional navigation cannot reach it. The built-in highlight
	// is hidden because the widget draws its own focus ring.
	ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.f, 0.f, 0.f, 0.f));
	bool pressed = ImGui::InvisibleButton(str_id, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
	ImGui::PopStyleColor();
	bool hovered = ImGui::IsItemHovered();
	bool active = ImGui::IsItemActive();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 rectMax(position.x + width, position.y + height);
	float rounding = height > OverlayPx(34.f) ? OverlayPx(11.f) : OverlayPx(9.f);

	ImU32 textColor = OVERLAY_COL_TEXT;
	switch (kind) {
		case OverlayButtonKind::PRIMARY: {
			ImU32 gradientTop = OVERLAY_COL_AMBER_HI;
			ImU32 gradientBottom = OVERLAY_COL_AMBER_LO;
			if (active) {
				gradientTop = OverlayColorLerp(gradientTop, IM_COL32(0, 0, 0, 255), 0.08f);
				gradientBottom = OverlayColorLerp(gradientBottom, IM_COL32(0, 0, 0, 255), 0.08f);
			}
			else if (hovered) {
				gradientTop = OverlayColorLerp(gradientTop, IM_COL32(255, 255, 255, 255), 0.09f);
				gradientBottom = OverlayColorLerp(gradientBottom, IM_COL32(255, 255, 255, 255), 0.09f);
			}
			OverlayDrawAmberFace(drawList, position, rectMax, rounding, gradientTop, gradientBottom);
			textColor = OVERLAY_COL_AMBER_INK;
			break;
		}
		case OverlayButtonKind::DANGER: {
			if (hovered || active) {
				drawList->AddRectFilled(position, rectMax, OverlayAlpha(IM_COL32(229, 72, 77, active ? 56 : 38)), rounding);
			}
			drawList->AddRect(position, rectMax, OverlayAlpha(IM_COL32(229, 72, 77, 102)), rounding);
			textColor = OVERLAY_COL_RED_SOFT;
			break;
		}
		case OverlayButtonKind::SECONDARY:
		default: {
			// Hover and press move the face toward the wash, which differs from the face on every theme.
			ImU32 gradientTop = overlay_palette.buttonTop;
			ImU32 gradientBottom = overlay_palette.buttonBottom;
			if (active || hovered) {
				float towardWash = active ? 0.16f : 0.08f;
				gradientTop = OverlayColorLerp(gradientTop, overlay_palette.wash, towardWash);
				gradientBottom = OverlayColorLerp(gradientBottom, overlay_palette.wash, towardWash);
			}
			OverlayDrawRectVGradient(drawList, position, rectMax, gradientTop, gradientBottom, rounding);
			drawList->AddRect(position, rectMax, OverlayAlpha(overlay_palette.buttonBorder), rounding);
			break;
		}
	}

	ImFont* labelFont = overlay_fonts.semi12;
	ImVec2 labelSize = labelFont->CalcTextSizeA(labelFont->FontSize, FLT_MAX, 0.f, label);
	float textY = position.y + (height - labelSize.y) * 0.5f;
	if (trailing_hint) {
		drawList->AddText(labelFont, labelFont->FontSize, ImVec2(position.x + OverlayPx(13.f), textY), OverlayAlpha(textColor), label);
		ImFont* hintFont = overlay_fonts.reg12;
		ImVec2 hintSize = hintFont->CalcTextSizeA(hintFont->FontSize, FLT_MAX, 0.f, trailing_hint);
		ImU32 hintColor = (textColor & ~((ImU32)0xFF << IM_COL32_A_SHIFT)) | ((ImU32)166 << IM_COL32_A_SHIFT);
		drawList->AddText(
			hintFont
			, hintFont->FontSize
			, ImVec2(rectMax.x - hintSize.x - OverlayPx(13.f), position.y + (height - hintSize.y) * 0.5f)
			, OverlayAlpha(hintColor)
			, trailing_hint
		);
	}
	else {
		drawList->AddText(labelFont, labelFont->FontSize, ImVec2(position.x + (width - labelSize.x) * 0.5f, textY), OverlayAlpha(textColor), label);
	}
	OverlayFocusRingForLastItem(rounding);
	return pressed;
}

bool OverlayIconButton(const char* str_id, OverlayIconType icon_type, bool selected)
{
	float size = OverlayPx(32.f);
	ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.f, 0.f, 0.f, 0.f));
	bool pressed = ImGui::InvisibleButton(str_id, ImVec2(size, size), ImGuiButtonFlags_EnableNav);
	ImGui::PopStyleColor();
	bool hovered = ImGui::IsItemHovered();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 rectMax(position.x + size, position.y + size);
	float rounding = OverlayPx(9.f);
	ImU32 iconColor = OverlayAlpha(hovered ? OVERLAY_COL_TEXT : OVERLAY_COL_TEXT_DIM);
	if (selected) {
		OverlayDrawGlow(drawList, position, rectMax, rounding, OVERLAY_COL_AMBER_LO, OverlayPx(8.f), 30);
		OverlayDrawAmberFace(drawList, position, rectMax, rounding, OVERLAY_COL_AMBER_HI, OVERLAY_COL_AMBER_LO);
		iconColor = OverlayAlpha(OVERLAY_COL_AMBER_INK);
	}
	else if (hovered) {
		drawList->AddRectFilled(position, rectMax, OverlayAlpha(OVERLAY_COL_WASH_A(18)), rounding);
	}
	ImVec2 center(position.x + size * 0.5f, position.y + size * 0.5f);
	// Straight strokes are polylines. AddLine moves both ends half a pixel off the centre.
	float strokeWidth = OverlayPx(1.7f);
	const float pi = 3.14159265f;
	switch (icon_type) {
		case OverlayIconType::LIGHT: {
			drawList->AddCircle(center, OverlayPx(3.9f), iconColor, 0, strokeWidth);
			for (int iRay = 0; iRay < 8; iRay++) {
				ImVec2 direction(cosf(pi / 4.f * iRay), sinf(pi / 4.f * iRay));
				ImVec2 ray[2] = {
					ImVec2(center.x + direction.x * OverlayPx(5.8f), center.y + direction.y * OverlayPx(5.8f))
					, ImVec2(center.x + direction.x * OverlayPx(8.f), center.y + direction.y * OverlayPx(8.f))
				};
				drawList->AddPolyline(ray, 2, iconColor, 0, strokeWidth);
			}
			break;
		}
		case OverlayIconType::SETTINGS: {
			static const float toothCorners[4][2] = { { -0.346f, 5.6f }, { -0.172f, 7.6f }, { 0.172f, 7.6f }, { 0.346f, 5.6f } };
			ImVec2 outline[32];
			for (int iPoint = 0; iPoint < 32; iPoint++) {
				float angle = pi / 4.f * (iPoint / 4) + toothCorners[iPoint % 4][0];
				float radius = OverlayPx(toothCorners[iPoint % 4][1]);
				outline[iPoint] = ImVec2(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
			}
			drawList->AddPolyline(outline, 32, iconColor, ImDrawFlags_Closed, strokeWidth);
			drawList->AddCircle(center, OverlayPx(2.4f), iconColor, 0, strokeWidth);
			break;
		}
		case OverlayIconType::CLOSE:
		default: {
			float halfExtent = OverlayPx(5.f);
			ImVec2 strokeDown[2] = { ImVec2(center.x - halfExtent, center.y - halfExtent), ImVec2(center.x + halfExtent, center.y + halfExtent) };
			ImVec2 strokeUp[2] = { ImVec2(center.x - halfExtent, center.y + halfExtent), ImVec2(center.x + halfExtent, center.y - halfExtent) };
			drawList->AddPolyline(strokeDown, 2, iconColor, 0, strokeWidth);
			drawList->AddPolyline(strokeUp, 2, iconColor, 0, strokeWidth);
			break;
		}
	}
	OverlayFocusRingForLastItem(OverlayPx(9.f));
	return pressed;
}

bool OverlayTabItem(const char* label, bool selected, int badge_count)
{
	ImFont* labelFont = overlay_fonts.semi13;
	ImVec2 labelSize = labelFont->CalcTextSizeA(labelFont->FontSize, FLT_MAX, 0.f, label);
	float paddingX = OverlayPx(14.f);
	float height = OverlayPx(30.f);
	float badgeSpan = badge_count > 0 ? OverlayBadgeWidth(badge_count) + OverlayPx(6.f) : 0.f;
	float width = labelSize.x + paddingX * 2.f + badgeSpan;

	ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.f, 0.f, 0.f, 0.f));
	bool pressed = ImGui::InvisibleButton(label, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
	ImGui::PopStyleColor();
	bool hovered = ImGui::IsItemHovered();
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 rectMax(position.x + width, position.y + height);
	float rounding = OverlayPx(10.f);

	ImU32 textColor = OVERLAY_COL_TEXT_DIM;
	if (selected) {
		OverlayDrawGlow(drawList, position, rectMax, rounding, OVERLAY_COL_AMBER_LO, OverlayPx(8.f), 30);
		OverlayDrawAmberFace(drawList, position, rectMax, rounding, OVERLAY_COL_AMBER_HI, OVERLAY_COL_AMBER_LO);
		textColor = OVERLAY_COL_AMBER_INK;
	}
	else if (overlay_palette.tabTop & IM_COL32_A_MASK) {
		// A theme that tiles its tabs draws every tab as a tile; hover moves the tile toward the wash.
		ImU32 gradientTop = overlay_palette.tabTop;
		ImU32 gradientBottom = overlay_palette.tabBottom;
		if (hovered) {
			gradientTop = OverlayColorLerp(gradientTop, overlay_palette.wash, 0.08f);
			gradientBottom = OverlayColorLerp(gradientBottom, overlay_palette.wash, 0.08f);
		}
		OverlayDrawRectVGradient(drawList, position, rectMax, gradientTop, gradientBottom, rounding);
		drawList->AddRect(position, rectMax, OverlayAlpha(overlay_palette.tabBorder), rounding);
		textColor = OVERLAY_COL_TEXT;
	}
	else if (hovered) {
		drawList->AddRectFilled(position, rectMax, OverlayAlpha(OVERLAY_COL_WASH_A(15)), rounding);
		textColor = OVERLAY_COL_TEXT;
	}
	drawList->AddText(
		labelFont
		, labelFont->FontSize
		, ImVec2(position.x + paddingX, position.y + (height - labelSize.y) * 0.5f)
		, OverlayAlpha(textColor)
		, label
	);
	if (badge_count > 0) {
		OverlayDrawBadge(
			drawList
			, ImVec2(position.x + paddingX + labelSize.x + OverlayPx(6.f), position.y + (height - OverlayPx(16.f)) * 0.5f)
			, badge_count
			, selected
		);
	}
	OverlayFocusRingForLastItem(rounding);
	return pressed;
}

// --- static components ------------------------------------------------------------

void OverlayChip(const char* label, ImU32 text_color, ImU32 border_color, ImU32 dot_color)
{
	ImFont* labelFont = overlay_fonts.semi11;
	ImVec2 labelSize = labelFont->CalcTextSizeA(labelFont->FontSize, FLT_MAX, 0.f, label);
	float height = OverlayPx(21.f);
	float dotSpan = dot_color ? OverlayPx(9.f) : 0.f;
	float width = labelSize.x + OverlayPx(20.f) + dotSpan;
	ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(width, height));
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRect(position, ImVec2(position.x + width, position.y + height), OverlayAlpha(border_color), height * 0.5f);
	if (dot_color) {
		drawList->AddCircleFilled(
			ImVec2(position.x + OverlayPx(12.5f), position.y + height * 0.5f)
			, OverlayPx(2.5f)
			, OverlayAlpha(dot_color)
		);
	}
	drawList->AddText(
		labelFont
		, labelFont->FontSize
		, ImVec2(position.x + OverlayPx(10.f) + dotSpan, position.y + (height - labelSize.y) * 0.5f)
		, OverlayAlpha(text_color)
		, label
	);
}

void OverlayEyebrowLabel(const char* label)
{
	float tracking = OverlayPx(1.7f);
	ImVec2 labelSize = OverlayTrackedTextSize(overlay_fonts.semi11, label, tracking);
	ImVec2 position = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(labelSize.x, labelSize.y + OverlayPx(4.f)));
	OverlayDrawTrackedText(ImGui::GetWindowDrawList(), overlay_fonts.semi11, position, OVERLAY_COL_TEXT_FAINT, label, tracking);
}

void OverlayDrawMeter(ImDrawList* draw_list, ImVec2 position, float width, float height, float fraction, ImU32 fill_color)
{
	if (fraction < 0.f) {
		fraction = 0.f;
	}
	if (fraction > 1.f) {
		fraction = 1.f;
	}
	float rounding = height * 0.5f;
	draw_list->AddRectFilled(position, ImVec2(position.x + width, position.y + height), OverlayAlpha(OVERLAY_COL_WASH_A(26)), rounding);
	if (fraction > 0.f) {
		draw_list->AddRectFilled(position, ImVec2(position.x + width * fraction, position.y + height), OverlayAlpha(fill_color), rounding);
	}
}

// --- rows -------------------------------------------------------------------------

static ImDrawListSplitter overlay_row_splitter;
static ImVec2 overlay_row_origin;
static float overlay_row_width = 0.f;

void OverlayRowBegin(const char* str_id)
{
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	overlay_row_origin = ImGui::GetCursorScreenPos();
	overlay_row_width = ImGui::GetContentRegionAvail().x;
	overlay_row_splitter.Split(drawList, 2);
	overlay_row_splitter.SetCurrentChannel(drawList, 1);
	ImGui::PushID(str_id);
	ImGui::BeginGroup();
	ImGui::SetCursorScreenPos(ImVec2(overlay_row_origin.x + OverlayPx(10.f), overlay_row_origin.y + OverlayPx(9.f)));
	ImGui::BeginGroup();
}

void OverlayRowEnd()
{
	ImGui::EndGroup();
	ImVec2 contentMax = ImGui::GetItemRectMax();
	ImVec2 rowMax(overlay_row_origin.x + overlay_row_width, contentMax.y + OverlayPx(9.f));
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	overlay_row_splitter.SetCurrentChannel(drawList, 0);
	if (ImGui::IsMouseHoveringRect(overlay_row_origin, rowMax)) {
		drawList->AddRectFilled(overlay_row_origin, rowMax, OverlayAlpha(OVERLAY_COL_WASH_A(13)), OverlayPx(11.f));
	}
	overlay_row_splitter.Merge(drawList);
	ImGui::SetCursorScreenPos(overlay_row_origin);
	ImGui::Dummy(ImVec2(overlay_row_width, rowMax.y - overlay_row_origin.y));
	ImGui::EndGroup();
	ImGui::PopID();
}

ImVec2 OverlayRowOrigin()
{
	return overlay_row_origin;
}

float OverlayRowWidth()
{
	return overlay_row_width;
}

// --- cards ------------------------------------------------------------------------

static ImDrawListSplitter overlay_card_splitter;
static ImVec2 overlay_card_origin;

void OverlayCardBegin(const char* str_id, float width_px)
{
	(void)width_px;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	overlay_card_origin = ImGui::GetCursorScreenPos();
	overlay_card_splitter.Split(drawList, 2);
	overlay_card_splitter.SetCurrentChannel(drawList, 1);
	ImGui::PushID(str_id);
	ImGui::BeginGroup();
	ImGui::SetCursorScreenPos(ImVec2(overlay_card_origin.x + OverlayPx(16.f), overlay_card_origin.y + OverlayPx(14.f)));
	ImGui::BeginGroup();
}

float OverlayCardEnd(float width_px, float minimum_height_px)
{
	ImGui::EndGroup();
	ImVec2 contentMax = ImGui::GetItemRectMax();
	ImVec2 cardMax(overlay_card_origin.x + width_px, contentMax.y + OverlayPx(14.f));
	if (minimum_height_px > 0.f && cardMax.y - overlay_card_origin.y < minimum_height_px) {
		cardMax.y = overlay_card_origin.y + minimum_height_px;
	}
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	overlay_card_splitter.SetCurrentChannel(drawList, 0);
	float rounding = OverlayPx(12.f);
	drawList->AddRectFilled(overlay_card_origin, cardMax, OverlayAlpha(overlay_palette.cardFill), rounding);
	drawList->AddRect(overlay_card_origin, cardMax, OverlayAlpha(overlay_palette.cardBorder), rounding);
	overlay_card_splitter.Merge(drawList);
	ImGui::SetCursorScreenPos(overlay_card_origin);
	ImGui::Dummy(ImVec2(cardMax.x - overlay_card_origin.x, cardMax.y - overlay_card_origin.y));
	ImGui::EndGroup();
	ImGui::PopID();
	return cardMax.y - overlay_card_origin.y;
}

// --- inline flow --------------------------------------------------------------------

typedef struct _OVERLAY_FLOW_TOKEN {
	const char* begin;
	const char* end;
	ImFont* font;
	ImU32 color;
	bool keycap;
	bool spaceBefore;
	float width;
	float spaceWidth;
} OVERLAY_FLOW_TOKEN;

float OverlayInlineFlow(ImDrawList* draw_list, ImVec2 origin, float max_width, const OVERLAY_INLINE_SEGMENT* segments, int segment_count)
{
	OVERLAY_FLOW_TOKEN tokens[48];
	int tokenCount = 0;
	bool pendingSpace = false;
	bool anyKeycap = false;
	float lineHeight = 0.f;

	for (int iSegment = 0; iSegment < segment_count && tokenCount < 48; iSegment++) {
		const OVERLAY_INLINE_SEGMENT &segment = segments[iSegment];
		if (!segment.text || !segment.text[0]) {
			continue;
		}
		if (segment.keycap) {
			anyKeycap = true;
			OVERLAY_FLOW_TOKEN &token = tokens[tokenCount++];
			token.begin = segment.text;
			token.end = segment.text + strlen(segment.text);
			token.font = overlay_fonts.semi11;
			token.color = 0;
			token.keycap = true;
			token.spaceBefore = pendingSpace;
			token.width = OverlayKeycapWidth(segment.text, false);
			token.spaceWidth = overlay_fonts.reg12->CalcTextSizeA(overlay_fonts.reg12->FontSize, FLT_MAX, 0.f, " ").x;
			pendingSpace = false;
			continue;
		}
		float fontHeight = segment.font->FontSize;
		if (fontHeight > lineHeight) {
			lineHeight = fontHeight;
		}
		float spaceWidth = segment.font->CalcTextSizeA(segment.font->FontSize, FLT_MAX, 0.f, " ").x;
		const char* cursor = segment.text;
		while (*cursor && tokenCount < 48) {
			if (*cursor == ' ') {
				pendingSpace = true;
				cursor++;
				continue;
			}
			const char* wordEnd = cursor;
			while (*wordEnd && *wordEnd != ' ') {
				wordEnd++;
			}
			OVERLAY_FLOW_TOKEN &token = tokens[tokenCount++];
			token.begin = cursor;
			token.end = wordEnd;
			token.font = segment.font;
			token.color = segment.color;
			token.keycap = false;
			token.spaceBefore = pendingSpace;
			token.width = segment.font->CalcTextSizeA(segment.font->FontSize, FLT_MAX, 0.f, cursor, wordEnd).x;
			token.spaceWidth = spaceWidth;
			pendingSpace = false;
			cursor = wordEnd;
		}
	}
	if (lineHeight <= 0.f) {
		lineHeight = overlay_fonts.reg13->FontSize;
	}
	float keycapHeight = anyKeycap ? OverlayKeycapHeight(false) : 0.f;
	if (keycapHeight > lineHeight) {
		lineHeight = keycapHeight;
	}
	float lineAdvance = lineHeight + OverlayPx(4.f);

	float x = 0.f;
	float y = 0.f;
	for (int iToken = 0; iToken < tokenCount; iToken++) {
		OVERLAY_FLOW_TOKEN &token = tokens[iToken];
		float spaceSpan = (token.spaceBefore && x > 0.f) ? token.spaceWidth : 0.f;
		if (x > 0.f && x + spaceSpan + token.width > max_width) {
			x = 0.f;
			y += lineAdvance;
			spaceSpan = 0.f;
		}
		x += spaceSpan;
		if (draw_list) {
			if (token.keycap) {
				char keycapLabel[24];
				size_t labelLength = (size_t)(token.end - token.begin);
				if (labelLength > sizeof(keycapLabel) - 1) {
					labelLength = sizeof(keycapLabel) - 1;
				}
				memcpy(keycapLabel, token.begin, labelLength);
				keycapLabel[labelLength] = 0;
				OverlayDrawKeycap(
					draw_list
					, ImVec2(origin.x + x, origin.y + y + (lineHeight - OverlayKeycapHeight(false)) * 0.5f)
					, keycapLabel
					, false
				);
			}
			else {
				draw_list->AddText(
					token.font
					, token.font->FontSize
					, ImVec2(origin.x + x, origin.y + y + (lineHeight - token.font->FontSize) * 0.5f)
					, OverlayAlpha(token.color)
					, token.begin
					, token.end
				);
			}
		}
		x += token.width;
	}
	return y + lineHeight;
}

// --- brand orb ----------------------------------------------------------------------

void OverlayDrawBrandOrb(ImDrawList* draw_list, ImVec2 center, float radius)
{
	const float quarter = 3.14159265f * 0.5f;
	const float startAngle = 0.7f;
	ImU32 quadrantColors[4] = {
		IM_COL32(225, 73, 63, 255)
		, IM_COL32(125, 191, 60, 255)
		, IM_COL32(61, 155, 233, 255)
		, IM_COL32(242, 177, 46, 255)
	};
	for (int iQuadrant = 0; iQuadrant < 4; iQuadrant++) {
		draw_list->PathLineTo(center);
		draw_list->PathArcTo(center, radius, startAngle + quarter * iQuadrant, startAngle + quarter * (iQuadrant + 1), 12);
		draw_list->PathFillConvex(OverlayAlpha(quadrantColors[iQuadrant]));
	}
	// Bottom-left ambient occlusion, then a top-left specular highlight.
	draw_list->AddCircle(center, radius * 0.86f, OverlayAlpha(IM_COL32(0, 0, 0, 60)), 0, radius * 0.28f);
	draw_list->AddCircleFilled(
		ImVec2(center.x - radius * 0.3f, center.y - radius * 0.35f)
		, radius * 0.35f
		, OverlayAlpha(IM_COL32(255, 255, 255, 80))
	);
	draw_list->AddCircle(center, radius, OverlayAlpha(IM_COL32(255, 255, 255, 60)), 0, OverlayPx(1.2f));
}
