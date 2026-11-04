#pragma once

extern CRITICAL_SECTION xlive_critsec_xnotify;
extern bool xlive_notify_system_ui_open;

void XLiveNotifyAddEvent(uint32_t notification_id, size_t notification_value);
bool XLiveNotifyDeleteListener(HANDLE notification_listener);
// Returns the XNOTIFYUI_POS_* anchor that the title set for notification popups.
uint32_t XLiveNotifyGetUIPosition();
bool InitXNotify();
bool UninitXNotify();
