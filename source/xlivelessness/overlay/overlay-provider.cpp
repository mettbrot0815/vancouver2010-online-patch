// The UI-provider side of the overlay: lets an XLLN-Module replace the built-in
// overlay UI while core keeps owning device capture, input capture, pause
// signalling and the event stream. Public ABI in xlln/xlln-overlay-api.hpp.
#include <winsock2.h>
#include <Windows.h>
#include "overlay.hpp"
#include "overlay-events.hpp"
#include "overlay-input.hpp"
#include "../xlln/xlln-overlay-api.hpp"
#include "../xlive/xrender.hpp"
#include "../xlln/debug-log.hpp"
#include <atomic>
#include <string.h>
#include <d3d9.h>
#include <dxgi.h>
#include "../xlln/xlln-friends.hpp"

// Guarded by xlln_critsec_overlay.
static XLLN_OVERLAY_UI_PROVIDER overlay_provider = {0};
// Lock-free mirrors for the hotkey/render threads.
static std::atomic<bool> overlay_provider_registered(false);
static std::atomic<bool> overlay_provider_ui_open(false);

// The current device as last reported through the GFWL contract, kept so a
// provider registering after device creation still receives OnDeviceCreated.
// The presentation parameters are copied rather than pointed at: the caller owns its buffer only
// for the length of the call, and the replay below can happen at any time afterwards.
static IUnknown* overlay_provider_current_device = 0;
static uint32_t overlay_provider_current_device_api = 0;
static union _OVERLAY_PROVIDER_PRESENTATION_PARAMETERS {
	D3DPRESENT_PARAMETERS d3d9;
	DXGI_SWAP_CHAIN_DESC dxgi;
} overlay_provider_current_device_pp = {0};
static bool overlay_provider_current_device_pp_valid = false;
static HWND overlay_provider_current_hwnd = 0;

// Takes a copy of whichever presentation-parameter struct device_api names.
static void OverlayProviderStorePresentationParameters(uint32_t device_api, const void* presentation_parameters)
{
	if (!presentation_parameters) {
		overlay_provider_current_device_pp_valid = false;
		return;
	}
	if (device_api == XLLN_OVERLAY_DEVICE_API_D3D9 || device_api == XLLN_OVERLAY_DEVICE_API_D3D9EX) {
		overlay_provider_current_device_pp.d3d9 = *(const D3DPRESENT_PARAMETERS*)presentation_parameters;
	}
	else {
		overlay_provider_current_device_pp.dxgi = *(const DXGI_SWAP_CHAIN_DESC*)presentation_parameters;
	}
	overlay_provider_current_device_pp_valid = true;
}

bool OverlayProviderIsActive()
{
	return overlay_provider_registered.load();
}

bool OverlayProviderIsUIOpen()
{
	return overlay_provider_registered.load() && overlay_provider_ui_open.load();
}

bool OverlayProviderGuideRequested()
{
	// Hotkey thread: the provider contract requires this callback to be quick
	// and lock free, mirroring OverlayGuideToggleRequest.
	if (!overlay_provider_registered.load()) {
		return false;
	}
	BOOL (WINAPI *onGuideRequested)() = overlay_provider.OnGuideRequested;
	if (!onGuideRequested) {
		return false;
	}
	return onGuideRequested() ? true : false;
}

void OverlayProviderOnFrame()
{
	if (!overlay_provider_registered.load()) {
		return;
	}
	void (WINAPI *onFrame)() = overlay_provider.OnFrame;
	if (onFrame) {
		onFrame();
	}
}

bool OverlayProviderHandleWindowMessage(HWND window_handle, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (!OverlayProviderIsUIOpen()) {
		return false;
	}
	BOOL (WINAPI *onWindowMessage)(HWND, UINT, WPARAM, LPARAM) = overlay_provider.OnWindowMessage;
	bool keepMessage;
	if (!onWindowMessage) {
		// The provider set no handler, so keep the keyboard and mouse messages anyway. The title
		// is paused and must not act on input meant for the provider UI.
		keepMessage = (message >= WM_KEYFIRST && message <= WM_KEYLAST)
			|| (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
			|| message == WM_INPUT;
	}
	else {
		keepMessage = onWindowMessage(window_handle, message, wparam, lparam) ? true : false;
	}
	// Raw input is only released once it reaches DefWindowProc, and a message kept here never
	// reaches the Title that would make that call. Make it whoever decided to keep the message.
	if (keepMessage && message == WM_INPUT) {
		DefWindowProcW(window_handle, message, wparam, lparam);
	}
	return keepMessage;
}

void OverlayProviderDeliverEvent(const OVERLAY_EVENT &overlay_event)
{
	if (!overlay_provider_registered.load()) {
		return;
	}
	void (WINAPI *onEvent)(const XLLN_OVERLAY_EVENT_INFO*) = overlay_provider.OnEvent;
	if (!onEvent) {
		return;
	}
	XLLN_OVERLAY_EVENT_INFO eventInfo;
	eventInfo.cbSize = sizeof(eventInfo);
	eventInfo.eventType = (uint32_t)overlay_event.type;
	eventInfo.xuid = overlay_event.xuid;
	eventInfo.inviteToken = overlay_event.inviteToken;
	eventInfo.remoteInstanceId = overlay_event.remoteInstanceId;
	eventInfo.localUserIndex = overlay_event.localUserIndex;
	eventInfo.title = overlay_event.title;
	eventInfo.detail = overlay_event.detail;
	eventInfo.imageId = overlay_event.imageId;
	eventInfo.titleId = overlay_event.titleId;
	onEvent(&eventInfo);
}

// Each of the three copies the callback out before it tests it. XLLNOverlayProviderUnregister clears
// the struct from the thread the module calls it on, so a test of the member followed by a call
// reads it twice, and a module that unregisters between the two reads sends this thread to zero.

void OverlayProviderDeviceCreated(IUnknown* d3d_device, uint32_t device_api, void* presentation_parameters, HWND title_hwnd)
{
	overlay_provider_current_device = d3d_device;
	overlay_provider_current_device_api = device_api;
	OverlayProviderStorePresentationParameters(device_api, presentation_parameters);
	overlay_provider_current_hwnd = title_hwnd;
	void (WINAPI *onDeviceCreated)(IUnknown*, uint32_t, void*) = overlay_provider.OnDeviceCreated;
	if (overlay_provider_registered.load() && onDeviceCreated) {
		onDeviceCreated(d3d_device, device_api, presentation_parameters);
	}
}

void OverlayProviderDeviceReset(void* presentation_parameters)
{
	OverlayProviderStorePresentationParameters(overlay_provider_current_device_api, presentation_parameters);
	void (WINAPI *onDeviceReset)(void*) = overlay_provider.OnDeviceReset;
	if (overlay_provider_registered.load() && onDeviceReset) {
		onDeviceReset(presentation_parameters);
	}
}

void OverlayProviderDeviceDestroyed()
{
	overlay_provider_current_device = 0;
	overlay_provider_current_device_api = 0;
	overlay_provider_current_device_pp_valid = false;
	overlay_provider_current_hwnd = 0;
	void (WINAPI *onDeviceDestroyed)() = overlay_provider.OnDeviceDestroyed;
	if (overlay_provider_registered.load() && onDeviceDestroyed) {
		onDeviceDestroyed();
	}
}

// --- exported ABI ------------------------------------------------------------

// #41149
uint32_t WINAPI XLLNOverlayProviderRegister(const XLLN_OVERLAY_UI_PROVIDER* ui_provider)
{
	TRACE_FX();
	if (!ui_provider
		|| ui_provider->cbSize < sizeof(XLLN_OVERLAY_UI_PROVIDER)
		|| ui_provider->apiVersion != XLLN_OVERLAY_API_VERSION
	) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s rejected: bad pointer, cbSize or apiVersion (expected version %u)."
			, __func__
			, XLLN_OVERLAY_API_VERSION
		);
		return ERROR_INVALID_PARAMETER;
	}

	EnterCriticalSection(&xlln_critsec_overlay);
	if (overlay_provider_registered.load()) {
		LeaveCriticalSection(&xlln_critsec_overlay);
		return ERROR_ALREADY_REGISTERED;
	}
	memcpy(&overlay_provider, ui_provider, sizeof(XLLN_OVERLAY_UI_PROVIDER));
	overlay_provider_registered.store(true);
	void (WINAPI *onDeviceCreated)(IUnknown*, uint32_t, void*) = overlay_provider.OnDeviceCreated;
	IUnknown* deviceAlreadyCreated = overlay_provider_current_device;
	const uint32_t deviceApiAlreadyCreated = overlay_provider_current_device_api;
	void* devicePpAlreadyCreated = overlay_provider_current_device_pp_valid ? (void*)&overlay_provider_current_device_pp : 0;
	LeaveCriticalSection(&xlln_critsec_overlay);

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s overlay UI provider registered; the built-in overlay UI stands down."
		, __func__
	);

	// Late registration: replay the device the title already handed over.
	if (deviceAlreadyCreated && onDeviceCreated) {
		onDeviceCreated(deviceAlreadyCreated, deviceApiAlreadyCreated, devicePpAlreadyCreated);
	}

	return ERROR_SUCCESS;
}

// #41150
uint32_t WINAPI XLLNOverlayProviderUnregister()
{
	TRACE_FX();
	EnterCriticalSection(&xlln_critsec_overlay);
	if (!overlay_provider_registered.load()) {
		LeaveCriticalSection(&xlln_critsec_overlay);
		return ERROR_NOT_FOUND;
	}
	if (overlay_provider_ui_open.load()) {
		XLLNOverlayGuideSetOpen(FALSE);
	}
	overlay_provider_registered.store(false);
	memset(&overlay_provider, 0, sizeof(overlay_provider));
	LeaveCriticalSection(&xlln_critsec_overlay);
	return ERROR_SUCCESS;
}

// #41151
uint32_t WINAPI XLLNOverlayQueueToast(uint32_t event_type, const char* title, const char* detail)
{
	TRACE_FX();
	if (event_type > XLLN_OVERLAY_EVENT_ACHIEVEMENT_UNLOCKED) {
		return ERROR_INVALID_PARAMETER;
	}
	OverlayEventQueuePush((OverlayEventType)event_type, title, detail);
	return ERROR_SUCCESS;
}

// #41152
uint32_t WINAPI XLLNOverlayInviteRespond(uint32_t invite_token, uint32_t remote_instance_id, uint32_t local_user_index, BOOL accepted)
{
	TRACE_FX();
	if (!invite_token) {
		return ERROR_INVALID_PARAMETER;
	}
	// remote_instance_id / local_user_index remain for ABI compatibility; the
	// invite token alone identifies the pending invite.
	bool acceptRefused = false;
	if (!XllnFriendsInviteResolveByToken(invite_token, accepted ? true : false, &acceptRefused)) {
		return ERROR_NOT_FOUND;
	}
	// The invite was answered, but as a decline: it is for another Title, so no join was armed and
	// no XN_LIVE_INVITE_ACCEPTED was raised. Saying SUCCESS here would promise a join that is not
	// coming.
	if (acceptRefused) {
		return ERROR_INVALID_STATE;
	}
	return ERROR_SUCCESS;
}

// #41153
uint32_t WINAPI XLLNOverlayGuideSetOpen(BOOL is_open)
{
	TRACE_FX();
	if (!overlay_provider_registered.load()) {
		return ERROR_NOT_FOUND;
	}
	bool wasOpen = overlay_provider_ui_open.exchange(is_open ? true : false);
	if (wasOpen == (is_open ? true : false)) {
		return ERROR_SUCCESS;
	}
	if (is_open) {
		OverlayInputOnGuideOpened(overlay_provider_current_hwnd);
	}
	else {
		OverlayInputOnGuideClosed();
	}
	// Recompute XN_SYS_UI: OverlayIsGuideOpen folds the provider state in.
	XllnUpdateXliveSystemUIOpenState();
	return ERROR_SUCCESS;
}
