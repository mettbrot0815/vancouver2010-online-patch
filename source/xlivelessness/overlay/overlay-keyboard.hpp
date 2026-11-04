#pragma once
#include <winsock2.h>
#include <Windows.h>
#include <stdint.h>
#include "../xlive/xdefs.hpp"

// The text entry the Title asks for through XShowKeyboardUI, drawn by the overlay so it appears
// over the game in every display mode. The desktop window answers only what the overlay cannot.

void InitOverlayKeyboard();
void UninitOverlayKeyboard();

// Hands a text request to the overlay. Any thread. The strings are copied, so the caller does not
// have to keep them. Returns false when the overlay cannot answer the request, and the caller then
// falls back to the desktop window.
bool OverlayKeyboardRequest(
	uint32_t flags
	, const wchar_t* default_text
	, const wchar_t* title_text
	, const wchar_t* description_text
	, wchar_t* result_text
	, size_t result_text_size
	, XOVERLAPPED* xoverlapped
);

// True from the moment a request is taken until it is answered. Any thread.
bool OverlayKeyboardIsOpen();

// True while the text field holds the keyboard. The guide hotkey defaults to Home, which is also
// how a player moves the caret, so the hotkey thread reads this and stands off. Lock free.
bool OverlayKeyboardHoldsTextFocus();

// Render thread, inside the frame.
void OverlayKeyboardBuild();
// Render thread. Answers an outstanding request with ERROR_CANCELLED.
void OverlayKeyboardCancelActive();

// Completes the XOVERLAPPED for a request that the frame finished. This runs the Title's own
// completion routine, so it must be called after the frame lock is released, never inside it.
void OverlayKeyboardFlushCompletion();
