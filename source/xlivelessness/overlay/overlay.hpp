#pragma once
#include <winsock2.h>
#include <Windows.h>
#include <stdint.h>

// Everything here runs on the thread the title calls XLiveRender from, unless a declaration says
// otherwise.

// Config (persisted through xlln-config).
extern bool xlln_overlay_enabled;
// The interface size the player chose, as a multiplier on the automatic scale that the backbuffer
// height gives. 1.0 draws the panels at the size the design was laid out at, which leaves the body
// text under the size that stays readable while a game runs behind it, so the default is above it.
// 0 or less is read as the default, which is what an older config file holds.
#define OVERLAY_SCALE_COMPACT 1.0f
#define OVERLAY_SCALE_DEFAULT 1.2f
#define OVERLAY_SCALE_LARGE 1.5f
#define OVERLAY_SCALE_LARGER 2.0f
extern float xlln_overlay_scale;
// 0xFF = automatic (honour the title's XNotifyPositionUI request); otherwise an
// XNOTIFYUI_POS_* override from config.
extern uint8_t xlln_overlay_toast_position;

extern CRITICAL_SECTION xlln_critsec_overlay;
// Serialises the frame against the device callbacks. The frame path takes xlive_critsec_hotkeys
// inside it, so a caller that holds that section must not take this one.
extern CRITICAL_SECTION xlln_critsec_overlay_frame;

void InitOverlay();
void UninitOverlay();

// Device lifecycle, driven from xrender.cpp. device_api is one of XLLN_OVERLAY_DEVICE_API_*, and
// presentation_parameters is a D3DPRESENT_PARAMETERS* for Direct3D 9 and 9Ex, and a
// DXGI_SWAP_CHAIN_DESC* for Direct3D 10 and 11.
void OverlayOnDeviceCreated(uint32_t device_api, IUnknown* d3d_device, const void* presentation_parameters);
void OverlayOnDeviceReset(const void* presentation_parameters);
void OverlayOnDeviceDestroyed();
// Called once per title frame, from XLiveRender (#5002).
void OverlayOnXLiveRender();

// True when a supported device is attached and the overlay is enabled.
bool OverlayIsAvailable();
// True while the guide is open. This decides the XN_SYS_UI pause and whether XLLN keeps the
// title's input.
bool OverlayIsGuideOpen();

// Called from the hotkey thread (holds xlive_critsec_hotkeys — must stay lock free).
// Returns true when the overlay claims the guide hotkey press.
bool OverlayGuideToggleRequest();
// Queue a close from any thread.
void OverlayGuideRequestClose();
// Queue "open the guide on the Home tab" from any thread. False when the built-in
// overlay cannot show (disabled, no device, or a UI provider is active).
bool OverlayGuideRequestOpenHome();
// Queue "open the guide on the Achievements tab" from any thread. False when the
// built-in overlay cannot show (disabled, no device, or a UI provider is active).
bool OverlayGuideRequestOpenAchievements();
// Queue "open the guide on the Friends tab" from any thread. False when the built-in
// overlay cannot show.
bool OverlayGuideRequestOpenFriends();
// Queue "open the guide on the Invites tab" - where a title's game-invite request lands.
bool OverlayGuideRequestOpenInvites();
// Queue "open the guide to answer a text request from the Title". False when the built-in overlay
// cannot show, and the desktop window then answers instead.
bool OverlayGuideRequestOpenKeyboard();
// Queue a reread of the achievements and the gamerscore from any thread.
void OverlayAchievementsInvalidate();

// --- UI-provider internals (module-replaceable overlay UI) ---------------------
// Public ABI in xlln/xlln-overlay-api.hpp; implementation in overlay-provider.cpp.
struct _OVERLAY_EVENT;
bool OverlayProviderIsActive();
bool OverlayProviderIsUIOpen();
// Hotkey thread; lock free.
bool OverlayProviderGuideRequested();
void OverlayProviderOnFrame();
bool OverlayProviderHandleWindowMessage(HWND window_handle, UINT message, WPARAM wparam, LPARAM lparam);
void OverlayProviderDeliverEvent(const struct _OVERLAY_EVENT &overlay_event);
void OverlayProviderDeviceCreated(IUnknown* d3d_device, uint32_t device_api, void* presentation_parameters, HWND title_hwnd);
void OverlayProviderDeviceReset(void* presentation_parameters);
void OverlayProviderDeviceDestroyed();
