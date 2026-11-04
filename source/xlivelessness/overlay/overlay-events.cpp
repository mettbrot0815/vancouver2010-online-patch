#include <winsock2.h>
#include <Windows.h>
#include "overlay-events.hpp"
#include "../utils/utils.hpp"
#include <deque>
#include <string.h>

uint32_t xlln_overlay_toast_scope = XLLN_OVERLAY_TOAST_SCOPE_EVERYONE;

CRITICAL_SECTION xlln_critsec_overlay_events;

static std::deque<OVERLAY_EVENT> xlln_overlay_event_queue;
// Guards against unbounded growth if no consumer is running (no D3D device / overlay disabled).
static const size_t xlln_overlay_event_queue_max = 64;

void OverlayEventQueuePush(const OVERLAY_EVENT &overlay_event)
{
	EnterCriticalSection(&xlln_critsec_overlay_events);
	if (xlln_overlay_event_queue.size() >= xlln_overlay_event_queue_max) {
		xlln_overlay_event_queue.pop_front();
	}
	xlln_overlay_event_queue.push_back(overlay_event);
	LeaveCriticalSection(&xlln_critsec_overlay_events);
}

void OverlayEventQueuePush(OverlayEventType event_type, const char* title, const char* detail)
{
	OVERLAY_EVENT overlayEvent;
	overlayEvent.type = event_type;
	if (title) {
		strncpy_s(overlayEvent.title, title, _TRUNCATE);
	}
	if (detail) {
		strncpy_s(overlayEvent.detail, detail, _TRUNCATE);
	}
	OverlayEventQueuePush(overlayEvent);
}

bool OverlayEventQueuePop(OVERLAY_EVENT* overlay_event)
{
	if (!overlay_event) {
		return false;
	}
	bool hasEvent = false;
	EnterCriticalSection(&xlln_critsec_overlay_events);
	if (!xlln_overlay_event_queue.empty()) {
		*overlay_event = xlln_overlay_event_queue.front();
		xlln_overlay_event_queue.pop_front();
		hasEvent = true;
	}
	LeaveCriticalSection(&xlln_critsec_overlay_events);
	return hasEvent;
}

void OverlayEventQueueClear()
{
	EnterCriticalSection(&xlln_critsec_overlay_events);
	xlln_overlay_event_queue.clear();
	LeaveCriticalSection(&xlln_critsec_overlay_events);
}
