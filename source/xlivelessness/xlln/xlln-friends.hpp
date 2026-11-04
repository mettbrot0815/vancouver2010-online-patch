#pragma once
#include "../xlive/xdefs.hpp"
#include "../xlive/live-over-lan.hpp"
#include <map>

typedef struct _XLLN_REMOTE_USER_PRESENCE {
	uint32_t instanceId = 0;
	char username[XUSER_NAME_SIZE] = {0};
	uint32_t titleId = 0;
	// UTF-8. The sender resolves this from its own SPA, and sends it empty when unknown.
	char titleName[64] = {0};
	char richPresence[128] = {0};
	__time64_t timeOfLastContact = 0;
} XLLN_REMOTE_USER_PRESENCE;

typedef struct _XLLN_FRIEND_INFO {
	// Informational only. The XUID is the identity of a friend.
	char username[XUSER_NAME_SIZE] = {0};
} XLLN_FRIEND_INFO;

typedef struct _XLLN_PENDING_INVITE {
	// XUSER_INDEX_NONE means there is no pending invite.
	uint32_t userIndex = XUSER_INDEX_NONE;
	// Identifies the invite to the overlay UI. Never 0 for a stored invite.
	uint32_t inviteToken = 0;
	// The inviter's token for the same invite. The response carries this value back.
	uint32_t inviterInviteToken = 0;
	uint32_t inviterInstanceId = 0;
	XUID inviterXuid = 0;
	char inviterUsername[XUSER_NAME_SIZE] = {0};
	uint32_t titleId = 0;
	wchar_t inviteMessage[64] = {0};
	__time64_t expiresAt = 0;
	// Owned here. Accepting gives ownership to the remote session store. Declining destroys it.
	LIVE_SESSION* liveSession = 0;
} XLLN_PENDING_INVITE;

// 0 = always prompt, 1 = auto-accept invites from friends, 2 = auto-accept any invite.
extern uint32_t xlln_friends_invite_auto_accept;

extern CRITICAL_SECTION xlln_critsec_friends;
// Key: XUID of the remote user. Guarded by xlln_critsec_friends.
extern std::map<XUID, XLLN_REMOTE_USER_PRESENCE> xlln_remote_user_presence;
// Key: XUID of the friend. Guarded by xlln_critsec_friends.
extern std::map<XUID, XLLN_FRIEND_INFO> xlln_friends[XLIVE_LOCAL_USER_COUNT];
// Remote players who added a local user as a friend, so the UI can offer to add them back.
// Not persisted. An entry goes away when the local user adds that player back.
// Key: XUID of the adder. Guarded by xlln_critsec_friends.
extern std::map<XUID, XLLN_FRIEND_INFO> xlln_friends_added_me[XLIVE_LOCAL_USER_COUNT];

void XllnFriendsLoad(uint32_t user_index);
void XllnFriendsUnload(uint32_t user_index);
bool XllnFriendsAdd(uint32_t user_index, XUID xuid, const char* username);
bool XllnFriendsRemove(uint32_t user_index, XUID xuid);
bool XllnFriendsIsFriend(uint32_t user_index, XUID xuid);
// Matches an invite response against an invite this instance sent, and removes the record.
bool XllnFriendsConsumeSentInviteToken(XUID invitee_xuid, uint32_t invite_token);

// Records that a remote player added a local user as a friend. Does nothing when no
// signed-in local user holds added_xuid.
void XllnFriendsAddedReceived(uint32_t adder_instance_id, XUID adder_xuid, const char* adder_username, XUID added_xuid);

// Tests whether an invite can go out now: the session XllnFriendsSendInvite would serialise
// exists, it does not set XSESSION_CREATE_INVITES_DISABLED, and it has a free slot.
// On refusal result_reason, when given, receives the text the UI shows the user.
bool XllnSessionCanInvite(char* result_reason, size_t result_reason_size);

void XllnFriendsPresenceReceived(uint32_t instance_id, XUID xuid, const char* username, uint32_t title_id, const char* title_name, const char* rich_presence, bool is_online);
void XllnFriendsPresenceRemoveStale();
void XllnFriendsPresenceClear();
void XllnFriendsBroadcastPresence();
void XllnFriendsBroadcastPresenceUser(uint32_t user_index, bool is_online);
// Broadcasts the user's presence again when the resolved presence line changed. Call it after
// a title writes a context or a property. Sends at most once every 2 seconds per user.
void XllnFriendsUserPresenceMaybeChanged(uint32_t user_index);

uint32_t XllnFriendsSendInvite(uint32_t user_index, size_t invite_xuid_count, const XUID* invite_xuids, const wchar_t* invite_message, size_t* result_sent_count);
void XllnFriendsInviteReceived(uint32_t user_index, uint32_t inviter_instance_id, XUID inviter_xuid, const char* inviter_username, uint32_t title_id, uint32_t invite_token, const wchar_t* invite_message, LIVE_SESSION* live_session);
// Copies the newest pending invite. The copy carries no liveSession, because the invite store
// keeps ownership of that session.
bool XllnFriendsGetPendingInvite(XLLN_PENDING_INVITE* result_invite);
// Accepts or declines one invite, named by the token from XLLN_PENDING_INVITE or from an
// overlay INVITE_RECEIVED event. Returns false when the token is unknown or expired.
bool XllnFriendsInviteResolveByToken(uint32_t invite_token, bool accepted, bool* result_accept_refused = 0);
