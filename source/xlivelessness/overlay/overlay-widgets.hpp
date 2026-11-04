#pragma once
#include <imgui.h>
#include <stdint.h>

// Palette and draw helpers for the XLLN overlay's built-in UI.
// Sizes passed to these helpers are design units. Each helper multiplies them by the
// current UI scale through OverlayPx(). Colors passed to the draw helpers are multiplied
// by overlay_widget_alpha, so the parts drawn straight onto the draw list fade at the
// same rate as the ImGui window that holds them.

// --- palette --------------------------------------------------------------------
// The colours of the theme in effect, filled by overlay-theme before a frame draws and read through
// the OVERLAY_COL_* names below. A theme leaves an optional entry it does not want transparent.
typedef struct _OVERLAY_PALETTE {
	ImU32 text;
	ImU32 textDim;
	ImU32 textFaint;
	ImU32 amber;
	ImU32 amberHi;
	ImU32 amberLo;
	// Text on amber.
	ImU32 amberInk;
	ImU32 green;
	ImU32 red;
	// Danger text.
	ImU32 redSoft;
	// Band and toasts.
	ImU32 glass;
	// Panels and modals.
	ImU32 glass2;
	ImU32 border;
	ImU32 borderHi;
	// The colour of the hover and surface washes. Its own alpha is not used; each wash gives one.
	ImU32 wash;
	ImU32 keycapTop;
	ImU32 keycapBottom;
	ImU32 keycapLip;
	ImU32 keycapText;
	// The ring around the presence dot on an avatar.
	ImU32 avatarRing;
	// Over the game while the guide is open, at full strength.
	ImU32 dim;
	// A text field.
	ImU32 fieldBg;
	ImU32 buttonTop;
	ImU32 buttonBottom;
	ImU32 buttonBorder;
	ImU32 cardFill;
	ImU32 cardBorder;
	// Optional: a tile behind a tab that is not selected.
	ImU32 tabTop;
	ImU32 tabBottom;
	ImU32 tabBorder;
	// Optional: a bar behind a panel's heading, and the colour of the heading on it.
	ImU32 headingTop;
	ImU32 headingBottom;
	ImU32 headingText;
	// Optional: a raised frame from the top edge of the panel to just under the heading bar.
	ImU32 headingFrameTop;
	ImU32 headingFrameBottom;
	ImU32 headingFrameBorder;
	// Alpha of the sheen over the upper half of an amber surface and of its bottom lip. 0 = none.
	uint32_t amberGloss;
	// Optional: a gradient over the panel window's flat background.
	ImU32 panelTop;
	ImU32 panelBottom;
	// The alpha of a white sheen over the upper half of the band. 0 = none.
	uint32_t bandGloss;
} OVERLAY_PALETTE;
extern OVERLAY_PALETTE overlay_palette;

inline ImU32 OverlayColorWithAlpha(ImU32 color, uint32_t alpha)
{
	return (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

#define OVERLAY_COL_TEXT (overlay_palette.text)
#define OVERLAY_COL_TEXT_DIM (overlay_palette.textDim)
#define OVERLAY_COL_TEXT_FAINT (overlay_palette.textFaint)
#define OVERLAY_COL_AMBER (overlay_palette.amber)
#define OVERLAY_COL_AMBER_HI (overlay_palette.amberHi)
#define OVERLAY_COL_AMBER_LO (overlay_palette.amberLo)
#define OVERLAY_COL_AMBER_INK (overlay_palette.amberInk)
// The light rim on an amber surface. The same in every theme, because the surface is.
#define OVERLAY_COL_AMBER_RIM IM_COL32(255, 255, 255, 64)
#define OVERLAY_COL_GREEN (overlay_palette.green)
#define OVERLAY_COL_RED (overlay_palette.red)
#define OVERLAY_COL_RED_SOFT (overlay_palette.redSoft)
#define OVERLAY_COL_GLASS (overlay_palette.glass)
#define OVERLAY_COL_GLASS_2 (overlay_palette.glass2)
#define OVERLAY_COL_BORDER (overlay_palette.border)
#define OVERLAY_COL_BORDER_HI (overlay_palette.borderHi)
#define OVERLAY_COL_WASH_A(a) OverlayColorWithAlpha(overlay_palette.wash, (a))
#define OVERLAY_COL_KEYCAP_TOP (overlay_palette.keycapTop)
#define OVERLAY_COL_KEYCAP_BOTTOM (overlay_palette.keycapBottom)
#define OVERLAY_COL_AVATAR_RING (overlay_palette.avatarRing)

// Set by the overlay each frame before building UI.
extern float overlay_widget_scale;
extern float overlay_widget_alpha;

typedef struct _OVERLAY_FONT_SET {
	ImFont* reg12 = 0;
	ImFont* reg13 = 0;
	ImFont* semi11 = 0;
	ImFont* semi12 = 0;
	ImFont* semi13 = 0;
	ImFont* bold13 = 0;
	ImFont* bold16 = 0;
	ImFont* bold17 = 0;
	ImFont* bold22 = 0;
} OVERLAY_FONT_SET;
extern OVERLAY_FONT_SET overlay_fonts;

// --- primitives -----------------------------------------------------------------

inline float OverlayPx(float design_px)
{
	return design_px * overlay_widget_scale;
}

ImU32 OverlayAlpha(ImU32 color);
ImU32 OverlayColorLerp(ImU32 color_a, ImU32 color_b, float t);
// The dim the theme puts over the game, at a fraction of its full strength.
ImU32 OverlayDimColor(float strength);
// Washed-out variant used for offline identities (mimics saturate(.15) brightness(.7)).
ImU32 OverlayColorDesaturate(ImU32 color);

// Rounded rectangle with a vertical two-stop gradient (clip-strip based, any rounding).
void OverlayDrawRectVGradient(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, ImU32 color_top, ImU32 color_bottom, float rounding, ImDrawFlags draw_flags = 0);
// A white sheen from alpha_top to alpha_bottom, top corners rounded, so a surface looks like glass.
void OverlayDrawSheen(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, float rounding, uint32_t alpha_top, uint32_t alpha_bottom);
// An amber surface: the gradient, the sheen and the lip when the theme asks for them, and the rim.
void OverlayDrawAmberFace(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, float rounding, ImU32 color_top, ImU32 color_bottom);
// Rounded rectangle with a horizontal three-stop gradient (invite modal art).
void OverlayDrawRectHGradient3(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, ImU32 color_0, ImU32 color_1, ImU32 color_2, float middle_stop, float rounding, ImDrawFlags draw_flags = 0);
// Rounded rectangle with a 135-degree gradient (avatars). Corners are flat-shaded
// over `rounding` px which is imperceptible at avatar sizes.
void OverlayDrawRectDiagGradient(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, ImU32 color_top_left, ImU32 color_bottom_right, float rounding);
// Layered drop shadow / glow approximations (ImGui has no blur).
void OverlayDrawSoftShadow(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, float rounding, float offset_y, float spread, int layer_alpha);
void OverlayDrawGlow(ImDrawList* draw_list, ImVec2 rect_min, ImVec2 rect_max, float rounding, ImU32 color, float spread, int layer_alpha);

// Letter-spaced label (eyebrow headers, wordmark sublabel). Tracking in scaled px.
ImVec2 OverlayTrackedTextSize(ImFont* font, const char* text, float tracking);
void OverlayDrawTrackedText(ImDrawList* draw_list, ImFont* font, ImVec2 position, ImU32 color, const char* text, float tracking);

// --- components -----------------------------------------------------------------

// Keyboard keycap. standalone = hint pill / legend sizing difference.
float OverlayKeycapHeight(bool standalone);
float OverlayKeycapWidth(const char* label, bool standalone);
void OverlayDrawKeycap(ImDrawList* draw_list, ImVec2 position, const char* label, bool standalone);
// Layout-participating keycap (advances the ImGui cursor like an item).
void OverlayKeycapItem(const char* label, bool standalone);

// Identity avatar: squircle with a per-name gradient, initial and optional
// presence dot. amber = system/highlight identity (achievements, hints).
void OverlayDrawAvatar(ImDrawList* draw_list, ImVec2 position, float size_px, const char* seed_name, bool amber, bool online_dot, bool dimmed);
// Green success variant with a check mark instead of an initial (join flow).
void OverlayDrawAvatarCheck(ImDrawList* draw_list, ImVec2 position, float size_px);
void OverlayAvatarItem(const char* seed_name, float design_size, bool amber, bool online_dot, bool dimmed);

// Notification count badge (tab bar).
float OverlayBadgeWidth(int count);
void OverlayDrawBadge(ImDrawList* draw_list, ImVec2 position, int count, bool on_amber);

enum class OverlayButtonKind : uint32_t {
	SECONDARY = 0,
	PRIMARY,
	DANGER,
};
float OverlayButtonHeight();
float OverlayButtonWidth(const char* label);
// Pill button. size (in scaled px) optional per axis; trailing_hint renders a dim
// right-aligned annotation (quick-action rows).
bool OverlayButton(const char* str_id, const char* label, OverlayButtonKind kind, ImVec2 size_px = ImVec2(0.f, 0.f), const char* trailing_hint = 0);

enum class OverlayIconType : uint32_t {
	CLOSE = 0,
	// A disc with rays around it: the button that moves to the next theme.
	LIGHT,
	// A cog: one outline of eight teeth around a hub.
	SETTINGS,
};
// selected draws the button the way a chosen tab is drawn, for an icon that opens a panel.
bool OverlayIconButton(const char* str_id, OverlayIconType icon_type, bool selected = false);

// Top-band tab. Selected = amber gradient pill with glow.
bool OverlayTabItem(const char* label, bool selected, int badge_count);

// Static outlined pill (session meta). Optional leading dot.
void OverlayChip(const char* label, ImU32 text_color, ImU32 border_color, ImU32 dot_color = 0);

// Letter-spaced section header (NOW PLAYING / QUICK ACTIONS / PROFILE).
void OverlayEyebrowLabel(const char* label);

void OverlayDrawMeter(ImDrawList* draw_list, ImVec2 position, float width, float height, float fraction, ImU32 fill_color);

// Amber focus outline on the last item while keyboard/controller navigation targets it.
void OverlayFocusRingForLastItem(float rounding);

// Hoverable list row (players, invites). Content between Begin/End gets a rounded
// hover wash behind it. Not nestable with itself or with cards.
void OverlayRowBegin(const char* str_id);
void OverlayRowEnd();
ImVec2 OverlayRowOrigin();
float OverlayRowWidth();

// Subtle surface card (home panel). width_px is the final scaled width;
// minimum_height_px keeps sibling cards in a grid row the same height.
void OverlayCardBegin(const char* str_id, float width_px);
float OverlayCardEnd(float width_px, float minimum_height_px = 0.f);

// Mixed-style word-wrapped inline text (toast headlines: bold gamertag + regular
// verb, inline keycaps). Pass a null draw list to measure. Returns total height.
typedef struct _OVERLAY_INLINE_SEGMENT {
	const char* text = 0;
	ImFont* font = 0;
	ImU32 color = 0;
	bool keycap = false;
} OVERLAY_INLINE_SEGMENT;
float OverlayInlineFlow(ImDrawList* draw_list, ImVec2 origin, float max_width, const OVERLAY_INLINE_SEGMENT* segments, int segment_count);

// The four-color XLLN orb.
void OverlayDrawBrandOrb(ImDrawList* draw_list, ImVec2 center, float radius);
