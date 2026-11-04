#include <winsock2.h>
#include <Windows.h>
#include "overlay.hpp"
#include "overlay-renderer.hpp"
#include "overlay-input.hpp"
#include "overlay-events.hpp"
#include "overlay-widgets.hpp"
#include "overlay-textures.hpp"
#include "overlay-keyboard.hpp"
#include "overlay-gamepad.hpp"
#include "overlay-theme.hpp"
#include "../xlive/xdefs.hpp"
#include "../xlive/xlive.hpp"
#include "../xlive/xrender.hpp"
#include "../xlive/xnotify.hpp"
#include "../xlln/xlln-network.hpp"
#include "../xlln/xlln-friends.hpp"
#include "../xlln/xlln.hpp"
#include "../xlln/xlln-config.hpp"
#include "../xlive/xuser.hpp"
#include "../xlln/debug-log.hpp"
#include "../xlln/xlln-overlay-api.hpp"
#include "../resource.h"
#include <d3d9.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <atomic>
#include <string>
#include <vector>
#include <stdio.h>
#include <float.h>

bool xlln_overlay_enabled = true;
float xlln_overlay_scale = OVERLAY_SCALE_DEFAULT;
uint8_t xlln_overlay_toast_position = 0xFF;

CRITICAL_SECTION xlln_critsec_overlay;

// The frame and the device callbacks touch the same ImGui context and the same GPU objects, and
// each can arrive on a different Title thread. The frame only tries to take this section, because
// a frame that waits stalls the thread the Title renders on; a device callback waits for a frame
// that is still drawing.
CRITICAL_SECTION xlln_critsec_overlay_frame;

typedef struct _OVERLAY_FRAME_LOCK {
	_OVERLAY_FRAME_LOCK()
	{
		EnterCriticalSection(&xlln_critsec_overlay_frame);
	}
	~_OVERLAY_FRAME_LOCK()
	{
		LeaveCriticalSection(&xlln_critsec_overlay_frame);
	}
} OVERLAY_FRAME_LOCK;

typedef struct _OVERLAY_TOAST {
	OVERLAY_EVENT event;
	double shownAt = 0.0;
	float duration = 4.5f;
} OVERLAY_TOAST;

enum class OverlayGuidePanel : uint32_t {
	HOME = 0,
	FRIENDS,
	PLAYERS,
	INVITES,
	ACHIEVEMENTS,
	// Reached from the band's gear rather than from a tab, so it takes no room in the tab row.
	SETTINGS,
};

// Cross-thread state (hotkey thread, message threads, render thread).
static std::atomic<bool> overlay_guide_open(false);
static std::atomic<bool> overlay_device_ready(false);
static std::atomic<uint32_t> overlay_toggle_requests(0);
static std::atomic<uint32_t> overlay_close_requests(0);
// Panel deep-link from other threads (XShowAchievementsUI): OverlayGuidePanel value + 1, 0 = none.
static std::atomic<uint32_t> overlay_panel_open_requests(0);
// A text request (XShowKeyboardUI) needs the guide up to draw its dialog on.
static std::atomic<uint32_t> overlay_keyboard_open_requests(0);
// Cleared when the renderer refuses every frame. An open guide takes the Title's input and pauses
// it through XN_SYS_UI, so it must not stay open when there is nothing on screen to answer.
static std::atomic<bool> overlay_render_target_usable(true);
static std::atomic<bool> overlay_achievements_dirty(true);

// Render-thread state.
static HWND overlay_title_hwnd = 0;
static uint32_t overlay_display_width = 0;
static uint32_t overlay_display_height = 0;
static bool overlay_imgui_context_created = false;
static float overlay_fonts_built_scale = 0.f;
// The theme the palette and the style were last filled from.
#define OVERLAY_THEME_NONE ((OverlayTheme)0xFFFFFFFF)
static OverlayTheme overlay_theme_applied = OVERLAY_THEME_NONE;
static double overlay_time_last_frame = 0.0;
static uint32_t overlay_frames_refused = 0;
// About a second at a normal frame rate, so a Title that binds an intermediate target for a few
// frames is not caught by this.
#define OVERLAY_FRAMES_REFUSED_MAX 60
static float overlay_open_animation = 0.f;
static OverlayGuidePanel overlay_active_panel = OverlayGuidePanel::HOME;
// The guide came up only to answer a text request, so it goes away again with that request.
static bool overlay_guide_opened_for_keyboard = false;
static bool overlay_guide_focus_panel_pending = false;
static int32_t overlay_gamerscore_total = 0;
static std::vector<OVERLAY_TOAST> overlay_toasts;
static std::vector<OVERLAY_EVENT> overlay_pending_invites;
static int overlay_invite_modal_index = -1;
static RECT overlay_saved_cursor_clip = {0};
static bool overlay_saved_cursor_clip_valid = false;
static RECT overlay_saved_cursor_clip_window = {0};
static bool overlay_legend_gamepad = false;
// The title's display name (window text), captured when the guide opens. Used by
// the NOW PLAYING card and the invite modal art.
static char overlay_title_text[96] = {0};

static double OverlayTimeNow()
{
	static LARGE_INTEGER frequency = {0};
	if (!frequency.QuadPart) {
		QueryPerformanceFrequency(&frequency);
	}
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (double)counter.QuadPart / (double)frequency.QuadPart;
}

void InitOverlay()
{
	InitOverlayInput();
	InitOverlayKeyboard();
	InitOverlayGamepad();
	overlay_toasts.reserve(8);
}

void UninitOverlay()
{
	OVERLAY_FRAME_LOCK frameLock;
	OverlayOnDeviceDestroyed();
	UninitOverlayInput();
	UninitOverlayKeyboard();
	UninitOverlayGamepad();
	OverlayEventQueueClear();
	overlay_toasts.clear();
	overlay_pending_invites.clear();
	overlay_invite_modal_index = -1;
	if (overlay_imgui_context_created) {
		ImGui::DestroyContext();
		overlay_imgui_context_created = false;
		overlay_fonts = OVERLAY_FONT_SET();
		overlay_fonts_built_scale = 0.f;
		overlay_theme_applied = OVERLAY_THEME_NONE;
	}
}

bool OverlayIsAvailable()
{
	return xlln_overlay_enabled && overlay_device_ready.load() && overlay_render_target_usable.load();
}

bool OverlayIsGuideOpen()
{
	return overlay_guide_open.load() || OverlayProviderIsUIOpen();
}

bool OverlayGuideToggleRequest()
{
	// Runs on the hotkey thread while xlive_critsec_hotkeys is held: atomics only.
	// A registered UI provider takes the built-in overlay's place in the chain.
	if (OverlayProviderIsActive()) {
		return OverlayProviderGuideRequested();
	}
	if (!xlln_overlay_enabled || !overlay_device_ready.load() || !overlay_render_target_usable.load()) {
		return false;
	}
	overlay_toggle_requests.fetch_add(1);
	return true;
}

void OverlayGuideRequestClose()
{
	if (overlay_guide_open.load()) {
		overlay_close_requests.fetch_add(1);
	}
}

// Queues the guide open on one panel. Refuses when a provider module owns the UI or no device
// is attached yet, which is what lets a caller fall back to a native window.
static bool OverlayGuideRequestOpenPanel_(OverlayGuidePanel panel)
{
	if (OverlayProviderIsActive() || !xlln_overlay_enabled || !overlay_device_ready.load() || !overlay_render_target_usable.load()) {
		return false;
	}
	overlay_panel_open_requests.store((uint32_t)panel + 1);
	return true;
}

bool OverlayGuideRequestOpenHome()
{
	return OverlayGuideRequestOpenPanel_(OverlayGuidePanel::HOME);
}

bool OverlayGuideRequestOpenAchievements()
{
	return OverlayGuideRequestOpenPanel_(OverlayGuidePanel::ACHIEVEMENTS);
}

bool OverlayGuideRequestOpenFriends()
{
	return OverlayGuideRequestOpenPanel_(OverlayGuidePanel::FRIENDS);
}

bool OverlayGuideRequestOpenInvites()
{
	return OverlayGuideRequestOpenPanel_(OverlayGuidePanel::INVITES);
}

bool OverlayGuideRequestOpenKeyboard()
{
	if (!OverlayIsAvailable() || OverlayProviderIsActive()) {
		return false;
	}
	overlay_keyboard_open_requests.fetch_add(1);
	return true;
}

void OverlayAchievementsInvalidate()
{
	overlay_achievements_dirty.store(true);
}

// --- device lifecycle -----------------------------------------------------------

static void OverlayCaptureTitleText();

// Each API describes the display it presents to in its own structure, and both can leave the size
// unset, which is what the window then answers for.
static void OverlayReadPresentationParameters(uint32_t device_api, const void* presentation_parameters, HWND* out_title_hwnd, uint32_t* out_width, uint32_t* out_height)
{
	*out_title_hwnd = 0;
	*out_width = 0;
	*out_height = 0;
	if (!presentation_parameters) {
		return;
	}

	if (device_api == XLLN_OVERLAY_DEVICE_API_D3D9 || device_api == XLLN_OVERLAY_DEVICE_API_D3D9EX) {
		const D3DPRESENT_PARAMETERS* parameters = (const D3DPRESENT_PARAMETERS*)presentation_parameters;
		*out_title_hwnd = parameters->hDeviceWindow;
		*out_width = parameters->BackBufferWidth;
		*out_height = parameters->BackBufferHeight;
	}
	else {
		const DXGI_SWAP_CHAIN_DESC* description = (const DXGI_SWAP_CHAIN_DESC*)presentation_parameters;
		*out_title_hwnd = description->OutputWindow;
		*out_width = description->BufferDesc.Width;
		*out_height = description->BufferDesc.Height;
	}

	if ((!*out_width || !*out_height) && *out_title_hwnd) {
		RECT clientRect;
		if (GetClientRect(*out_title_hwnd, &clientRect)) {
			*out_width = clientRect.right - clientRect.left;
			*out_height = clientRect.bottom - clientRect.top;
		}
	}
}

void OverlayOnDeviceCreated(uint32_t device_api, IUnknown* d3d_device, const void* presentation_parameters)
{
	OVERLAY_FRAME_LOCK frameLock;
	OverlayOnDeviceDestroyed();
	if (!d3d_device || !presentation_parameters) {
		return;
	}
	if (!OverlayRendererInit(device_api, d3d_device, presentation_parameters)) {
		return;
	}
	OverlayReadPresentationParameters(device_api, presentation_parameters, &overlay_title_hwnd, &overlay_display_width, &overlay_display_height);
	overlay_time_last_frame = 0.0;
	overlay_frames_refused = 0;
	overlay_render_target_usable.store(true);
	overlay_device_ready.store(true);

	// Toasts reference the title's display name before the guide first opens.
	OverlayCaptureTitleText();

	// Once per process: titles recreate the device on resolution or display changes, and the hint
	// should not return each time.
	static bool overlayHintShown = false;
	if (!overlayHintShown) {
		overlayHintShown = true;
		OverlayEventQueuePush(OverlayEventType::HINT, "Press HOME to open the XLLN Guide", "");
	}
}

void OverlayOnDeviceReset(const void* presentation_parameters)
{
	OVERLAY_FRAME_LOCK frameLock;
	const uint32_t deviceApi = OverlayRendererDeviceApi();
	if (!deviceApi) {
		return;
	}
	OverlayRendererOnDeviceReset(presentation_parameters);
	// The backend clears its target decision here, so the overlay has to give itself the same fresh
	// start. A reset is how a Title changes what it presents, which is exactly the change that can
	// make a target the overlay refused acceptable again.
	overlay_frames_refused = 0;
	overlay_render_target_usable.store(true);
	if (presentation_parameters) {
		uint32_t displayWidth = 0;
		uint32_t displayHeight = 0;
		OverlayReadPresentationParameters(deviceApi, presentation_parameters, &overlay_title_hwnd, &displayWidth, &displayHeight);
		if (displayWidth && displayHeight) {
			overlay_display_width = displayWidth;
			overlay_display_height = displayHeight;
		}
	}
}

static void OverlayGuideClose();

void OverlayOnDeviceDestroyed()
{
	OVERLAY_FRAME_LOCK frameLock;
	if (overlay_guide_open.load()) {
		OverlayGuideClose();
	}
	overlay_device_ready.store(false);
	OverlayRendererShutdown();
	overlay_title_hwnd = 0;
}

// --- guide open/close (render thread) --------------------------------------------

static void OverlayCaptureTitleText()
{
	overlay_title_text[0] = 0;
	// The SPA's own title name (string 0x8000) is authoritative when the exe embeds one.
	const char* spaTitleName = XllnSpaTitleNameUtf8();
	if (spaTitleName && *spaTitleName) {
		snprintf(overlay_title_text, sizeof(overlay_title_text), "%s", spaTitleName);
		return;
	}
	if (overlay_title_hwnd) {
		char windowText[96] = {0};
		// Not GetWindowTextA: for a Title whose window thread is not this thread it sends WM_GETTEXT and
		// waits for that thread. This runs while xlln_critsec_overlay_frame is held, and a device
		// callback on the window thread waits for that same section, so a plain send could deadlock the
		// two. This one gives up instead of waiting on a window thread that cannot answer.
		DWORD_PTR windowTextResult = 0;
		SendMessageTimeoutA(overlay_title_hwnd, WM_GETTEXT, (WPARAM)sizeof(windowText), (LPARAM)windowText, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &windowTextResult);
		// Keep the leading printable-ASCII run: window titles often append state
		// after a dash ("Game — PAUSED"), and ANSI high bytes are not valid UTF-8.
		uint32_t textLength = 0;
		for (; windowText[textLength]; textLength++) {
			uint8_t character = (uint8_t)windowText[textLength];
			if (character < 0x20 || character >= 0x7F) {
				break;
			}
			if (character == '-' && textLength && windowText[textLength - 1] == ' ') {
				textLength--;
				break;
			}
		}
		while (textLength && windowText[textLength - 1] == ' ') {
			textLength--;
		}
		memcpy(overlay_title_text, windowText, textLength);
		overlay_title_text[textLength] = 0;
	}
	if (!overlay_title_text[0]) {
		snprintf(overlay_title_text, sizeof(overlay_title_text), "Title %08X", xlive_title_id);
	}
}

static void OverlayGuideOpen()
{
	if (overlay_guide_open.load()) {
		return;
	}
	overlay_guide_open.store(true);
	overlay_active_panel = OverlayGuidePanel::HOME;
	overlay_guide_focus_panel_pending = true;
	overlay_legend_gamepad = OverlayGamepadIsConnected();

	OverlayCaptureTitleText();

	// Fresh achievement/gamerscore data every time the guide comes up.
	overlay_achievements_dirty.store(true);

	// Deep link into a waiting invite, matching the "press HOME to respond" toast.
	overlay_invite_modal_index = overlay_pending_invites.empty() ? -1 : 0;

	OverlayInputOnGuideOpened(overlay_title_hwnd);

	// Free the mouse: remember the title's cursor confinement and lift it.
	RECT virtualScreen;
	virtualScreen.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
	virtualScreen.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
	virtualScreen.right = virtualScreen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
	virtualScreen.bottom = virtualScreen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
	RECT currentClip;
	overlay_saved_cursor_clip_valid = GetClipCursor(&currentClip) && !EqualRect(&currentClip, &virtualScreen);
	if (overlay_saved_cursor_clip_valid) {
		overlay_saved_cursor_clip = currentClip;
		if (!GetWindowRect(overlay_title_hwnd, &overlay_saved_cursor_clip_window)) {
			SetRectEmpty(&overlay_saved_cursor_clip_window);
		}
		ClipCursor(0);
	}

	// Tell the title to pause (XN_SYS_UI). The recompute path picks up the overlay
	// state through XllnIsXliveSystemUIOpen.
	XllnUpdateXliveSystemUIOpenState();

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO, "%s guide opened.", __func__);
}

static void OverlayGuideClose()
{
	if (!overlay_guide_open.load()) {
		return;
	}
	overlay_guide_open.store(false);
	overlay_invite_modal_index = -1;
	overlay_guide_opened_for_keyboard = false;
	// A Title waiting on a text request still gets an answer when the guide goes away under it.
	OverlayKeyboardCancelActive();

	OverlayInputOnGuideClosed();

	if (overlay_saved_cursor_clip_valid) {
		// If the Title moved, changed size or is not in front, its old clip traps the cursor.
		RECT titleWindowRect;
		if (GetWindowThreadProcessId(GetForegroundWindow(), 0) == GetWindowThreadProcessId(overlay_title_hwnd, 0)
			&& GetWindowRect(overlay_title_hwnd, &titleWindowRect)
			&& EqualRect(&titleWindowRect, &overlay_saved_cursor_clip_window)
		) {
			ClipCursor(&overlay_saved_cursor_clip);
		}
		overlay_saved_cursor_clip_valid = false;
	}
	if (overlay_imgui_context_created) {
		ImGui::GetIO().MouseDrawCursor = false;
	}

	XllnUpdateXliveSystemUIOpenState();

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO, "%s guide closed.", __func__);
}

static void OverlayApplyQueuedTransitions()
{
	uint32_t closeCount = overlay_close_requests.exchange(0);
	uint32_t toggleCount = overlay_toggle_requests.exchange(0);
	uint32_t panelRequest = overlay_panel_open_requests.exchange(0);
	uint32_t keyboardRequest = overlay_keyboard_open_requests.exchange(0);
	bool targetOpen = overlay_guide_open.load();
	if (closeCount) {
		targetOpen = false;
	}
	if (toggleCount % 2) {
		targetOpen = !targetOpen;
	}
	if (panelRequest) {
		targetOpen = true;
	}
	if (keyboardRequest) {
		targetOpen = true;
	}
	if (targetOpen && !overlay_guide_open.load()) {
		overlay_guide_opened_for_keyboard = keyboardRequest != 0;
		OverlayGuideOpen();
	}
	else if (!targetOpen && overlay_guide_open.load()) {
		OverlayGuideClose();
	}
	// After OverlayGuideOpen so the deep link overrides its default panel.
	if (panelRequest && overlay_guide_open.load()) {
		overlay_active_panel = (OverlayGuidePanel)(panelRequest - 1);
	}
}

// --- events → toasts / invites ---------------------------------------------------

static float OverlayToastDefaultDuration(OverlayEventType event_type)
{
	switch (event_type) {
		case OverlayEventType::HINT: return 8.f;
		case OverlayEventType::INVITE_RECEIVED: return 7.f;
		default: return 4.5f;
	}
}

static void OverlayDrainEvents()
{
	OVERLAY_EVENT overlayEvent;
	while (OverlayEventQueuePop(&overlayEvent)) {
		if (overlayEvent.type == OverlayEventType::INVITE_RECEIVED) {
			// Keep at most one pending invite per remote player; a newer one replaces it.
			for (size_t iInvite = 0; iInvite < overlay_pending_invites.size(); iInvite++) {
				if (overlay_pending_invites[iInvite].xuid == overlayEvent.xuid) {
					overlay_pending_invites.erase(overlay_pending_invites.begin() + iInvite);
					if (overlay_invite_modal_index >= (int)iInvite) {
						overlay_invite_modal_index--;
					}
					break;
				}
			}
			overlay_pending_invites.push_back(overlayEvent);
		}
		if (overlayEvent.type == OverlayEventType::ACHIEVEMENT_UNLOCKED) {
			overlay_achievements_dirty = true;
		}
		OVERLAY_TOAST toast;
		toast.event = overlayEvent;
		toast.duration = overlayEvent.durationSeconds > 0.f ? overlayEvent.durationSeconds : OverlayToastDefaultDuration(overlayEvent.type);
		size_t waitingCount = 0;
		size_t oldestWaitingIndex = 0;
		for (size_t iToast = 0; iToast < overlay_toasts.size(); iToast++) {
			if (overlay_toasts[iToast].shownAt > 0.0) {
				continue;
			}
			if (!waitingCount) {
				oldestWaitingIndex = iToast;
			}
			waitingCount++;
		}
		if (waitingCount >= 12) {
			overlay_toasts.erase(overlay_toasts.begin() + oldestWaitingIndex);
		}
		overlay_toasts.push_back(toast);
	}

	double timeNow = OverlayTimeNow();
	for (size_t iToast = 0; iToast < overlay_toasts.size();) {
		if (overlay_toasts[iToast].shownAt > 0.0 && timeNow - overlay_toasts[iToast].shownAt > overlay_toasts[iToast].duration) {
			overlay_toasts.erase(overlay_toasts.begin() + iToast);
		}
		else {
			iToast++;
		}
	}
}

// --- fonts & theme ---------------------------------------------------------------

static void OverlaySetupTheme()
{
	ImGuiStyle &style = ImGui::GetStyle();
	style = ImGuiStyle();
	style.WindowRounding = OverlayPx(16.f);
	style.ChildRounding = OverlayPx(12.f);
	style.FrameRounding = OverlayPx(9.f);
	style.PopupRounding = OverlayPx(12.f);
	style.ScrollbarRounding = OverlayPx(8.f);
	style.GrabRounding = OverlayPx(8.f);
	style.WindowBorderSize = 1.f;
	style.ChildBorderSize = 0.f;
	style.FrameBorderSize = 0.f;
	style.PopupBorderSize = 1.f;
	style.WindowPadding = ImVec2(OverlayPx(12.f), OverlayPx(12.f));
	style.FramePadding = ImVec2(OverlayPx(12.f), OverlayPx(6.f));
	style.ItemSpacing = ImVec2(OverlayPx(8.f), OverlayPx(8.f));
	style.ItemInnerSpacing = ImVec2(OverlayPx(6.f), OverlayPx(6.f));
	style.ScrollbarSize = OverlayPx(18.f);
	style.GrabMinSize = OverlayPx(24.f);

	ImVec4* colors = style.Colors;
	colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT);
	colors[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_FAINT);
	colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_GLASS_2);
	colors[ImGuiCol_ChildBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
	colors[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_GLASS_2);
	colors[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_BORDER);
	colors[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.f);
	colors[ImGuiCol_FrameBg] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(15));
	colors[ImGuiCol_FrameBgHovered] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(26));
	colors[ImGuiCol_FrameBgActive] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(36));
	colors[ImGuiCol_TitleBg] = colors[ImGuiCol_WindowBg];
	colors[ImGuiCol_TitleBgActive] = colors[ImGuiCol_WindowBg];
	colors[ImGuiCol_ScrollbarBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
	colors[ImGuiCol_ScrollbarGrab] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(41));
	colors[ImGuiCol_ScrollbarGrabHovered] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(61));
	colors[ImGuiCol_ScrollbarGrabActive] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(77));
	colors[ImGuiCol_CheckMark] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_AMBER);
	colors[ImGuiCol_SliderGrab] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_AMBER);
	colors[ImGuiCol_SliderGrabActive] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_AMBER_LO);
	colors[ImGuiCol_Button] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(18));
	colors[ImGuiCol_ButtonHovered] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(33));
	colors[ImGuiCol_ButtonActive] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(46));
	colors[ImGuiCol_Header] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(15));
	colors[ImGuiCol_HeaderHovered] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(26));
	colors[ImGuiCol_HeaderActive] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_WASH_A(36));
	colors[ImGuiCol_Separator] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_BORDER);
	colors[ImGuiCol_NavHighlight] = ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_AMBER);
	colors[ImGuiCol_ModalWindowDimBg] = ImGui::ColorConvertU32ToFloat4(overlay_palette.dim);
}

// A theme change from the sun or Settings is taken up at the start of the next frame, so no frame
// draws in two themes.
static void OverlayEnsureTheme()
{
	OverlayTheme theme = OverlayThemeInEffect();
	if (theme == overlay_theme_applied) {
		return;
	}
	overlay_theme_applied = theme;
	OverlayThemeFillPalette(theme);
	OverlaySetupTheme();
}

static ImFont* OverlayAddFontFromCandidates(const char* const* face_candidates, float size_px, const ImWchar* glyph_ranges)
{
	wchar_t windowsDirectory[MAX_PATH];
	if (!GetWindowsDirectoryW(windowsDirectory, MAX_PATH)) {
		return 0;
	}
	ImFontConfig fontConfig;
	fontConfig.GlyphRanges = glyph_ranges;
	for (uint32_t iCandidate = 0; face_candidates[iCandidate]; iCandidate++) {
		char fontPath[MAX_PATH * 2];
		snprintf(fontPath, sizeof(fontPath), "%ls\\Fonts\\%s", windowsDirectory, face_candidates[iCandidate]);
		// AddFontFromFileTTF asserts on missing files in debug builds; probe first.
		if (GetFileAttributesA(fontPath) == INVALID_FILE_ATTRIBUTES) {
			continue;
		}
		ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(fontPath, size_px, &fontConfig);
		if (font) {
			return font;
		}
	}
	return 0;
}

static void OverlayEnsureFonts()
{
	float automaticScale = overlay_display_height ? ((float)overlay_display_height / 1080.f) : 1.f;
	float userScale = xlln_overlay_scale > 0.f ? xlln_overlay_scale : OVERLAY_SCALE_DEFAULT;
	// Clamp the player's choice, not the product: a product clamp folds the two largest sizes into one
	// on a display that already doubles the automatic scale.
	if (userScale < OVERLAY_SCALE_COMPACT) {
		userScale = OVERLAY_SCALE_COMPACT;
	}
	if (userScale > OVERLAY_SCALE_LARGER) {
		userScale = OVERLAY_SCALE_LARGER;
	}
	float targetScale = automaticScale * userScale;
	// At 4.0 the atlas is 2048 by 2048, which every device the overlay draws on can hold.
	if (targetScale < 0.5f) {
		targetScale = 0.5f;
	}
	if (targetScale > 4.f) {
		targetScale = 4.f;
	}
	if (overlay_fonts.reg13 && targetScale > overlay_fonts_built_scale - 0.05f && targetScale < overlay_fonts_built_scale + 0.05f) {
		return;
	}
	overlay_widget_scale = targetScale;
	overlay_fonts_built_scale = targetScale;

	ImGuiIO &imguiIo = ImGui::GetIO();
	imguiIo.Fonts->Clear();
	overlay_fonts = OVERLAY_FONT_SET();

	// Latin-1, plus the punctuation the UI uses, the marks some games need, and the legend arrows.
	static const ImWchar glyphRanges[] = {
		0x0020, 0x00FF,
		0x2013, 0x2026,
		0x2117, 0x2122,
		0x2190, 0x2193,
		0,
	};
	static const char* const facesRegular[] = { "segoeui.ttf", "tahoma.ttf", "arial.ttf", 0 };
	static const char* const facesSemibold[] = { "seguisb.ttf", "segoeuib.ttf", "tahomabd.ttf", "arialbd.ttf", 0 };
	static const char* const facesBold[] = { "segoeuib.ttf", "tahomabd.ttf", "arialbd.ttf", 0 };

	// A name says what a face is for, not how many pixels tall it is. Each size is chosen so that
	// the scale above lands it where it has to be: the body text on 18 pixels at 1080p, and nothing
	// on less than 14.
	overlay_fonts.reg12 = OverlayAddFontFromCandidates(facesRegular, 12.5f * targetScale, glyphRanges);
	overlay_fonts.reg13 = OverlayAddFontFromCandidates(facesRegular, 15.f * targetScale, glyphRanges);
	overlay_fonts.semi11 = OverlayAddFontFromCandidates(facesSemibold, 11.5f * targetScale, glyphRanges);
	overlay_fonts.semi12 = OverlayAddFontFromCandidates(facesSemibold, 12.5f * targetScale, glyphRanges);
	overlay_fonts.semi13 = OverlayAddFontFromCandidates(facesSemibold, 15.f * targetScale, glyphRanges);
	overlay_fonts.bold13 = OverlayAddFontFromCandidates(facesBold, 15.5f * targetScale, glyphRanges);
	overlay_fonts.bold16 = OverlayAddFontFromCandidates(facesBold, 16.f * targetScale, glyphRanges);
	overlay_fonts.bold17 = OverlayAddFontFromCandidates(facesBold, 17.5f * targetScale, glyphRanges);
	overlay_fonts.bold22 = OverlayAddFontFromCandidates(facesBold, 22.f * targetScale, glyphRanges);

	if (!overlay_fonts.reg13) {
		ImFontConfig fontConfig;
		fontConfig.SizePixels = 13.f * targetScale;
		overlay_fonts.reg13 = imguiIo.Fonts->AddFontDefault(&fontConfig);
	}
	if (!overlay_fonts.reg12) {
		overlay_fonts.reg12 = overlay_fonts.reg13;
	}
	if (!overlay_fonts.semi13) {
		overlay_fonts.semi13 = overlay_fonts.reg13;
	}
	if (!overlay_fonts.semi11) {
		overlay_fonts.semi11 = overlay_fonts.reg12;
	}
	if (!overlay_fonts.semi12) {
		overlay_fonts.semi12 = overlay_fonts.semi13;
	}
	if (!overlay_fonts.bold13) {
		overlay_fonts.bold13 = overlay_fonts.semi13;
	}
	if (!overlay_fonts.bold16) {
		overlay_fonts.bold16 = overlay_fonts.bold13;
	}
	if (!overlay_fonts.bold17) {
		overlay_fonts.bold17 = overlay_fonts.bold16;
	}
	if (!overlay_fonts.bold22) {
		overlay_fonts.bold22 = overlay_fonts.bold17;
	}
	imguiIo.Fonts->Build();
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
		, "%s built the fonts at scale %.3f (display %u px high, choice %.2f); the body text is %.0f px and the atlas is %d by %d."
		, __func__
		, targetScale
		, overlay_display_height
		, userScale
		, overlay_fonts.reg13->FontSize
		, imguiIo.Fonts->TexWidth
		, imguiIo.Fonts->TexHeight
	);
	imguiIo.FontDefault = overlay_fonts.reg13;
	OverlayRendererMarkFontsDirty();
	OverlaySetupTheme();
}

static void OverlayEnsureImGuiContext()
{
	if (overlay_imgui_context_created) {
		return;
	}
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &imguiIo = ImGui::GetIO();
	imguiIo.IniFilename = 0;
	imguiIo.LogFilename = 0;
	imguiIo.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	imguiIo.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	// Ctrl+Tab changes the panel, not the ImGui window.
	ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;
	ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
	imguiIo.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
	imguiIo.BackendPlatformName = "xlln-overlay";
	imguiIo.BackendRendererName = "xlln-overlay";
	overlay_imgui_context_created = true;
}

// --- shared bits -------------------------------------------------------------------

static const char* OverlayGuideKeyName()
{
	switch (xlive_hotkey_id_guide) {
		case VK_HOME: return "Home";
		case VK_END: return "End";
		case VK_INSERT: return "Insert";
		case VK_DELETE: return "Delete";
		case VK_PRIOR: return "PgUp";
		case VK_NEXT: return "PgDn";
		case VK_TAB: return "Tab";
	}
	if (xlive_hotkey_id_guide >= VK_F1 && xlive_hotkey_id_guide <= VK_F12) {
		static const char* functionKeyNames[12] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
		return functionKeyNames[xlive_hotkey_id_guide - VK_F1];
	}
	static char keyName[32];
	UINT scanCode = MapVirtualKeyA(xlive_hotkey_id_guide, MAPVK_VK_TO_VSC);
	if (scanCode && GetKeyNameTextA((LONG)(scanCode << 16), keyName, sizeof(keyName))) {
		return keyName;
	}
	return "Home";
}

// First signed-in local user, for the identity chip and legend.
static bool OverlaySignedInUser(char* username, size_t username_size, bool* signed_in_to_live, bool* online_enabled = 0)
{
	bool found = false;
	EnterCriticalSection(&xlive_critsec_local_user);
	for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
		if (xlive_local_users[iUser].signin_state != eXUserSigninState_NotSignedIn) {
			strncpy_s(username, username_size, xlive_local_users[iUser].username, _TRUNCATE);
			if (signed_in_to_live) {
				*signed_in_to_live = xlive_local_users[iUser].signin_state == eXUserSigninState_SignedInToLive;
			}
			if (online_enabled) {
				*online_enabled = xlive_local_users[iUser].online_enabled;
			}
			found = true;
			break;
		}
	}
	LeaveCriticalSection(&xlive_critsec_local_user);
	return found;
}

// First signed-in local user: the actor for friends/invites actions in the guide.
// XLIVE_LOCAL_USER_COUNT when nobody is signed in.
static uint32_t OverlayFirstSignedInUserIndex()
{
	uint32_t userIndex = XLIVE_LOCAL_USER_COUNT;
	EnterCriticalSection(&xlive_critsec_local_user);
	for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
		if (xlive_local_users[iUser].signin_state != eXUserSigninState_NotSignedIn) {
			userIndex = iUser;
			break;
		}
	}
	LeaveCriticalSection(&xlive_critsec_local_user);
	return userIndex;
}

// --- toasts ----------------------------------------------------------------------

// Resolved toast anchor: the config override first, then the title's
// XNotifyPositionUI request, otherwise the GFWL default (bottom centre).
static uint32_t OverlayToastPosition()
{
	if (xlln_overlay_toast_position != 0xFF) {
		return xlln_overlay_toast_position & XNOTIFYUI_POS_MASK;
	}
	return XLiveNotifyGetUIPosition();
}

// Fills segments with the toast headline: the gamertag, then a verb. The verb comes from
// the event type and not from the event's detail text, because each producer of an event
// writes its detail text differently.
static int OverlayToastComposeHeadline(const OVERLAY_EVENT &overlay_event, OVERLAY_INLINE_SEGMENT* segments)
{
	const char* verb = 0;
	switch (overlay_event.type) {
		case OverlayEventType::PLAYER_ONLINE: verb = "is online"; break;
		case OverlayEventType::PLAYER_OFFLINE: verb = "went offline"; break;
		case OverlayEventType::SIGNED_IN: break;
		case OverlayEventType::SIGNED_OUT: break;
		case OverlayEventType::INVITE_RECEIVED: verb = "invited you to play"; break;
		case OverlayEventType::INVITE_DECLINED: verb = "declined your invite"; break;
		case OverlayEventType::ACHIEVEMENT_UNLOCKED: verb = "unlocked"; break;
		default: break;
	}
	int segmentCount = 0;
	segments[segmentCount].text = overlay_event.title;
	segments[segmentCount].font = overlay_fonts.semi13;
	segments[segmentCount].color = OVERLAY_COL_TEXT;
	segments[segmentCount].keycap = false;
	segmentCount++;
	if (verb) {
		segments[segmentCount].text = " ";
		segments[segmentCount].font = overlay_fonts.reg13;
		segments[segmentCount].color = OVERLAY_COL_TEXT;
		segments[segmentCount].keycap = false;
		segmentCount++;
		segments[segmentCount].text = verb;
		segments[segmentCount].font = overlay_fonts.reg13;
		segments[segmentCount].color = OVERLAY_COL_TEXT;
		segments[segmentCount].keycap = false;
		segmentCount++;
	}
	return segmentCount;
}

static int OverlayToastComposeSubLine(const OVERLAY_EVENT &overlay_event, OVERLAY_INLINE_SEGMENT* segments, char* text_buffer, size_t text_buffer_size)
{
	if (overlay_event.type == OverlayEventType::INVITE_RECEIVED) {
		// "<game> — press [Home] to respond" (invites are same-title in GFWL).
		if (overlay_title_text[0]) {
			snprintf(text_buffer, text_buffer_size, "%s \xE2\x80\x94 press ", overlay_title_text);
		}
		else {
			snprintf(text_buffer, text_buffer_size, "press ");
		}
		segments[0].text = text_buffer;
		segments[0].font = overlay_fonts.reg12;
		segments[0].color = OVERLAY_COL_TEXT_DIM;
		segments[0].keycap = false;
		segments[1].text = OverlayGuideKeyName();
		segments[1].font = 0;
		segments[1].color = 0;
		segments[1].keycap = true;
		segments[2].text = " to respond";
		segments[2].font = overlay_fonts.reg12;
		segments[2].color = OVERLAY_COL_TEXT_DIM;
		segments[2].keycap = false;
		return 3;
	}
	// Suppress details that just repeat the composed verb.
	const char* detail = overlay_event.detail;
	if (!detail[0]) {
		return 0;
	}
	switch (overlay_event.type) {
		case OverlayEventType::PLAYER_ONLINE: {
			if (!strcmp(detail, "Is online")) {
				return 0;
			}
			break;
		}
		case OverlayEventType::PLAYER_OFFLINE: {
			if (!strcmp(detail, "Went offline")) {
				return 0;
			}
			break;
		}
		default: {
			break;
		}
	}
	segments[0].text = detail;
	segments[0].font = overlay_fonts.reg12;
	segments[0].color = OVERLAY_COL_TEXT_DIM;
	segments[0].keycap = false;
	return 1;
}

// Draws the first-run hint toast. It always sits at the top centre and so it ignores the
// toast anchor that the other toasts follow.
static void OverlayBuildHintToast(const OVERLAY_TOAST &toast, float alpha, float slide_progress)
{
	ImGuiIO &imguiIo = ImGui::GetIO();
	float slideOffset = (1.f - slide_progress) * -OverlayPx(12.f);
	ImGui::SetNextWindowPos(
		ImVec2(imguiIo.DisplaySize.x * 0.5f, OverlayPx(20.f) + slideOffset)
		, ImGuiCond_Always
		, ImVec2(0.5f, 0.f)
	);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, OverlayPx(22.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(OverlayPx(9.f), OverlayPx(8.f)));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_GLASS));
	overlay_widget_alpha = alpha;
	if (ImGui::Begin(
		"##xlln_hint"
		, 0
		, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
		| ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
	)) {
		float keycapHeight = OverlayKeycapHeight(true);
		OverlayKeycapItem(OverlayGuideKeyName(), true);
		ImGui::SameLine(0.f, OverlayPx(10.f));
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (keycapHeight - overlay_fonts.reg13->FontSize) * 0.5f);
		ImGui::PushFont(overlay_fonts.reg13);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
		ImGui::TextUnformatted(toast.event.title);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::SameLine(0.f, OverlayPx(6.f));
		ImGui::Dummy(ImVec2(0.f, 0.f));

		ImVec2 windowPosition = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();
		OverlayDrawSoftShadow(
			ImGui::GetBackgroundDrawList()
			, windowPosition
			, ImVec2(windowPosition.x + windowSize.x, windowPosition.y + windowSize.y)
			, OverlayPx(22.f)
			, OverlayPx(6.f)
			, OverlayPx(14.f)
			, 16
		);
	}
	ImGui::End();
	overlay_widget_alpha = 1.f;
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
}

static void OverlayBuildToasts()
{
	if (overlay_toasts.empty()) {
		return;
	}
	ImGuiIO &imguiIo = ImGui::GetIO();
	uint32_t toastPosition = OverlayToastPosition();
	float margin = OverlayPx(20.f);
	bool anchorTop = (toastPosition & XNOTIFYUI_POS_TOPCENTER) != 0;
	bool anchorBottom = (toastPosition & XNOTIFYUI_POS_BOTTOMCENTER) != 0;
	bool anchorLeft = (toastPosition & XNOTIFYUI_POS_CENTERLEFT) != 0;
	bool anchorRight = (toastPosition & XNOTIFYUI_POS_CENTERRIGHT) != 0;

	float anchorX = anchorLeft ? margin : (anchorRight ? imguiIo.DisplaySize.x - margin : imguiIo.DisplaySize.x * 0.5f);
	float pivotX = anchorLeft ? 0.f : (anchorRight ? 1.f : 0.5f);
	float stackY = anchorTop ? margin : (anchorBottom ? imguiIo.DisplaySize.y - margin : imguiIo.DisplaySize.y * 0.4f);
	float stackDirection = anchorBottom ? -1.f : 1.f;

	double timeNow = OverlayTimeNow();
	int visibleCount = 0;
	for (size_t iToast = 0; iToast < overlay_toasts.size() && visibleCount < 3; iToast++) {
		OVERLAY_TOAST &toast = overlay_toasts[iToast];
		if (toast.shownAt <= 0.0) {
			toast.shownAt = timeNow;
		}
		// A toast in its first frame has alpha 0. Count it here, or every waiting toast starts at once.
		if (toast.event.type != OverlayEventType::HINT) {
			visibleCount++;
		}
		float elapsed = (float)(timeNow - toast.shownAt);
		float alphaIn = elapsed / 0.20f;
		float alphaOut = (toast.duration - elapsed) / 0.30f;
		float alpha = alphaIn < 1.f ? alphaIn : (alphaOut < 1.f ? alphaOut : 1.f);
		if (alpha <= 0.f) {
			continue;
		}
		if (alpha > 1.f) {
			alpha = 1.f;
		}
		float slideProgress = alphaIn < 1.f ? alphaIn : 1.f;
		// ease-out cubic
		slideProgress = 1.f - (1.f - slideProgress) * (1.f - slideProgress) * (1.f - slideProgress);

		if (toast.event.type == OverlayEventType::HINT) {
			OverlayBuildHintToast(toast, alpha, slideProgress);
			continue;
		}
		float slideOffset = (1.f - slideProgress) * OverlayPx(24.f) * (anchorRight ? 1.f : (anchorLeft ? -1.f : 0.f));

		ImGui::SetNextWindowPos(ImVec2(anchorX + slideOffset, stackY), ImGuiCond_Always, ImVec2(pivotX, anchorBottom ? 1.f : 0.f));
		// Fixed toast width (the message wraps within it) so the stack stays edge
		// aligned and every timer bar is the same length.
		float toastWidth = OverlayPx(320.f);
		ImGui::SetNextWindowSizeConstraints(ImVec2(toastWidth, 0.f), ImVec2(toastWidth, FLT_MAX));
		bool isAccented = toast.event.type == OverlayEventType::INVITE_RECEIVED
			|| toast.event.type == OverlayEventType::ACHIEVEMENT_UNLOCKED;
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, OverlayPx(13.f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(OverlayPx(12.f), OverlayPx(11.f)));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_GLASS));
		ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(isAccented ? OverlayColorWithAlpha(OVERLAY_COL_AMBER, 90) : OVERLAY_COL_BORDER));
		overlay_widget_alpha = alpha;
		char toastWindowId[32];
		snprintf(toastWindowId, sizeof(toastWindowId), "##xlln_toast_%u", (uint32_t)iToast);
		if (ImGui::Begin(
			toastWindowId
			, 0
			, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize
			| ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
		)) {
			// Identity avatar.
			float avatarSize = OverlayPx(36.f);
			ImVec2 avatarPosition = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(avatarSize, avatarSize));
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			switch (toast.event.type) {
				case OverlayEventType::JOINING_SESSION: {
					OverlayDrawAvatarCheck(drawList, avatarPosition, avatarSize);
					break;
				}
				case OverlayEventType::ACHIEVEMENT_UNLOCKED: {
					ImTextureID achievementArt = OverlayTextureForSpaImage(toast.event.imageId);
					if (achievementArt) {
						drawList->AddImageRounded(
							achievementArt
							, avatarPosition
							, ImVec2(avatarPosition.x + avatarSize, avatarPosition.y + avatarSize)
							, ImVec2(0.f, 0.f)
							, ImVec2(1.f, 1.f)
							, OverlayAlpha(IM_COL32(255, 255, 255, 255))
							, OverlayPx(8.f)
						);
					}
					else {
						OverlayDrawAvatar(drawList, avatarPosition, avatarSize, toast.event.title, true, false, false);
					}
					break;
				}
				case OverlayEventType::GENERIC:
				case OverlayEventType::HINT: {
					OverlayDrawAvatar(drawList, avatarPosition, avatarSize, toast.event.title, true, false, false);
					break;
				}
				case OverlayEventType::SIGNED_IN:
				case OverlayEventType::SIGNED_OUT: {
					// The account name is the sub-line here, so take the avatar from that.
					OverlayDrawAvatar(drawList, avatarPosition, avatarSize, toast.event.detail, false
						, toast.event.type == OverlayEventType::SIGNED_IN && IsOnlineXUID(toast.event.xuid)
						, toast.event.type == OverlayEventType::SIGNED_OUT);
					break;
				}
				default: {
					bool dimmed = toast.event.type == OverlayEventType::PLAYER_OFFLINE || toast.event.type == OverlayEventType::SIGNED_OUT;
					bool onlineDot = toast.event.type == OverlayEventType::PLAYER_ONLINE
						|| toast.event.type == OverlayEventType::SIGNED_IN
						|| toast.event.type == OverlayEventType::INVITE_RECEIVED;
					OverlayDrawAvatar(drawList, avatarPosition, avatarSize, toast.event.title, false, onlineDot, dimmed);
					break;
				}
			}

			ImGui::SameLine(0.f, OverlayPx(12.f));
			ImGui::BeginGroup();
			float flowWidth = toastWidth - OverlayPx(24.f) - avatarSize - OverlayPx(12.f);
			OVERLAY_INLINE_SEGMENT headlineSegments[4];
			int headlineSegmentCount = OverlayToastComposeHeadline(toast.event, headlineSegments);
			float headlineHeight = OverlayInlineFlow(0, ImVec2(0.f, 0.f), flowWidth, headlineSegments, headlineSegmentCount);
			ImVec2 headlinePosition = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(flowWidth, headlineHeight));
			OverlayInlineFlow(drawList, headlinePosition, flowWidth, headlineSegments, headlineSegmentCount);

			OVERLAY_INLINE_SEGMENT subSegments[4];
			char subTextBuffer[128];
			int subSegmentCount = OverlayToastComposeSubLine(toast.event, subSegments, subTextBuffer, sizeof(subTextBuffer));
			if (subSegmentCount) {
				float subHeight = OverlayInlineFlow(0, ImVec2(0.f, 0.f), flowWidth, subSegments, subSegmentCount);
				ImVec2 subPosition = ImVec2(headlinePosition.x, ImGui::GetCursorScreenPos().y - OverlayPx(3.f));
				ImGui::Dummy(ImVec2(flowWidth, subHeight - OverlayPx(3.f)));
				OverlayInlineFlow(drawList, subPosition, flowWidth, subSegments, subSegmentCount);
			}
			ImGui::EndGroup();

			// Remaining-time hairline, inset past the corner rounding.
			float drainFraction = 1.f - (elapsed / toast.duration);
			ImVec2 windowPosition = ImGui::GetWindowPos();
			ImVec2 windowSize = ImGui::GetWindowSize();
			if (drainFraction > 0.f) {
				float drainInset = OverlayPx(13.f);
				drawList->AddRectFilled(
					ImVec2(windowPosition.x + drainInset, windowPosition.y + windowSize.y - OverlayPx(2.5f))
					, ImVec2(windowPosition.x + drainInset + (windowSize.x - drainInset * 2.f) * drainFraction, windowPosition.y + windowSize.y - OverlayPx(1.f))
					, OverlayAlpha(OVERLAY_COL_AMBER)
				);
			}
			OverlayDrawSoftShadow(
				ImGui::GetBackgroundDrawList()
				, windowPosition
				, ImVec2(windowPosition.x + windowSize.x, windowPosition.y + windowSize.y)
				, OverlayPx(13.f)
				, OverlayPx(7.f)
				, OverlayPx(16.f)
				, 16
			);
			stackY += stackDirection * (windowSize.y + OverlayPx(10.f));
		}
		ImGui::End();
		overlay_widget_alpha = 1.f;
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(3);
	}
}

// --- guide: band -------------------------------------------------------------------

static float OverlayBuildBand(float band_x, float band_y, float band_width)
{
	ImGui::SetNextWindowPos(ImVec2(band_x, band_y), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(band_width, 0.f), ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(OverlayPx(14.f), OverlayPx(10.f)));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_GLASS));
	float bandBottom = band_y;
	// No directional navigation in the band: a cursor here could never return to the panel. The
	// shoulder buttons and Ctrl+Tab change tabs instead.
	if (ImGui::Begin(
		"##xlln_guide_band"
		, 0
		, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoNav
	)) {
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		float rowHeight = OverlayPx(40.f);
		if (overlay_palette.bandGloss) {
			// Size from the frame before; the band keeps one height, so only its first frame lacks the sheen.
			ImVec2 bandPosition = ImGui::GetWindowPos();
			ImVec2 bandSize = ImGui::GetWindowSize();
			OverlayDrawSheen(
				drawList
				, ImVec2(bandPosition.x + 1.f, bandPosition.y + 1.f)
				, ImVec2(bandPosition.x + bandSize.x - 1.f, bandPosition.y + bandSize.y * 0.5f)
				, OverlayPx(16.f) - 1.f
				, overlay_palette.bandGloss
				, overlay_palette.bandGloss * 2 / 5
			);
		}
		ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
		float cursorX = rowOrigin.x;

		float orbDiameter = OverlayPx(30.f);
		// The title's own icon (SPA image 0x8000, 64x64) takes the brand spot when
		// available; the XLLN orb stays the fallback for titles without a SPA.
		ImTextureID titleIcon = OverlayTextureForSpaImage(0x8000);
		if (titleIcon) {
			ImVec2 iconMin(cursorX, rowOrigin.y + (rowHeight - orbDiameter) * 0.5f);
			drawList->AddImageRounded(
				titleIcon
				, iconMin
				, ImVec2(iconMin.x + orbDiameter, iconMin.y + orbDiameter)
				, ImVec2(0.f, 0.f)
				, ImVec2(1.f, 1.f)
				, OverlayAlpha(IM_COL32(255, 255, 255, 255))
				, OverlayPx(7.f)
			);
		}
		else {
			OverlayDrawBrandOrb(
				drawList
				, ImVec2(cursorX + orbDiameter * 0.5f, rowOrigin.y + rowHeight * 0.5f)
				, orbDiameter * 0.5f
			);
		}
		cursorX += orbDiameter + OverlayPx(9.f);
		int brandmarkWidth = 0;
		int brandmarkHeight = 0;
		ImTextureID brandmarkTexture = OverlayRendererBrandmarkTexture((int)(OverlayPx(19.f) + 0.5f), &brandmarkWidth, &brandmarkHeight);
		if (brandmarkTexture) {
			float brandmarkTop = rowOrigin.y + (rowHeight - (float)brandmarkHeight) * 0.5f;
			drawList->AddImage(
				brandmarkTexture
				, ImVec2(cursorX, brandmarkTop)
				, ImVec2(cursorX + (float)brandmarkWidth, brandmarkTop + (float)brandmarkHeight)
				, ImVec2(0.f, 0.f)
				, ImVec2(1.f, 1.f)
				, OverlayAlpha(IM_COL32(255, 255, 255, 255))
			);
			cursorX += (float)brandmarkWidth + OverlayPx(16.f);
		}
		else {
			struct { const char* text; ImU32 color; } wordmarkSegments[3] = {
				{ "X", IM_COL32(146, 200, 59, 255) },
				{ "LL", IM_COL32(251, 163, 46, 255) },
				{ "N", IM_COL32(163, 175, 192, 255) },
			};
			float wordmarkTop = rowOrigin.y + (rowHeight - overlay_fonts.bold16->FontSize) * 0.5f;
			for (int iSegment = 0; iSegment < 3; iSegment++) {
				drawList->AddText(overlay_fonts.bold16, overlay_fonts.bold16->FontSize, ImVec2(cursorX, wordmarkTop), OverlayAlpha(wordmarkSegments[iSegment].color), wordmarkSegments[iSegment].text);
				cursorX += overlay_fonts.bold16->CalcTextSizeA(overlay_fonts.bold16->FontSize, FLT_MAX, 0.f, wordmarkSegments[iSegment].text).x;
			}
			cursorX += OverlayPx(16.f);
		}

		// Tabs.
		ImGui::SetCursorScreenPos(ImVec2(cursorX, rowOrigin.y + (rowHeight - OverlayPx(30.f)) * 0.5f));
		if (OverlayTabItem("Home", overlay_active_panel == OverlayGuidePanel::HOME, 0)) {
			overlay_active_panel = OverlayGuidePanel::HOME;
		}
		ImGui::SameLine(0.f, OverlayPx(4.f));
		{
			int addedMeCount = 0;
			uint32_t localUserIndex = OverlayFirstSignedInUserIndex();
			if (localUserIndex != XLIVE_LOCAL_USER_COUNT) {
				EnterCriticalSection(&xlln_critsec_friends);
				addedMeCount = (int)xlln_friends_added_me[localUserIndex].size();
				LeaveCriticalSection(&xlln_critsec_friends);
			}
			if (OverlayTabItem("Friends", overlay_active_panel == OverlayGuidePanel::FRIENDS, addedMeCount)) {
				overlay_active_panel = OverlayGuidePanel::FRIENDS;
			}
		}
		ImGui::SameLine(0.f, OverlayPx(4.f));
		if (OverlayTabItem("Players", overlay_active_panel == OverlayGuidePanel::PLAYERS, 0)) {
			overlay_active_panel = OverlayGuidePanel::PLAYERS;
		}
		ImGui::SameLine(0.f, OverlayPx(4.f));
		if (OverlayTabItem("Invites", overlay_active_panel == OverlayGuidePanel::INVITES, (int)overlay_pending_invites.size())) {
			overlay_active_panel = OverlayGuidePanel::INVITES;
		}
		ImGui::SameLine(0.f, OverlayPx(4.f));
		if (OverlayTabItem("Achievements", overlay_active_panel == OverlayGuidePanel::ACHIEVEMENTS, 0)) {
			overlay_active_panel = OverlayGuidePanel::ACHIEVEMENTS;
		}

		// Right cluster: identity chip, clock, theme, close.
		char signedInUsername[XUSER_NAME_SIZE] = {0};
		bool signedInToLive = false;
		bool onlineEnabled = false;
		bool anySignedIn = OverlaySignedInUser(signedInUsername, sizeof(signedInUsername), &signedInToLive, &onlineEnabled);
		const char* chipName = anySignedIn ? signedInUsername : "Not signed in";
		const char* chipSignin = onlineEnabled ? "LIVE" : (signedInToLive ? "Offline" : "Local");
		char chipStateText[24] = {0};
		if (anySignedIn) {
			if (overlay_gamerscore_total > 0) {
				snprintf(chipStateText, sizeof(chipStateText), "%s \xC2\xB7 %dG", chipSignin, overlay_gamerscore_total);
			}
			else {
				snprintf(chipStateText, sizeof(chipStateText), "%s", chipSignin);
			}
		}
		const char* chipState = anySignedIn ? chipStateText : 0;

		SYSTEMTIME localTime;
		GetLocalTime(&localTime);
		char clockText[8];
		snprintf(clockText, sizeof(clockText), "%02u:%02u", localTime.wHour, localTime.wMinute);
		float clockWidth = overlay_fonts.semi13->CalcTextSizeA(overlay_fonts.semi13->FontSize, FLT_MAX, 0.f, clockText).x;

		float chipNameWidth = overlay_fonts.semi13->CalcTextSizeA(overlay_fonts.semi13->FontSize, FLT_MAX, 0.f, chipName).x;
		float chipStateWidth = chipState ? overlay_fonts.semi11->CalcTextSizeA(overlay_fonts.semi11->FontSize, FLT_MAX, 0.f, chipState).x : 0.f;
		float chipTextWidth = chipNameWidth > chipStateWidth ? chipNameWidth : chipStateWidth;
		float chipWidth = OverlayPx(6.f) + OverlayPx(28.f) + OverlayPx(9.f) + chipTextWidth + OverlayPx(12.f);

		float iconSize = OverlayPx(32.f);
		float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - OverlayPx(14.f);
		float closeX = rightEdge - iconSize;
		float lightX = closeX - OverlayPx(4.f) - iconSize;
		float settingsX = lightX - OverlayPx(4.f) - iconSize;
		float clockX = settingsX - OverlayPx(10.f) - clockWidth;
		float chipX = clockX - OverlayPx(12.f) - chipWidth;

		// Identity chip.
		{
			ImVec2 chipMin(chipX, rowOrigin.y);
			ImVec2 chipMax(chipX + chipWidth, rowOrigin.y + rowHeight);
			drawList->AddRectFilled(chipMin, chipMax, OverlayAlpha(OVERLAY_COL_WASH_A(13)), OverlayPx(11.f));
			drawList->AddRect(chipMin, chipMax, OverlayAlpha(OVERLAY_COL_BORDER), OverlayPx(11.f));
			float chipAvatarSize = OverlayPx(28.f);
			ImVec2 chipAvatarPosition(chipMin.x + OverlayPx(6.f), rowOrigin.y + (rowHeight - chipAvatarSize) * 0.5f);
			OverlayDrawAvatar(drawList, chipAvatarPosition, chipAvatarSize, anySignedIn ? signedInUsername : "?", false, onlineEnabled, !anySignedIn);
			float chipTextX = chipAvatarPosition.x + chipAvatarSize + OverlayPx(9.f);
			if (chipState) {
				float chipTextHeight = overlay_fonts.semi13->FontSize + overlay_fonts.semi11->FontSize + OverlayPx(1.f);
				float chipTextTop = rowOrigin.y + (rowHeight - chipTextHeight) * 0.5f;
				drawList->AddText(overlay_fonts.semi13, overlay_fonts.semi13->FontSize, ImVec2(chipTextX, chipTextTop), OverlayAlpha(OVERLAY_COL_TEXT), chipName);
				drawList->AddText(
					overlay_fonts.semi11
					, overlay_fonts.semi11->FontSize
					, ImVec2(chipTextX, chipTextTop + overlay_fonts.semi13->FontSize + OverlayPx(1.f))
					, OverlayAlpha(onlineEnabled ? OVERLAY_COL_AMBER : OVERLAY_COL_TEXT_DIM)
					, chipState
				);
			}
			else {
				drawList->AddText(
					overlay_fonts.semi13
					, overlay_fonts.semi13->FontSize
					, ImVec2(chipTextX, rowOrigin.y + (rowHeight - overlay_fonts.semi13->FontSize) * 0.5f)
					, OverlayAlpha(OVERLAY_COL_TEXT_DIM)
					, chipName
				);
			}
		}

		// Clock.
		drawList->AddText(
			overlay_fonts.semi13
			, overlay_fonts.semi13->FontSize
			, ImVec2(clockX, rowOrigin.y + (rowHeight - overlay_fonts.semi13->FontSize) * 0.5f)
			, OverlayAlpha(OVERLAY_COL_TEXT_DIM)
			, clockText
		);

		// Icon actions.
		ImGui::SetCursorScreenPos(ImVec2(settingsX, rowOrigin.y + (rowHeight - iconSize) * 0.5f));
		if (OverlayIconButton("##xlln_band_settings", OverlayIconType::SETTINGS, overlay_active_panel == OverlayGuidePanel::SETTINGS)) {
			overlay_active_panel = OverlayGuidePanel::SETTINGS;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Settings");
		}
		ImGui::SetCursorScreenPos(ImVec2(lightX, rowOrigin.y + (rowHeight - iconSize) * 0.5f));
		if (OverlayIconButton("##xlln_band_theme", OverlayIconType::LIGHT)) {
			OverlayThemeCycle();
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Switch to the %s theme", OverlayThemeName(OverlayThemeNext()));
		}
		ImGui::SetCursorScreenPos(ImVec2(closeX, rowOrigin.y + (rowHeight - iconSize) * 0.5f));
		if (OverlayIconButton("##xlln_band_close", OverlayIconType::CLOSE)) {
			OverlayGuideRequestClose();
		}

		// Reserve the band row so the window sizes itself.
		ImGui::SetCursorScreenPos(rowOrigin);
		ImGui::Dummy(ImVec2(band_width - OverlayPx(28.f), rowHeight));

		ImVec2 windowPosition = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();
		bandBottom = windowPosition.y + windowSize.y;
		OverlayDrawSoftShadow(
			ImGui::GetBackgroundDrawList()
			, windowPosition
			, ImVec2(windowPosition.x + windowSize.x, windowPosition.y + windowSize.y)
			, OverlayPx(16.f)
			, OverlayPx(10.f)
			, OverlayPx(22.f)
			, 18
		);
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	return bandBottom;
}

// --- guide: panels -----------------------------------------------------------------

static void OverlayPanelHeading(const char* title, const char* subtitle)
{
	// A theme with a heading bar draws the heading on it, in the bar's own text colour.
	bool onBar = (overlay_palette.headingTop & IM_COL32_A_MASK) != 0;
	ImU32 titleColor = onBar ? overlay_palette.headingText : OVERLAY_COL_TEXT;
	ImU32 subtitleColor = onBar ? OverlayColorWithAlpha(overlay_palette.headingText, 200) : OVERLAY_COL_TEXT_FAINT;
	if (onBar) {
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 windowPosition = ImGui::GetWindowPos();
		float windowWidth = ImGui::GetWindowWidth();
		ImVec2 barMin(windowPosition.x + OverlayPx(10.f), ImGui::GetCursorScreenPos().y - OverlayPx(2.f));
		ImVec2 barMax(windowPosition.x + windowWidth - OverlayPx(10.f), barMin.y + overlay_fonts.bold16->FontSize + OverlayPx(8.f));
		if (overlay_palette.headingFrameTop & IM_COL32_A_MASK) {
			// Inside the border the interface drew, and rounded with it at the top.
			ImVec2 frameMin(windowPosition.x + 2.f, windowPosition.y + 2.f);
			ImVec2 frameMax(windowPosition.x + windowWidth - 2.f, barMax.y + OverlayPx(8.f));
			OverlayDrawRectVGradient(drawList, frameMin, frameMax, overlay_palette.headingFrameTop, overlay_palette.headingFrameBottom, OverlayPx(16.f) - 2.f, ImDrawFlags_RoundCornersTop);
			drawList->AddLine(ImVec2(frameMin.x, frameMax.y), ImVec2(frameMax.x, frameMax.y), OverlayAlpha(overlay_palette.headingFrameBorder));
		}
		OverlayDrawRectVGradient(drawList, barMin, barMax, overlay_palette.headingTop, overlay_palette.headingBottom, OverlayPx(6.f));
		OverlayDrawSheen(
			drawList
			, ImVec2(barMin.x + 1.f, barMin.y + 1.f)
			, ImVec2(barMax.x - 1.f, barMin.y + (barMax.y - barMin.y) * 0.5f)
			, OverlayPx(6.f) - 1.f
			, 90
			, 30
		);
	}
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + OverlayPx(8.f));
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + OverlayPx(2.f));
	ImGui::PushFont(overlay_fonts.bold16);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(titleColor));
	ImGui::TextUnformatted(title);
	ImGui::PopStyleColor();
	ImGui::PopFont();
	if (subtitle && subtitle[0]) {
		ImGui::SameLine(0.f, OverlayPx(10.f));
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (overlay_fonts.bold16->FontSize - overlay_fonts.reg12->FontSize) * 0.7f);
		ImGui::PushFont(overlay_fonts.reg12);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(subtitleColor));
		ImGui::TextUnformatted(subtitle);
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}
	ImGui::Dummy(ImVec2(0.f, OverlayPx(onBar ? 12.f : 2.f)));
}

static void OverlayPanelEmptyState(const char* message)
{
	ImGui::Dummy(ImVec2(0.f, OverlayPx(2.f)));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + OverlayPx(10.f));
	ImGui::PushFont(overlay_fonts.reg13);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - OverlayPx(10.f));
	ImGui::TextUnformatted(message);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	ImGui::PopFont();
	ImGui::Dummy(ImVec2(0.f, OverlayPx(2.f)));
}

// Name over a dim context line, used inside rows next to an avatar.
static void OverlayIdentityText(const char* name, const char* status, ImU32 status_color, const char* status_detail)
{
	ImGui::BeginGroup();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, OverlayPx(2.f)));
	ImGui::PushFont(overlay_fonts.bold13);
	ImGui::TextUnformatted(name);
	ImGui::PopFont();
	ImGui::PushFont(overlay_fonts.reg12);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(status_color));
	ImGui::TextUnformatted(status);
	ImGui::PopStyleColor();
	if (status_detail && status_detail[0]) {
		ImGui::SameLine(0.f, 0.f);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
		ImGui::TextUnformatted(status_detail);
		ImGui::PopStyleColor();
	}
	ImGui::PopFont();
	ImGui::PopStyleVar();
	ImGui::EndGroup();
}

// A choice made here is written to the config, so it is still the choice on the next run.
static void OverlayBuildPanelSettings()
{
	float availableWidth = ImGui::GetContentRegionAvail().x;
	float columnGap = OverlayPx(12.f);
	float sizeCardWidth = (availableWidth - columnGap) * 0.6f;
	float toastCardWidth = availableWidth - columnGap - sizeCardWidth;
	float itemSpacingY = ImGui::GetStyle().ItemSpacing.y;
	float toastCellSize = OverlayPx(30.f);
	float toastCellGap = OverlayPx(6.f);

	float cardHeight = OverlayPx(28.f)
		+ overlay_fonts.semi11->FontSize + OverlayPx(4.f)
		+ 4 * (itemSpacingY + OverlayButtonHeight())
		+ itemSpacingY + overlay_fonts.reg12->FontSize;

	static const struct _OVERLAY_SIZE_CHOICE {
		const char* label;
		const char* detail;
		float scale;
	} sizeChoices[] = {
		{ "Compact", "the original layout", OVERLAY_SCALE_COMPACT },
		{ "Default", "readable in a game", OVERLAY_SCALE_DEFAULT },
		{ "Large", "one step up", OVERLAY_SCALE_LARGE },
		{ "Larger", "two steps up", OVERLAY_SCALE_LARGER },
	};

	OverlayCardBegin("##xlln_settings_theme", availableWidth);
	{
		OverlayEyebrowLabel("THEME");
		static const char* const themeLabels[OVERLAY_THEME_COUNT] = { "Dark", "Light", "Classic" };
		int themeCount = xlln_overlay_theme_classic_unlocked ? OVERLAY_THEME_COUNT : OVERLAY_THEME_COUNT - 1;
		float themeGap = OverlayPx(8.f);
		float themeButtonWidth = (availableWidth - OverlayPx(32.f) - themeGap * (themeCount - 1)) / themeCount;
		OverlayTheme themeInEffect = OverlayThemeInEffect();
		for (int iTheme = 0; iTheme < themeCount; iTheme++) {
			if (iTheme) {
				ImGui::SameLine(0.f, themeGap);
			}
			char buttonId[48];
			snprintf(buttonId, sizeof(buttonId), "##xlln_settings_theme_%d", iTheme);
			if (OverlayButton(
				buttonId
				, themeLabels[iTheme]
				, (uint32_t)themeInEffect == (uint32_t)iTheme ? OverlayButtonKind::PRIMARY : OverlayButtonKind::SECONDARY
				, ImVec2(themeButtonWidth, OverlayButtonHeight())
			)) {
				xlln_overlay_theme = (uint32_t)iTheme;
				XllnConfig(true);
			}
		}
	}
	OverlayCardEnd(availableWidth);

	OverlayCardBegin("##xlln_settings_size", sizeCardWidth);
	{
		OverlayEyebrowLabel("INTERFACE SIZE");
		float innerWidth = sizeCardWidth - OverlayPx(32.f);
		float chosenScale = xlln_overlay_scale > 0.f ? xlln_overlay_scale : OVERLAY_SCALE_DEFAULT;
		for (int iChoice = 0; iChoice < (int)(sizeof(sizeChoices) / sizeof(sizeChoices[0])); iChoice++) {
			const _OVERLAY_SIZE_CHOICE &choice = sizeChoices[iChoice];
			bool chosen = chosenScale > choice.scale - 0.01f && chosenScale < choice.scale + 0.01f;
			char buttonId[48];
			snprintf(buttonId, sizeof(buttonId), "##xlln_settings_size_%d", iChoice);
			if (OverlayButton(
				buttonId
				, choice.label
				, chosen ? OverlayButtonKind::PRIMARY : OverlayButtonKind::SECONDARY
				, ImVec2(innerWidth, OverlayButtonHeight())
				, choice.detail[0] ? choice.detail : 0
			)) {
				xlln_overlay_scale = choice.scale;
				XllnConfig(true);
			}
		}

		// Read from the face that was built, so the number is the one on screen and not a repeat of
		// the size asked for.
		char sizeNote[64];
		snprintf(sizeNote, sizeof(sizeNote), "Body text %.0f px on this display", overlay_fonts.reg13->FontSize);
		ImGui::PushFont(overlay_fonts.reg12);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
		ImGui::TextUnformatted(sizeNote);
		ImGui::PopStyleColor();
		ImGui::PopFont();
	}
	OverlayCardEnd(sizeCardWidth, cardHeight);

	ImGui::SameLine(0.f, columnGap);

	OverlayCardBegin("##xlln_settings_toasts", toastCardWidth);
	{
		OverlayEyebrowLabel("NOTIFICATION POSITION");
		float innerWidth = toastCardWidth - OverlayPx(32.f);
		bool automaticChosen = xlln_overlay_toast_position == 0xFF;
		if (OverlayButton(
			"##xlln_settings_toast_auto"
			, "Automatic"
			, automaticChosen ? OverlayButtonKind::PRIMARY : OverlayButtonKind::SECONDARY
			, ImVec2(innerWidth, OverlayButtonHeight())
			, "where the game asks"
		)) {
			xlln_overlay_toast_position = 0xFF;
			XllnConfig(true);
		}

		// A three by three grid of the corners and edges. Each cell shows a position by its place in
		// the grid, so no cell needs a label and there is nothing here to translate.
		static const uint8_t toastPositions[3][3] = {
			{ XNOTIFYUI_POS_TOPLEFT, XNOTIFYUI_POS_TOPCENTER, XNOTIFYUI_POS_TOPRIGHT },
			{ XNOTIFYUI_POS_CENTERLEFT, XNOTIFYUI_POS_CENTER, XNOTIFYUI_POS_CENTERRIGHT },
			{ XNOTIFYUI_POS_BOTTOMLEFT, XNOTIFYUI_POS_BOTTOMCENTER, XNOTIFYUI_POS_BOTTOMRIGHT },
		};
		for (int iRow = 0; iRow < 3; iRow++) {
			for (int iColumn = 0; iColumn < 3; iColumn++) {
				uint8_t position = toastPositions[iRow][iColumn];
				bool chosen = !automaticChosen && (xlln_overlay_toast_position & XNOTIFYUI_POS_MASK) == position;
				char buttonId[48];
				snprintf(buttonId, sizeof(buttonId), "##xlln_settings_toast_%d_%d", iRow, iColumn);
				if (iColumn) {
					ImGui::SameLine(0.f, toastCellGap);
				}
				else {
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (innerWidth - 3.f * toastCellSize - 2.f * toastCellGap) * 0.5f);
				}
				if (OverlayButton(
					buttonId
					, ""
					, chosen ? OverlayButtonKind::PRIMARY : OverlayButtonKind::SECONDARY
					, ImVec2(toastCellSize, toastCellSize)
				)) {
					xlln_overlay_toast_position = position;
					XllnConfig(true);
				}
			}
		}
	}
	OverlayCardEnd(toastCardWidth, cardHeight);

	ImGui::Dummy(ImVec2(0.f, OverlayPx(10.f)));
	// The only place a player learns whether XLLN found the controller.
	const char* gamepadNote = "No gamepad found. The overlay takes the keyboard and the mouse.";
	if (OverlayGamepadIsConnected()) {
		gamepadNote = OverlayGamepadHasGuideButton()
			? "Gamepad ready. The guide button opens this overlay, and so do View and Menu together."
			: "Gamepad ready. View and Menu together open this overlay.";
	}
	ImGui::PushFont(overlay_fonts.reg12);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
	ImGui::TextUnformatted(gamepadNote);
	ImGui::PopStyleColor();
	ImGui::PopFont();

	ImGui::Dummy(ImVec2(0.f, OverlayPx(6.f)));
	ImGui::PushFont(overlay_fonts.reg12);
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_FAINT));
	ImGui::TextUnformatted("Whether the overlay runs at all is set in the XLLN config file, because a switch turned off here could not be turned back on here.");
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

static void OverlayBuildPanelHome()
{
	float availableWidth = ImGui::GetContentRegionAvail().x;
	float columnGap = OverlayPx(12.f);
	float nowPlayingWidth = (availableWidth - columnGap) * 0.575f;
	float quickActionsWidth = availableWidth - columnGap - nowPlayingWidth;

	uint32_t remotePlayerCount = 0;
	EnterCriticalSection(&xlln_critsec_friends);
	remotePlayerCount = (uint32_t)xlln_remote_user_presence.size();
	LeaveCriticalSection(&xlln_critsec_friends);

	char signedInUsername[XUSER_NAME_SIZE] = {0};
	bool signedInToLive = false;
	bool onlineEnabled = false;
	bool anySignedIn = OverlaySignedInUser(signedInUsername, sizeof(signedInUsername), &signedInToLive, &onlineEnabled);

	// Both grid cards share the taller height.
	float itemSpacingY = ImGui::GetStyle().ItemSpacing.y;
	int quickButtonCount = overlay_pending_invites.empty() ? 2 : 3;
	float gridCardHeight = OverlayPx(28.f)
		+ overlay_fonts.semi11->FontSize + OverlayPx(4.f)
		+ quickButtonCount * (itemSpacingY + OverlayButtonHeight());

	// NOW PLAYING card.
	OverlayCardBegin("##xlln_home_nowplaying", nowPlayingWidth);
	{
		float innerWidth = nowPlayingWidth - OverlayPx(32.f);
		OverlayEyebrowLabel("NOW PLAYING");
		ImGui::PushFont(overlay_fonts.bold17);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + innerWidth);
		ImGui::TextUnformatted(overlay_title_text);
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
		char titleIdText[32];
		snprintf(titleIdText, sizeof(titleIdText), "Title ID %08X", xlive_title_id);
		ImGui::PushFont(overlay_fonts.reg12);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
		ImGui::TextUnformatted(titleIdText);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::Dummy(ImVec2(0.f, OverlayPx(6.f)));
		if (anySignedIn) {
			if (onlineEnabled) {
				OverlayChip("Online", OVERLAY_COL_GREEN, OverlayColorWithAlpha(OVERLAY_COL_GREEN, 102), OVERLAY_COL_GREEN);
			}
			else {
				OverlayChip(signedInToLive ? "Offline" : "Local profile", OVERLAY_COL_TEXT_DIM, OVERLAY_COL_BORDER_HI, signedInToLive ? OVERLAY_COL_TEXT_DIM : 0);
			}
		}
		else {
			OverlayChip("Signed out", OVERLAY_COL_TEXT_DIM, OVERLAY_COL_BORDER_HI, 0);
		}
		ImGui::SameLine(0.f, OverlayPx(8.f));
		char playersChipText[32];
		snprintf(playersChipText, sizeof(playersChipText), "%u player%s online", remotePlayerCount, remotePlayerCount == 1 ? "" : "s");
		OverlayChip(playersChipText, OVERLAY_COL_TEXT_DIM, OVERLAY_COL_BORDER_HI, 0);
		if (!overlay_pending_invites.empty()) {
			ImGui::SameLine(0.f, OverlayPx(8.f));
			char invitesChipText[32];
			snprintf(invitesChipText, sizeof(invitesChipText), "%u invite%s pending", (uint32_t)overlay_pending_invites.size(), overlay_pending_invites.size() == 1 ? "" : "s");
			OverlayChip(invitesChipText, OVERLAY_COL_AMBER, OverlayColorWithAlpha(OVERLAY_COL_AMBER, 102), OVERLAY_COL_AMBER);
		}
	}
	float nowPlayingHeight = OverlayCardEnd(nowPlayingWidth, gridCardHeight);

	ImGui::SameLine(0.f, columnGap);

	// QUICK ACTIONS card.
	OverlayCardBegin("##xlln_home_quick", quickActionsWidth);
	{
		float buttonWidth = quickActionsWidth - OverlayPx(32.f);
		OverlayEyebrowLabel("QUICK ACTIONS");
		if (!overlay_pending_invites.empty()) {
			char pendingHint[24];
			snprintf(pendingHint, sizeof(pendingHint), "%u pending", (uint32_t)overlay_pending_invites.size());
			if (OverlayButton("##xlln_qa_invites", "View game invite", OverlayButtonKind::PRIMARY, ImVec2(buttonWidth, 0.f), pendingHint)) {
				overlay_invite_modal_index = 0;
			}
		}
		if (OverlayButton("##xlln_qa_desktop", "Open XLLN desktop window", OverlayButtonKind::SECONDARY, ImVec2(buttonWidth, 0.f), "accounts & more")) {
			XllnShowWindow(XllnShowType::MAIN_SHOW);
		}
		if (OverlayButton("##xlln_qa_close", "Close guide", OverlayButtonKind::SECONDARY, ImVec2(buttonWidth, 0.f), OverlayGuideKeyName())) {
			OverlayGuideRequestClose();
		}
	}
	OverlayCardEnd(quickActionsWidth, nowPlayingHeight);
}

// Sends one invite from the row buttons and toasts the outcome. can_invite/reason come
// from XllnSessionCanInvite, evaluated once per frame before the row loop.
static void OverlayRowSendInvite(uint32_t inviter_user_index, XUID invitee_xuid, bool can_invite, const char* cannot_invite_reason)
{
	if (!can_invite) {
		OverlayEventQueuePush(OverlayEventType::GENERIC, "Can't invite right now", cannot_invite_reason);
		return;
	}
	size_t sentCount = 0;
	XllnFriendsSendInvite(inviter_user_index, 1, &invitee_xuid, 0, &sentCount);
	if (sentCount) {
		OverlayEventQueuePush(OverlayEventType::GENERIC, "Invite sent", "Waiting for a response");
	}
	else {
		OverlayEventQueuePush(OverlayEventType::GENERIC, "Invite not sent", "They appear to be offline");
	}
}

static void OverlayBuildPanelPlayers()
{
	// Actions act for the first signed-in local user.
	uint32_t inviterUserIndex = OverlayFirstSignedInUserIndex();

	struct OVERLAY_REMOTE_PLAYER {
		XUID xuid = 0;
		bool isFriend = false;
		std::string username;
		std::string context;
	};
	std::vector<OVERLAY_REMOTE_PLAYER> remotePlayers;
	EnterCriticalSection(&xlln_critsec_friends);
	for (auto const &itrPresence : xlln_remote_user_presence) {
		OVERLAY_REMOTE_PLAYER player;
		player.xuid = itrPresence.first;
		player.isFriend = inviterUserIndex != XLIVE_LOCAL_USER_COUNT && xlln_friends[inviterUserIndex].count(itrPresence.first) != 0;
		player.username = itrPresence.second.username[0] ? itrPresence.second.username : "Player";
		if (itrPresence.second.titleName[0] && itrPresence.second.richPresence[0]) {
			player.context = std::string(itrPresence.second.titleName) + " \xC2\xB7 " + itrPresence.second.richPresence;
		}
		else if (itrPresence.second.titleName[0]) {
			player.context = itrPresence.second.titleName;
		}
		else {
			player.context = "Online";
		}
		remotePlayers.push_back(player);
	}
	LeaveCriticalSection(&xlln_critsec_friends);

	if (remotePlayers.empty()) {
		OverlayPanelEmptyState("Nobody discovered yet. Players appear here as XLLN instances announce themselves on the network.");
		return;
	}

	char cannotInviteReason[64];
	bool canInvite = XllnSessionCanInvite(cannotInviteReason, sizeof(cannotInviteReason));

	for (size_t iPlayer = 0; iPlayer < remotePlayers.size(); iPlayer++) {
		OVERLAY_REMOTE_PLAYER &player = remotePlayers[iPlayer];
		char rowId[24];
		snprintf(rowId, sizeof(rowId), "player_%u", (uint32_t)iPlayer);
		OverlayRowBegin(rowId);
		OverlayAvatarItem(player.username.c_str(), 36.f, false, true, false);
		ImGui::SameLine(0.f, OverlayPx(12.f));
		OverlayIdentityText(player.username.c_str(), player.context.c_str(), OVERLAY_COL_GREEN, 0);
		if (inviterUserIndex != XLIVE_LOCAL_USER_COUNT) {
			// Right-aligned action cluster; every button needs an explicit position because
			// SameLine alone would snap it back up to the row's text line.
			float actionsY = OverlayRowOrigin().y + OverlayPx(9.f) + (OverlayPx(36.f) - OverlayButtonHeight()) * 0.5f;
			float cursorRight = OverlayRowOrigin().x + OverlayRowWidth() - OverlayPx(10.f);

			float inviteButtonWidth = OverlayButtonWidth("Invite");
			cursorRight -= inviteButtonWidth;
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(cursorRight, actionsY));
			if (OverlayButton("##invite", "Invite", canInvite ? OverlayButtonKind::PRIMARY : OverlayButtonKind::SECONDARY)) {
				OverlayRowSendInvite(inviterUserIndex, player.xuid, canInvite, cannotInviteReason);
			}
			cursorRight -= OverlayPx(6.f);

			const char* friendButtonText = player.isFriend ? "Unfriend" : "Add friend";
			float friendButtonWidth = OverlayButtonWidth(friendButtonText);
			cursorRight -= friendButtonWidth;
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(cursorRight, actionsY));
			if (OverlayButton("##friend", friendButtonText, OverlayButtonKind::SECONDARY)) {
				if (player.isFriend) {
					XllnFriendsRemove(inviterUserIndex, player.xuid);
				}
				else {
					XllnFriendsAdd(inviterUserIndex, player.xuid, player.username.c_str());
				}
			}
		}
		OverlayRowEnd();
	}
}

// Arms the same direct-ip join mechanism invite-accept uses, from the freshest advertise
// snapshot of the friend's session. Returns false when the session is gone or refuses joins.
static bool OverlayArmJoinSession(uint32_t local_user_index, XUID remote_xuid, uint32_t remote_instance_id, uint32_t remote_title_id)
{
	bool armed = false;

	EnterCriticalSection(&xlln_critsec_liveoverlan_sessions);

	do {
		LIVE_SESSION* liveSession = LiveOverLanFindRemoteXSessionByInstanceId(remote_instance_id);
		if (!liveSession) {
			auto const &itrRemoteSessionXLocator = liveoverlan_remote_sessions_xlocator.find(remote_instance_id);
			if (itrRemoteSessionXLocator != liveoverlan_remote_sessions_xlocator.end()) {
				liveSession = itrRemoteSessionXLocator->second->liveSession;
			}
		}
		if (!liveSession || !LiveOverLanIsSessionJoinable(liveSession)) {
			break;
		}

		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
			, "%s arming a join to instance (0x%08x) session (0x%016I64x) flags (0x%08x) public (%zu/%zu) private (%zu/%zu)."
			, __func__
			, liveSession->instanceId
			, *(uint64_t*)&liveSession->xnkid
			, liveSession->sessionFlags
			, liveSession->slotsPublicFilledCount
			, liveSession->slotsPublicMaxCount
			, liveSession->slotsPrivateFilledCount
			, liveSession->slotsPrivateMaxCount
		);

		// Same fill as the invite-accept path so XInviteGetAcceptedInfo and a subsequent
		// XSessionSearchByID find the host's session.
		xlln_direct_ip_connect.joinRequestSignature = 0;
		xlln_direct_ip_connect.timeoutAt = 0;
		xlln_direct_ip_connect.localPlayerId = local_user_index;
		xlln_direct_ip_connect.remoteInstanceId = liveSession->instanceId;
		xlln_direct_ip_connect.remoteXuid = remote_xuid;
		xlln_direct_ip_connect.remoteTitleId = remote_title_id;
		xlln_direct_ip_connect.remoteSessionId = liveSession->xnkid;
		xlln_direct_ip_connect.remoteKeyExchangeKey = liveSession->xnkey;

		XLiveNotifyAddEvent(XN_LIVE_INVITE_ACCEPTED, local_user_index);

		armed = true;
	} while (0);

	LeaveCriticalSection(&xlln_critsec_liveoverlan_sessions);

	return armed;
}

static void OverlayBuildPanelFriends()
{
	uint32_t localUserIndex = OverlayFirstSignedInUserIndex();
	if (localUserIndex == XLIVE_LOCAL_USER_COUNT) {
		OverlayPanelEmptyState("Sign in a profile to see friends.");
		return;
	}

	struct OVERLAY_FRIEND_ROW {
		XUID xuid = 0;
		bool online = false;
		bool addedMe = false;
		uint32_t instanceId = 0;
		uint32_t titleId = 0;
		std::string username;
		std::string context;
	};
	std::vector<OVERLAY_FRIEND_ROW> friendRows;
	{
		EnterCriticalSection(&xlln_critsec_friends);

		// "Added you" rows first: the only group waiting on an action.
		for (auto const &itrAddedMe : xlln_friends_added_me[localUserIndex]) {
			OVERLAY_FRIEND_ROW row;
			row.xuid = itrAddedMe.first;
			row.addedMe = true;
			row.username = itrAddedMe.second.username[0] ? itrAddedMe.second.username : "Player";
			row.context = "Added you as a friend";
			auto const &itrPresence = xlln_remote_user_presence.find(itrAddedMe.first);
			if (itrPresence != xlln_remote_user_presence.end()) {
				row.online = true;
				row.instanceId = itrPresence->second.instanceId;
				row.titleId = itrPresence->second.titleId;
			}
			friendRows.push_back(row);
		}

		// Online friends with their presence line, then offline friends greyed
		// (the persisted friends.ini list keeps them visible).
		for (int pass = 0; pass < 2; pass++) {
			for (auto const &itrFriend : xlln_friends[localUserIndex]) {
				auto const &itrPresence = xlln_remote_user_presence.find(itrFriend.first);
				bool online = itrPresence != xlln_remote_user_presence.end();
				if ((pass == 0) != online) {
					continue;
				}
				OVERLAY_FRIEND_ROW row;
				row.xuid = itrFriend.first;
				row.online = online;
				row.username = itrFriend.second.username[0] ? itrFriend.second.username : "Player";
				if (online) {
					row.instanceId = itrPresence->second.instanceId;
					row.titleId = itrPresence->second.titleId;
					if (itrPresence->second.username[0]) {
						row.username = itrPresence->second.username;
					}
					if (itrPresence->second.titleName[0] && itrPresence->second.richPresence[0]) {
						row.context = std::string(itrPresence->second.titleName) + " \xC2\xB7 " + itrPresence->second.richPresence;
					}
					else if (itrPresence->second.titleName[0]) {
						row.context = itrPresence->second.titleName;
					}
					else {
						row.context = "Online";
					}
				}
				else {
					row.context = "Offline";
				}
				friendRows.push_back(row);
			}
		}

		LeaveCriticalSection(&xlln_critsec_friends);
	}

	if (friendRows.empty()) {
		OverlayPanelEmptyState("No friends yet. Add players from the Players tab; friends stay listed here even while offline.");
		return;
	}

	// Whether each friend's advertised session can be joined right now (same title only).
	// Snapshotted before drawing so the session lock is not held during layout.
	std::vector<uint8_t> rowJoinable(friendRows.size(), 0);
	{
		EnterCriticalSection(&xlln_critsec_liveoverlan_sessions);
		for (size_t iRow = 0; iRow < friendRows.size(); iRow++) {
			if (!friendRows[iRow].online || friendRows[iRow].addedMe || friendRows[iRow].titleId != xlive_title_id) {
				continue;
			}
			LIVE_SESSION* liveSession = LiveOverLanFindRemoteXSessionByInstanceId(friendRows[iRow].instanceId);
			if (liveSession && LiveOverLanIsSessionJoinable(liveSession)) {
				rowJoinable[iRow] = 1;
			}
		}
		LeaveCriticalSection(&xlln_critsec_liveoverlan_sessions);
	}

	char cannotInviteReason[64];
	bool canInvite = XllnSessionCanInvite(cannotInviteReason, sizeof(cannotInviteReason));

	for (size_t iRow = 0; iRow < friendRows.size(); iRow++) {
		OVERLAY_FRIEND_ROW &row = friendRows[iRow];
		char rowId[24];
		snprintf(rowId, sizeof(rowId), "friend_%u", (uint32_t)iRow);
		OverlayRowBegin(rowId);
		OverlayAvatarItem(row.username.c_str(), 36.f, false, row.online, !row.online);
		ImGui::SameLine(0.f, OverlayPx(12.f));
		OverlayIdentityText(
			row.username.c_str()
			, row.context.c_str()
			, row.addedMe ? OVERLAY_COL_AMBER : (row.online ? OVERLAY_COL_GREEN : OVERLAY_COL_TEXT_DIM)
			, 0
		);

		// Right-aligned action cluster; every button needs an explicit position because
		// SameLine alone would snap it back up to the row's text line.
		float actionsY = OverlayRowOrigin().y + OverlayPx(9.f) + (OverlayPx(36.f) - OverlayButtonHeight()) * 0.5f;
		float cursorRight = OverlayRowOrigin().x + OverlayRowWidth() - OverlayPx(10.f);

		if (row.addedMe) {
			float addBackWidth = OverlayButtonWidth("Add back");
			cursorRight -= addBackWidth;
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(cursorRight, actionsY));
			if (OverlayButton("##addback", "Add back", OverlayButtonKind::PRIMARY)) {
				XllnFriendsAdd(localUserIndex, row.xuid, row.username.c_str());
			}
		}
		else {
			bool joinShown = row.online && rowJoinable[iRow];
			if (joinShown) {
				float joinWidth = OverlayButtonWidth("Join");
				cursorRight -= joinWidth;
				ImGui::SameLine();
				ImGui::SetCursorScreenPos(ImVec2(cursorRight, actionsY));
				if (OverlayButton("##join", "Join", OverlayButtonKind::PRIMARY)) {
					if (OverlayArmJoinSession(localUserIndex, row.xuid, row.instanceId, row.titleId)) {
						OverlayEventQueuePush(OverlayEventType::JOINING_SESSION, row.username.c_str(), "Joining their session");
						OverlayGuideRequestClose();
					}
					else {
						OverlayEventQueuePush(OverlayEventType::GENERIC, row.username.c_str(), "Their session is full or has closed");
					}
				}
				cursorRight -= OverlayPx(6.f);
			}
			if (row.online) {
				float inviteWidth = OverlayButtonWidth("Invite");
				cursorRight -= inviteWidth;
				ImGui::SameLine();
				ImGui::SetCursorScreenPos(ImVec2(cursorRight, actionsY));
				// One primary action per row: Join has priority when it is available.
				OverlayButtonKind inviteKind = (canInvite && !joinShown) ? OverlayButtonKind::PRIMARY : OverlayButtonKind::SECONDARY;
				if (OverlayButton("##invite", "Invite", inviteKind)) {
					OverlayRowSendInvite(localUserIndex, row.xuid, canInvite, cannotInviteReason);
				}
				cursorRight -= OverlayPx(6.f);
			}
			float removeWidth = OverlayButtonWidth("Remove");
			cursorRight -= removeWidth;
			ImGui::SameLine();
			ImGui::SetCursorScreenPos(ImVec2(cursorRight, actionsY));
			if (OverlayButton("##remove", "Remove", OverlayButtonKind::SECONDARY)) {
				XllnFriendsRemove(localUserIndex, row.xuid);
			}
		}
		OverlayRowEnd();
	}
}

static void OverlayResolveInvite(int invite_index, bool accepted);

// --- achievements panel -----------------------------------------------------------

struct OVERLAY_ACHIEVEMENT_ROW {
	uint32_t id = 0;
	uint32_t imageId = 0;
	uint32_t cred = 0;
	bool unlocked = false;
	bool secret = false;
	std::string label;
	std::string description;
	std::string unlockedDate;
	std::string pack;
};
static std::vector<OVERLAY_ACHIEVEMENT_ROW> overlay_achievement_rows;
static uint32_t overlay_achievements_unlocked_count = 0;
static uint32_t overlay_achievements_total_cred = 0;
static uint32_t overlay_achievements_unlocked_cred = 0;

static std::string OverlayWideToUtf8(const wchar_t* wide_text)
{
	if (!wide_text || !wide_text[0]) {
		return std::string();
	}
	int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide_text, -1, NULL, 0, NULL, NULL);
	if (utf8Length <= 1) {
		return std::string();
	}
	std::string utf8((size_t)utf8Length - 1, 0);
	WideCharToMultiByte(CP_UTF8, 0, wide_text, -1, &utf8[0], utf8Length, NULL, NULL);
	return utf8;
}

static void OverlayEnsureAchievementRows()
{
	if (!overlay_achievements_dirty.exchange(false)) {
		return;
	}

	overlay_achievement_rows.clear();
	overlay_achievements_unlocked_count = 0;
	overlay_achievements_total_cred = 0;
	overlay_achievements_unlocked_cred = 0;
	overlay_gamerscore_total = 0;

	uint32_t userIndex = OverlayFirstSignedInUserIndex();

	std::vector<XLLN_XACHIEVEMENT_DETAILS_COW> unlockRecords;
	if (userIndex != XLIVE_LOCAL_USER_COUNT) {
		XllnAchievementsReadUnlocked(userIndex, &unlockRecords);
		overlay_gamerscore_total = XUserGetGamerscore(userIndex);
	}

	for (auto const &achievementDetail : xlive_achievement_details) {
		OVERLAY_ACHIEVEMENT_ROW row;
		row.id = achievementDetail.dwId;
		row.imageId = achievementDetail.dwImageId;
		row.cred = achievementDetail.dwCred;
		overlay_achievements_total_cred += achievementDetail.dwCred;

		const XLLN_XACHIEVEMENT_DETAILS_COW* unlockRecord = 0;
		for (auto const &unlockedCow : unlockRecords) {
			if (unlockedCow.dwAchievementId == achievementDetail.dwId) {
				unlockRecord = &unlockedCow;
				break;
			}
		}
		row.unlocked = unlockRecord != 0;
		if (row.unlocked) {
			overlay_achievements_unlocked_count++;
			overlay_achievements_unlocked_cred += achievementDetail.dwCred;
		}
		row.secret = !row.unlocked && !AchievementShowUnachieved(achievementDetail.dwFlags);
		if (row.secret) {
			row.label = "Secret achievement";
			row.description = "Keep playing to reveal it.";
		}
		else {
			row.label = OverlayWideToUtf8(achievementDetail.pwszLabel);
			row.description = OverlayWideToUtf8(row.unlocked ? achievementDetail.pwszDescription : achievementDetail.pwszUnachieved);
		}
		const char* packName = XllnSpaAchievementPackName((uint16_t)achievementDetail.dwId);
		if (packName) {
			row.pack = packName;
			std::string packNameUpper = row.pack;
			for (char &packNameChar : packNameUpper) {
				if (packNameChar >= 'a' && packNameChar <= 'z') {
					packNameChar -= 'a' - 'A';
				}
			}
			if (packNameUpper.find("DLC") == std::string::npos) {
				row.pack += " DLC";
			}
		}
		if (unlockRecord) {
			FILETIME localFileTime;
			SYSTEMTIME systemTime;
			if (FileTimeToLocalFileTime(&unlockRecord->ftAchieved, &localFileTime) && FileTimeToSystemTime(&localFileTime, &systemTime)) {
				static const char* monthNames[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
				char dateText[40];
				snprintf(dateText, sizeof(dateText), "Unlocked %s \xC2\xB7 %u %s %u"
					, AchievementEarnedOnline(unlockRecord->dwFlags) ? "online" : "offline"
					, systemTime.wDay
					, monthNames[(systemTime.wMonth - 1) % 12]
					, systemTime.wYear
				);
				row.unlockedDate = dateText;
			}
		}
		overlay_achievement_rows.push_back(row);
	}
}

static void OverlayBuildPanelAchievements()
{
	if (OverlayFirstSignedInUserIndex() == XLIVE_LOCAL_USER_COUNT) {
		OverlayPanelEmptyState("Sign in a profile to see achievements.");
		return;
	}
	if (overlay_achievement_rows.empty()) {
		OverlayPanelEmptyState("No achievement data. The title exposes no SPA resource, so achievements cannot be listed.");
		return;
	}
	for (size_t iRow = 0; iRow < overlay_achievement_rows.size(); iRow++) {
		const OVERLAY_ACHIEVEMENT_ROW &row = overlay_achievement_rows[iRow];
		char rowId[32];
		snprintf(rowId, sizeof(rowId), "achievement_%u", row.id);
		OverlayRowBegin(rowId);

		// Art tile, dimmed until earned; the gradient avatar stands in when art is missing.
		float artSize = OverlayPx(36.f);
		ImVec2 artPosition = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(artSize, artSize));
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImTextureID achievementArt = row.secret ? 0 : OverlayTextureForSpaImage(row.imageId);
		if (achievementArt) {
			drawList->AddImageRounded(
				achievementArt
				, artPosition
				, ImVec2(artPosition.x + artSize, artPosition.y + artSize)
				, ImVec2(0.f, 0.f)
				, ImVec2(1.f, 1.f)
				, OverlayAlpha(row.unlocked ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 96))
				, OverlayPx(8.f)
			);
		}
		else {
			OverlayDrawAvatar(drawList, artPosition, artSize, row.label.c_str(), false, false, !row.unlocked);
		}

		ImGui::SameLine(0.f, OverlayPx(12.f));
		OverlayIdentityText(
			row.label.c_str()
			, row.description.c_str()
			, row.unlocked ? OVERLAY_COL_TEXT_DIM : OVERLAY_COL_TEXT_FAINT
			, 0
		);

		// Right column: the cred value, with the unlock date underneath once earned.
		char credText[16];
		snprintf(credText, sizeof(credText), "%uG", row.cred);
		ImVec2 credSize = overlay_fonts.semi13->CalcTextSizeA(overlay_fonts.semi13->FontSize, FLT_MAX, 0.f, credText);
		float rowRightEdge = OverlayRowOrigin().x + OverlayRowWidth() - OverlayPx(12.f);
		drawList->AddText(
			overlay_fonts.semi13
			, overlay_fonts.semi13->FontSize
			, ImVec2(rowRightEdge - credSize.x, OverlayRowOrigin().y + OverlayPx(row.unlockedDate.empty() ? 18.f : 11.f))
			, OverlayAlpha(row.unlocked ? OVERLAY_COL_AMBER : OVERLAY_COL_TEXT_FAINT)
			, credText
		);
		if (!row.unlockedDate.empty()) {
			ImVec2 dateSize = overlay_fonts.reg12->CalcTextSizeA(overlay_fonts.reg12->FontSize, FLT_MAX, 0.f, row.unlockedDate.c_str());
			drawList->AddText(
				overlay_fonts.reg12
				, overlay_fonts.reg12->FontSize
				, ImVec2(rowRightEdge - dateSize.x, OverlayRowOrigin().y + OverlayPx(28.f))
				, OverlayAlpha(OVERLAY_COL_TEXT_FAINT)
				, row.unlockedDate.c_str()
			);
		}

		if (!row.pack.empty()) {
			ImVec2 textMin = ImGui::GetItemRectMin();
			float labelWidth = overlay_fonts.bold13->CalcTextSizeA(overlay_fonts.bold13->FontSize, FLT_MAX, 0.f, row.label.c_str()).x;
			ImVec2 tagTextSize = overlay_fonts.semi11->CalcTextSizeA(overlay_fonts.semi11->FontSize, FLT_MAX, 0.f, row.pack.c_str());
			float tagHeight = tagTextSize.y + OverlayPx(4.f);
			ImVec2 tagMin(textMin.x + labelWidth + OverlayPx(8.f), textMin.y + (overlay_fonts.bold13->FontSize - tagHeight) * 0.5f);
			ImVec2 tagMax(tagMin.x + tagTextSize.x + OverlayPx(12.f), tagMin.y + tagHeight);
			if (tagMax.x <= rowRightEdge - credSize.x - OverlayPx(8.f)) {
				drawList->AddRectFilled(tagMin, tagMax, OverlayAlpha(OVERLAY_COL_WASH_A(13)), OverlayPx(6.f));
				drawList->AddRect(tagMin, tagMax, OverlayAlpha(OVERLAY_COL_BORDER), OverlayPx(6.f));
				drawList->AddText(overlay_fonts.semi11, overlay_fonts.semi11->FontSize, ImVec2(tagMin.x + OverlayPx(6.f), tagMin.y + OverlayPx(2.f)), OverlayAlpha(OVERLAY_COL_TEXT_DIM), row.pack.c_str());
			}
		}

		OverlayRowEnd();
	}
}

static void OverlayBuildPanelInvites()
{
	if (overlay_pending_invites.empty()) {
		OverlayPanelEmptyState("No pending invites. Game invites from other XLLN players appear here.");
		return;
	}
	int resolveIndex = -1;
	bool resolveAccepted = false;
	for (size_t iInvite = 0; iInvite < overlay_pending_invites.size(); iInvite++) {
		const OVERLAY_EVENT &invite = overlay_pending_invites[iInvite];
		char rowId[24];
		snprintf(rowId, sizeof(rowId), "invite_%u", (uint32_t)iInvite);
		OverlayRowBegin(rowId);
		OverlayAvatarItem(invite.title, 36.f, false, true, false);
		ImGui::SameLine(0.f, OverlayPx(12.f));
		// detail carries the inviter's message when they wrote one, the plain verb otherwise.
		OverlayIdentityText(invite.title, invite.detail[0] ? invite.detail : "Invited you to play", OVERLAY_COL_TEXT_DIM, 0);
		// Only the running Title can act on an invite, so another game's is shown without the
		// action. acceptWidth carries the gap with it.
		const bool inviteIsForThisTitle = invite.titleId == xlive_title_id;
		float acceptWidth = inviteIsForThisTitle ? OverlayButtonWidth("Accept & join") + OverlayPx(6.f) : 0.f;
		float declineWidth = OverlayButtonWidth("Decline");
		float actionsWidth = acceptWidth + declineWidth;
		// Both buttons get explicit positions: SameLine alone would snap the second
		// one back up to the row's text line.
		float actionsX = OverlayRowOrigin().x + OverlayRowWidth() - OverlayPx(10.f) - actionsWidth;
		float actionsY = OverlayRowOrigin().y + OverlayPx(9.f) + (OverlayPx(36.f) - OverlayButtonHeight()) * 0.5f;
		ImGui::SameLine();
		if (inviteIsForThisTitle) {
			ImGui::SetCursorScreenPos(ImVec2(actionsX, actionsY));
			if (OverlayButton("##accept", "Accept & join", OverlayButtonKind::PRIMARY)) {
				resolveIndex = (int)iInvite;
				resolveAccepted = true;
			}
			ImGui::SameLine();
		}
		ImGui::SetCursorScreenPos(ImVec2(actionsX + acceptWidth, actionsY));
		if (OverlayButton("##decline", "Decline", OverlayButtonKind::SECONDARY)) {
			resolveIndex = (int)iInvite;
			resolveAccepted = false;
		}
		OverlayRowEnd();
	}
	if (resolveIndex >= 0) {
		OverlayResolveInvite(resolveIndex, resolveAccepted);
	}
}

static void OverlayPanelLegend()
{
	// Full-bleed hairline above the legend.
	ImGui::Dummy(ImVec2(0.f, OverlayPx(2.f)));
	ImVec2 windowPosition = ImGui::GetWindowPos();
	float lineY = ImGui::GetCursorScreenPos().y;
	ImGui::GetWindowDrawList()->AddLine(
		ImVec2(windowPosition.x, lineY)
		, ImVec2(windowPosition.x + ImGui::GetWindowWidth(), lineY)
		, OverlayAlpha(OVERLAY_COL_BORDER)
	);
	ImGui::Dummy(ImVec2(0.f, OverlayPx(6.f)));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + OverlayPx(8.f));

	struct _OVERLAY_LEGEND_ENTRY {
		const char* keycap;
		const char* action;
	};
	// The hints follow whichever device answered last; nothing else names the panel and close keys.
	for (int iKey = ImGuiKey_NamedKey_BEGIN; iKey < ImGuiKey_NamedKey_END; iKey++) {
		if (ImGui::IsKeyPressed((ImGuiKey)iKey, false)) {
			overlay_legend_gamepad = ImGui::IsGamepadKey((ImGuiKey)iKey);
		}
	}
	_OVERLAY_LEGEND_ENTRY legendEntries[4] = {
		{ "Ctrl+Tab", "Panel" },
		{ "\xE2\x86\x91 \xE2\x86\x93", "Navigate" },
		{ "Enter", "Select" },
		{ OverlayGuideKeyName(), "Close" },
	};
	if (overlay_legend_gamepad && OverlayGamepadIsConnected()) {
		legendEntries[0] = { "LB/RB", "Panel" };
		legendEntries[1] = { "D-pad", "Navigate" };
		legendEntries[2] = { "A", "Select" };
		legendEntries[3] = { "B", "Close" };
	}
	float keycapHeight = OverlayKeycapHeight(false);
	for (int iEntry = 0; iEntry < 4; iEntry++) {
		OverlayKeycapItem(legendEntries[iEntry].keycap, false);
		ImGui::SameLine(0.f, OverlayPx(6.f));
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (keycapHeight - overlay_fonts.reg12->FontSize) * 0.5f);
		ImGui::PushFont(overlay_fonts.reg12);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_FAINT));
		ImGui::TextUnformatted(legendEntries[iEntry].action);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		if (iEntry < 3) {
			ImGui::SameLine(0.f, OverlayPx(18.f));
		}
	}

	// Version + identity, right aligned.
	char signedInUsername[XUSER_NAME_SIZE] = {0};
	char legendIdentity[96];
	if (OverlaySignedInUser(signedInUsername, sizeof(signedInUsername), 0)) {
		snprintf(legendIdentity, sizeof(legendIdentity), "XLLN %s \xC2\xB7 signed in as %s", STRINGIZE(DLL_VERSION_MAJOR) "." STRINGIZE(DLL_VERSION_MINOR) "." STRINGIZE(DLL_VERSION_REVISION), signedInUsername);
	}
	else {
		snprintf(legendIdentity, sizeof(legendIdentity), "XLLN %s", STRINGIZE(DLL_VERSION_MAJOR) "." STRINGIZE(DLL_VERSION_MINOR) "." STRINGIZE(DLL_VERSION_REVISION));
	}
	ImGui::PushFont(overlay_fonts.reg12);
	float identityWidth = ImGui::CalcTextSize(legendIdentity).x;
	ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.f - identityWidth - OverlayPx(8.f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_FAINT));
	ImGui::TextUnformatted(legendIdentity);
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

// --- guide: invite modal -------------------------------------------------------------

static void OverlayBuildInviteModal()
{
	if (overlay_invite_modal_index < 0 || overlay_invite_modal_index >= (int)overlay_pending_invites.size()) {
		overlay_invite_modal_index = -1;
		return;
	}
	const OVERLAY_EVENT &invite = overlay_pending_invites[overlay_invite_modal_index];
	ImGuiIO &imguiIo = ImGui::GetIO();

	// A second dim layer. The guide already dims the game, this dims the guide too.
	ImGui::GetBackgroundDrawList()->AddRectFilled(
		ImVec2(0.f, 0.f)
		, imguiIo.DisplaySize
		, OverlayDimColor(0.55f * overlay_open_animation)
	);

	float modalWidth = OverlayPx(430.f);
	ImGui::SetNextWindowPos(ImVec2(imguiIo.DisplaySize.x * 0.5f, imguiIo.DisplaySize.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(modalWidth, 0.f), ImGuiCond_Always);
	ImGui::SetNextWindowFocus();
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, OverlayPx(18.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
	ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_BORDER_HI));
	if (ImGui::Begin(
		"##xlln_invite_modal"
		, 0
		, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
	)) {
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 windowPosition = ImGui::GetWindowPos();

		// Art header: gradient, eyebrow pill, game title.
		float artHeight = OverlayPx(110.f);
		ImVec2 artMin = windowPosition;
		ImVec2 artMax(windowPosition.x + modalWidth, windowPosition.y + artHeight);
		OverlayDrawRectHGradient3(
			drawList
			, artMin
			, artMax
			, IM_COL32(49, 20, 107, 255)
			, IM_COL32(122, 27, 77, 255)
			, IM_COL32(216, 85, 31, 255)
			, 0.55f
			, OverlayPx(18.f)
			, ImDrawFlags_RoundCornersTop
		);
		drawList->PushClipRect(artMin, artMax, true);
		for (int iHalo = 0; iHalo < 4; iHalo++) {
			drawList->AddCircleFilled(
				ImVec2(artMin.x + modalWidth * 0.82f, artMin.y + artHeight * 0.2f)
				, artHeight * (0.95f - iHalo * 0.2f)
				, OverlayAlpha(IM_COL32(255, 214, 140, 9 + iHalo * 5))
			);
		}
		drawList->PopClipRect();
		{
			const char* eyebrowText = "GAME INVITE";
			float eyebrowTracking = OverlayPx(2.2f);
			ImVec2 eyebrowSize = OverlayTrackedTextSize(overlay_fonts.semi11, eyebrowText, eyebrowTracking);
			ImVec2 eyebrowMin(artMin.x + OverlayPx(18.f), artMin.y + OverlayPx(12.f));
			ImVec2 eyebrowMax(eyebrowMin.x + eyebrowSize.x + OverlayPx(20.f), eyebrowMin.y + eyebrowSize.y + OverlayPx(6.f));
			drawList->AddRectFilled(eyebrowMin, eyebrowMax, OverlayAlpha(IM_COL32(0, 0, 0, 90)), (eyebrowMax.y - eyebrowMin.y) * 0.5f);
			OverlayDrawTrackedText(
				drawList
				, overlay_fonts.semi11
				, ImVec2(eyebrowMin.x + OverlayPx(10.f), eyebrowMin.y + OverlayPx(3.f))
				, IM_COL32(255, 217, 160, 255)
				, eyebrowText
				, eyebrowTracking
			);
		}
		{
			ImVec2 titlePosition(artMin.x + OverlayPx(18.f), artMax.y - OverlayPx(14.f) - overlay_fonts.bold22->FontSize);
			drawList->AddText(overlay_fonts.bold22, overlay_fonts.bold22->FontSize, ImVec2(titlePosition.x, titlePosition.y + OverlayPx(2.f)), OverlayAlpha(IM_COL32(0, 0, 0, 128)), overlay_title_text);
			drawList->AddText(overlay_fonts.bold22, overlay_fonts.bold22->FontSize, titlePosition, OverlayAlpha(IM_COL32(255, 255, 255, 255)), overlay_title_text);
		}
		ImGui::Dummy(ImVec2(modalWidth, artHeight));

		// Body.
		float bodyPadding = OverlayPx(18.f);
		ImGui::SetCursorScreenPos(ImVec2(windowPosition.x + bodyPadding, windowPosition.y + artHeight + OverlayPx(16.f)));
		ImGui::BeginGroup();
		OverlayAvatarItem(invite.title, 42.f, false, true, false);
		ImGui::SameLine(0.f, OverlayPx(12.f));
		ImGui::BeginGroup();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, OverlayPx(2.f)));
		ImGui::PushFont(overlay_fonts.bold16);
		ImGui::TextUnformatted(invite.title);
		ImGui::PopFont();
		ImGui::PushFont(overlay_fonts.reg12);
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
		ImGui::TextUnformatted("invited you to play");
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::PopStyleVar();
		ImGui::EndGroup();
		// The inviter's message, quoted, when they wrote one (detail otherwise carries the plain verb).
		if (invite.detail[0] && strcmp(invite.detail, "Invited you to play") != 0) {
			ImGui::Dummy(ImVec2(0.f, OverlayPx(6.f)));
			char quotedMessage[144];
			snprintf(quotedMessage, sizeof(quotedMessage), "\xE2\x80\x9C%s\xE2\x80\x9D", invite.detail);
			ImGui::PushFont(overlay_fonts.reg13);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + modalWidth - bodyPadding * 2.f);
			ImGui::TextUnformatted(quotedMessage);
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}
		ImGui::Dummy(ImVec2(0.f, OverlayPx(10.f)));

		// Only the running Title can act on an invite, so another game's is shown without the action.
		const bool inviteIsForThisTitle = invite.titleId == xlive_title_id;
		if (!inviteIsForThisTitle) {
			ImGui::PushFont(overlay_fonts.reg13);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(OVERLAY_COL_TEXT_DIM));
			ImGui::TextUnformatted("This invite is for a different game.");
			ImGui::PopStyleColor();
			ImGui::PopFont();
			ImGui::Dummy(ImVec2(0.f, OverlayPx(8.f)));
		}

		float buttonWidth = inviteIsForThisTitle
			? (modalWidth - bodyPadding * 2.f - OverlayPx(9.f)) * 0.5f
			: modalWidth - bodyPadding * 2.f;
		float buttonHeight = OverlayPx(38.f);
		if (inviteIsForThisTitle) {
			if (OverlayButton("##xlln_invite_accept", "Accept & join", OverlayButtonKind::PRIMARY, ImVec2(buttonWidth, buttonHeight))) {
				OverlayResolveInvite(overlay_invite_modal_index, true);
			}
			ImGui::SameLine(0.f, OverlayPx(9.f));
		}
		if (OverlayButton("##xlln_invite_decline", "Decline", OverlayButtonKind::SECONDARY, ImVec2(buttonWidth, buttonHeight))) {
			OverlayResolveInvite(overlay_invite_modal_index, false);
		}
		ImGui::EndGroup();
		ImGui::Dummy(ImVec2(0.f, OverlayPx(16.f)));

		ImVec2 windowSize = ImGui::GetWindowSize();
		OverlayDrawSoftShadow(
			ImGui::GetBackgroundDrawList()
			, windowPosition
			, ImVec2(windowPosition.x + windowSize.x, windowPosition.y + windowSize.y)
			, OverlayPx(18.f)
			, OverlayPx(12.f)
			, OverlayPx(26.f)
			, 20
		);
	}
	ImGui::End();
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(2);
}

// --- guide -------------------------------------------------------------------------

// Changes the panel from the shoulder buttons or Ctrl+Tab and closes the guide on B, unless another
// reader took that press (cancel_taken). Directional navigation cannot cross from the panel to the band.
static void OverlayGuideNavigation(bool cancel_taken)
{
	// Settings is not a tab; its cog is in the band, out of the pad's reach, so a pad opens it here.
	static const OverlayGuidePanel panelOrder[] = {
		OverlayGuidePanel::HOME
		, OverlayGuidePanel::FRIENDS
		, OverlayGuidePanel::PLAYERS
		, OverlayGuidePanel::INVITES
		, OverlayGuidePanel::ACHIEVEMENTS
		, OverlayGuidePanel::SETTINGS
	};
	const int panelCount = (int)(sizeof(panelOrder) / sizeof(panelOrder[0]));

	int step = 0;
	if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Tab)) {
		step = 1;
	}
	else if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab)) {
		step = -1;
	}
	if (step) {
		int panelIndex = 0;
		for (int iPanel = 0; iPanel < panelCount; iPanel++) {
			if (panelOrder[iPanel] == overlay_active_panel) {
				panelIndex = iPanel;
				break;
			}
		}
		overlay_active_panel = panelOrder[(panelIndex + step + panelCount) % panelCount];
		overlay_guide_focus_panel_pending = true;
	}

	// A click on the band or outside the guide leaves no window to navigate in; the first navigation
	// press after it puts the cursor back on the panel.
	if (!ImGui::GetIO().NavActive
		&& (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)
		|| ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)
		|| ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)
		|| ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)
		|| ImGui::IsKeyPressed(ImGuiKey_Tab))) {
		overlay_guide_focus_panel_pending = true;
	}

	// B closes the guide, unless an item is taking input: then B ends the edit, handled by the interface.
	if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsAnyItemActive() && !cancel_taken) {
		// An invite card draws over the guide, so B takes that away before it takes the guide.
		if (overlay_invite_modal_index >= 0) {
			overlay_invite_modal_index = -1;
		}
		else {
			OverlayGuideRequestClose();
		}
	}
}

static void OverlayBuildGuide()
{
	ImGuiIO &imguiIo = ImGui::GetIO();

	// Dimmed game behind the guide (the visual half of the XN_SYS_UI pause).
	if (overlay_open_animation > 0.f) {
		ImGui::GetBackgroundDrawList()->AddRectFilled(
			ImVec2(0.f, 0.f)
			, imguiIo.DisplaySize
			, OverlayDimColor(overlay_open_animation)
		);
	}

	// The Title asked for an answer, not for the guide, so the band and the panels are not drawn
	// until the request is finished.
	if (OverlayKeyboardIsOpen()) {
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, overlay_open_animation);
		overlay_widget_alpha = overlay_open_animation;
		OverlayKeyboardBuild();
		overlay_widget_alpha = 1.f;
		ImGui::PopStyleVar();
		if (overlay_guide_opened_for_keyboard && !OverlayKeyboardIsOpen()) {
			OverlayGuideRequestClose();
		}
		return;
	}

	OverlayGuideNavigation(OverlayThemeSecretUpdate());

	float bandWidth = OverlayPx(1060.f);
	if (bandWidth > imguiIo.DisplaySize.x - OverlayPx(32.f)) {
		bandWidth = imguiIo.DisplaySize.x - OverlayPx(32.f);
	}
	float bandX = (imguiIo.DisplaySize.x - bandWidth) * 0.5f;
	float bandY = OverlayPx(18.f) - (1.f - overlay_open_animation) * OverlayPx(24.f);

	// The band and the panels draw at a lower alpha while the invite modal has the focus.
	bool inviteModalOpen = overlay_invite_modal_index >= 0 && overlay_invite_modal_index < (int)overlay_pending_invites.size();
	float chromeAlpha = overlay_open_animation * (inviteModalOpen ? 0.55f : 1.f);
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, chromeAlpha);
	overlay_widget_alpha = chromeAlpha;

	// The band shows the gamerscore, so this runs for every panel.
	OverlayEnsureAchievementRows();
	float bandBottom = OverlayBuildBand(bandX, bandY, bandWidth);

	// Panel below the band.
	float panelMaxHeight = imguiIo.DisplaySize.y * 0.6f;
	ImGui::SetNextWindowPos(ImVec2(bandX, bandBottom + OverlayPx(12.f)), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(bandWidth, 0.f), ImGuiCond_Always);
	if (ImGui::Begin(
		"##xlln_guide_panel"
		, 0
		, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
	)) {
		if (overlay_palette.panelTop & IM_COL32_A_MASK) {
			// Inside the drawn border; the size is from the frame before, which shows only as a panel grows.
			ImVec2 panelPosition = ImGui::GetWindowPos();
			ImVec2 panelSize = ImGui::GetWindowSize();
			OverlayDrawRectVGradient(
				ImGui::GetWindowDrawList()
				, ImVec2(panelPosition.x + 1.f, panelPosition.y + 1.f)
				, ImVec2(panelPosition.x + panelSize.x - 1.f, panelPosition.y + panelSize.y - 1.f)
				, overlay_palette.panelTop
				, overlay_palette.panelBottom
				, OverlayPx(16.f) - 1.f
			);
		}
		char panelSubtitle[64];
		switch (overlay_active_panel) {
			case OverlayGuidePanel::FRIENDS: {
				uint32_t friendsTotal = 0;
				uint32_t friendsOnline = 0;
				uint32_t addedMeCount = 0;
				uint32_t localUserIndex = OverlayFirstSignedInUserIndex();
				if (localUserIndex != XLIVE_LOCAL_USER_COUNT) {
					EnterCriticalSection(&xlln_critsec_friends);
					friendsTotal = (uint32_t)xlln_friends[localUserIndex].size();
					for (auto const &itrFriend : xlln_friends[localUserIndex]) {
						if (xlln_remote_user_presence.count(itrFriend.first)) {
							friendsOnline++;
						}
					}
					addedMeCount = (uint32_t)xlln_friends_added_me[localUserIndex].size();
					LeaveCriticalSection(&xlln_critsec_friends);
				}
				if (addedMeCount) {
					snprintf(panelSubtitle, sizeof(panelSubtitle), "%u of %u online \xC2\xB7 %u added you", friendsOnline, friendsTotal, addedMeCount);
				}
				else {
					snprintf(panelSubtitle, sizeof(panelSubtitle), "%u of %u online", friendsOnline, friendsTotal);
				}
				OverlayPanelHeading("Friends", panelSubtitle);
				break;
			}
			case OverlayGuidePanel::PLAYERS: {
				uint32_t remotePlayerCount = 0;
				EnterCriticalSection(&xlln_critsec_friends);
				remotePlayerCount = (uint32_t)xlln_remote_user_presence.size();
				LeaveCriticalSection(&xlln_critsec_friends);
				snprintf(panelSubtitle, sizeof(panelSubtitle), "%u online \xC2\xB7 discovered on your network", remotePlayerCount);
				OverlayPanelHeading("Players", panelSubtitle);
				break;
			}
			case OverlayGuidePanel::INVITES: {
				snprintf(panelSubtitle, sizeof(panelSubtitle), "%u pending", (uint32_t)overlay_pending_invites.size());
				OverlayPanelHeading("Invites", panelSubtitle);
				break;
			}
			case OverlayGuidePanel::SETTINGS: {
				OverlayPanelHeading("Settings", "theme, interface size and notification position");
				break;
			}
			case OverlayGuidePanel::ACHIEVEMENTS: {
				snprintf(panelSubtitle, sizeof(panelSubtitle), "%u of %u unlocked \xC2\xB7 %uG / %uG"
					, overlay_achievements_unlocked_count
					, (uint32_t)overlay_achievement_rows.size()
					, overlay_achievements_unlocked_cred
					, overlay_achievements_total_cred
				);
				OverlayPanelHeading("Achievements", OverlayFirstSignedInUserIndex() == XLIVE_LOCAL_USER_COUNT ? 0 : panelSubtitle);
				break;
			}
			default: {
				break;
			}
		}

		float bodyMaxHeight = panelMaxHeight - OverlayPx(110.f);
		if (bodyMaxHeight < OverlayPx(120.f)) {
			bodyMaxHeight = OverlayPx(120.f);
		}
		ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(FLT_MAX, bodyMaxHeight));
		// The padding keeps the bottom border of a card visible. The child rounds its height down, so
		// fractional padding makes a scroll bar show.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, ImTrunc(OverlayPx(3.f))));
		// Flattened so the cursor crosses between the panel and this child. The cursor is placed in this
		// child because it is the window that scrolls.
		ImGui::BeginChild(
			"##xlln_guide_panel_body"
			, ImVec2(0.f, 0.f)
			, ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding
			, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings
		);
		ImGui::PopStyleVar();
		// Put the cursor on the first item when the panel changes: moving it to a window leaves it on no
		// item, and the interface only picks one for a new window. Focusing does not press anything.
		if (overlay_guide_focus_panel_pending) {
			overlay_guide_focus_panel_pending = false;
			ImGui::SetKeyboardFocusHere();
			ImGui::SetNavCursorVisible(true);
		}
		switch (overlay_active_panel) {
			case OverlayGuidePanel::HOME: {
				OverlayBuildPanelHome();
				break;
			}
			case OverlayGuidePanel::FRIENDS: {
				OverlayBuildPanelFriends();
				break;
			}
			case OverlayGuidePanel::PLAYERS: {
				OverlayBuildPanelPlayers();
				break;
			}
			case OverlayGuidePanel::INVITES: {
				OverlayBuildPanelInvites();
				break;
			}
			case OverlayGuidePanel::SETTINGS: {
				OverlayBuildPanelSettings();
				break;
			}
			case OverlayGuidePanel::ACHIEVEMENTS: {
				OverlayBuildPanelAchievements();
				break;
			}
		}
		ImGui::EndChild();

		OverlayPanelLegend();

		ImVec2 windowPosition = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();
		OverlayDrawSoftShadow(
			ImGui::GetBackgroundDrawList()
			, windowPosition
			, ImVec2(windowPosition.x + windowSize.x, windowPosition.y + windowSize.y)
			, OverlayPx(16.f)
			, OverlayPx(10.f)
			, OverlayPx(22.f)
			, 18
		);
	}
	ImGui::End();
	ImGui::PopStyleVar();

	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, overlay_open_animation);
	overlay_widget_alpha = overlay_open_animation;
	OverlayBuildInviteModal();
	overlay_widget_alpha = 1.f;
	ImGui::PopStyleVar();

	// Escape backs out: modal first, then the guide itself.
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		if (overlay_invite_modal_index >= 0) {
			overlay_invite_modal_index = -1;
		}
		else {
			OverlayGuideRequestClose();
		}
	}
}

// Invite resolution: clears local state and hands the answer to the shared
// pending-invite store (xlln-friends), which joins/declines and notifies the inviter.
static void OverlayResolveInvite(int invite_index, bool accepted)
{
	if (invite_index < 0 || invite_index >= (int)overlay_pending_invites.size()) {
		return;
	}
	OVERLAY_EVENT invite = overlay_pending_invites[invite_index];
	overlay_pending_invites.erase(overlay_pending_invites.begin() + invite_index);
	if (overlay_invite_modal_index == invite_index) {
		overlay_invite_modal_index = -1;
	}
	else if (overlay_invite_modal_index > invite_index) {
		overlay_invite_modal_index--;
	}
	if (!XllnFriendsInviteResolveByToken(invite.inviteToken, accepted)) {
		OverlayEventQueuePush(OverlayEventType::GENERIC, invite.title, "That invite has expired");
		return;
	}
	if (accepted) {
		OverlayGuideRequestClose();
	}
}

// --- frame -----------------------------------------------------------------------

// Takes xlln_critsec_overlay_frame for one frame without waiting. ImGui keeps one global context and
// a device cannot take a sequence of calls from two threads at once, so a Title that enters
// XLiveRender twice at the same moment gets one frame and the other caller leaves. A section is
// re-entrant on its own thread, so the flag refuses a Title that calls XLiveRender from inside the
// frame it is already in.
static bool overlay_frame_in_progress = false;
static bool overlay_frame_refused_logged = false;
// The thread the Title renders on is the first thing to know when the overlay does not appear. A
// Title that stops and restarts its render thread is normal, so the change is reported a few times
// and then no more.
static DWORD overlay_frame_thread_id = 0;
static uint32_t overlay_frame_thread_logged = 0;

typedef struct _OVERLAY_FRAME_GUARD {
	bool entered;
	_OVERLAY_FRAME_GUARD()
	{
		entered = TryEnterCriticalSection(&xlln_critsec_overlay_frame) != FALSE;
		if (entered && overlay_frame_in_progress) {
			LeaveCriticalSection(&xlln_critsec_overlay_frame);
			entered = false;
		}
		if (entered) {
			overlay_frame_in_progress = true;
		}
	}
	~_OVERLAY_FRAME_GUARD()
	{
		if (entered) {
			overlay_frame_in_progress = false;
			LeaveCriticalSection(&xlln_critsec_overlay_frame);
		}
	}
} OVERLAY_FRAME_GUARD;

void OverlayOnXLiveRender()
{
	OVERLAY_FRAME_GUARD frameGuard;
	if (!frameGuard.entered) {
		if (!overlay_frame_refused_logged) {
			overlay_frame_refused_logged = true;
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
				, "%s the Title called XLiveRender on thread (0x%08x) while a frame or a device callback was in progress, so this frame is skipped."
				, __func__
				, (uint32_t)GetCurrentThreadId()
			);
		}
		return;
	}
	if (overlay_frame_thread_id != GetCurrentThreadId()) {
		overlay_frame_thread_id = GetCurrentThreadId();
		if (overlay_frame_thread_logged < 8) {
			overlay_frame_thread_logged++;
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
				, "%s the Title calls XLiveRender on thread (0x%08x)."
				, __func__
				, (uint32_t)overlay_frame_thread_id
			);
		}
	}

	// A registered UI provider owns the visuals: forward the event stream and the
	// frame tick, and keep the built-in UI fully out of the way.
	if (OverlayProviderIsActive()) {
		OVERLAY_EVENT overlayEvent;
		while (OverlayEventQueuePop(&overlayEvent)) {
			OverlayProviderDeliverEvent(overlayEvent);
		}
		OverlayProviderOnFrame();
		return;
	}

	if (!xlln_overlay_enabled || !overlay_device_ready.load()) {
		return;
	}

	OverlayEnsureImGuiContext();
	// Before the early return below: a closed guide is when a pad press must be read to open it.
	OverlayGamepadPoll(overlay_title_hwnd);
	OverlayTexturesBeginFrame();
	OverlayApplyQueuedTransitions();
	OverlayDrainEvents();

	bool guideOpen = overlay_guide_open.load();
	if (!guideOpen && overlay_toasts.empty() && overlay_open_animation <= 0.f) {
		return;
	}

	OverlayEnsureTheme();
	OverlayEnsureFonts();
	if (!OverlayRendererBeginFrame()) {
		return;
	}

	ImGuiIO &imguiIo = ImGui::GetIO();
	imguiIo.DisplaySize = ImVec2((float)overlay_display_width, (float)overlay_display_height);
	if (imguiIo.DisplaySize.x <= 0.f || imguiIo.DisplaySize.y <= 0.f) {
		return;
	}
	double timeNow = OverlayTimeNow();
	float deltaTime = overlay_time_last_frame > 0.0 ? (float)(timeNow - overlay_time_last_frame) : (1.f / 60.f);
	if (deltaTime <= 0.f) {
		deltaTime = 1.f / 1000.f;
	}
	if (deltaTime > 0.1f) {
		deltaTime = 0.1f;
	}
	imguiIo.DeltaTime = deltaTime;
	overlay_time_last_frame = timeNow;

	// Open/close slide+fade animation driver.
	float animationTarget = guideOpen ? 1.f : 0.f;
	float animationStep = deltaTime / 0.20f;
	if (overlay_open_animation < animationTarget) {
		overlay_open_animation = overlay_open_animation + animationStep > animationTarget ? animationTarget : overlay_open_animation + animationStep;
	}
	else if (overlay_open_animation > animationTarget) {
		overlay_open_animation = overlay_open_animation - animationStep < animationTarget ? animationTarget : overlay_open_animation - animationStep;
	}

	OverlayInputNewFrame(overlay_title_hwnd, guideOpen);

	if (guideOpen) {
		// Prefer the hardware cursor when the title still shows one; otherwise draw our own.
		CURSORINFO cursorInfo;
		cursorInfo.cbSize = sizeof(cursorInfo);
		bool hardwareCursorVisible = GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING);
		imguiIo.MouseDrawCursor = !hardwareCursorVisible;
	}

	overlay_widget_alpha = 1.f;
	ImGui::NewFrame();
	if (guideOpen || overlay_open_animation > 0.f) {
		if (guideOpen) {
			OverlayBuildGuide();
		}
		else if (overlay_open_animation > 0.f) {
			// Fading out: keep only the dim rectangle.
			ImGui::GetBackgroundDrawList()->AddRectFilled(
				ImVec2(0.f, 0.f)
				, imguiIo.DisplaySize
				, OverlayDimColor(overlay_open_animation)
			);
		}
	}
	OverlayBuildToasts();
	ImGui::EndFrame();
	ImGui::Render();

	ImDrawData* drawData = ImGui::GetDrawData();
	if (drawData && drawData->CmdListsCount > 0) {
		OverlayRendererRenderDrawData(drawData);

		// An open guide holds the Title's input and pauses it through XN_SYS_UI. A Title that never
		// binds the target it presents refuses every frame, so the guide would hold both with nothing
		// on screen. Stand down instead and let the XLLN window answer.
		if (OverlayRendererTargetAccepted()) {
			overlay_frames_refused = 0;
		}
		else if (++overlay_frames_refused >= OVERLAY_FRAMES_REFUSED_MAX && overlay_render_target_usable.load()) {
			overlay_render_target_usable.store(false);
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
				, "%s the Title never binds the target it presents, so after %u frames the overlay stands down and the XLLN window takes over."
				, __func__
				, overlay_frames_refused
			);
			OverlayGuideClose();
		}
	}
}
