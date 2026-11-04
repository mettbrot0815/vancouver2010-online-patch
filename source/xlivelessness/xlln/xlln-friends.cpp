#include <winsock2.h>
#include "../xlive/xdefs.hpp"
#include "xlln-friends.hpp"
#include "xlln.hpp"
#include "xlln-config.hpp"
#include "xlln-network.hpp"
#include "debug-log.hpp"
#include "../xlive/xlive.hpp"
#include "../xlive/xlocator.hpp"
#include "../xlive/xsession.hpp"
#include "../xlive/xuser.hpp"
#include "../xlive/xnotify.hpp"
#include "../overlay/overlay-events.hpp"
#include "../utils/utils.hpp"
#include <time.h>
#include <vector>
#include <set>
#include <string>

#define XLLN_PENDING_INVITE_MAX_COUNT 8
#define XLLN_PENDING_INVITE_LIFETIME_SECONDS 120
// Both maps are keyed by a XUID a remote instance chooses, so both need a ceiling. Well above any
// real player count on a hub.
#define XLLN_REMOTE_USER_PRESENCE_MAX_COUNT 512
#define XLLN_REMOTE_USER_USERNAMES_MAX_COUNT 512

// Invites this instance has sent and is still willing to hear an answer to. Guarded by
// xlln_critsec_friends.
typedef struct _XLLN_SENT_INVITE {
	uint32_t inviteToken = 0;
	XUID inviteeXuid = 0;
	__time64_t expiresAt = 0;
} XLLN_SENT_INVITE;
static std::vector<XLLN_SENT_INVITE> xlln_sent_invites;

uint32_t xlln_friends_invite_auto_accept = 0;

CRITICAL_SECTION xlln_critsec_friends;
std::map<XUID, XLLN_REMOTE_USER_PRESENCE> xlln_remote_user_presence;
std::map<XUID, XLLN_FRIEND_INFO> xlln_friends[XLIVE_LOCAL_USER_COUNT];
std::map<XUID, XLLN_FRIEND_INFO> xlln_friends_added_me[XLIVE_LOCAL_USER_COUNT];

// Pending received invites, newest last. Guarded by xlln_critsec_friends.
static std::vector<XLLN_PENDING_INVITE> xlln_pending_invites;
static uint32_t xlln_pending_invite_next_token = 1;

// Set when a friend's stored name changes because of a packet. The file is written by the ten
// second sweep instead of the receive thread, which a peer can drive at packet rate.
static bool xlln_friends_save_pending[XLIVE_LOCAL_USER_COUNT] = {};

static void XllnFriendsSendPresence_(uint32_t user_index, bool is_online, uint32_t destination_instance_id, bool include_rich_presence);
static void XllnFriendsSendFriendAdded_(uint32_t user_index, XUID added_xuid);

// Requires xlln_critsec_friends to already be held.
// Formats the friends file. Requires xlln_critsec_friends.
static std::string XllnFriendsSerialise_(uint32_t user_index)
{
	std::string content;
	char line[128 + XUSER_NAME_SIZE];
	snprintf(line, sizeof(line), "# XLLN Friends of \"%s\". One friend per line: <XUID> = <last known username>.\n", xlive_local_users[user_index].username);
	content += line;
	for (auto const &itrFriend : xlln_friends[user_index]) {
		snprintf(line, sizeof(line), "0x%016I64X = %s\n", itrFriend.first, itrFriend.second.username);
		content += line;
	}
	return content;
}

// Writes what XllnFriendsSerialise_ formatted. This does file input and output, so it must not run
// while xlln_critsec_friends is held: the overlay takes that lock on every frame it draws.
static void XllnFriendsSave_(uint32_t user_index, const std::string &content)
{
	if (!xlln_file_config_path || !xlive_local_users[user_index].username[0]) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
			, "%s Unable to save friends of user %u, no config path or username."
			, __func__
			, user_index
		);
		return;
	}

	wchar_t* pathConfig = PathFromFilename(xlln_file_config_path);
	wchar_t* pathUser = FormMallocString(L"%sprofile/user/%hs/", pathConfig, xlive_local_users[user_index].username);
	delete[] pathConfig;
	pathConfig = 0;

	uint32_t errorMkdir = EnsureDirectoryExists(pathUser);
	if (errorMkdir) {
		XLLN_DEBUG_LOG_ECODE(errorMkdir, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s EnsureDirectoryExists(...) error on path \"%ls\"."
			, __func__
			, pathUser
		);
		free(pathUser);
		return;
	}

	wchar_t* pathFriends = FormMallocString(L"%sfriends.ini", pathUser);
	free(pathUser);
	pathUser = 0;

	FILE* fpFriends;
	errno_t errorFileOpen = _wfopen_s(&fpFriends, pathFriends, L"w");
	if (errorFileOpen) {
		XLLN_DEBUG_LOG_ECODE(errorFileOpen, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s fopen(\"%ls\", \"w\") error:"
			, __func__
			, pathFriends
		);
		free(pathFriends);
		return;
	}
	free(pathFriends);
	pathFriends = 0;

	fwrite(content.c_str(), sizeof(char), content.size(), fpFriends);

	fclose(fpFriends);
	fpFriends = 0;
}

void XllnFriendsLoad(uint32_t user_index)
{
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		return;
	}

	// File input, so this must not run while xlln_critsec_friends is held, for the reason
	// XllnFriendsSave_ gives. The parsed list replaces the live one under the lock at the end.
	std::map<XUID, XLLN_FRIEND_INFO> friendsLoaded;

	do {
		if (!xlln_file_config_path || !xlive_local_users[user_index].username[0]) {
			break;
		}

		wchar_t* pathConfig = PathFromFilename(xlln_file_config_path);
		wchar_t* pathFriends = FormMallocString(L"%sprofile/user/%hs/friends.ini", pathConfig, xlive_local_users[user_index].username);
		delete[] pathConfig;
		pathConfig = 0;

		FILE* fpFriends;
		errno_t errorFileOpen = _wfopen_s(&fpFriends, pathFriends, L"r");
		if (errorFileOpen) {
			XLLN_DEBUG_LOG_ECODE(errorFileOpen, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
				, "%s No friends file \"%ls\":"
				, __func__
				, pathFriends
			);
			free(pathFriends);
			break;
		}
		free(pathFriends);
		pathFriends = 0;

		char fileLine[0x100];
		while (fgets(fileLine, sizeof(fileLine), fpFriends)) {
			char* cursor = fileLine;
			while (*cursor == ' ' || *cursor == '\t') {
				cursor++;
			}
			if (!*cursor || *cursor == '#' || *cursor == ';' || *cursor == '[' || *cursor == '\r' || *cursor == '\n') {
				continue;
			}

			XUID xuid = 0;
			int32_t parsedLength = 0;
			if (sscanf_s(cursor, "0x%I64x %n", &xuid, &parsedLength) != 1 || !xuid) {
				continue;
			}
			cursor += parsedLength;
			if (*cursor != '=') {
				continue;
			}
			cursor++;
			while (*cursor == ' ' || *cursor == '\t') {
				cursor++;
			}
			size_t usernameLength = strcspn(cursor, "\r\n");
			cursor[usernameLength] = 0;
			while (usernameLength && (cursor[usernameLength - 1] == ' ' || cursor[usernameLength - 1] == '\t')) {
				cursor[--usernameLength] = 0;
			}

			XLLN_FRIEND_INFO &friendInfo = friendsLoaded[xuid];
			strncpy_s(friendInfo.username, cursor, _TRUNCATE);
		}

		fclose(fpFriends);
		fpFriends = 0;
	} while (0);

	EnterCriticalSection(&xlln_critsec_friends);

	xlln_friends[user_index] = friendsLoaded;
	const size_t friendsCount = xlln_friends[user_index].size();

	LeaveCriticalSection(&xlln_critsec_friends);

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s Loaded %zu friend(s) of user %u \"%s\"."
		, __func__
		, friendsCount
		, user_index
		, xlive_local_users[user_index].username
	);
}

void XllnFriendsUnload(uint32_t user_index)
{
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		return;
	}

	EnterCriticalSection(&xlln_critsec_friends);

	xlln_friends[user_index].clear();
	xlln_friends_added_me[user_index].clear();

	for (size_t iInvite = 0; iInvite < xlln_pending_invites.size();) {
		if (xlln_pending_invites[iInvite].userIndex == user_index) {
			if (xlln_pending_invites[iInvite].liveSession) {
				LiveOverLanDestroyLiveSession(&xlln_pending_invites[iInvite].liveSession);
			}
			xlln_pending_invites.erase(xlln_pending_invites.begin() + iInvite);
		}
		else {
			iInvite++;
		}
	}

	LeaveCriticalSection(&xlln_critsec_friends);
}

bool XllnFriendsAdd(uint32_t user_index, XUID xuid, const char* username)
{
	if (user_index >= XLIVE_LOCAL_USER_COUNT || !xuid) {
		return false;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		return false;
	}
	if (xlive_local_users[user_index].xuid == xuid) {
		return false;
	}

	EnterCriticalSection(&xlln_critsec_friends);

	XLLN_FRIEND_INFO &friendInfo = xlln_friends[user_index][xuid];
	if (username && *username) {
		strncpy_s(friendInfo.username, username, _TRUNCATE);
	}
	else {
		auto itrAddedMe = xlln_friends_added_me[user_index].find(xuid);
		if (itrAddedMe != xlln_friends_added_me[user_index].end() && !friendInfo.username[0]) {
			strncpy_s(friendInfo.username, itrAddedMe->second.username, _TRUNCATE);
		}
	}
	// Adding (back) consumes any pending "Added you" prompt from that player.
	xlln_friends_added_me[user_index].erase(xuid);
	const std::string friendsFileContent = XllnFriendsSerialise_(user_index);

	LeaveCriticalSection(&xlln_critsec_friends);

	XllnFriendsSave_(user_index, friendsFileContent);

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s User %u added friend 0x%016I64x \"%s\"."
		, __func__
		, user_index
		, xuid
		, username ? username : ""
	);


	XllnFriendsSendFriendAdded_(user_index, xuid);

	return true;
}

bool XllnFriendsRemove(uint32_t user_index, XUID xuid)
{
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		return false;
	}

	bool removed = false;

	EnterCriticalSection(&xlln_critsec_friends);

	std::string friendsFileContent;
	if (xlln_friends[user_index].erase(xuid)) {
		removed = true;
		friendsFileContent = XllnFriendsSerialise_(user_index);
	}

	LeaveCriticalSection(&xlln_critsec_friends);

	if (removed) {
		XllnFriendsSave_(user_index, friendsFileContent);
	}

	if (removed) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
			, "%s User %u removed friend 0x%016I64x."
			, __func__
			, user_index
			, xuid
		);

	}

	return removed;
}

bool XllnFriendsConsumeSentInviteToken(XUID invitee_xuid, uint32_t invite_token)
{
	if (!invite_token) {
		return false;
	}
	
	__time64_t ltime;
	_time64(&ltime);
	
	bool matched = false;
	EnterCriticalSection(&xlln_critsec_friends);
	
	for (size_t iInvite = 0; iInvite < xlln_sent_invites.size();) {
		if (xlln_sent_invites[iInvite].expiresAt <= ltime) {
			xlln_sent_invites.erase(xlln_sent_invites.begin() + iInvite);
			continue;
		}
		if (!matched
			&& xlln_sent_invites[iInvite].inviteToken == invite_token
			&& xlln_sent_invites[iInvite].inviteeXuid == invitee_xuid
		) {
			matched = true;
			xlln_sent_invites.erase(xlln_sent_invites.begin() + iInvite);
			continue;
		}
		iInvite++;
	}
	
	LeaveCriticalSection(&xlln_critsec_friends);
	
	return matched;
}

bool XllnFriendsIsFriend(uint32_t user_index, XUID xuid)
{
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		return false;
	}

	EnterCriticalSection(&xlln_critsec_friends);
	bool isFriend = xlln_friends[user_index].count(xuid) != 0;
	LeaveCriticalSection(&xlln_critsec_friends);

	return isFriend;
}

static void XllnFriendsSendFriendAdded_(uint32_t user_index, XUID added_xuid)
{
	if (!xlln_network_instance_base_port) {
		return;
	}

	uint32_t destinationInstanceId = 0;
	{
		EnterCriticalSection(&xlln_critsec_friends);

		auto itrPresence = xlln_remote_user_presence.find(added_xuid);
		if (itrPresence != xlln_remote_user_presence.end()) {
			destinationInstanceId = itrPresence->second.instanceId;
		}

		LeaveCriticalSection(&xlln_critsec_friends);
	}
	// A courtesy signal only for players who are online right now; there is no offline queue.
	if (!destinationInstanceId) {
		return;
	}

	const size_t packetSizeType = sizeof(XllnNetworkPacket::TYPE);
	const size_t packetSizeFriendAdded = sizeof(XllnNetworkPacket::FRIEND_ADDED);
	const size_t packetSizeTotal = packetSizeType + packetSizeFriendAdded;

	XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
	sendPacket->destinationInstanceId = destinationInstanceId;
	{
		uint8_t* sendData = new uint8_t[packetSizeTotal];
		sendData[0] = XllnNetworkPacket::TYPE::XLLN_NPT_FRIEND_ADDED;

		XllnNetworkPacket::FRIEND_ADDED &packetFriendAdded = *(XllnNetworkPacket::FRIEND_ADDED*)&sendData[packetSizeType];
		memset(&packetFriendAdded, 0, packetSizeFriendAdded);
		packetFriendAdded.adderInstanceId = xlln_global_instance_id;
		packetFriendAdded.adderXuid = xlive_local_users[user_index].xuid;
		strncpy_s(packetFriendAdded.adderUsername, xlive_local_users[user_index].username, _TRUNCATE);
		packetFriendAdded.addedXuid = added_xuid;

		sendPacket->data = sendData;
		sendPacket->dataSize = packetSizeTotal;
	}

	if (!SendPacketToRemoteInstance(sendPacket)) {
		delete sendPacket;
	}
	sendPacket = 0;
}

void XllnFriendsAddedReceived(uint32_t adder_instance_id, XUID adder_xuid, const char* adder_username, XUID added_xuid)
{
	if (!adder_xuid || !added_xuid) {
		return;
	}

	uint32_t userIndex = XUSER_INDEX_NONE;
	for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
		if (xlive_local_users[iUser].signin_state != eXUserSigninState_NotSignedIn && xlive_local_users[iUser].xuid == added_xuid) {
			userIndex = iUser;
			break;
		}
	}
	if (userIndex == XUSER_INDEX_NONE) {
		return;
	}

	bool alreadyFriend;
	{
		EnterCriticalSection(&xlln_critsec_friends);

		alreadyFriend = xlln_friends[userIndex].count(adder_xuid) != 0;
		if (!alreadyFriend) {
			XLLN_FRIEND_INFO &addedMe = xlln_friends_added_me[userIndex][adder_xuid];
			strncpy_s(addedMe.username, adder_username, _TRUNCATE);
		}

		LeaveCriticalSection(&xlln_critsec_friends);
	}

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s 0x%016I64x \"%s\" added local user %u (0x%016I64x) as a friend."
		, __func__
		, adder_xuid
		, adder_username
		, userIndex
		, added_xuid
	);

	OVERLAY_EVENT overlayEvent;
	overlayEvent.type = OverlayEventType::GENERIC;
	strncpy_s(overlayEvent.title, adder_username[0] ? adder_username : "A player", _TRUNCATE);
	strncpy_s(overlayEvent.detail, alreadyFriend ? "Added you as a friend too" : "Added you as a friend", _TRUNCATE);
	overlayEvent.xuid = adder_xuid;
	overlayEvent.remoteInstanceId = adder_instance_id;
	overlayEvent.localUserIndex = userIndex;
	OverlayEventQueuePush(overlayEvent);

}

// Whether xlln_overlay_toast_scope lets a PLAYER_ONLINE or PLAYER_OFFLINE toast for this player
// through. No other event type reaches here, so the scope never hides one.
// Must be called without xlln_critsec_friends held.
static bool XllnFriendsToastScopeAllows_(XUID xuid)
{
	if (xlln_overlay_toast_scope == XLLN_OVERLAY_TOAST_SCOPE_OFF) {
		return false;
	}
	if (xlln_overlay_toast_scope == XLLN_OVERLAY_TOAST_SCOPE_FRIENDS) {
		for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
			if (xlive_local_users[iUser].signin_state == eXUserSigninState_NotSignedIn) {
				continue;
			}
			if (XllnFriendsIsFriend(iUser, xuid)) {
				return true;
			}
		}
		return false;
	}
	return true;
}

void XllnFriendsPresenceReceived(uint32_t instance_id, XUID xuid, const char* username, uint32_t title_id, const char* title_name, const char* rich_presence, bool is_online)
{
	if (!xuid) {
		return;
	}
	// A user signed in here is never a remote peer, whatever instance id announced them.
	for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
		if (xlive_local_users[iUser].signin_state != eXUserSigninState_NotSignedIn
			&& xlive_local_users[iUser].xuid == xuid
		) {
			return;
		}
	}

	bool newPeer = false;

	__time64_t timeNow;
	_time64(&timeNow);

	EnterCriticalSection(&xlln_critsec_friends);

	char offlineUsername[XUSER_NAME_SIZE] = {0};
	if (!is_online) {
		auto itrPresence = xlln_remote_user_presence.find(xuid);
		// Only the instance that holds a XUID can sign it out. An instance that empties an entry it
		// does not hold makes its own next claim look like a first sighting, and so takes the address
		// this user's invites go to and the session key sent with them.
		if (itrPresence != xlln_remote_user_presence.end() && itrPresence->second.instanceId == instance_id) {
			strncpy_s(offlineUsername, itrPresence->second.username, _TRUNCATE);
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
				, "%s PRESENCE-ERASE xuid (0x%016I64x) instanceId (0x%08x)."
				, __func__, xuid, instance_id
			);
			xlln_remote_user_presence.erase(itrPresence);
		}
	}
	else {
		auto itrPresence = xlln_remote_user_presence.find(xuid);
		if (itrPresence == xlln_remote_user_presence.end()) {
			// The sender chooses this XUID, so a peer inventing one per packet must not be able to
			// displace the real entries.
			if (xlln_remote_user_presence.size() >= XLLN_REMOTE_USER_PRESENCE_MAX_COUNT) {
				LeaveCriticalSection(&xlln_critsec_friends);
				return;
			}
			newPeer = true;
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
				, "%s PRESENCE-NEWPEER xuid (0x%016I64x) instanceId (0x%08x) mapSize (%zu)."
				, __func__, xuid, instance_id, xlln_remote_user_presence.size()
			);
		}
		else if (itrPresence->second.instanceId != instance_id
			&& (uint64_t)itrPresence->second.timeOfLastContact > ((uint64_t)timeNow) - 15
		) {
			// XllnFriendsSendInvite addresses the session key from this map, so rebinding a XUID
			// that another instance still answers for would hand the key to whoever asked last.
			// An instance that holds one announces every broadcast cycle, so a holder quiet for
			// more than one and a half cycles has gone and the XUID is free to move. Its own
			// instance id changes when it comes back, and it must not wait out the whole timeout.
			const uint32_t instanceIdHolding = itrPresence->second.instanceId;

			LeaveCriticalSection(&xlln_critsec_friends);

			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
				, "%s instance (0x%08x) claims user 0x%016I64x, which instance (0x%08x) still holds."
				, __func__
				, instance_id
				, xuid
				, instanceIdHolding
			);
			return;
		}

		XLLN_REMOTE_USER_PRESENCE &presence = xlln_remote_user_presence[xuid];
		presence.instanceId = instance_id;
		strncpy_s(presence.username, username, _TRUNCATE);
		presence.titleId = title_id;
		strncpy_s(presence.titleName, title_name, _TRUNCATE);
		strncpy_s(presence.richPresence, rich_presence, _TRUNCATE);
		_time64(&presence.timeOfLastContact);

		// Keep the persisted last-known name of this friend up to date with what they call themselves now.
		if (username[0]) {
			for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
				auto itrFriend = xlln_friends[iUser].find(xuid);
				if (itrFriend == xlln_friends[iUser].end() || strcmp(itrFriend->second.username, username) == 0) {
					continue;
				}
				strncpy_s(itrFriend->second.username, username, _TRUNCATE);
				if (xlive_local_users[iUser].signin_state != eXUserSigninState_NotSignedIn) {
					xlln_friends_save_pending[iUser] = true;
				}
			}
		}
	}

	LeaveCriticalSection(&xlln_critsec_friends);

	if (is_online && username[0]) {
		EnterCriticalSection(&xlive_critsec_remote_user);

		auto itrRemoteUser = xlln_remote_user_usernames.find(xuid);
		if (itrRemoteUser == xlln_remote_user_usernames.end() || !itrRemoteUser->second || strcmp(itrRemoteUser->second, username) != 0) {
			if (itrRemoteUser != xlln_remote_user_usernames.end() && itrRemoteUser->second) {
				delete[] itrRemoteUser->second;
			}
			if (itrRemoteUser != xlln_remote_user_usernames.end() || xlln_remote_user_usernames.size() < XLLN_REMOTE_USER_USERNAMES_MAX_COUNT) {
				xlln_remote_user_usernames[xuid] = CloneString(username);
			}
		}

		LeaveCriticalSection(&xlive_critsec_remote_user);
	}

	// Introduce ourselves to a newly seen peer right away instead of waiting for the next
	// broadcast cycle; the peer might not be receiving our broadcasts at all. Only done on
	// first sight, so two instances cannot answer each other again and again.
	if (newPeer) {
		for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
			if (xlive_local_users[iUser].signin_state != eXUserSigninState_SignedInToLive) {
				continue;
			}
			XllnFriendsSendPresence_(iUser, true, instance_id, XllnFriendsIsFriend(iUser, xuid));
		}
	}

	// Overlay toasts: a peer's first presence and their clean signout. The sub-line
	// carries the sender-resolved "<game> · <status>" when available.
	if (newPeer && XllnFriendsToastScopeAllows_(xuid)) {
		OVERLAY_EVENT overlayEvent;
		overlayEvent.type = OverlayEventType::PLAYER_ONLINE;
		strncpy_s(overlayEvent.title, username[0] ? username : "Player", _TRUNCATE);
		if (title_name[0] && rich_presence[0]) {
			snprintf(overlayEvent.detail, sizeof(overlayEvent.detail), "%s \xC2\xB7 %s", title_name, rich_presence);
		}
		else if (title_name[0]) {
			snprintf(overlayEvent.detail, sizeof(overlayEvent.detail), "%s", title_name);
		}
		else {
			strncpy_s(overlayEvent.detail, "Online", _TRUNCATE);
		}
		overlayEvent.xuid = xuid;
		overlayEvent.remoteInstanceId = instance_id;
		OverlayEventQueuePush(overlayEvent);
	}
	else if (!is_online && offlineUsername[0] && XllnFriendsToastScopeAllows_(xuid)) {
		OVERLAY_EVENT overlayEvent;
		overlayEvent.type = OverlayEventType::PLAYER_OFFLINE;
		strncpy_s(overlayEvent.title, offlineUsername, _TRUNCATE);
		strncpy_s(overlayEvent.detail, "Went offline", _TRUNCATE);
		overlayEvent.xuid = xuid;
		overlayEvent.remoteInstanceId = instance_id;
		OverlayEventQueuePush(overlayEvent);
	}

}

void XllnFriendsPresenceRemoveStale()
{
	__time64_t ltime;
	_time64(&ltime);
	// Three and a half broadcast cycles. At one and a half a single dropped datagram evicted the peer,
	// which then reappeared on the next cycle as a new arrival - an offline and an online toast for a
	// peer that never went anywhere.
	uint64_t timeToRemove = ((uint64_t)ltime) - 35;

	std::vector<std::pair<XUID, std::string>> wentOffline;

	EnterCriticalSection(&xlln_critsec_friends);

	for (auto itrPresence = xlln_remote_user_presence.begin(); itrPresence != xlln_remote_user_presence.end();) {
		if ((uint64_t)itrPresence->second.timeOfLastContact <= timeToRemove) {
			if (itrPresence->second.username[0]) {
				wentOffline.push_back(std::make_pair(itrPresence->first, std::string(itrPresence->second.username)));
			}
			itrPresence = xlln_remote_user_presence.erase(itrPresence);
		}
		else {
			itrPresence++;
		}
	}

	std::string friendsFileContent[XLIVE_LOCAL_USER_COUNT];
	bool friendsFileWrite[XLIVE_LOCAL_USER_COUNT] = {};
	for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
		if (!xlln_friends_save_pending[iUser]) {
			continue;
		}
		xlln_friends_save_pending[iUser] = false;
		if (xlive_local_users[iUser].signin_state != eXUserSigninState_NotSignedIn) {
			friendsFileContent[iUser] = XllnFriendsSerialise_(iUser);
			friendsFileWrite[iUser] = true;
		}
	}

	std::set<XUID> stillPresent;
	for (auto const &itrPresence : xlln_remote_user_presence) {
		stillPresent.insert(itrPresence.first);
	}

	LeaveCriticalSection(&xlln_critsec_friends);

	for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
		if (friendsFileWrite[iUser]) {
			XllnFriendsSave_(iUser, friendsFileContent[iUser]);
		}
	}

	// The username cache is keyed by a XUID the network chose and nothing used to remove from it.
	// Only the names presence put there are swept. Voice, XLocator and the XSession join and search
	// paths each add a XUID with no name yet and never broadcast presence, so an absence from the
	// presence map does not mean gone for those.
	{
		EnterCriticalSection(&xlive_critsec_remote_user);

		for (auto itrRemoteUser = xlln_remote_user_usernames.begin(); itrRemoteUser != xlln_remote_user_usernames.end();) {
			if (!itrRemoteUser->second || stillPresent.count(itrRemoteUser->first)) {
				itrRemoteUser++;
				continue;
			}
			delete[] itrRemoteUser->second;
			itrRemoteUser = xlln_remote_user_usernames.erase(itrRemoteUser);
		}

		LeaveCriticalSection(&xlive_critsec_remote_user);
	}

	for (auto const &offlinePlayer : wentOffline) {
		if (!XllnFriendsToastScopeAllows_(offlinePlayer.first)) {
			continue;
		}
		OVERLAY_EVENT overlayEvent;
		overlayEvent.type = OverlayEventType::PLAYER_OFFLINE;
		strncpy_s(overlayEvent.title, offlinePlayer.second.c_str(), _TRUNCATE);
		strncpy_s(overlayEvent.detail, "Went offline", _TRUNCATE);
		overlayEvent.xuid = offlinePlayer.first;
		OverlayEventQueuePush(overlayEvent);
	}

}

void XllnFriendsPresenceClear()
{
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
		, "%s presence map cleared (size %zu), caller 0x%p."
		, __func__, xlln_remote_user_presence.size(), _ReturnAddress()
	);
	EnterCriticalSection(&xlln_critsec_friends);
	xlln_remote_user_presence.clear();
	LeaveCriticalSection(&xlln_critsec_friends);

}

// Sends this user's presence to one instance, or to the local network when destination_instance_id
// is 0. The rich status line is only ever filled in for a recipient allowed to read it; everyone
// else gets identity and title, which is what the Players list needs to offer them as a friend.
static void XllnFriendsSendPresence_(uint32_t user_index, bool is_online, uint32_t destination_instance_id, bool include_rich_presence)
{
	if (!xlln_network_instance_base_port) {
		return;
	}

	const size_t packetSizeType = sizeof(XllnNetworkPacket::TYPE);
	const size_t packetSizeUserPresence = sizeof(XllnNetworkPacket::USER_PRESENCE);

	XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
	sendPacket->destinationInstanceId = destination_instance_id ? destination_instance_id : INADDR_BROADCAST;
	sendPacket->skipHubServers = !destination_instance_id;
	{
		const size_t packetSizeTotal = packetSizeType + packetSizeUserPresence;

		uint8_t* sendData = new uint8_t[packetSizeTotal];
		sendData[0] = XllnNetworkPacket::TYPE::XLLN_NPT_USER_PRESENCE;

		XllnNetworkPacket::USER_PRESENCE &packetUserPresence = *(XllnNetworkPacket::USER_PRESENCE*)&sendData[packetSizeType];
		memset(&packetUserPresence, 0, packetSizeUserPresence);
		packetUserPresence.instanceId = xlln_global_instance_id;
		packetUserPresence.xuid = xlive_local_users[user_index].xuid;
		strncpy_s(packetUserPresence.username, xlive_local_users[user_index].username, _TRUNCATE);
		packetUserPresence.titleId = xlive_title_id;
		const char* spaTitleName = XllnSpaTitleNameUtf8();
		if (spaTitleName) {
			strncpy_s(packetUserPresence.titleName, spaTitleName, _TRUNCATE);
		}
		if (is_online && include_rich_presence) {
			XUserGetPresenceText(user_index, packetUserPresence.richPresence, sizeof(packetUserPresence.richPresence));
		}
		packetUserPresence.isOnline = is_online ? 1 : 0;

		sendPacket->data = sendData;
		sendPacket->dataSize = packetSizeTotal;
	}

	if (!SendPacketToRemoteInstance(sendPacket)) {
		delete sendPacket;
	}
	sendPacket = 0;
}

// Local-network discovery stays open: the broadcast leg reaches anyone on the subnet so they can
// see us in their Players list and offer us as a friend, but it carries no rich status and it skips
// hub servers, which would relay it to every client running this title worldwide.
// Addressed sends cover the rest. Friends get the full status line wherever they are. Instances only
// known through a hub get the lean packet, because nothing broadcasts over a hub - having their
// address at all means we searched, joined or played with them, and they would otherwise never be
// able to see us well enough to become friends.
static void XllnFriendsBroadcastPresence_(uint32_t user_index, bool is_online)
{
	XllnFriendsSendPresence_(user_index, is_online, 0, false);
	
	std::vector<uint32_t> friendInstanceIds;
	{
		EnterCriticalSection(&xlln_critsec_friends);
		
		for (auto const &itrPresence : xlln_remote_user_presence) {
			if (itrPresence.second.instanceId && xlln_friends[user_index].count(itrPresence.first)) {
				friendInstanceIds.push_back(itrPresence.second.instanceId);
			}
		}
		
		LeaveCriticalSection(&xlln_critsec_friends);
	}
	
	std::vector<uint32_t> hubInstanceIds;
	{
		EnterCriticalSection(&xlln_critsec_network_net_entity);
		
		for (auto const &itrNetEntity : xlln_net_entity_instance_id_to_remote_address) {
			if (!itrNetEntity.second.learntViaHub || !itrNetEntity.first || itrNetEntity.first == xlln_global_instance_id) {
				continue;
			}
			bool alreadyAFriend = false;
			for (uint32_t friendInstanceId : friendInstanceIds) {
				if (friendInstanceId == itrNetEntity.first) {
					alreadyAFriend = true;
					break;
				}
			}
			if (!alreadyAFriend) {
				hubInstanceIds.push_back(itrNetEntity.first);
			}
		}
		
		LeaveCriticalSection(&xlln_critsec_network_net_entity);
	}
	
	for (uint32_t instanceId : friendInstanceIds) {
		XllnFriendsSendPresence_(user_index, is_online, instanceId, true);
	}
	for (uint32_t instanceId : hubInstanceIds) {
		XllnFriendsSendPresence_(user_index, is_online, instanceId, false);
	}
}

void XllnFriendsBroadcastPresenceUser(uint32_t user_index, bool is_online)
{
	if (user_index >= XLIVE_LOCAL_USER_COUNT || !xlive_local_users[user_index].xuid) {
		return;
	}
	if (is_online && xlive_local_users[user_index].signin_state != eXUserSigninState_SignedInToLive) {
		return;
	}

	XllnFriendsBroadcastPresence_(user_index, is_online);
}

void XllnFriendsBroadcastPresence()
{
	for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
		if (xlive_local_users[iUser].signin_state != eXUserSigninState_SignedInToLive) {
			continue;
		}
		XllnFriendsBroadcastPresence_(iUser, true);
	}
}

// Chooses which of the local Title's sessions an invite is about. A Title can host a lobby and a
// match at the same time, and only a session presence advertises can carry an invite, so the order
// is a joinable presence session, then any presence session, then a hosted session without one.
// That last is no use to an invitee, but it lets the caller say so rather than report no session.
// Both xlln_critsec_liveoverlan_broadcast and xlive_critsec_xsession must be held.
static LIVE_SESSION* XllnFriendsPickLocalSession_(XNKID* result_joined_session_id, bool* result_member_of_session)
{
	if (xlive_xlocator_local_session) {
		return xlive_xlocator_local_session;
	}

	LIVE_SESSION* sessionWithPresence = 0;
	LIVE_SESSION* sessionWithoutPresence = 0;
	for (auto const &entry : xlive_xsession_local_sessions) {
		LIVE_SESSION* liveSession = entry.second->liveSession;
		if (liveSession->sessionType != XLLN_LIVEOVERLAN_SESSION_TYPE_XSESSION) {
			continue;
		}
		if (!(liveSession->sessionFlags & XSESSION_CREATE_HOST)) {
			if (!*result_member_of_session) {
				*result_joined_session_id = liveSession->xnkid;
				*result_member_of_session = true;
			}
			continue;
		}
		if (liveSession->sessionFlags & XSESSION_CREATE_USES_PRESENCE) {
			if (LiveOverLanIsSessionJoinable(liveSession)) {
				return liveSession;
			}
			if (!sessionWithPresence) {
				sessionWithPresence = liveSession;
			}
		}
		else if (!sessionWithoutPresence) {
			sessionWithoutPresence = liveSession;
		}
	}

	return sessionWithPresence ? sessionWithPresence : sessionWithoutPresence;
}

// Reports whether a chosen session is one an invite can be sent from at all.
static bool XllnFriendsSessionCarriesInvite_(const LIVE_SESSION* live_session)
{
	if (!live_session) {
		return false;
	}
	// XLocator sessions carry XLOCATOR_SERVERTYPE_ values in sessionFlags, not XSESSION_CREATE_ ones.
	if (live_session->sessionType != XLLN_LIVEOVERLAN_SESSION_TYPE_XSESSION) {
		return true;
	}
	return !!(live_session->sessionFlags & XSESSION_CREATE_USES_PRESENCE);
}

// Serialise the session an invitee should join. When we are only a member of someone else's session,
// forward the host's own advertisement (matched by session id) so the invitee learns the host's
// identity (instanceId, XNKID, XNKEY) rather than ours.
static bool XllnFriendsSerialiseCurrentSession_(uint8_t** result_buffer, size_t* result_buffer_size)
{
	*result_buffer = 0;
	*result_buffer_size = 0;

	XNKID joinedSessionId;
	bool memberOfSession = false;

	{
		EnterCriticalSection(&xlln_critsec_liveoverlan_broadcast);
		// Guards the local sessions read below; XCloseHandle frees them from the Title's thread.
		EnterCriticalSection(&xlive_critsec_xsession);

		LIVE_SESSION* localSession = XllnFriendsPickLocalSession_(&joinedSessionId, &memberOfSession);
		if (XllnFriendsSessionCarriesInvite_(localSession)) {
			LiveOverLanSerialiseLiveSessionIntoNetPacket(localSession, result_buffer, result_buffer_size);
		}

		LeaveCriticalSection(&xlive_critsec_xsession);
		LeaveCriticalSection(&xlln_critsec_liveoverlan_broadcast);
	}

	if (*result_buffer) {
		return true;
	}
	if (!memberOfSession) {
		return false;
	}

	{
		EnterCriticalSection(&xlln_critsec_liveoverlan_sessions);

		for (auto const &itrRemoteSession : liveoverlan_remote_sessions_xsession) {
			if (memcmp(&itrRemoteSession.second->liveSession->xnkid, &joinedSessionId, sizeof(XNKID)) == 0) {
				LiveOverLanSerialiseLiveSessionIntoNetPacket(itrRemoteSession.second->liveSession, result_buffer, result_buffer_size);
				break;
			}
		}

		LeaveCriticalSection(&xlln_critsec_liveoverlan_sessions);
	}

	return *result_buffer != 0;
}

bool XllnSessionCanInvite(char* result_reason, size_t result_reason_size)
{
	if (result_reason && result_reason_size) {
		result_reason[0] = 0;
	}

	// The session an invite would be about, but only the fields the gate needs are snapshotted.
	// When we are merely a member of someone else's session, the host's advertise is the
	// authority on flags and slot fill.
	bool sessionFound = false;
	uint8_t sessionType = 0;
	uint32_t sessionFlags = 0;
	size_t slotsFree = 0;

	XNKID joinedSessionId;
	bool memberOfSession = false;

	{
		EnterCriticalSection(&xlln_critsec_liveoverlan_broadcast);
		// Guards the local sessions read below; XCloseHandle frees them from the Title's thread.
		EnterCriticalSection(&xlive_critsec_xsession);

		LIVE_SESSION* localSession = XllnFriendsPickLocalSession_(&joinedSessionId, &memberOfSession);
		if (localSession) {
			sessionFound = true;
			sessionType = localSession->sessionType;
			sessionFlags = localSession->sessionFlags;
			slotsFree = LiveOverLanSessionSlotsFree(localSession);
		}

		LeaveCriticalSection(&xlive_critsec_xsession);
		LeaveCriticalSection(&xlln_critsec_liveoverlan_broadcast);
	}

	if (!sessionFound && memberOfSession) {
		EnterCriticalSection(&xlln_critsec_liveoverlan_sessions);

		for (auto const &itrRemoteSession : liveoverlan_remote_sessions_xsession) {
			if (memcmp(&itrRemoteSession.second->liveSession->xnkid, &joinedSessionId, sizeof(XNKID)) == 0) {
				sessionFound = true;
				sessionType = itrRemoteSession.second->liveSession->sessionType;
				sessionFlags = itrRemoteSession.second->liveSession->sessionFlags;
				slotsFree = LiveOverLanSessionSlotsFree(itrRemoteSession.second->liveSession);
				break;
			}
		}

		LeaveCriticalSection(&xlln_critsec_liveoverlan_sessions);
	}

	const char* reason = 0;
	if (!sessionFound) {
		reason = "Host or join a session first";
	}
	// XLocator sessions carry XLOCATOR_SERVERTYPE_ values in sessionFlags, not XSESSION_CREATE_ modifiers.
	else if (sessionType == XLLN_LIVEOVERLAN_SESSION_TYPE_XSESSION && !(sessionFlags & XSESSION_CREATE_USES_PRESENCE)) {
		// Presence is what carries a session to the invitee, so a session without it has no invite
		// to send. A Title that runs a lobby beside its match advertises presence on the lobby.
		reason = "This session cannot be joined from an invite";
	}
	else if (sessionType == XLLN_LIVEOVERLAN_SESSION_TYPE_XSESSION && (sessionFlags & XSESSION_CREATE_INVITES_DISABLED)) {
		reason = "Invites are disabled for this session";
	}
	else if (!slotsFree) {
		reason = "The session is full";
	}

	if (reason) {
		if (result_reason && result_reason_size) {
			strncpy_s(result_reason, result_reason_size, reason, _TRUNCATE);
		}
		return false;
	}

	return true;
}

static char xlln_friends_presence_last_sent[XLIVE_LOCAL_USER_COUNT][128] = {};
static __time64_t xlln_friends_presence_last_sent_at[XLIVE_LOCAL_USER_COUNT] = {};

void XllnFriendsUserPresenceMaybeChanged(uint32_t user_index)
{
	if (user_index >= XLIVE_LOCAL_USER_COUNT || !xlln_network_instance_base_port) {
		return;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		return;
	}

	char presenceText[128] = {0};
	XUserGetPresenceText(user_index, presenceText, sizeof(presenceText));

	bool sendNow = false;
	{
		EnterCriticalSection(&xlln_critsec_friends);

		__time64_t ltime;
		_time64(&ltime);
		if (strcmp(xlln_friends_presence_last_sent[user_index], presenceText) != 0
			&& ltime - xlln_friends_presence_last_sent_at[user_index] >= 2
		) {
			strncpy_s(xlln_friends_presence_last_sent[user_index], presenceText, _TRUNCATE);
			xlln_friends_presence_last_sent_at[user_index] = ltime;
			sendNow = true;
		}

		LeaveCriticalSection(&xlln_critsec_friends);
	}
	// Anything suppressed by the rate limit still goes out with the regular presence cycle.
	if (sendNow) {
		XllnFriendsBroadcastPresenceUser(user_index, true);
	}
}

uint32_t XllnFriendsSendInvite(uint32_t user_index, size_t invite_xuid_count, const XUID* invite_xuids, const wchar_t* invite_message, size_t* result_sent_count)
{
	if (result_sent_count) {
		*result_sent_count = 0;
	}
	if (user_index >= XLIVE_LOCAL_USER_COUNT || !invite_xuid_count || !invite_xuids) {
		return ERROR_INVALID_PARAMETER;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!xlln_network_instance_base_port) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s XLLN networking is disabled."
			, __func__
		);
		return ERROR_NETWORK_UNREACHABLE;
	}

	char cannotInviteReason[64];
	if (!XllnSessionCanInvite(cannotInviteReason, sizeof(cannotInviteReason))) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s User %u cannot send invites: %s."
			, __func__
			, user_index
			, cannotInviteReason
		);
		return ERROR_ACCESS_DENIED;
	}

	uint8_t* liveSessionSerialisedPacket = 0;
	size_t liveSessionSerialisedPacketSize = 0;
	if (!XllnFriendsSerialiseCurrentSession_(&liveSessionSerialisedPacket, &liveSessionSerialisedPacketSize)) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s User %u has no session to invite to."
			, __func__
			, user_index
		);
		return ERROR_NOT_FOUND;
	}

	const size_t packetSizeType = sizeof(XllnNetworkPacket::TYPE);
	const size_t packetSizeFriendInvite = sizeof(XllnNetworkPacket::FRIEND_INVITE);
	const size_t packetSize = packetSizeType + packetSizeFriendInvite + (liveSessionSerialisedPacketSize - packetSizeType);

	size_t sentCount = 0;

	for (size_t iXuid = 0; iXuid < invite_xuid_count; iXuid++) {
		XUID inviteeXuid = invite_xuids[iXuid];

		uint32_t destinationInstanceId = 0;
		{
			EnterCriticalSection(&xlln_critsec_friends);

			auto itrPresence = xlln_remote_user_presence.find(inviteeXuid);
			if (itrPresence != xlln_remote_user_presence.end()) {
				destinationInstanceId = itrPresence->second.instanceId;
			}

			LeaveCriticalSection(&xlln_critsec_friends);
		}

		if (!destinationInstanceId) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
				, "%s Cannot invite 0x%016I64x, that user is not online."
				, __func__
				, inviteeXuid
			);
			continue;
		}

		XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
		sendPacket->destinationInstanceId = destinationInstanceId;
		{
			uint8_t* packetBuffer = new uint8_t[packetSize];
			packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_FRIEND_INVITE;

			XllnNetworkPacket::FRIEND_INVITE &friendInvite = *(XllnNetworkPacket::FRIEND_INVITE*)&packetBuffer[packetSizeType];
			memset(&friendInvite, 0, packetSizeFriendInvite);
			friendInvite.inviterInstanceId = xlln_global_instance_id;
			friendInvite.inviterXuid = xlive_local_users[user_index].xuid;
			strncpy_s(friendInvite.inviterUsername, xlive_local_users[user_index].username, _TRUNCATE);
			friendInvite.inviteeXuid = inviteeXuid;
			friendInvite.titleId = xlive_title_id;
			{
				EnterCriticalSection(&xlln_critsec_friends);
				while (!(friendInvite.inviteToken = xlln_pending_invite_next_token++));
				__time64_t ltime;
				_time64(&ltime);
				XLLN_SENT_INVITE sentInvite;
				sentInvite.inviteToken = friendInvite.inviteToken;
				sentInvite.inviteeXuid = inviteeXuid;
				sentInvite.expiresAt = ltime + XLLN_PENDING_INVITE_LIFETIME_SECONDS;
				xlln_sent_invites.push_back(sentInvite);
				LeaveCriticalSection(&xlln_critsec_friends);
			}
			if (invite_message && *invite_message) {
				wcsncpy_s(friendInvite.inviteMessage, invite_message, _TRUNCATE);
			}

			memcpy(&packetBuffer[packetSizeType + packetSizeFriendInvite], &liveSessionSerialisedPacket[packetSizeType], liveSessionSerialisedPacketSize - packetSizeType);

			sendPacket->data = packetBuffer;
			sendPacket->dataSize = packetSize;
		}

		if (SendPacketToRemoteInstance(sendPacket)) {
			sentCount++;
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
				, "%s User %u invited 0x%016I64x on instance 0x%08x."
				, __func__
				, user_index
				, inviteeXuid
				, destinationInstanceId
			);
		}
		else {
			delete sendPacket;
		}
		sendPacket = 0;
	}

	delete[] liveSessionSerialisedPacket;
	liveSessionSerialisedPacket = 0;

	if (result_sent_count) {
		*result_sent_count = sentCount;
	}

	return sentCount ? ERROR_SUCCESS : ERROR_NOT_FOUND;
}

// Requires xlln_critsec_friends to already be held.
static void XllnFriendsPruneExpiredInvites_()
{
	__time64_t ltime;
	_time64(&ltime);
	for (size_t iInvite = 0; iInvite < xlln_pending_invites.size();) {
		if (xlln_pending_invites[iInvite].expiresAt < ltime) {
			if (xlln_pending_invites[iInvite].liveSession) {
				LiveOverLanDestroyLiveSession(&xlln_pending_invites[iInvite].liveSession);
			}
			xlln_pending_invites.erase(xlln_pending_invites.begin() + iInvite);
		}
		else {
			iInvite++;
		}
	}
}

// Tells the inviter what happened so their UI can surface it.
static void XllnFriendsSendInviteResponse_(const XLLN_PENDING_INVITE &invite, bool accepted)
{
	if (!invite.inviterInstanceId || !xlln_network_instance_base_port) {
		return;
	}

	XUID inviteeXuid = 0;
	char inviteeUsername[XUSER_NAME_SIZE] = {0};
	EnterCriticalSection(&xlive_critsec_local_user);
	if (invite.userIndex < XLIVE_LOCAL_USER_COUNT && xlive_local_users[invite.userIndex].signin_state != eXUserSigninState_NotSignedIn) {
		inviteeXuid = xlive_local_users[invite.userIndex].xuid;
		strncpy_s(inviteeUsername, xlive_local_users[invite.userIndex].username, _TRUNCATE);
	}
	LeaveCriticalSection(&xlive_critsec_local_user);
	if (!inviteeXuid) {
		return;
	}

	const size_t packetSizeType = sizeof(XllnNetworkPacket::TYPE);
	const size_t packetSizeFriendInviteResponse = sizeof(XllnNetworkPacket::FRIEND_INVITE_RESPONSE);
	const size_t packetSize = packetSizeType + packetSizeFriendInviteResponse;

	XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
	sendPacket->destinationInstanceId = invite.inviterInstanceId;

	uint8_t* packetBuffer = new uint8_t[packetSize];
	packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_FRIEND_INVITE_RESPONSE;
	XllnNetworkPacket::FRIEND_INVITE_RESPONSE &inviteResponse = *(XllnNetworkPacket::FRIEND_INVITE_RESPONSE*)&packetBuffer[packetSizeType];
	memset(&inviteResponse, 0, packetSizeFriendInviteResponse);
	inviteResponse.inviteeInstanceId = xlln_global_instance_id;
	inviteResponse.inviterXuid = invite.inviterXuid;
	inviteResponse.inviteeXuid = inviteeXuid;
	memcpy(inviteResponse.inviteeUsername, inviteeUsername, sizeof(inviteResponse.inviteeUsername));
	inviteResponse.inviteToken = invite.inviterInviteToken;
	inviteResponse.accepted = accepted ? 1 : 0;

	sendPacket->data = packetBuffer;
	sendPacket->dataSize = packetSize;

	if (!SendPacketToRemoteInstance(sendPacket)) {
		delete sendPacket;
	}
}

// Tells whether the instance that sent an invite is the one that announced the inviter's XUID.
// The XUID in the packet is chosen by the sender, and friend XUIDs travel in clear presence
// packets, so without this a listener could name a friend and be auto-accepted as that friend.
static bool XllnFriendsInviterIsBoundToInstance_(XUID inviter_xuid, uint32_t inviter_instance_id)
{
	bool bound = false;

	EnterCriticalSection(&xlln_critsec_friends);

	auto itrPresence = xlln_remote_user_presence.find(inviter_xuid);
	if (itrPresence != xlln_remote_user_presence.end()) {
		bound = itrPresence->second.instanceId == inviter_instance_id;
	}

	LeaveCriticalSection(&xlln_critsec_friends);

	return bound;
}

void XllnFriendsInviteReceived(uint32_t user_index, uint32_t inviter_instance_id, XUID inviter_xuid, const char* inviter_username, uint32_t title_id, uint32_t invite_token, const wchar_t* invite_message, LIVE_SESSION* live_session)
{
	uint32_t inviteToken = 0;
	{
		EnterCriticalSection(&xlln_critsec_friends);

		XllnFriendsPruneExpiredInvites_();

		// A newer invite from the same player replaces their previous one.
		for (size_t iInvite = 0; iInvite < xlln_pending_invites.size();) {
			if (xlln_pending_invites[iInvite].inviterXuid == inviter_xuid && xlln_pending_invites[iInvite].userIndex == user_index) {
				if (xlln_pending_invites[iInvite].liveSession) {
					LiveOverLanDestroyLiveSession(&xlln_pending_invites[iInvite].liveSession);
				}
				xlln_pending_invites.erase(xlln_pending_invites.begin() + iInvite);
			}
			else {
				iInvite++;
			}
		}
		// Bound the queue by dropping the oldest.
		while (xlln_pending_invites.size() >= XLLN_PENDING_INVITE_MAX_COUNT) {
			if (xlln_pending_invites[0].liveSession) {
				LiveOverLanDestroyLiveSession(&xlln_pending_invites[0].liveSession);
			}
			xlln_pending_invites.erase(xlln_pending_invites.begin());
		}

		while (!(inviteToken = xlln_pending_invite_next_token++));

		XLLN_PENDING_INVITE pendingInvite;
		pendingInvite.userIndex = user_index;
		pendingInvite.inviteToken = inviteToken;
		pendingInvite.inviterInstanceId = inviter_instance_id;
		pendingInvite.inviterXuid = inviter_xuid;
		strncpy_s(pendingInvite.inviterUsername, inviter_username, _TRUNCATE);
		pendingInvite.titleId = title_id;
		pendingInvite.inviterInviteToken = invite_token;
		if (invite_message) {
			wcsncpy_s(pendingInvite.inviteMessage, invite_message, _TRUNCATE);
		}
		__time64_t ltime;
		_time64(&ltime);
		pendingInvite.expiresAt = ltime + XLLN_PENDING_INVITE_LIFETIME_SECONDS;
		pendingInvite.liveSession = live_session;
		xlln_pending_invites.push_back(pendingInvite);

		LeaveCriticalSection(&xlln_critsec_friends);
	}

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s User %u received a game invite from 0x%016I64x \"%s\" titleId (0x%08x)."
		, __func__
		, user_index
		, inviter_xuid
		, inviter_username
		, title_id
	);

	// Accepting hands the session to the running title, so an invite into a different game is
	// something we can show but never act on - there is no way to bring that game up from here.
	const bool inviteIsForThisTitle = (title_id == xlive_title_id);
	if (!inviteIsForThisTitle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
			, "%s The invite is for titleId (0x%08x) but this is titleId (0x%08x); it can be shown but not joined."
			, __func__
			, title_id
			, xlive_title_id
		);
	}
	if (inviteIsForThisTitle
		&& (xlln_friends_invite_auto_accept == 2
		|| (xlln_friends_invite_auto_accept == 1
			&& XllnFriendsIsFriend(user_index, inviter_xuid)
			&& XllnFriendsInviterIsBoundToInstance_(inviter_xuid, inviter_instance_id)))
	) {
		if (XllnFriendsInviteResolveByToken(inviteToken, true)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
				, "%s Auto-accepted the game invite from 0x%016I64x."
				, __func__
				, inviter_xuid
			);
			// There is no prompt to draw attention to, so at least give an audible cue that the title is about to join a session.
			MessageBeep(MB_ICONASTERISK);
			return;
		}
	}

	{
		OVERLAY_EVENT overlayEvent;
		overlayEvent.type = OverlayEventType::INVITE_RECEIVED;
		strncpy_s(overlayEvent.title, inviter_username, _TRUNCATE);
		// detail carries the inviter's message when they wrote one so the guide's invite
		// row/modal can show it; the toast sub-line is composed from the type either way.
		overlayEvent.detail[0] = 0;
		if (invite_message && invite_message[0]) {
			char inviteMessageUtf8[sizeof(overlayEvent.detail) * 3] = {0};
			WideCharToMultiByte(CP_UTF8, 0, invite_message, -1, inviteMessageUtf8, sizeof(inviteMessageUtf8) - 1, NULL, NULL);
			strncpy_s(overlayEvent.detail, inviteMessageUtf8, _TRUNCATE);
		}
		if (!overlayEvent.detail[0]) {
			strncpy_s(overlayEvent.detail, "Invited you to play", _TRUNCATE);
		}
		overlayEvent.xuid = inviter_xuid;
		overlayEvent.inviteToken = inviteToken;
		overlayEvent.titleId = title_id;
		overlayEvent.remoteInstanceId = inviter_instance_id;
		overlayEvent.localUserIndex = user_index;
		OverlayEventQueuePush(overlayEvent);
	}

	// The window must show itself from its own thread; ShowWindow from another thread is unreliable.
}

bool XllnFriendsGetPendingInvite(XLLN_PENDING_INVITE* result_invite)
{
	if (!result_invite) {
		return false;
	}

	EnterCriticalSection(&xlln_critsec_friends);

	XllnFriendsPruneExpiredInvites_();

	bool pending = !xlln_pending_invites.empty();
	if (pending) {
		*result_invite = xlln_pending_invites.back();
		// The session stays owned by the pending invite store.
		result_invite->liveSession = 0;
	}

	LeaveCriticalSection(&xlln_critsec_friends);

	return pending;
}

bool XllnFriendsInviteResolveByToken(uint32_t invite_token, bool accepted, bool* result_accept_refused)
{
	if (result_accept_refused) {
		*result_accept_refused = false;
	}
	if (!invite_token) {
		return false;
	}

	XLLN_PENDING_INVITE resolvedInvite;
	{
		EnterCriticalSection(&xlln_critsec_friends);

		XllnFriendsPruneExpiredInvites_();

		size_t iFound = xlln_pending_invites.size();
		for (size_t iInvite = 0; iInvite < xlln_pending_invites.size(); iInvite++) {
			if (xlln_pending_invites[iInvite].inviteToken == invite_token) {
				iFound = iInvite;
				break;
			}
		}
		if (iFound == xlln_pending_invites.size()) {
			LeaveCriticalSection(&xlln_critsec_friends);
			return false;
		}
		resolvedInvite = xlln_pending_invites[iFound];
		xlln_pending_invites.erase(xlln_pending_invites.begin() + iFound);

		LeaveCriticalSection(&xlln_critsec_friends);
	}

	// An invite into a different game can be shown, but accepting it would arm a join with that
	// game's session and key and raise XN_LIVE_INVITE_ACCEPTED inside this one. The auto-accept path
	// checks the Title before it calls here; an accept the player makes arrives without a check.
	if (accepted && resolvedInvite.titleId != xlive_title_id) {
		accepted = false;
		if (result_accept_refused) {
			*result_accept_refused = true;
		}

		OverlayEventQueuePush(OverlayEventType::GENERIC, resolvedInvite.inviterUsername, "That invite is for a different game");

		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
			, "%s User %u cannot accept the game invite from 0x%016I64x: it is for titleId (0x%08x) but this is titleId (0x%08x)."
			, __func__
			, resolvedInvite.userIndex
			, resolvedInvite.inviterXuid
			, resolvedInvite.titleId
			, xlive_title_id
		);
	}

	bool joined = false;
	bool refusedNoRoom = false;
	if (accepted
		&& resolvedInvite.liveSession
		&& resolvedInvite.userIndex < XLIVE_LOCAL_USER_COUNT
		&& xlive_local_users[resolvedInvite.userIndex].signin_state != eXUserSigninState_NotSignedIn
	) {
		LIVE_SESSION* liveSession = resolvedInvite.liveSession;

		// The invite's embedded session is a send-time snapshot; prefer the freshest advertise
		// from the same host and refuse the join when the session has since filled, ended, or
		// gone in-game with join-in-progress disabled. The title's own join path stays the
		// final authority; this only prevents arming a join that cannot succeed.
		bool sessionHasRoom;
		{
			EnterCriticalSection(&xlln_critsec_liveoverlan_sessions);

			LIVE_SESSION* liveSessionAdvertised = 0;
			if (liveSession->sessionType == XLLN_LIVEOVERLAN_SESSION_TYPE_XLOCATOR) {
				auto const &itrRemoteSession = liveoverlan_remote_sessions_xlocator.find(liveSession->instanceId);
				if (itrRemoteSession != liveoverlan_remote_sessions_xlocator.end()
					&& memcmp(&itrRemoteSession->second->liveSession->xnkid, &liveSession->xnkid, sizeof(XNKID)) == 0
				) {
					liveSessionAdvertised = itrRemoteSession->second->liveSession;
				}
			}
			else {
				const LIVE_SESSION_REMOTE_KEY sessionKey(liveSession->instanceId, *(uint64_t*)&liveSession->xnkid);
				auto const &itrRemoteSession = liveoverlan_remote_sessions_xsession.find(sessionKey);
				if (itrRemoteSession != liveoverlan_remote_sessions_xsession.end()) {
					liveSessionAdvertised = itrRemoteSession->second->liveSession;
				}
			}
			sessionHasRoom = LiveOverLanSessionHasRoom(liveSessionAdvertised ? liveSessionAdvertised : liveSession);

			LeaveCriticalSection(&xlln_critsec_liveoverlan_sessions);
		}

		if (!sessionHasRoom) {
			refusedNoRoom = true;

			OverlayEventQueuePush(OverlayEventType::GENERIC, resolvedInvite.inviterUsername, "Their session is full or has closed");

			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
				, "%s User %u accepted the game invite from 0x%016I64x but the session has no room."
				, __func__
				, resolvedInvite.userIndex
				, resolvedInvite.inviterXuid
			);
		}
		else {
			// Feed the same mechanism Direct IP Connect uses so XInviteGetAcceptedInfo and a subsequent
			// XSessionSearchByID find the host's session.
			xlln_direct_ip_connect.joinRequestSignature = 0;
			xlln_direct_ip_connect.timeoutAt = 0;
			xlln_direct_ip_connect.localPlayerId = resolvedInvite.userIndex;
			xlln_direct_ip_connect.remoteInstanceId = liveSession->instanceId;
			xlln_direct_ip_connect.remoteXuid = resolvedInvite.inviterXuid;
			xlln_direct_ip_connect.remoteTitleId = resolvedInvite.titleId;
			xlln_direct_ip_connect.remoteSessionId = liveSession->xnkid;
			xlln_direct_ip_connect.remoteKeyExchangeKey = liveSession->xnkey;

			LiveOverLanAddRemoteLiveSession(liveSession->sessionType, liveSession);

			XLiveNotifyAddEvent(XN_LIVE_INVITE_ACCEPTED, resolvedInvite.userIndex);

			OverlayEventQueuePush(OverlayEventType::JOINING_SESSION, resolvedInvite.inviterUsername, "Joining their session");

			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
				, "%s User %u accepted the game invite from 0x%016I64x, host instance 0x%08x."
				, __func__
				, resolvedInvite.userIndex
				, resolvedInvite.inviterXuid
				, liveSession->instanceId
			);

			joined = true;
		}
	}
	if (!joined && resolvedInvite.liveSession) {
		LiveOverLanDestroyLiveSession(&resolvedInvite.liveSession);
	}

	XllnFriendsSendInviteResponse_(resolvedInvite, joined);


	// False only when the token was unknown or expired; a room-refused accept still resolved
	// the invite and its toast already explained the outcome.
	return accepted ? (joined || refusedNoRoom) : true;
}
