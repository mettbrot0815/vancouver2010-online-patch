#pragma once
#include <winsock2.h>
#include <Windows.h>
#include "../xlive/xdefs.hpp"

extern CRITICAL_SECTION xlln_critsec_overlay_input;

void InitOverlayInput();
void UninitOverlayInput();

// Returns true when the overlay consumed the message (guide open + input message).
bool OverlayInputOnXLiveInput(XLIVE_INPUT_INFO* xlive_input_info);
bool OverlayInputOnPreTranslateMessage(const MSG* window_message);

// Render-thread, once per overlay frame: drain queued message events and poll devices.
void OverlayInputNewFrame(HWND title_hwnd, bool guide_open);

// Guide open/close transitions (render thread).
void OverlayInputOnGuideOpened(HWND title_hwnd);
void OverlayInputOnGuideClosed();
