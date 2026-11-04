#include <winsock2.h>
#include <Windows.h>
#include "overlay-input.hpp"
#include "overlay.hpp"
#include "overlay-gamepad.hpp"
#include "../xlln/debug-log.hpp"
#include <imgui.h>
#include <vector>

CRITICAL_SECTION xlln_critsec_overlay_input;

enum class OverlayInputEventType : uint32_t {
	MOUSE_WHEEL_VERTICAL,
	MOUSE_WHEEL_HORIZONTAL,
	CHARACTER,
};

typedef struct _OVERLAY_INPUT_EVENT {
	OverlayInputEventType type;
	float wheelDelta;
	uint32_t character;
} OVERLAY_INPUT_EVENT;

// Written by whichever thread delivers window messages, drained on the render thread.
static std::vector<OVERLAY_INPUT_EVENT> overlay_input_queued_events;
static bool overlay_input_window_focused = true;
// The title window, and which keys and buttons the guide swallowed input for. On close, any that
// the title may believe are held but that are now released get a synthetic release, so a key or
// button held when the guide opened and let go while it was open does not stay stuck down in a
// message-driven title.
static HWND overlay_input_title_hwnd = 0;
// Only a press sets these. A release is replayed to the Title on close for a press the guide
// swallowed, so anything that is not a press must not mark them: mouse movement is not a click,
// and a dead-key carries a character code where a virtual-key would be.
static bool overlay_input_swallowed_key[256] = {false};
static bool overlay_input_swallowed_mouse_button[3] = {false, false, false};

void InitOverlayInput()
{
	overlay_input_queued_events.reserve(16);
}

void UninitOverlayInput()
{
	OverlayInputOnGuideClosed();
	EnterCriticalSection(&xlln_critsec_overlay_input);
	overlay_input_queued_events.clear();
	LeaveCriticalSection(&xlln_critsec_overlay_input);
}

static void OverlayInputQueueEvent(const OVERLAY_INPUT_EVENT &input_event)
{
	EnterCriticalSection(&xlln_critsec_overlay_input);
	if (overlay_input_queued_events.size() < 256) {
		overlay_input_queued_events.push_back(input_event);
	}
	LeaveCriticalSection(&xlln_critsec_overlay_input);
}

// Translates one window message into overlay input. Returns true when the message
// belongs to the overlay and must not reach the title. Runs on message threads.
static bool OverlayInputHandleWindowMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// A registered UI provider decides for itself (only consulted while its UI is open).
	if (OverlayProviderIsActive()) {
		return OverlayProviderHandleWindowMessage(hWnd, uMsg, wParam, lParam);
	}

	if (!OverlayIsGuideOpen()) {
		return false;
	}

	switch (uMsg) {
		case WM_CLOSE: {
			OverlayGuideRequestClose();
			return false;
		}
		case WM_MOUSEWHEEL: {
			OVERLAY_INPUT_EVENT inputEvent;
			inputEvent.type = OverlayInputEventType::MOUSE_WHEEL_VERTICAL;
			inputEvent.wheelDelta = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
			OverlayInputQueueEvent(inputEvent);
			return true;
		}
		case WM_MOUSEHWHEEL: {
			OVERLAY_INPUT_EVENT inputEvent;
			inputEvent.type = OverlayInputEventType::MOUSE_WHEEL_HORIZONTAL;
			inputEvent.wheelDelta = (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA;
			OverlayInputQueueEvent(inputEvent);
			return true;
		}
		case WM_CHAR:
		case WM_SYSCHAR: {
			if (wParam > 0 && wParam < 0x10000) {
				OVERLAY_INPUT_EVENT inputEvent;
				inputEvent.type = OverlayInputEventType::CHARACTER;
				inputEvent.character = (uint32_t)wParam;
				OverlayInputQueueEvent(inputEvent);
			}
			return true;
		}
		case WM_SETCURSOR: {
			// The overlay draws its own software cursor; suppress the title's cursor churn.
			return LOWORD(lParam) == HTCLIENT;
		}
		case WM_INPUT: {
			// Deny raw input to the Title while the guide is up. The system only releases the packet
			// behind this message when it reaches DefWindowProc, and the Title never gets to make that
			// call for a message taken here, so make it on its behalf.
			DefWindowProcW(hWnd, uMsg, wParam, lParam);
			return true;
		}
	}

	// Keep the rest of the keyboard and mouse stream while the guide is open. The render thread
	// polls the keys and the buttons, so nothing here has to translate them.
	if (uMsg >= WM_KEYFIRST && uMsg <= WM_KEYLAST) {
		if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) && wParam < 256) {
			overlay_input_swallowed_key[wParam] = true;
		}
		return true;
	}
	if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST) {
		if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONDBLCLK) {
			overlay_input_swallowed_mouse_button[0] = true;
		}
		else if (uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONDBLCLK) {
			overlay_input_swallowed_mouse_button[1] = true;
		}
		else if (uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONDBLCLK) {
			overlay_input_swallowed_mouse_button[2] = true;
		}
		return true;
	}

	return false;
}

bool OverlayInputOnXLiveInput(XLIVE_INPUT_INFO* xlive_input_info)
{
	if (!xlive_input_info) {
		return false;
	}
	bool consumed = OverlayInputHandleWindowMessage(
		xlive_input_info->hWnd
		, xlive_input_info->uMSG
		, xlive_input_info->wParam
		, xlive_input_info->lParam
	);
	if (consumed) {
		xlive_input_info->lRet = (xlive_input_info->uMSG == WM_SETCURSOR) ? TRUE : 0;
	}
	return consumed;
}

// A kept key never reaches the Title's TranslateMessage, so its WM_CHAR would never be made.
static void OverlayInputTranslateKeptKey(const MSG* window_message)
{
	if (window_message->message == WM_KEYDOWN || window_message->message == WM_SYSKEYDOWN) {
		TranslateMessage(window_message);
	}
}

bool OverlayInputOnPreTranslateMessage(const MSG* window_message)
{
	if (!window_message) {
		return false;
	}
	if (!OverlayInputHandleWindowMessage(
		window_message->hwnd
		, window_message->message
		, window_message->wParam
		, window_message->lParam
	)) {
		return false;
	}
	OverlayInputTranslateKeptKey(window_message);
	return true;
}

void OverlayInputOnGuideOpened(HWND title_hwnd)
{
	overlay_input_title_hwnd = title_hwnd;
	memset(overlay_input_swallowed_key, 0, sizeof(overlay_input_swallowed_key));
	memset(overlay_input_swallowed_mouse_button, 0, sizeof(overlay_input_swallowed_mouse_button));
}

void OverlayInputOnGuideClosed()
{
	// Replay the releases the guide swallowed. Only a key or button that is physically up now is
	// released; one still held is genuinely down, so the title's belief already matches and it is
	// left alone.
	if (overlay_input_title_hwnd) {
		for (uint32_t virtualKey = 0; virtualKey < 256; virtualKey++) {
			if (overlay_input_swallowed_key[virtualKey] && !(GetAsyncKeyState((int)virtualKey) & 0x8000)) {
				LPARAM keyUpLParam = (LPARAM)((MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC) << 16) | 0xC0000001);
				PostMessageW(overlay_input_title_hwnd, WM_KEYUP, virtualKey, keyUpLParam);
			}
		}
		if (overlay_input_swallowed_mouse_button[0] || overlay_input_swallowed_mouse_button[1] || overlay_input_swallowed_mouse_button[2]) {
			POINT cursorPosition;
			LPARAM cursorLParam = 0;
			if (GetCursorPos(&cursorPosition) && ScreenToClient(overlay_input_title_hwnd, &cursorPosition)) {
				cursorLParam = MAKELPARAM(cursorPosition.x, cursorPosition.y);
			}
			static const int buttonVirtualKey[3] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON};
			static const UINT buttonUpMessage[3] = {WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP};
			for (int iButton = 0; iButton < 3; iButton++) {
				if (overlay_input_swallowed_mouse_button[iButton] && !(GetAsyncKeyState(buttonVirtualKey[iButton]) & 0x8000)) {
					PostMessageW(overlay_input_title_hwnd, buttonUpMessage[iButton], 0, cursorLParam);
				}
			}
		}
	}
	overlay_input_title_hwnd = 0;
	memset(overlay_input_swallowed_key, 0, sizeof(overlay_input_swallowed_key));
	memset(overlay_input_swallowed_mouse_button, 0, sizeof(overlay_input_swallowed_mouse_button));
}

typedef struct _OVERLAY_POLLED_KEY {
	int virtualKey;
	ImGuiKey imguiKey;
	bool wasDown;
} OVERLAY_POLLED_KEY;

// Navigation subset, plus what a text field needs for the caret and the editing shortcuts.
// Enough to drive the guide entirely from the keyboard even when no window messages reach the
// overlay. Typed characters arrive separately, through WM_CHAR.
static OVERLAY_POLLED_KEY overlay_polled_keys[] = {
	{ VK_ESCAPE, ImGuiKey_Escape, false },
	{ VK_RETURN, ImGuiKey_Enter, false },
	{ VK_TAB, ImGuiKey_Tab, false },
	{ VK_SPACE, ImGuiKey_Space, false },
	{ VK_BACK, ImGuiKey_Backspace, false },
	{ VK_DELETE, ImGuiKey_Delete, false },
	{ VK_UP, ImGuiKey_UpArrow, false },
	{ VK_DOWN, ImGuiKey_DownArrow, false },
	{ VK_LEFT, ImGuiKey_LeftArrow, false },
	{ VK_RIGHT, ImGuiKey_RightArrow, false },
	{ VK_PRIOR, ImGuiKey_PageUp, false },
	{ VK_NEXT, ImGuiKey_PageDown, false },
	{ VK_HOME, ImGuiKey_Home, false },
	{ VK_END, ImGuiKey_End, false },
	{ 'A', ImGuiKey_A, false },
	{ 'B', ImGuiKey_B, false },
	{ 'C', ImGuiKey_C, false },
	{ 'V', ImGuiKey_V, false },
	{ 'X', ImGuiKey_X, false },
	{ 'Y', ImGuiKey_Y, false },
	{ 'Z', ImGuiKey_Z, false },
};

static bool overlay_polled_mouse_buttons[3] = { false, false, false };

void OverlayInputNewFrame(HWND title_hwnd, bool guide_open)
{
	ImGuiIO &imguiIo = ImGui::GetIO();

	// Drain message-thread events.
	EnterCriticalSection(&xlln_critsec_overlay_input);
	for (size_t iEvent = 0; iEvent < overlay_input_queued_events.size(); iEvent++) {
		const OVERLAY_INPUT_EVENT &inputEvent = overlay_input_queued_events[iEvent];
		switch (inputEvent.type) {
			case OverlayInputEventType::MOUSE_WHEEL_VERTICAL: {
				imguiIo.AddMouseWheelEvent(0.f, inputEvent.wheelDelta);
				break;
			}
			case OverlayInputEventType::MOUSE_WHEEL_HORIZONTAL: {
				imguiIo.AddMouseWheelEvent(inputEvent.wheelDelta, 0.f);
				break;
			}
			case OverlayInputEventType::CHARACTER: {
				imguiIo.AddInputCharacterUTF16((ImWchar16)inputEvent.character);
				break;
			}
		}
	}
	overlay_input_queued_events.clear();
	LeaveCriticalSection(&xlln_critsec_overlay_input);

	// Before the return below, so a closed guide is still fed a pad at rest.
	OverlayGamepadNewFrame(guide_open);

	if (!guide_open) {
		return;
	}

	// Only react while the title window is actually in the foreground.
	HWND foregroundWindow = GetForegroundWindow();
	bool windowFocused = (title_hwnd && (foregroundWindow == title_hwnd || IsChild(title_hwnd, foregroundWindow)));
	if (windowFocused != overlay_input_window_focused) {
		overlay_input_window_focused = windowFocused;
		imguiIo.AddFocusEvent(windowFocused);
	}
	if (!windowFocused) {
		return;
	}

	// Mouse position by polling; works regardless of the title's input model. The cursor is in
	// window-client pixels, but ImGui hit-tests in DisplaySize (backbuffer) pixels, so scale when the
	// title presents a backbuffer of a different size than its window. Equal sizes leave it unchanged.
	POINT cursorPosition;
	RECT clientRect;
	if (GetCursorPos(&cursorPosition) && ScreenToClient(title_hwnd, &cursorPosition) && GetClientRect(title_hwnd, &clientRect)) {
		float clientWidth = (float)(clientRect.right - clientRect.left);
		float clientHeight = (float)(clientRect.bottom - clientRect.top);
		float mouseX = (float)cursorPosition.x;
		float mouseY = (float)cursorPosition.y;
		if (clientWidth > 0.f && clientHeight > 0.f) {
			mouseX *= imguiIo.DisplaySize.x / clientWidth;
			mouseY *= imguiIo.DisplaySize.y / clientHeight;
		}
		imguiIo.AddMousePosEvent(mouseX, mouseY);
	}

	// Mouse buttons by polling (respecting swapped buttons).
	bool buttonsSwapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
	int mouseButtonVirtualKeys[3] = {
		buttonsSwapped ? VK_RBUTTON : VK_LBUTTON
		, buttonsSwapped ? VK_LBUTTON : VK_RBUTTON
		, VK_MBUTTON
	};
	for (int iButton = 0; iButton < 3; iButton++) {
		bool isDown = (GetAsyncKeyState(mouseButtonVirtualKeys[iButton]) & 0x8000) != 0;
		if (isDown != overlay_polled_mouse_buttons[iButton]) {
			overlay_polled_mouse_buttons[iButton] = isDown;
			imguiIo.AddMouseButtonEvent(iButton, isDown);
		}
	}

	// Modifiers.
	imguiIo.AddKeyEvent(ImGuiMod_Ctrl, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
	imguiIo.AddKeyEvent(ImGuiMod_Shift, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
	imguiIo.AddKeyEvent(ImGuiMod_Alt, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);

	// Navigation keys by polling.
	for (size_t iKey = 0; iKey < sizeof(overlay_polled_keys) / sizeof(overlay_polled_keys[0]); iKey++) {
		OVERLAY_POLLED_KEY &polledKey = overlay_polled_keys[iKey];
		bool isDown = (GetAsyncKeyState(polledKey.virtualKey) & 0x8000) != 0;
		if (isDown != polledKey.wasDown) {
			polledKey.wasDown = isDown;
			imguiIo.AddKeyEvent(polledKey.imguiKey, isDown);
		}
	}
}
