#pragma once
#include <stdint.h>

// The themes the overlay draws in. CLASSIC stays hidden until a player enters the unlock sequence.
enum class OverlayTheme : uint32_t {
	DARK = 0,
	LIGHT,
	CLASSIC,
};
#define OVERLAY_THEME_COUNT 3

// Config (persisted through xlln-config).
extern uint32_t xlln_overlay_theme;
extern bool xlln_overlay_theme_classic_unlocked;

// The theme that is drawn: the choice, or DARK when the choice is a CLASSIC that is still locked.
OverlayTheme OverlayThemeInEffect();
// The theme the band's sun button goes to next.
OverlayTheme OverlayThemeNext();
// A lower-case name for use inside a sentence.
const char* OverlayThemeName(OverlayTheme theme);
// Fills overlay_palette with the colours of a theme.
void OverlayThemeFillPalette(OverlayTheme theme);
// Moves the choice to the next theme that is offered and writes the config.
void OverlayThemeCycle();

// Once per frame while the band and panel show. Watches for the unlock sequence; returns true when
// this frame's B press was a step of it, so the guide does not also read it as a close.
bool OverlayThemeSecretUpdate();
