#pragma once
#include <winsock2.h>
#include <Windows.h>
#include <stdint.h>

// Gamepad input for the overlay. XInput is loaded at run time: a link against a version the machine
// lacks stops the whole dll from loading.

void InitOverlayGamepad();
void UninitOverlayGamepad();

// Reads the pads and records a guide request. Runs once per Title frame, before the frame path
// returns on a closed guide, so a pad can still open it.
void OverlayGamepadPoll(HWND title_hwnd);

// Takes the pending guide request. Lock free: the hotkey thread calls this inside its own section.
bool OverlayGamepadTakeGuideRequest();

// Feeds the pad to the interface before its frame; a closed guide gets a released pad.
void OverlayGamepadNewFrame(bool guide_open);

// True while a pad is connected and answering.
bool OverlayGamepadIsConnected();

// True when the loaded XInput reports the Guide button; otherwise View+Menu is the only chord.
bool OverlayGamepadHasGuideButton();

// X, read from the raw pad: the interface takes that button for its own window switching (layer
// toggle, window picker), so X is never fed to it.
bool OverlayGamepadFaceLeftDown();
