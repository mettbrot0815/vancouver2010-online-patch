#pragma once
#include <stdint.h>

// Any XLLN thread produces these events. The overlay consumes them on the title's render
// thread. Strings are UTF-8 and copied by value, so no producer shares memory with the consumer.

enum class OverlayEventType : uint32_t {
	GENERIC = 0,
	// First-run "press HOME" style hint.
	HINT,
	PLAYER_ONLINE,
	PLAYER_OFFLINE,
	SIGNED_IN,
	SIGNED_OUT,
	INVITE_RECEIVED,
	INVITE_DECLINED,
	JOINING_SESSION,
	// Append only: the values are mirrored in the module-provider ABI (xlln-overlay-api.hpp).
	ACHIEVEMENT_UNLOCKED,
};

typedef struct _OVERLAY_EVENT {
	OverlayEventType type = OverlayEventType::GENERIC;
	// Primary line (typically a gamertag).
	char title[64] = {0};
	// Secondary line.
	char detail[128] = {0};
	uint64_t xuid = 0;
	// Invite bookkeeping (INVITE_* events).
	uint32_t inviteToken = 0;
	uint32_t remoteInstanceId = 0;
	// The local signed-in user the event addresses (invitee for INVITE_RECEIVED).
	uint32_t localUserIndex = 0;
	// The Title an invite is for (INVITE_RECEIVED). Only the running Title can accept it.
	uint32_t titleId = 0;
	// Seconds the toast stays on screen. 0 = type default.
	float durationSeconds = 0.f;
	// SPA image id (achievement art) for ACHIEVEMENT_UNLOCKED events.
	uint32_t imageId = 0;
} OVERLAY_EVENT;

// The push sites in xlln-friends.cpp enforce this scope, and only for presence toasts.
// The event queue does not filter, so every other event shows whatever the scope.
#define XLLN_OVERLAY_TOAST_SCOPE_EVERYONE 0
#define XLLN_OVERLAY_TOAST_SCOPE_FRIENDS 1
#define XLLN_OVERLAY_TOAST_SCOPE_OFF 2
extern uint32_t xlln_overlay_toast_scope;

extern CRITICAL_SECTION xlln_critsec_overlay_events;

void OverlayEventQueuePush(const OVERLAY_EVENT &overlay_event);
void OverlayEventQueuePush(OverlayEventType event_type, const char* title, const char* detail);
// Consumer side (overlay render thread). Returns false when the queue is empty.
bool OverlayEventQueuePop(OVERLAY_EVENT* overlay_event);
void OverlayEventQueueClear();
