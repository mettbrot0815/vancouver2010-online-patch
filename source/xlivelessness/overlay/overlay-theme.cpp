#include <winsock2.h>
#include <Windows.h>
#include "overlay-theme.hpp"
#include "overlay-widgets.hpp"
#include "overlay-events.hpp"
#include "../xlln/xlln-config.hpp"
#include "../xlln/debug-log.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <string.h>

uint32_t xlln_overlay_theme = (uint32_t)OverlayTheme::DARK;
bool xlln_overlay_theme_classic_unlocked = false;

OVERLAY_PALETTE overlay_palette;

OverlayTheme OverlayThemeInEffect()
{
	uint32_t theme = xlln_overlay_theme;
	if (theme >= OVERLAY_THEME_COUNT) {
		theme = (uint32_t)OverlayTheme::DARK;
	}
	if (theme == (uint32_t)OverlayTheme::CLASSIC && !xlln_overlay_theme_classic_unlocked) {
		theme = (uint32_t)OverlayTheme::DARK;
	}
	return (OverlayTheme)theme;
}

OverlayTheme OverlayThemeNext()
{
	uint32_t next = ((uint32_t)OverlayThemeInEffect() + 1) % OVERLAY_THEME_COUNT;
	if (next == (uint32_t)OverlayTheme::CLASSIC && !xlln_overlay_theme_classic_unlocked) {
		next = (uint32_t)OverlayTheme::DARK;
	}
	return (OverlayTheme)next;
}

const char* OverlayThemeName(OverlayTheme theme)
{
	switch (theme) {
		case OverlayTheme::LIGHT: return "light";
		case OverlayTheme::CLASSIC: return "classic";
		default: return "dark";
	}
}

void OverlayThemeCycle()
{
	xlln_overlay_theme = (uint32_t)OverlayThemeNext();
	XllnConfig(true);
}

// --- palettes ---------------------------------------------------------------------

// Washes are white, so a hover lightens.
static void OverlayThemeFillDark(OVERLAY_PALETTE &palette)
{
	palette.text = IM_COL32(243, 246, 251, 255);
	palette.textDim = IM_COL32(169, 178, 194, 255);
	palette.textFaint = IM_COL32(110, 119, 136, 255);
	palette.green = IM_COL32(70, 208, 122, 255);
	palette.red = IM_COL32(229, 72, 77, 255);
	palette.redSoft = IM_COL32(255, 180, 182, 255);
	palette.glass = IM_COL32(14, 17, 24, 219);
	palette.glass2 = IM_COL32(20, 24, 33, 235);
	palette.border = IM_COL32(255, 255, 255, 23);
	palette.borderHi = IM_COL32(255, 255, 255, 41);
	palette.wash = IM_COL32(255, 255, 255, 255);
	palette.keycapTop = IM_COL32(43, 48, 60, 255);
	palette.keycapBottom = IM_COL32(26, 30, 39, 255);
	palette.keycapLip = IM_COL32(0, 0, 0, 140);
	palette.keycapText = palette.text;
	palette.avatarRing = IM_COL32(22, 26, 34, 255);
	palette.dim = IM_COL32(5, 6, 10, 128);
	palette.fieldBg = IM_COL32(10, 12, 18, 235);
	palette.buttonTop = IM_COL32(255, 255, 255, 20);
	palette.buttonBottom = IM_COL32(255, 255, 255, 12);
	palette.buttonBorder = palette.borderHi;
	palette.cardFill = IM_COL32(255, 255, 255, 10);
	palette.cardBorder = palette.border;
}

// Washes are dark, so a hover darkens. Green and red are darker than Dark's, for contrast on white.
static void OverlayThemeFillLight(OVERLAY_PALETTE &palette)
{
	palette.text = IM_COL32(27, 34, 48, 255);
	palette.textDim = IM_COL32(84, 96, 116, 255);
	palette.textFaint = IM_COL32(132, 142, 158, 255);
	palette.green = IM_COL32(22, 142, 84, 255);
	palette.red = IM_COL32(204, 52, 58, 255);
	palette.redSoft = IM_COL32(178, 40, 46, 255);
	palette.glass = IM_COL32(247, 249, 252, 232);
	palette.glass2 = IM_COL32(252, 253, 255, 242);
	palette.border = IM_COL32(22, 30, 46, 30);
	palette.borderHi = IM_COL32(22, 30, 46, 52);
	palette.wash = IM_COL32(22, 30, 46, 255);
	palette.keycapTop = IM_COL32(255, 255, 255, 255);
	palette.keycapBottom = IM_COL32(226, 231, 239, 255);
	palette.keycapLip = IM_COL32(0, 0, 0, 48);
	palette.keycapText = palette.text;
	palette.avatarRing = IM_COL32(250, 251, 253, 255);
	palette.dim = IM_COL32(5, 6, 10, 128);
	palette.fieldBg = IM_COL32(238, 242, 247, 255);
	palette.buttonTop = IM_COL32(255, 255, 255, 255);
	palette.buttonBottom = IM_COL32(238, 241, 246, 255);
	palette.buttonBorder = palette.borderHi;
	palette.cardFill = IM_COL32(22, 30, 46, 8);
	palette.cardBorder = IM_COL32(22, 30, 46, 26);
}

// White and pale blue, a blue bar over each heading, glossy tiles, and a navy dim over the game.
static void OverlayThemeFillClassic(OVERLAY_PALETTE &palette)
{
	palette.text = IM_COL32(38, 72, 110, 255);
	palette.textDim = IM_COL32(92, 124, 158, 255);
	palette.textFaint = IM_COL32(140, 164, 190, 255);
	palette.green = IM_COL32(22, 142, 84, 255);
	palette.red = IM_COL32(204, 52, 58, 255);
	palette.redSoft = IM_COL32(178, 40, 46, 255);
	palette.glass = IM_COL32(238, 244, 250, 242);
	palette.glass2 = IM_COL32(228, 238, 248, 245);
	palette.border = IM_COL32(52, 104, 156, 70);
	palette.borderHi = IM_COL32(52, 104, 156, 120);
	palette.wash = IM_COL32(30, 80, 130, 255);
	palette.keycapTop = IM_COL32(255, 255, 255, 255);
	palette.keycapBottom = IM_COL32(220, 232, 245, 255);
	palette.keycapLip = IM_COL32(52, 104, 156, 100);
	palette.keycapText = palette.text;
	palette.avatarRing = IM_COL32(240, 246, 252, 255);
	palette.dim = IM_COL32(6, 10, 52, 160);
	palette.fieldBg = IM_COL32(255, 255, 255, 255);
	palette.buttonTop = IM_COL32(255, 255, 255, 255);
	palette.buttonBottom = IM_COL32(222, 234, 246, 255);
	palette.buttonBorder = IM_COL32(120, 160, 200, 220);
	palette.cardFill = IM_COL32(255, 255, 255, 150);
	palette.cardBorder = IM_COL32(140, 175, 210, 170);
	palette.tabTop = IM_COL32(255, 255, 255, 235);
	palette.tabBottom = IM_COL32(216, 230, 244, 235);
	palette.tabBorder = IM_COL32(120, 160, 200, 200);
	palette.headingTop = IM_COL32(120, 190, 240, 255);
	palette.headingBottom = IM_COL32(36, 120, 200, 255);
	palette.headingText = IM_COL32(255, 255, 255, 255);
	palette.headingFrameTop = IM_COL32(248, 250, 253, 255);
	palette.headingFrameBottom = IM_COL32(206, 216, 228, 255);
	palette.headingFrameBorder = IM_COL32(150, 168, 190, 220);
	palette.amberGloss = 100;
	palette.panelTop = IM_COL32(252, 254, 255, 255);
	palette.panelBottom = IM_COL32(206, 224, 242, 255);
	palette.bandGloss = 80;
}

void OverlayThemeFillPalette(OverlayTheme theme)
{
	// Every optional entry starts transparent; a theme sets only the ones it wants.
	memset(&overlay_palette, 0, sizeof(overlay_palette));
	// The amber marks the chosen item in every theme, so it and the text colour on it are set once.
	overlay_palette.amber = IM_COL32(255, 167, 51, 255);
	overlay_palette.amberHi = IM_COL32(255, 180, 74, 255);
	overlay_palette.amberLo = IM_COL32(240, 138, 29, 255);
	overlay_palette.amberInk = IM_COL32(35, 19, 3, 255);
	switch (theme) {
		case OverlayTheme::LIGHT: {
			OverlayThemeFillLight(overlay_palette);
			break;
		}
		case OverlayTheme::CLASSIC: {
			OverlayThemeFillClassic(overlay_palette);
			break;
		}
		default: {
			OverlayThemeFillDark(overlay_palette);
			break;
		}
	}
}

// --- the sequence that unlocks CLASSIC ------------------------------------------------

enum class OverlaySecretInput : uint32_t {
	NONE = 0,
	// More than one input in the same frame.
	MANY,
	UP,
	DOWN,
	LEFT,
	RIGHT,
	B,
	A,
};

static const OverlaySecretInput overlay_secret_sequence[] = {
	OverlaySecretInput::UP,
	OverlaySecretInput::UP,
	OverlaySecretInput::DOWN,
	OverlaySecretInput::DOWN,
	OverlaySecretInput::LEFT,
	OverlaySecretInput::RIGHT,
	OverlaySecretInput::LEFT,
	OverlaySecretInput::RIGHT,
	OverlaySecretInput::B,
	OverlaySecretInput::A,
};
#define OVERLAY_SECRET_LENGTH (sizeof(overlay_secret_sequence) / sizeof(overlay_secret_sequence[0]))
// A pause longer than this between two steps starts the sequence over.
#define OVERLAY_SECRET_STEP_TIMEOUT 3.0
// Any value that is not 0 and not -1 claims a key. Nothing else in the overlay claims one.
#define OVERLAY_SECRET_KEY_OWNER ((ImGuiID)0x584C4C4E)

static uint32_t overlay_secret_progress = 0;
static double overlay_secret_step_time = 0.0;

// The input pressed this frame, from the arrow keys and the letters, or from the pad.
static OverlaySecretInput OverlaySecretInputPressed()
{
	static const struct {
		ImGuiKey keyboardKey;
		ImGuiKey gamepadKey;
		OverlaySecretInput input;
	} inputKeys[] = {
		{ ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, OverlaySecretInput::UP },
		{ ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, OverlaySecretInput::DOWN },
		{ ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, OverlaySecretInput::LEFT },
		{ ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, OverlaySecretInput::RIGHT },
		{ ImGuiKey_B, ImGuiKey_GamepadFaceRight, OverlaySecretInput::B },
		{ ImGuiKey_A, ImGuiKey_GamepadFaceDown, OverlaySecretInput::A },
	};
	OverlaySecretInput pressed = OverlaySecretInput::NONE;
	for (size_t iKey = 0; iKey < sizeof(inputKeys) / sizeof(inputKeys[0]); iKey++) {
		if (!ImGui::IsKeyPressed(inputKeys[iKey].keyboardKey, false) && !ImGui::IsKeyPressed(inputKeys[iKey].gamepadKey, false)) {
			continue;
		}
		if (pressed != OverlaySecretInput::NONE) {
			return OverlaySecretInput::MANY;
		}
		pressed = inputKeys[iKey].input;
	}
	return pressed;
}

// Unlocks the classic theme and switches to it. The toast is shown only the first time.
static void OverlaySecretUnlock()
{
	bool firstTime = !xlln_overlay_theme_classic_unlocked;
	xlln_overlay_theme_classic_unlocked = true;
	xlln_overlay_theme = (uint32_t)OverlayTheme::CLASSIC;
	XllnConfig(true);
	if (firstTime) {
		OverlayEventQueuePush(OverlayEventType::ACHIEVEMENT_UNLOCKED, "Classic theme", "The sun in the band now cycles through three themes.");
	}
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s the classic theme was unlocked%s."
		, __func__
		, firstTime ? "" : " again"
	);
}

bool OverlayThemeSecretUpdate()
{
	double timeNow = ImGui::GetTime();
	if (overlay_secret_progress && timeNow - overlay_secret_step_time > OVERLAY_SECRET_STEP_TIMEOUT) {
		overlay_secret_progress = 0;
	}

	bool tookCancel = false;
	OverlaySecretInput pressed = OverlaySecretInputPressed();
	if (pressed == OverlaySecretInput::MANY) {
		overlay_secret_progress = 0;
	}
	else if (pressed != OverlaySecretInput::NONE) {
		if (pressed == overlay_secret_sequence[overlay_secret_progress]) {
			overlay_secret_progress++;
			tookCancel = pressed == OverlaySecretInput::B;
		}
		else {
			// A wrong step can still be the first step of a new attempt.
			overlay_secret_progress = pressed == overlay_secret_sequence[0] ? 1 : 0;
		}
		overlay_secret_step_time = timeNow;
		if (overlay_secret_progress >= OVERLAY_SECRET_LENGTH) {
			overlay_secret_progress = 0;
			OverlaySecretUnlock();
		}
	}

	// The interface reads B as cancel and A as activate, so the awaited key is claimed while the
	// sequence waits for it. A claim lasts one frame, so it is renewed each frame.
	OverlaySecretInput next = overlay_secret_progress < OVERLAY_SECRET_LENGTH
		? overlay_secret_sequence[overlay_secret_progress]
		: OverlaySecretInput::NONE;
	if (next == OverlaySecretInput::B) {
		ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, OVERLAY_SECRET_KEY_OWNER);
	}
	else if (next == OverlaySecretInput::A) {
		ImGui::SetKeyOwner(ImGuiKey_GamepadFaceDown, OVERLAY_SECRET_KEY_OWNER);
	}
	return tookCancel;
}
