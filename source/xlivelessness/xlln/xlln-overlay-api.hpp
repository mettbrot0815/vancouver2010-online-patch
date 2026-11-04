#pragma once
// XLLN Overlay UI-Provider API: the public ABI of xlive.dll.
//
// An XLLN-Module registers one UI provider and then draws the in-game overlay UI itself.
// XLLN keeps the device, the input capture, the XN_SYS_UI pause signal and the event stream.
// XLLN draws no overlay UI of its own while a provider is registered.
//
// Modules copy this header and resolve the exports from xlive.dll by ordinal:
//   XLLNOverlayProviderRegister   @41149
//   XLLNOverlayProviderUnregister @41150
//   XLLNOverlayQueueToast         @41151
//   XLLNOverlayInviteRespond      @41152
//   XLLNOverlayGuideSetOpen       @41153
//
// Set cbSize and apiVersion in every struct you pass in. XLLN rejects a registration when
// apiVersion is not XLLN_OVERLAY_API_VERSION, or when cbSize is smaller than that version's
// struct. Fields are only ever appended. A field appended to a struct XLLN fills in and hands
// out, such as XLLN_OVERLAY_EVENT_INFO, is covered by that struct's own cbSize and needs no
// apiVersion change; read such a field only when cbSize covers it. A change to the callbacks
// XLLN calls does bump apiVersion, because an older module cannot supply them.
#include <winsock2.h>
#include <Windows.h>
#include <stdint.h>

#define XLLN_OVERLAY_API_VERSION 1

// Values of XLLN_OVERLAY_EVENT_INFO::eventType.
#define XLLN_OVERLAY_EVENT_GENERIC 0
#define XLLN_OVERLAY_EVENT_HINT 1
#define XLLN_OVERLAY_EVENT_PLAYER_ONLINE 2
#define XLLN_OVERLAY_EVENT_PLAYER_OFFLINE 3
#define XLLN_OVERLAY_EVENT_SIGNED_IN 4
#define XLLN_OVERLAY_EVENT_SIGNED_OUT 5
#define XLLN_OVERLAY_EVENT_INVITE_RECEIVED 6
#define XLLN_OVERLAY_EVENT_INVITE_DECLINED 7
#define XLLN_OVERLAY_EVENT_JOINING_SESSION 8
#define XLLN_OVERLAY_EVENT_ACHIEVEMENT_UNLOCKED 9

// Values of the device_api parameter of OnDeviceCreated.
#define XLLN_OVERLAY_DEVICE_API_D3D9 9
#define XLLN_OVERLAY_DEVICE_API_D3D9EX 90
#define XLLN_OVERLAY_DEVICE_API_D3D10 10
#define XLLN_OVERLAY_DEVICE_API_D3D11 11

typedef struct _XLLN_OVERLAY_EVENT_INFO {
	uint32_t cbSize;
	uint32_t eventType;
	uint64_t xuid;
	uint32_t inviteToken;
	uint32_t remoteInstanceId;
	uint32_t localUserIndex;
	// UTF-8. Valid only during the OnEvent call. Copy the text if you keep it.
	const char* title;
	const char* detail;
	// SPA image id of the achievement art, for ACHIEVEMENT_UNLOCKED events. An appended field:
	// read it only when cbSize covers it.
	uint32_t imageId;
	// The Title an invite is for (INVITE_RECEIVED). Only the running Title can accept one, so a
	// module that offers an accept should compare this first. An appended field: read it only when
	// cbSize covers it.
	uint32_t titleId;
} XLLN_OVERLAY_EVENT_INFO;

typedef struct _XLLN_OVERLAY_UI_PROVIDER {
	uint32_t cbSize;
	// Must be XLLN_OVERLAY_API_VERSION.
	uint32_t apiVersion;

	// Every callback is optional. Leave a callback null when you do not use it.
	// Each callback runs on the title's render thread, with two exceptions: OnWindowMessage runs
	// on the thread that pumps the title's messages, and OnGuideRequested runs on XLLN's hotkey
	// thread.

	// Hands over the title's D3D device. device_api is one of XLLN_OVERLAY_DEVICE_API_*.
	// presentation_parameters points to D3DPRESENT_PARAMETERS (D3D9 and D3D9Ex) or to
	// DXGI_SWAP_CHAIN_DESC (D3D10 and D3D11), and XLLN keeps ownership of it.
	// Registration calls this at once when the title already made its device.
	void (WINAPI *OnDeviceCreated)(IUnknown* d3d_device, uint32_t device_api, void* presentation_parameters);
	void (WINAPI *OnDeviceReset)(void* presentation_parameters);
	void (WINAPI *OnDeviceDestroyed)();

	// Runs once per title frame. Draw the provider UI here.
	void (WINAPI *OnFrame)();

	// Hands over each window message while the provider UI is open, as set by
	// XLLNOverlayGuideSetOpen. Return TRUE to keep the message from the title, FALSE to let it
	// through. When this callback is null, XLLN keeps every keyboard and mouse message itself.
	BOOL (WINAPI *OnWindowMessage)(HWND window_handle, UINT message, WPARAM wparam, LPARAM lparam);

	// Delivers presence, invite and sign-in events, in the order they happened.
	void (WINAPI *OnEvent)(const XLLN_OVERLAY_EVENT_INFO* event_info);

	// Reports a press of the guide hotkey, after any title-specific module handler and before
	// XLLN's built-in overlay. Return TRUE to claim the press. Return quickly, and take no lock
	// that the render thread also takes.
	BOOL (WINAPI *OnGuideRequested)();
} XLLN_OVERLAY_UI_PROVIDER;

extern "C" {

// Returns ERROR_SUCCESS, ERROR_INVALID_PARAMETER for a bad cbSize or apiVersion, or
// ERROR_ALREADY_REGISTERED when a provider is already registered.
uint32_t WINAPI XLLNOverlayProviderRegister(const XLLN_OVERLAY_UI_PROVIDER* ui_provider);
// Returns ERROR_SUCCESS, or ERROR_NOT_FOUND when no provider is registered.
uint32_t WINAPI XLLNOverlayProviderUnregister();

// Queues one event for display. The built-in UI draws it, or a registered provider receives it
// through OnEvent. Any module may call this from any thread, registered or not. Returns
// ERROR_INVALID_PARAMETER when event_type is not one of XLLN_OVERLAY_EVENT_*.
uint32_t WINAPI XLLNOverlayQueueToast(uint32_t event_type, const char* title, const char* detail);

// Answers an INVITE_RECEIVED event. The parameters come from XLLN_OVERLAY_EVENT_INFO. On
// accept the title receives XN_LIVE_INVITE_ACCEPTED and joins through XInviteGetAcceptedInfo.
// Call from any thread. Returns ERROR_INVALID_PARAMETER when invite_token is 0, and
// ERROR_NOT_FOUND when the token is unknown or expired.
uint32_t WINAPI XLLNOverlayInviteRespond(uint32_t invite_token, uint32_t remote_instance_id, uint32_t local_user_index, BOOL accepted);

// A provider calls this when it opens or closes its UI. While open, XLLN raises
// XN_SYS_UI(TRUE), which pauses a title that acts on it, and sends the title's input to
// OnWindowMessage. Call from any thread. Returns ERROR_NOT_FOUND when no provider is
// registered.
uint32_t WINAPI XLLNOverlayGuideSetOpen(BOOL is_open);

}
