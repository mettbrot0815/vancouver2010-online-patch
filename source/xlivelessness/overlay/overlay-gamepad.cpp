#include <winsock2.h>
#include <Windows.h>
#include "overlay-gamepad.hpp"
#include "../xlln/debug-log.hpp"
#include <imgui.h>
#include <atomic>

// Declared here, not included: nothing links against XInput, and the Guide button comes from an
// entry point that no header declares.
#define XLLN_XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XLLN_XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XLLN_XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XLLN_XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XLLN_XINPUT_GAMEPAD_START 0x0010
#define XLLN_XINPUT_GAMEPAD_BACK 0x0020
#define XLLN_XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XLLN_XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XLLN_XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XLLN_XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XLLN_XINPUT_GAMEPAD_GUIDE 0x0400
#define XLLN_XINPUT_GAMEPAD_A 0x1000
#define XLLN_XINPUT_GAMEPAD_B 0x2000
#define XLLN_XINPUT_GAMEPAD_X 0x4000
#define XLLN_XINPUT_GAMEPAD_Y 0x8000

#define XLLN_XINPUT_LEFT_THUMB_DEADZONE 7849
#define XLLN_XINPUT_RIGHT_THUMB_DEADZONE 8689
#define XLLN_XINPUT_TRIGGER_THRESHOLD 30

#define XLLN_XINPUT_USER_COUNT 4
// Probing an empty slot is XInput's expensive call; four of them every frame cost the Title frames.
#define XLLN_XINPUT_PROBE_INTERVAL_MS 2000

typedef struct _XLLN_XINPUT_GAMEPAD {
	uint16_t wButtons;
	uint8_t bLeftTrigger;
	uint8_t bRightTrigger;
	int16_t sThumbLX;
	int16_t sThumbLY;
	int16_t sThumbRX;
	int16_t sThumbRY;
} XLLN_XINPUT_GAMEPAD;

typedef struct _XLLN_XINPUT_STATE {
	uint32_t dwPacketNumber;
	XLLN_XINPUT_GAMEPAD Gamepad;
} XLLN_XINPUT_STATE;

typedef uint32_t (WINAPI *XLLN_XINPUT_GET_STATE)(uint32_t user_index, XLLN_XINPUT_STATE* state);

static bool overlay_gamepad_load_attempted = false;
static HMODULE xinput_module = 0;
static XLLN_XINPUT_GET_STATE overlay_gamepad_get_state = 0;
// Ordinal 100 answers with the guide button set; the exported XInputGetState always clears it.
static bool overlay_gamepad_reports_guide_button = false;

// The pad the overlay listens to, and what it last reported. Only the frame thread touches these.
static int overlay_gamepad_user_index = -1;
static uint64_t overlay_gamepad_next_probe_tick = 0;
static XLLN_XINPUT_GAMEPAD overlay_gamepad_pad = {0};
static bool overlay_gamepad_chord_down = false;
// What the interface was given this frame, for the buttons that are read from the pad itself.
static XLLN_XINPUT_GAMEPAD overlay_gamepad_pad_given = {0};

static std::atomic<bool> overlay_gamepad_connected(false);
// Written by the frame thread, taken by the hotkey thread.
static std::atomic<uint32_t> overlay_gamepad_guide_requests(0);

// Loaded on the first frame, not in DllMain: the dll name depends on the Windows version, and
// LoadLibrary is not allowed under the loader lock. Never freed, for the same rule.
static bool OverlayGamepadEnsureXInput()
{
	if (overlay_gamepad_load_attempted) {
		return overlay_gamepad_get_state != 0;
	}
	overlay_gamepad_load_attempted = true;

	// Newest first. Only the first two export ordinal 100, which reports the Guide button.
	static const wchar_t* xinputModuleNames[] = {
		L"xinput1_4.dll"
		, L"xinput1_3.dll"
		, L"xinput9_1_0.dll"
	};
	// Since xinput9_1_0.dll does not have support for the Guide button, only load that if none of the others exist.
	bool checkedLoaded = true;
	for (size_t iModule = 0; iModule < sizeof(xinputModuleNames) / sizeof(xinputModuleNames[0]); iModule++) {
		if (checkedLoaded) {
			if (iModule == 2 && checkedLoaded) {
				iModule = -1;
				checkedLoaded = false;
				continue;
			}
			xinput_module = GetModuleHandleW(xinputModuleNames[iModule]);
			if (!xinput_module) {
				continue;
			}
		}
		else {
			xinput_module = LoadLibraryW(xinputModuleNames[iModule]);
			if (!xinput_module) {
				continue;
			}
		}
		
		overlay_gamepad_get_state = (XLLN_XINPUT_GET_STATE)GetProcAddress(xinput_module, (LPCSTR)100);
		overlay_gamepad_reports_guide_button = overlay_gamepad_get_state != 0;
		if (!overlay_gamepad_get_state) {
			overlay_gamepad_get_state = (XLLN_XINPUT_GET_STATE)GetProcAddress(xinput_module, "XInputGetState");
		}
		if (!overlay_gamepad_get_state) {
			FreeLibrary(xinput_module);
			xinput_module = 0;
			continue;
		}
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
			, "%s uses %ls for gamepad input. The guide button is %s."
			, __func__
			, xinputModuleNames[iModule]
			, overlay_gamepad_reports_guide_button ? "reported" : "not reported, so only View and Menu together ask for the guide"
		);
		return true;
	}

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
		, "%s could load no XInput dll, so the overlay takes no gamepad input."
		, __func__
	);
	return false;
}

void InitOverlayGamepad()
{
	if (xinput_module) {
		FreeLibrary(xinput_module);
		xinput_module = 0;
	}
	overlay_gamepad_get_state = 0;
	overlay_gamepad_reports_guide_button = false;
	overlay_gamepad_load_attempted = false;
	overlay_gamepad_user_index = -1;
	overlay_gamepad_next_probe_tick = 0;
	memset(&overlay_gamepad_pad, 0, sizeof(overlay_gamepad_pad));
	memset(&overlay_gamepad_pad_given, 0, sizeof(overlay_gamepad_pad_given));
	overlay_gamepad_chord_down = false;
	overlay_gamepad_connected.store(false);
	overlay_gamepad_guide_requests.store(0);
}

void UninitOverlayGamepad()
{
	InitOverlayGamepad();
}

bool OverlayGamepadIsConnected()
{
	return overlay_gamepad_connected.load();
}

bool OverlayGamepadHasGuideButton()
{
	return overlay_gamepad_reports_guide_button;
}

bool OverlayGamepadTakeGuideRequest()
{
	return overlay_gamepad_guide_requests.exchange(0) != 0;
}

bool OverlayGamepadFaceLeftDown()
{
	return (overlay_gamepad_pad_given.wButtons & XLLN_XINPUT_GAMEPAD_X) != 0;
}

// Reads the chosen pad. A pad that stops answering restarts the search, so a swapped pad is found.
static bool OverlayGamepadReadPad()
{
	XLLN_XINPUT_STATE xinputState;
	if (overlay_gamepad_user_index >= 0) {
		if (overlay_gamepad_get_state((uint32_t)overlay_gamepad_user_index, &xinputState) == ERROR_SUCCESS) {
			overlay_gamepad_pad = xinputState.Gamepad;
			return true;
		}
		overlay_gamepad_user_index = -1;
		overlay_gamepad_next_probe_tick = GetTickCount64() + XLLN_XINPUT_PROBE_INTERVAL_MS;
	}

	if (GetTickCount64() < overlay_gamepad_next_probe_tick) {
		return false;
	}
	for (int iUser = 0; iUser < XLLN_XINPUT_USER_COUNT; iUser++) {
		if (overlay_gamepad_get_state((uint32_t)iUser, &xinputState) == ERROR_SUCCESS) {
			overlay_gamepad_user_index = iUser;
			overlay_gamepad_pad = xinputState.Gamepad;
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
				, "%s takes gamepad input from pad %d."
				, __func__
				, iUser
			);
			return true;
		}
	}
	overlay_gamepad_next_probe_tick = GetTickCount64() + XLLN_XINPUT_PROBE_INTERVAL_MS;
	return false;
}

void OverlayGamepadPoll(HWND title_hwnd)
{
	if (!OverlayGamepadEnsureXInput()) {
		return;
	}
	
	// XInput answers every process; without this the guide opens behind a Title that lost focus.
	HWND foregroundWindow = GetForegroundWindow();
	bool windowFocused = title_hwnd && (foregroundWindow == title_hwnd || IsChild(title_hwnd, foregroundWindow));
	if (!windowFocused) {
		overlay_gamepad_chord_down = false;
		return;
	}
	
	bool connected = OverlayGamepadReadPad();
	if (!connected) {
		memset(&overlay_gamepad_pad, 0, sizeof(overlay_gamepad_pad));
	}
	overlay_gamepad_connected.store(connected);
	
	bool chordDown = false;
	if (connected) {
		// View+Menu works everywhere: Windows also gives the Guide button to the Game Bar, so a press that
		// only XLLN answers is needed.
		const uint16_t viewAndMenu = XLLN_XINPUT_GAMEPAD_BACK | XLLN_XINPUT_GAMEPAD_START;
		chordDown = (overlay_gamepad_pad.wButtons & viewAndMenu) == viewAndMenu;
		if (!chordDown && overlay_gamepad_reports_guide_button) {
			chordDown = (overlay_gamepad_pad.wButtons & XLLN_XINPUT_GAMEPAD_GUIDE) != 0;
		}
	}
	// On release, matching the guide key. A press that is still held must not ask again every frame.
	if (overlay_gamepad_chord_down && !chordDown) {
		overlay_gamepad_guide_requests.fetch_add(1);
	}
	overlay_gamepad_chord_down = chordDown;
}

void OverlayGamepadNewFrame(bool guide_open)
{
	ImGuiIO &imguiIo = ImGui::GetIO();
	if (overlay_gamepad_connected.load()) {
		imguiIo.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	}
	else {
		imguiIo.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
	}

	// A closed guide is fed a pad at rest, or a button held as it closes stays down in the interface.
	XLLN_XINPUT_GAMEPAD pad = {0};
	if (guide_open) {
		pad = overlay_gamepad_pad;
	}
	overlay_gamepad_pad_given = pad;

	#define OVERLAY_GAMEPAD_MAP_BUTTON(imgui_key, button_mask) \
		imguiIo.AddKeyEvent(imgui_key, (pad.wButtons & (button_mask)) != 0)
	// Reports how far a stick or trigger is pushed past its dead zone, not where it sits.
	#define OVERLAY_GAMEPAD_MAP_ANALOG(imgui_key, reading, at_rest, at_full) { \
		float travel = (float)((reading) - (at_rest)) / (float)((at_full) - (at_rest)); \
		if (travel < 0.f) { travel = 0.f; } \
		else if (travel > 1.f) { travel = 1.f; } \
		imguiIo.AddKeyAnalogEvent(imgui_key, travel > 0.10f, travel); \
	}

	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadStart, XLLN_XINPUT_GAMEPAD_START);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadBack, XLLN_XINPUT_GAMEPAD_BACK);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadFaceRight, XLLN_XINPUT_GAMEPAD_B);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadFaceUp, XLLN_XINPUT_GAMEPAD_Y);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadFaceDown, XLLN_XINPUT_GAMEPAD_A);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadDpadLeft, XLLN_XINPUT_GAMEPAD_DPAD_LEFT);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadDpadRight, XLLN_XINPUT_GAMEPAD_DPAD_RIGHT);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadDpadUp, XLLN_XINPUT_GAMEPAD_DPAD_UP);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadDpadDown, XLLN_XINPUT_GAMEPAD_DPAD_DOWN);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadL1, XLLN_XINPUT_GAMEPAD_LEFT_SHOULDER);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadR1, XLLN_XINPUT_GAMEPAD_RIGHT_SHOULDER);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadL3, XLLN_XINPUT_GAMEPAD_LEFT_THUMB);
	OVERLAY_GAMEPAD_MAP_BUTTON(ImGuiKey_GamepadR3, XLLN_XINPUT_GAMEPAD_RIGHT_THUMB);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadL2, pad.bLeftTrigger, XLLN_XINPUT_TRIGGER_THRESHOLD, 255);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadR2, pad.bRightTrigger, XLLN_XINPUT_TRIGGER_THRESHOLD, 255);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadLStickLeft, pad.sThumbLX, -XLLN_XINPUT_LEFT_THUMB_DEADZONE, -32768);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadLStickRight, pad.sThumbLX, +XLLN_XINPUT_LEFT_THUMB_DEADZONE, +32767);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadLStickUp, pad.sThumbLY, +XLLN_XINPUT_LEFT_THUMB_DEADZONE, +32767);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadLStickDown, pad.sThumbLY, -XLLN_XINPUT_LEFT_THUMB_DEADZONE, -32768);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadRStickLeft, pad.sThumbRX, -XLLN_XINPUT_RIGHT_THUMB_DEADZONE, -32768);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadRStickRight, pad.sThumbRX, +XLLN_XINPUT_RIGHT_THUMB_DEADZONE, +32767);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadRStickUp, pad.sThumbRY, +XLLN_XINPUT_RIGHT_THUMB_DEADZONE, +32767);
	OVERLAY_GAMEPAD_MAP_ANALOG(ImGuiKey_GamepadRStickDown, pad.sThumbRY, -XLLN_XINPUT_RIGHT_THUMB_DEADZONE, -32768);

	#undef OVERLAY_GAMEPAD_MAP_BUTTON
	#undef OVERLAY_GAMEPAD_MAP_ANALOG
}
