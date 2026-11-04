#pragma once
#include "xsocket.hpp"
#include <map>
#include <utility>

#define XLLN_LIVEOVERLAN_SESSION_TYPE_XLOCATOR 0
#define XLLN_LIVEOVERLAN_SESSION_TYPE_XSESSION 1

// The advertise carried no state: an older instance sent it, or the session is XLocator type, which has no state.
#define XLLN_LIVEOVERLAN_SESSION_STATE_UNKNOWN 0xFF

// The session state is packed into the spare bits of sessionFlags because an older instance drops
// an advertise whose serialised length grew. The stored value is the XSESSION_STATE plus one, so
// zero means no state was carried.
#define XLLN_LIVEOVERLAN_SESSION_FLAGS_MASK 0x00000FFF
#define XLLN_LIVEOVERLAN_SESSION_STATE_SHIFT 12
#define XLLN_LIVEOVERLAN_SESSION_STATE_MASK 0x0000F000

#pragma pack(push, 1) // Save then set byte alignment setting.

typedef struct
{
	uint32_t                               propertyId;
	uint8_t                                type;

	union
	{
		LONG                            nData;     // XUSER_DATA_TYPE_INT32
		LONGLONG                        i64Data;   // XUSER_DATA_TYPE_INT64
		double                          dblData;   // XUSER_DATA_TYPE_DOUBLE
		struct                                     // XUSER_DATA_TYPE_UNICODE
		{
			size_t                       cbData;    // Includes null-terminator
			// The address of this variable is the location of the string not the value of this variable (since it is serialised).
			LPWSTR                      pwszData;
		} string;
		FLOAT                           fData;     // XUSER_DATA_TYPE_FLOAT
		struct                                     // XUSER_DATA_TYPE_BINARY
		{
			size_t                       cbData;
			// The address of this variable is the location of the string not the value of this variable (since it is serialised).
			PBYTE                       pbData;
		} binary;
		FILETIME                        ftData;    // XUSER_DATA_TYPE_DATETIME
	};
} XUSER_PROPERTY_SERIALISED;

typedef struct _LIVE_SESSION {
	uint32_t instanceId = 0;
	// serverID
	XUID xuid = INVALID_XUID;
	// XLLN_LIVEOVERLAN_SESSION_TYPE_XLOCATOR or XLLN_LIVEOVERLAN_SESSION_TYPE_XSESSION.
	uint8_t sessionType = 0;
	// If sessionType is XLLN_LIVEOVERLAN_SESSION_TYPE_XLOCATOR then this contains XLOCATOR_SERVERTYPE_ macros. Otherwise if XLLN_LIVEOVERLAN_SESSION_TYPE_XSESSION then XSESSION_CREATE_ macros.
	uint32_t sessionFlags = 0;
	// sessionID
	XNKID xnkid;
	// keyExchangeKey
	XNKEY xnkey;
	size_t slotsPublicMaxCount = 0;
	size_t slotsPublicFilledCount = 0;
	size_t slotsPrivateMaxCount = 0;
	size_t slotsPrivateFilledCount = 0;
	uint32_t contextsCount = 0;
	uint32_t propertiesCount = 0;
	// The TCP title port a peer has to dial to reach this instance's stream listener.
	// Network byte order. Zero means the instance had no stream listener when the session was
	// serialised, so the peer has no port to dial.
	uint16_t titlePortTcp = 0;
	XUSER_CONTEXT* pContexts = 0;
	XUSER_PROPERTY* pProperties = 0;
	// XSESSION_STATE, or XLLN_LIVEOVERLAN_SESSION_STATE_UNKNOWN. Must stay after the pointers: the
	// serialised static prefix ends at the pContexts offset, so a field above them changes the wire format.
	uint8_t sessionState = XLLN_LIVEOVERLAN_SESSION_STATE_UNKNOWN;
} LIVE_SESSION;

#pragma pack(pop) // Return to original alignment setting.

typedef struct {
	LIVE_SESSION* liveSession;
	// hostAddress
	XNADDR xnAddr;
	uint64_t timeOfLastContact;
} LIVE_SESSION_REMOTE;

// Identifies one advertised remote XSession. A Title hosts several sessions at once and advertises
// them all under the one instance, so the instance alone does not name a session.
typedef std::pair<uint32_t, uint64_t> LIVE_SESSION_REMOTE_KEY;

typedef struct _LIVE_SESSION_XSESSION {
	LIVE_SESSION* liveSession = 0;
	// hostAddress
	XNADDR xnAddr;
	uint32_t gameType = 0;
	uint32_t gameMode = 0;
	XSESSION_STATE eState = XSESSION_STATE_LOBBY;
	// Treating as XUID of the creator.
	uint64_t qwNonce = INVALID_XUID;
	XNKID xnkidArbitration;
	std::map<XUID, XSESSION_MEMBER*> sessionMembers;
	
	~_LIVE_SESSION_XSESSION()
	{
		for (const auto& itrSessionMember : sessionMembers) {
			XSESSION_MEMBER* sessionMember = itrSessionMember.second;
			delete sessionMember;
		}
		sessionMembers.clear();
	}
} LIVE_SESSION_XSESSION;

void LiveOverLanDestroyLiveSession(LIVE_SESSION** live_session);
bool LiveOverLanDeserialiseLiveSession(
	const uint8_t* live_session_buffer
	, const size_t live_session_buffer_size
	, LIVE_SESSION** result_live_session
);
bool LiveOverLanSerialiseLiveSessionIntoNetPacket(
	LIVE_SESSION* live_session
	, uint8_t** result_buffer
	, size_t* result_buffer_size
);
void LiveOverLanAdvertiseNow();
// Free slots left in the session, 0 when the filled count is above the max.
size_t LiveOverLanSessionSlotsFreePublic(const LIVE_SESSION* live_session);
size_t LiveOverLanSessionSlotsFreePrivate(const LIVE_SESSION* live_session);
size_t LiveOverLanSessionSlotsFree(const LIVE_SESSION* live_session);
// Reports whether the session can still admit a peer. An unknown session state does not refuse,
// so an advertise from an older instance stays usable.
bool LiveOverLanSessionHasRoom(const LIVE_SESSION* live_session);
// Reports whether presence allows a join, then defers to LiveOverLanSessionHasRoom.
// JOIN_VIA_PRESENCE_FRIENDS_ONLY still counts as joinable: a friends list is one-sided, so only
// the join path can enforce it.
bool LiveOverLanIsSessionJoinable(const LIVE_SESSION* live_session);
bool LiveOverLanSessionPublicSlotsFree(const LIVE_SESSION* live_session);
// Finds an XSession the instance advertises, for a caller that has no session id. An instance can
// advertise more than one, so the first choice is a joinable session with a free public slot, then
// any joinable session, then the first the map holds.
// Call with xlln_critsec_liveoverlan_sessions held; the result is valid only while it stays held.
LIVE_SESSION* LiveOverLanFindRemoteXSessionByInstanceId(const uint32_t instance_id);
void LiveOverLanBroadcastLocalSessionUnadvertise(const XUID xuid);
void LiveOverLanBroadcastLocalXSessionUnadvertise(const XNKID xnkid);
void LiveOverLanBroadcastRemoteSessionUnadvertise(const uint32_t instance_id, const uint8_t session_type, const XUID xuid, const uint64_t session_id);
void LiveOverLanAddRemoteLiveSession(const uint8_t session_type, LIVE_SESSION* live_session);
// The session id the instance last advertised, so a peer address round-trip answers with the id
// the title stored rather than one derived from the instance id.
bool LiveOverLanGetInstanceAdvertisedSessionId(const uint32_t instance_id, XNKID* xnkid, uint16_t* title_port_network = nullptr);

void XllnThreadLiveOverLanStart();
void XllnThreadLiveOverLanStop();
void XllnThreadLiveOverLanAbandon();

extern CRITICAL_SECTION xlln_critsec_liveoverlan_broadcast;

extern CRITICAL_SECTION xlln_critsec_liveoverlan_sessions;
extern std::map<uint32_t, LIVE_SESSION_REMOTE*> liveoverlan_remote_sessions_xlocator;
extern std::map<LIVE_SESSION_REMOTE_KEY, LIVE_SESSION_REMOTE*> liveoverlan_remote_sessions_xsession;
