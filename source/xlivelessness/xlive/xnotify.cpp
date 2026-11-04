#include <winsock2.h>
#include "xdefs.hpp"
#include "xnotify.hpp"
#include "../xlln/debug-log.hpp"
#include "xlive.hpp"
#include <map>
#include <set>
#include <vector>

typedef struct {
	HANDLE listener;
	uint32_t area;
	// Key: notification_id. Value: notification_value.
	// Each listener holds its own copy of a notification. A title can have several listeners and
	// each one must see the notification. One shared queue lets the listener that polls first take
	// a notification that the other listeners then never get.
	std::map<uint32_t, size_t> pendingNotifications;
} XNOTIFY_LISTENER_INFO;

CRITICAL_SECTION xlive_critsec_xnotify;
// Key: listener handle.
static std::map<HANDLE, XNOTIFY_LISTENER_INFO> xlive_notify_listeners;
// Key: single notification area. Value: listener handles that are in that area.
static std::map<uint32_t, std::vector<HANDLE>> xlive_notify_listener_areas;
// Key: notification_area. Key: notification_id. Value: notification_value.
// Holds only the notifications that arrived while the area had no listener. The next listener
// created for the area receives them.
static std::map<uint32_t, std::map<uint32_t, size_t>> xlive_notify_pending_notifications;

bool xlive_notify_system_ui_open = false;

static uint32_t xlive_notify_ui_position = XNOTIFYUI_POS_BOTTOMCENTER;

static uint32_t GetNotificationArea(uint32_t notification_id)
{
	uint32_t notificationArea = 0;
	
	if (!notification_id) {
		return notificationArea;
	}
	
	switch (XNID_AREA(notification_id)) {
		case _XNAREA_SYSTEM:
			notificationArea = XNOTIFY_SYSTEM;
			break;
		case _XNAREA_LIVE:
			notificationArea = XNOTIFY_LIVE;
			break;
		case _XNAREA_FRIENDS:
			notificationArea = XNOTIFY_FRIENDS;
			break;
		case _XNAREA_CUSTOM:
			notificationArea = XNOTIFY_CUSTOM;
			break;
		case _XNAREA_XMP:
			notificationArea = XNOTIFY_XMP;
			break;
		case _XNAREA_MSGR:
			notificationArea = XNOTIFY_MSGR;
			break;
		case _XNAREA_PARTY:
			notificationArea = XNOTIFY_PARTY;
			break;
	}
	
	return notificationArea;
}

static bool XLiveNotifyRetainNotificationOnZeroListeners(uint32_t notification_id)
{
	// Only hold these specific notifications when there are no existing listeners for them.
	// XN_LIVE_CONNECTIONCHANGED carries the online connection state and is raised once at sign-in
	// (XLLNLogin), which runs before the title creates its LIVE notify listener. Without retention it is
	// dropped, so the title learns it is signed in (XN_SYS_SIGNINCHANGED, retained below) but never that
	// the online connection is established — leaving connection-status-gated UI stuck "offline".
	switch (notification_id) {
		case XN_LIVE_CONNECTIONCHANGED:
		case XN_LIVE_INVITE_ACCEPTED:
		case XN_SYS_UI:
		case XN_SYS_SIGNINCHANGED: {
			return true;
		}
	}
		
	return false;
}

static size_t XLiveNotifyValueSize(uint32_t notification_id)
{
	switch (notification_id) {
		case XN_SYS_SIGNINCHANGED:
		case XN_SYS_PROFILESETTINGCHANGED:
		case XN_LIVE_CONNECTIONCHANGED:
		case XN_LIVE_INVITE_ACCEPTED:
		case XN_FRIENDS_PRESENCE_CHANGED:
		case XN_FRIENDS_FRIEND_ADDED:
		case XN_FRIENDS_FRIEND_REMOVED:
		case XN_CUSTOM_ACTIONPRESSED:
		case XN_CUSTOM_GAMERCARD: {
			return sizeof(uint32_t);
		}
		case XN_SYS_UI:
		case XN_LIVE_LINK_STATE_CHANGED:
		case XN_LIVE_VOICECHAT_AWAY: {
			return sizeof(BOOL);
		}
		case XN_SYS_MUTELISTCHANGED:
		case XN_LIVE_CONTENT_INSTALLED:
		case XN_LIVE_PRESENCE_CHANGED: {
			return 0;
		}
		//case XN_LIVE_CONTENT_CHANGED: {
		//	return XLIVE_CONTENT_ID_SIZE;
		//}
	}
	
	__debugbreak();
	
	return 0;
}

static void XLiveNotifyAddEvent_(uint32_t notification_id, size_t notification_value)
{
	uint32_t notificationArea = GetNotificationArea(notification_id);
	
	if (!notificationArea) {
		return;
	}
	
	switch (notification_id) {
		case XN_SYS_UI: {
			xlive_notify_system_ui_open = (notification_value != 0);
			break;
		}
	}
	
	size_t notificationValueSize = XLiveNotifyValueSize(notification_id);
	bool delivered = false;

	for (const HANDLE &xnotifyListener : xlive_notify_listener_areas[notificationArea]) {
		auto const &itrNotificationListener = xlive_notify_listeners.find(xnotifyListener);
		if (itrNotificationListener == xlive_notify_listeners.end()) {
			continue;
		}

		size_t notificationValueCopy = notification_value;
		if (notificationValueSize > sizeof(size_t)) {
			uint8_t* valueCopy = new uint8_t[notificationValueSize];
			memcpy(valueCopy, (void*)notification_value, notificationValueSize);
			notificationValueCopy = (size_t)valueCopy;
		}

		auto const &itrExistingPending = itrNotificationListener->second.pendingNotifications.find(notification_id);
		if (itrExistingPending != itrNotificationListener->second.pendingNotifications.end() && notificationValueSize > sizeof(size_t)) {
			delete[] (void*)itrExistingPending->second;
		}
		itrNotificationListener->second.pendingNotifications[notification_id] = notificationValueCopy;

		SetEvent(xnotifyListener);
		delivered = true;
	}

	if (!delivered && XLiveNotifyRetainNotificationOnZeroListeners(notification_id)) {
		xlive_notify_pending_notifications[notificationArea][notification_id] = notification_value;
	}
	else if (notificationValueSize > sizeof(size_t)) {
		delete[] (void*)notification_value;
	}
}

void XLiveNotifyAddEvent(uint32_t notification_id, size_t notification_value)
{
	uint32_t notificationArea = GetNotificationArea(notification_id);
	
	if (!notificationArea) {
		return;
	}
	
	{
		EnterCriticalSection(&xlive_critsec_xnotify);
		
		XLiveNotifyAddEvent_(notification_id, notification_value);
		
		LeaveCriticalSection(&xlive_critsec_xnotify);
	}
}

static bool XLiveNotifyDeleteListener_(HANDLE notification_listener)
{
	if (!xlive_notify_listeners.count(notification_listener)) {
		return false;
	}
	
	{
		uint32_t iArea = 1;
		while (1) {
			// Erase entries of this listener.
			for (auto itrNotificationListener = xlive_notify_listener_areas[iArea].begin(); itrNotificationListener != xlive_notify_listener_areas[iArea].end(); ) {
				if (*itrNotificationListener == notification_listener) {
					xlive_notify_listener_areas[iArea].erase(itrNotificationListener++);
					break;
				}
				else {
					++itrNotificationListener;
				}
			}

			if (iArea == (1 << 31)) {
				break;
			}
			iArea <<= 1;
		}
	}

	{
		XNOTIFY_LISTENER_INFO &notificationListenerInfo = xlive_notify_listeners[notification_listener];
		for (auto const &pendingNotification : notificationListenerInfo.pendingNotifications) {
			size_t notificationValueSize = XLiveNotifyValueSize(pendingNotification.first);
			if (notificationValueSize > sizeof(size_t)) {
				delete[] (void*)pendingNotification.second;
			}
		}
	}

	xlive_notify_listeners.erase(notification_listener);
	
	return true;
}

bool XLiveNotifyDeleteListener(HANDLE notification_listener)
{
	bool result = false;
	EnterCriticalSection(&xlive_critsec_xnotify);
	result = XLiveNotifyDeleteListener_(notification_listener);
	LeaveCriticalSection(&xlive_critsec_xnotify);
	return result;
}

// #651
BOOL WINAPI XNotifyGetNext(HANDLE notification_listener, uint32_t notification_id_filter, uint32_t* notification_id, void* notification_value)
{
	TRACE_FX();
	if (!notification_listener) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s notification_listener is NULL.", __func__);
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (notification_listener == INVALID_HANDLE_VALUE) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s notification_listener is INVALID_HANDLE_VALUE.", __func__);
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!notification_id) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s notification_id is NULL.", __func__);
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	
	*notification_id = 0;
	
	uint32_t notificationArea = GetNotificationArea(notification_id_filter);
	
	if (notification_id_filter && !notificationArea) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s unknown notification_id_filter value (0x%08x)."
			, __func__
			, notification_id_filter
		);
		return FALSE;
	}
	
	{
		EnterCriticalSection(&xlive_critsec_xnotify);
		
		auto const &itrNotificationListener = xlive_notify_listeners.find(notification_listener);
		if (itrNotificationListener != xlive_notify_listeners.end()) {
			XNOTIFY_LISTENER_INFO &notificationListenerInfo = itrNotificationListener->second;
			if (notification_id_filter && !(notificationListenerInfo.area & notificationArea)) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
					, "%s notification_id_filter (0x%08x) is not part of notification_listener (0x%zx) subscribed notification area (0x%08x)."
					, __func__
					, notification_id_filter
					, notification_listener
					, notificationListenerInfo.area
				);
			}
			else {
				auto const &itrPendingNotification = notification_id_filter
					? notificationListenerInfo.pendingNotifications.find(notification_id_filter)
					: notificationListenerInfo.pendingNotifications.begin();
				if (itrPendingNotification != notificationListenerInfo.pendingNotifications.end()) {
					*notification_id = itrPendingNotification->first;
					size_t notificationValueSize = XLiveNotifyValueSize(*notification_id);
					if (notification_value && notificationValueSize) {
						if (notificationValueSize > sizeof(size_t)) {
							memcpy_s(notification_value, notificationValueSize, (void*)itrPendingNotification->second, notificationValueSize);
						}
						else {
							memcpy_s(notification_value, notificationValueSize, &itrPendingNotification->second, notificationValueSize);
						}
					}
					if (notificationValueSize > sizeof(size_t)) {
						delete[] (void*)itrPendingNotification->second;
					}
					notificationListenerInfo.pendingNotifications.erase(itrPendingNotification);
				}
			}
		}
		
		LeaveCriticalSection(&xlive_critsec_xnotify);
	}
	
	if (*notification_id) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
			, "%s notification_id (0x%08x) consumed by notification_listener (0x%zx)."
			, __func__
			, *notification_id
			, notification_listener
		);
	}
	else {
		ResetEvent(notification_listener);
	}
	
	return *notification_id ? TRUE : FALSE;
}

// #652
void WINAPI XNotifyPositionUI(uint32_t position_flags)
{
	TRACE_FX();
	if (
		position_flags & (~XNOTIFYUI_POS_MASK)
		|| (position_flags & XNOTIFYUI_POS_TOPCENTER && position_flags & XNOTIFYUI_POS_BOTTOMCENTER)
		|| (position_flags & XNOTIFYUI_POS_CENTERLEFT && position_flags & XNOTIFYUI_POS_CENTERRIGHT)
	) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Invalid position_flags (0x%08x). Check XNOTIFYUI_POS_* bits. Do not specify both TOP and BOTTOM or both LEFT and RIGHT."
			, __func__
			, position_flags
		);
		return;
	}
	
	EnterCriticalSection(&xlive_critsec_xnotify);
	xlive_notify_ui_position = position_flags & XNOTIFYUI_POS_MASK;
	LeaveCriticalSection(&xlive_critsec_xnotify);
}

uint32_t XLiveNotifyGetUIPosition()
{
	EnterCriticalSection(&xlive_critsec_xnotify);
	uint32_t position = xlive_notify_ui_position;
	LeaveCriticalSection(&xlive_critsec_xnotify);
	return position;
}

// #653
uint32_t WINAPI XNotifyDelayUI(uint32_t milliseconds)
{
	TRACE_FX();
	return ERROR_SUCCESS;
}

// #5270: Requires XNotifyGetNext to process the listener.
HANDLE WINAPI XNotifyCreateListener(uint64_t notification_area)
{
	TRACE_FX();
	if (HIDWORD(notification_area)) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
			, "%s HIDWORD(notification_area) value set (0x%08x)."
			, __func__
			, HIDWORD(notification_area)
		);
	}
	if ((uint32_t)notification_area & ~XNOTIFY_ALL) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
			, "%s unknown dwMsgFilter value (0x%08x) outside of XNOTIFY_ALL (0x%08x)."
			, __func__
			, (uint32_t)notification_area
			, XNOTIFY_ALL
		);
	}
	
	uint32_t unsanitisedAreas = (uint32_t)notification_area;
	if (!unsanitisedAreas) {
		unsanitisedAreas = XNOTIFY_ALL;
	}
	uint32_t sanitisedAreas = unsanitisedAreas & XNOTIFY_ALL;
	HANDLE xnotifyListener = CreateEvent(NULL, TRUE, FALSE, NULL);;
	
	{
		EnterCriticalSection(&xlive_critsec_xnotify);
		
		// A title may close its listener without XCloseHandle, so this value can be a recycled one
		// still mapped to the previous listener. Purge that before reusing the key.
		XLiveNotifyDeleteListener_(xnotifyListener);
		
		XNOTIFY_LISTENER_INFO &notificationListenerInfo = xlive_notify_listeners[xnotifyListener];
		notificationListenerInfo.listener = xnotifyListener;
		notificationListenerInfo.area = unsanitisedAreas;
		
		uint32_t iArea = 1;
		while (1) {
			if (iArea & sanitisedAreas) {
				xlive_notify_listener_areas[iArea].push_back(xnotifyListener);

				auto &retainedNotifications = xlive_notify_pending_notifications[iArea];
				if (retainedNotifications.size()) {
					for (auto const &retainedNotification : retainedNotifications) {
						notificationListenerInfo.pendingNotifications[retainedNotification.first] = retainedNotification.second;
					}
					retainedNotifications.clear();
					SetEvent(xnotifyListener);
				}
			}

			if (iArea == (1 << 31)) {
				break;
			}
			iArea <<= 1;
		}

		LeaveCriticalSection(&xlive_critsec_xnotify);
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
		, "%s hNotification (0x%08x) created."
		, __func__
		, xnotifyListener
	);
	
	return xnotifyListener;
}

bool InitXNotify()
{
	// Initialise every possible existing area so there are no key issues later.
	{
		EnterCriticalSection(&xlive_critsec_xnotify);
		
		uint32_t iArea = 1;
		while (1) {
			xlive_notify_listener_areas[iArea];
			xlive_notify_pending_notifications[iArea];
			
			if (iArea == (1 << 31)) {
				break;
			}
			iArea <<= 1;
		}
		
		LeaveCriticalSection(&xlive_critsec_xnotify);
	}
	
	return TRUE;
}

bool UninitXNotify()
{
	{
		EnterCriticalSection(&xlive_critsec_xnotify);
		
		for (auto itrNotificationListener = xlive_notify_listeners.begin(); itrNotificationListener != xlive_notify_listeners.end(); ) {
			HANDLE hNotificationListener = (itrNotificationListener++)->first;
			if (!hNotificationListener) {
				continue;
			}
			XLiveNotifyDeleteListener_(hNotificationListener);
		}
		xlive_notify_listeners.clear();
		xlive_notify_listener_areas.clear();
		
		{
			uint32_t iArea = 1;
			while (1) {
				xlive_notify_listener_areas[iArea];
				xlive_notify_pending_notifications[iArea];
				
				if (iArea == (1 << 31)) {
					break;
				}
				iArea <<= 1;
			}
		}
		
		LeaveCriticalSection(&xlive_critsec_xnotify);
	}
	
	return TRUE;
}
