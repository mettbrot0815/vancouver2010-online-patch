#include <winsock2.h>
#include <mswsock.h>
#include "../xlive/xdefs.hpp"
#include "xlln-network.hpp"
#include "xlln.hpp"
#include "xlln-friends.hpp"
#include "wnd-main.hpp"
#include "debug-log.hpp"
#include "../xlive/xlive.hpp"
#include "../xlive/live-over-lan.hpp"
#include "../xlive/xlocator.hpp"
#include "../xlive/xsession.hpp"
#include "../xlive/xnotify.hpp"
#include "../xlive/xnetqos.hpp"
#include "../utils/util-socket.hpp"
#include "../utils/utils.hpp"
#include "../utils/sha256.hpp"
#include "../overlay/overlay-events.hpp"
#include "../resource.h"
#include <ctime>
#include <list>

uint16_t XllnReserveUdpSessionPort_();

uint16_t XllnGetTitleDialPort_();

uint16_t XllnGetListeningUdpTitlePort_();

static bool xlln_thread_shutdown = true;
CRITICAL_SECTION xlln_critsec_network_send;
std::vector<XLLN_NET_SEND_PACKET_INFO*> xlln_network_send_queue;
HANDLE xlln_network_send_queue_notify = INVALID_HANDLE_VALUE;
static HANDLE xlln_thread_network_send_exited = INVALID_HANDLE_VALUE;

uint16_t xlln_network_instance_base_port = 39000;
uint16_t xlln_network_instance_port = 0;
HANDLE xlln_network_instance_port_mutex = INVALID_HANDLE_VALUE;

CRITICAL_SECTION xlln_critsec_network_broadcast_addresses;
std::vector<XllnNetworkBroadcastEntity::BROADCAST_ENTITY> xlln_network_broadcast_addresses;

static SOCKET xlln_network_socket_main = INVALID_SOCKET;
static SOCKET xlln_network_socket_multicast = INVALID_SOCKET;
CRITICAL_SECTION xlln_critsec_network_net_entity;
std::map<uint32_t, XLLN_NET_ENTITY> xlln_net_entity_instance_id_to_remote_address;
// The title UDP port this game instance actually listens on, as observed from any received title
// packet - including this instance's own looped-back broadcasts. Both instances of a title use the
// same port, which makes this a safe fallback for peers whose own traffic was never seen.
static uint16_t xlln_title_port_udp_observed = 0;
// Throttles the broadcast resolve so a title spamming an unreachable peer cannot flood the
// network with one ask per outbound packet. Guarded by xlln_critsec_network_net_entity.
static std::map<uint32_t, __time64_t> xlln_net_entity_unknown_instance_id_last_ask;

static std::vector<SOCKADDR_STORAGE> xlln_multicast_addresses;

HANDLE xlln_tcp_sockets_update = INVALID_HANDLE_VALUE;
static HANDLE xlln_thread_tcp_sockets_exited = INVALID_HANDLE_VALUE;

const char* XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE type)
{
	if (type >= XllnNetworkPacket::TYPE::XLLN_NPT_FIRST && type <= XllnNetworkPacket::TYPE::XLLN_NPT_LAST) {
		return XllnNetworkPacket::TYPE_NAMES[type];
	}
	
	return XllnNetworkPacket::TYPE_NAMES[XllnNetworkPacket::TYPE::XLLN_NPT_UNKNOWN];
}

const char* XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE response_code)
{
	if (response_code >= XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_FIRST && response_code <= XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_LAST) {
		return XllnNetworkPacket::TCP_RESPONSE_CODE_NAMES[response_code];
	}
	
	return XllnNetworkPacket::TCP_RESPONSE_CODE_NAMES[XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_UNKNOWN];
}

uint64_t XllnNetMachineIdFromInstanceId(uint32_t instance_id)
{
	return 0xFA00000000000000ULL | instance_id;
}

bool XllnNetEntityGetXnaddrXnkidByInstanceId(const uint32_t instanceId, XNADDR* xnaddr, XNKID* xnkid)
{
	if (!xnaddr && !xnkid) {
		return false;
	}
	if (xnaddr) {
		memset(xnaddr, 0, sizeof(XNADDR));
	}
	if (xnkid) {
		memset(xnkid, 0, sizeof(XNKID));
	}
	
	if (xnaddr) {
		xnaddr->inaOnline.s_addr = htonl(instanceId);
		xnaddr->ina.s_addr = htonl(instanceId);
		
		DWORD mac_fix = 0x00131000;
		
		memset(&(xnaddr->abEnet), 0, 6);
		memset(&(xnaddr->abOnline), 0, 6);
		
		memcpy(&(xnaddr->abEnet), &instanceId, 4);
		memcpy(&(xnaddr->abOnline), &instanceId, 4);
		
		memcpy((uint8_t*)&(xnaddr->abEnet) + 3, (uint8_t*)&mac_fix + 1, 3);
		memcpy((uint8_t*)&(xnaddr->abOnline) + 17, (uint8_t*)&mac_fix + 1, 3);
		
		// Bytes 8 to 16 are what XNetXnAddrToMachineId reads back.
		uint64_t machineId = XllnNetMachineIdFromInstanceId(instanceId);
		memcpy((uint8_t*)&(xnaddr->abOnline) + 8, &machineId, sizeof(machineId));
		
		// Publish the title UDP port the instance's game actually sends from. Titles build
		// their peer addressing from the XNADDR's port; with 0 the join request has no
		// destination and the join fails before any packet is sent.
		// Both instances of a title use the same title port, so the local game's own UDP
		// listener is a valid fallback when the remote's port was never observed (its title
		// traffic may not have reached us yet, or at all).
		uint16_t remoteTitlePort = 0;
		if (instanceId != xlln_global_instance_id) {
			EnterCriticalSection(&xlln_critsec_network_net_entity);
			auto itrNetEntityTitle = xlln_net_entity_instance_id_to_remote_address.find(instanceId);
			if (itrNetEntityTitle != xlln_net_entity_instance_id_to_remote_address.end()) {
				remoteTitlePort = itrNetEntityTitle->second.titlePortUdp;
			}
			LeaveCriticalSection(&xlln_critsec_network_net_entity);
			if (!remoteTitlePort) {
				remoteTitlePort = xlln_title_port_udp_observed;
			}
		}
		else {
			// Queries about THIS instance - e.g. a QOS response describing the local host - must
			// carry the port a peer can really reach, i.e. the port the game bound. Nothing records
			// the local instance in the remote-entity map, so without this the answer is port 0.
			// The boot-time reservation is only the fallback: a title that binds its session socket
			// to an explicit port would otherwise publish a port nothing listens on.
			remoteTitlePort = XllnGetTitleDialPort_();
		}
		xnaddr->wPortOnline = htons(remoteTitlePort);
	}
	
	if (xnkid) {
		// Derive a session XNKID that is unique per instance. Previously this was a constant
		// (0x8B in every byte for every instance), so a joining peer could not distinguish the
		// host's session identity from its own. Seed the id from the instanceId and keep the
		// ONLINE_PEER type flag.
		memset(xnkid, 0x8B, sizeof(XNKID));
		memcpy(&xnkid->ab[sizeof(XNKID) - sizeof(instanceId)], &instanceId, sizeof(instanceId));
		xnkid->ab[0] &= ~XNET_XNKID_MASK;
		xnkid->ab[0] |= XNET_XNKID_ONLINE_PEER;
	}

	return true;
}

// A session key must be unique per session, not per machine: one host can run concurrent sessions
// a peer has to tell apart. High bytes carry the instanceId (host identity), low bytes a counter.
void XllnGenerateSessionXnkid(XNKID* xnkid)
{
	if (!xnkid) {
		return;
	}

	static volatile LONG sessionCounter = 0;
	const uint32_t discriminator = (uint32_t)InterlockedIncrement(&sessionCounter);

	memset(xnkid, 0, sizeof(XNKID));
	memcpy(&xnkid->ab[1], &discriminator, sizeof(XNKID) - 1 - sizeof(xlln_global_instance_id));
	memcpy(&xnkid->ab[sizeof(XNKID) - sizeof(xlln_global_instance_id)], &xlln_global_instance_id, sizeof(xlln_global_instance_id));
	xnkid->ab[0] &= ~XNET_XNKID_MASK;
	xnkid->ab[0] |= XNET_XNKID_ONLINE_PEER;
}

// Cache (or refresh) the network address that an instance's packets are arriving from.
// Caches (or refreshes) the network address an instance's packets are arriving from, stamping the
// contact time so an instance that goes quiet can be aged out rather than addressed forever.
// Removes the control characters from a name a remote instance sent. The friends file holds one
// entry per line as "<XUID> = <name>", so a name carrying a line break would add a second entry
// that the next load reads back as a friend the player never added.
static void XllnRemoteNameSanitise_(char* name)
{
	size_t iOut = 0;
	for (size_t iIn = 0; name[iIn]; iIn++) {
		const unsigned char character = (unsigned char)name[iIn];
		if (character < 0x20 || character == 0x7F) {
			continue;
		}
		name[iOut++] = name[iIn];
	}
	name[iOut] = 0;
}

static const uint8_t XLLN_NET_ASSOCIATION_ASK_LIMIT = 15;

typedef struct _XLLN_NET_ASSOCIATION {
	XLLN_NET_ASSOCIATION_STATE state = XLLN_NET_ASSOCIATION_IDLE;
	uint8_t asksRemaining = 0;
	__time64_t nextAskAt = 0;
} XLLN_NET_ASSOCIATION;
// Guarded by xlln_critsec_network_net_entity.
static std::map<uint32_t, XLLN_NET_ASSOCIATION> xlln_net_associations;

bool XllnNetAssociationIsAskable(uint32_t instance_id)
{
	return instance_id && instance_id != xlln_global_instance_id && instance_id != INADDR_BROADCAST;
}

// Caller holds xlln_critsec_network_net_entity.
static bool XllnNetAssociationAdvance_(XLLN_NET_ASSOCIATION &association, __time64_t ltime)
{
	if (association.state != XLLN_NET_ASSOCIATION_PENDING_INITIATOR
		&& association.state != XLLN_NET_ASSOCIATION_PENDING_RESPONDER
	) {
		return false;
	}
	if (association.nextAskAt > ltime) {
		return false;
	}
	if (!association.asksRemaining) {
		association.state = XLLN_NET_ASSOCIATION_LOST;
		return false;
	}
	
	association.asksRemaining--;
	association.nextAskAt = ltime + 1;
	
	return association.state == XLLN_NET_ASSOCIATION_PENDING_INITIATOR;
}

// A title that never calls XNetConnect only polls, so the poll starts the exchange.
XLLN_NET_ASSOCIATION_STATE XllnNetAssociationPoll(uint32_t instance_id)
{
	// XLSP gives the title this instance's own id, and an id we never ask about cannot resolve.
	if (!XllnNetAssociationIsAskable(instance_id)) {
		return XLLN_NET_ASSOCIATION_CONNECTED;
	}
	
	bool ask = false;
	XLLN_NET_ASSOCIATION_STATE state;
	{
		EnterCriticalSection(&xlln_critsec_network_net_entity);
		
		__time64_t ltime;
		_time64(&ltime);
		
		XLLN_NET_ASSOCIATION &association = xlln_net_associations[instance_id];
		if (association.state == XLLN_NET_ASSOCIATION_IDLE) {
			association.state = XLLN_NET_ASSOCIATION_PENDING_INITIATOR;
			association.asksRemaining = XLLN_NET_ASSOCIATION_ASK_LIMIT;
		}
		ask = XllnNetAssociationAdvance_(association, ltime);
		state = association.state;
		
		LeaveCriticalSection(&xlln_critsec_network_net_entity);
	}
	
	// Enqueued outside the lock, because the send queue has its own critical section.
	if (ask) {
		XllnNetEntityBroadcastUnknownUserAsk(instance_id);
	}
	
	return state;
}

// When two asks cross, the higher instance id keeps the initiator role, so only one side
// connects first.
bool XllnNetAssociationAskReceived(uint32_t asker_instance_id)
{
	if (!XllnNetAssociationIsAskable(asker_instance_id)) {
		return false;
	}
	
	EnterCriticalSection(&xlln_critsec_network_net_entity);
	
	XLLN_NET_ASSOCIATION &association = xlln_net_associations[asker_instance_id];
	const bool reply = association.state != XLLN_NET_ASSOCIATION_PENDING_INITIATOR
		|| xlln_global_instance_id < asker_instance_id;
	if (reply && association.state != XLLN_NET_ASSOCIATION_CONNECTED) {
		association.state = XLLN_NET_ASSOCIATION_PENDING_RESPONDER;
		association.asksRemaining = XLLN_NET_ASSOCIATION_ASK_LIMIT;
	}
	
	LeaveCriticalSection(&xlln_critsec_network_net_entity);
	
	return reply;
}

// A reply finishes the exchange only for the instance that asked. One that crosses our own
// reply would otherwise connect both sides at once, which is what the ask order prevents.
bool XllnNetAssociationReplyCompletesExchange(uint32_t replier_instance_id)
{
	EnterCriticalSection(&xlln_critsec_network_net_entity);
	
	auto itrAssociation = xlln_net_associations.find(replier_instance_id);
	const bool completes = itrAssociation == xlln_net_associations.end()
		|| itrAssociation->second.state != XLLN_NET_ASSOCIATION_PENDING_RESPONDER;
	
	LeaveCriticalSection(&xlln_critsec_network_net_entity);
	
	return completes;
}

void XllnNetAssociationForget(uint32_t instance_id)
{
	EnterCriticalSection(&xlln_critsec_network_net_entity);
	
	xlln_net_associations.erase(instance_id);
	
	LeaveCriticalSection(&xlln_critsec_network_net_entity);
}

void XllnNetAssociationForgetLost(uint32_t instance_id)
{
	EnterCriticalSection(&xlln_critsec_network_net_entity);
	
	auto itrAssociation = xlln_net_associations.find(instance_id);
	if (itrAssociation != xlln_net_associations.end() && itrAssociation->second.state == XLLN_NET_ASSOCIATION_LOST) {
		xlln_net_associations.erase(itrAssociation);
	}
	
	LeaveCriticalSection(&xlln_critsec_network_net_entity);
}

void XllnNetAssociationForgetAll()
{
	EnterCriticalSection(&xlln_critsec_network_net_entity);
	
	xlln_net_associations.clear();
	
	LeaveCriticalSection(&xlln_critsec_network_net_entity);
}

// A title that stops polling still needs its ask repeated and its association timed out.
void XllnNetAssociationRetrySweep()
{
	std::vector<uint32_t> askInstanceIds;
	{
		EnterCriticalSection(&xlln_critsec_network_net_entity);
		
		__time64_t ltime;
		_time64(&ltime);
		
		for (auto &association : xlln_net_associations) {
			if (XllnNetAssociationAdvance_(association.second, ltime)) {
				askInstanceIds.push_back(association.first);
			}
		}
		
		LeaveCriticalSection(&xlln_critsec_network_net_entity);
	}
	
	for (uint32_t instanceId : askInstanceIds) {
		XllnNetEntityBroadcastUnknownUserAsk(instanceId);
	}
}

// An address that cannot be dialled must never displace a usable one: a peer that broadcasts
// on the loopback interface is seen with 127.0.0.1 as its source address and the last write
// wins, which would leave the cached peer address pointing at this very instance.
static bool XllnNetEntityAddressIsUnusable_(const SOCKADDR_STORAGE* sockaddr)
{
	if (!sockaddr || sockaddr->ss_family != AF_INET) {
		return true;
	}
	const uint32_t addressHBO = ntohl(((sockaddr_in*)sockaddr)->sin_addr.s_addr);
	if (addressHBO == INADDR_ANY) {
		return true;
	}
	if ((addressHBO >> 24) == 127) {
		return true; // Loopback.
	}
	if (addressHBO == INADDR_BROADCAST || (addressHBO >> 24) == 255) {
		return true; // Limited broadcast.
	}
	if ((addressHBO >> 28) == 0xE) {
		return true; // Multicast group address, never a host source.
	}
	return false;
}

static void XllnNetEntityUpdateRemoteAddress(const uint32_t instance_id, const SOCKADDR_STORAGE* sockaddr_external, const char* packet_name, bool via_hub = false, bool confirms_association = true)
{
	EnterCriticalSection(&xlln_critsec_network_net_entity);
	
	// Anything other than an ask proves the peer holds our address, which completes the exchange.
	if (confirms_association && XllnNetAssociationIsAskable(instance_id)) {
		XLLN_NET_ASSOCIATION &association = xlln_net_associations[instance_id];
		association.state = XLLN_NET_ASSOCIATION_CONNECTED;
		association.asksRemaining = 0;
		association.nextAskAt = 0;
	}
	
	XLLN_NET_ENTITY &netEntity = xlln_net_entity_instance_id_to_remote_address[instance_id];
	_time64(&netEntity.lastComm);
	if (via_hub) {
		netEntity.learntViaHub = true;
	}
	
	// An address that cannot be dialled must never displace a usable one: a peer that
	// broadcasts on the loopback interface is seen with 127.0.0.1 as its source address and
	// the last write wins, which would leave the cached address pointing at this very
	// instance.
	const bool addressUnusable = XllnNetEntityAddressIsUnusable_(sockaddr_external);
	if (netEntity.sockaddr.ss_family == AF_UNSPEC) {
		if (!addressUnusable) {
			netEntity.sockaddr = *sockaddr_external;
		}
	}
	else if (!SockAddrsMatch(&netEntity.sockaddr, sockaddr_external)) {
		if (addressUnusable) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
				, "%s %s packet instanceId remote address is not a usable address. Keeping the cached value."
				, __func__
				, packet_name
			);
		}
		else {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
				, "%s %s packet instanceId remote address different to cached value. Updating cache."
				, __func__
				, packet_name
			);
			netEntity.sockaddr = *sockaddr_external;
		}
	}
	
	LeaveCriticalSection(&xlln_critsec_network_net_entity);
}

// Forgets instances that have gone quiet. Without this the map only ever grows, and anything that
// addresses every known instance keeps sending to peers that left the network hours ago.
// The window is deliberately far wider than any advertise or presence interval, so an instance is
// only dropped once it is genuinely silent rather than merely between packets.
void XllnNetEntityRemoveStale()
{
	__time64_t ltime;
	_time64(&ltime);
	const __time64_t timeToRemove = ltime - 120;
	
	EnterCriticalSection(&xlln_critsec_network_net_entity);
	
	for (auto itrNetEntity = xlln_net_entity_instance_id_to_remote_address.begin(); itrNetEntity != xlln_net_entity_instance_id_to_remote_address.end();) {
		if (itrNetEntity->second.lastComm > timeToRemove) {
			++itrNetEntity;
			continue;
		}
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
			, "%s forgetting silent instanceId (0x%08x)."
			, __func__
			, itrNetEntity->first
		);
		// The association cannot outlive the address, or the status reports CONNECTED for an
		// instance the send path can no longer reach.
		xlln_net_associations.erase(itrNetEntity->first);
		itrNetEntity = xlln_net_entity_instance_id_to_remote_address.erase(itrNetEntity);
	}
	
	LeaveCriticalSection(&xlln_critsec_network_net_entity);
}

// Broadcasts a request for whichever instance owns instance_id_unknown to introduce itself, for
// a peer this instance has never received a packet from and so has no address for. The single
// reply teaches both ends: the asker is learnt from the ask, the owner from the reply.
void XllnNetEntityBroadcastUnknownUserAsk(const uint32_t instance_id_unknown)
{
	XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
	sendPacket->destinationInstanceId = INADDR_BROADCAST;
	{
		const size_t newPacketBufferUnknownUserAskSize = sizeof(XllnNetworkPacket::TYPE) + sizeof(XllnNetworkPacket::UNKNOWN_USER_ASK);
		uint8_t* newPacketBufferUnknownUserAsk = new uint8_t[newPacketBufferUnknownUserAskSize];
		newPacketBufferUnknownUserAsk[0] = XllnNetworkPacket::TYPE::XLLN_NPT_UNKNOWN_USER_ASK;
		
		// Placement over a raw buffer skips the struct's member initialisers; set every field.
		XllnNetworkPacket::UNKNOWN_USER_ASK* newPacketUnknownUserAsk = (XllnNetworkPacket::UNKNOWN_USER_ASK*)&newPacketBufferUnknownUserAsk[sizeof(XllnNetworkPacket::TYPE)];
		newPacketUnknownUserAsk->instanceId = xlln_global_instance_id;
		newPacketUnknownUserAsk->instanceIdUnknown = instance_id_unknown;
		
		sendPacket->data = newPacketBufferUnknownUserAsk;
		sendPacket->dataSize = newPacketBufferUnknownUserAskSize;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
		, "%s Broadcasting %s for instanceId (0x%08x)."
		, __func__
		, XllnNetworkPacket::TYPE_NAMES[XllnNetworkPacket::TYPE::XLLN_NPT_UNKNOWN_USER_ASK]
		, instance_id_unknown
	);
	
	if (!SendPacketToRemoteInstance(sendPacket)) {
		delete sendPacket;
	}
}

static bool ParseNetworkData(uint8_t* data_buffer, size_t data_size, SOCKADDR_STORAGE* sockaddr_external, bool via_hub = false)
{
	const size_t packetSizeType = sizeof(XllnNetworkPacket::TYPE);
	const size_t packetSizeTypeTitle = sizeof(XllnNetworkPacket::TITLE_PACKET);
	const size_t packetSizeTypeLolUnadvertise = sizeof(XllnNetworkPacket::LIVE_OVER_LAN_UNADVERTISE);
	const size_t packetSizeTypeQosRequest = sizeof(XllnNetworkPacket::QOS_REQUEST);
	const size_t packetSizeTypeQosResponse = sizeof(XllnNetworkPacket::QOS_RESPONSE);
	const size_t packetSizeTypeDirectIpRequest = sizeof(XllnNetworkPacket::DIRECT_IP_REQUEST);
	const size_t packetSizeTypeDirectIpResponse = sizeof(XllnNetworkPacket::DIRECT_IP_RESPONSE);
	const size_t packetSizeTypeHubRequest = sizeof(XllnNetworkPacket::HUB_REQUEST_PACKET);
	const size_t packetSizeTypeHubReply = sizeof(XllnNetworkPacket::HUB_REPLY_PACKET);
	const size_t packetSizeTypeForwarded = sizeof(XllnNetworkPacket::PACKET_FORWARDED);
	const size_t packetSizeTypeUnknownUserAsk = sizeof(XllnNetworkPacket::UNKNOWN_USER_ASK);
	const size_t packetSizeTypeUnknownUserReply = sizeof(XllnNetworkPacket::UNKNOWN_USER_REPLY);
	const size_t packetSizeTypeTcpConnectResponse = sizeof(XllnNetworkPacket::TCP_CONNECT_RESPONSE);
	const size_t packetSizeTypeTcpSequencedData = sizeof(XllnNetworkPacket::TCP_SEQUENCED_DATA);
	const size_t packetSizeTypeTcpSequencedDataAck = sizeof(XllnNetworkPacket::TCP_SEQUENCED_DATA_ACK);
	const size_t packetSizeTypeUserPresence = sizeof(XllnNetworkPacket::USER_PRESENCE);
	const size_t packetSizeTypeFriendInvite = sizeof(XllnNetworkPacket::FRIEND_INVITE);
	const size_t packetSizeTypeFriendInviteResponse = sizeof(XllnNetworkPacket::FRIEND_INVITE_RESPONSE);
	const size_t packetSizeTypeFriendAdded = sizeof(XllnNetworkPacket::FRIEND_ADDED);
	
	if (data_size < packetSizeType) {
		return false;
	}
	
	size_t iData = 0;
	
	XllnNetworkPacket::TYPE &packetType = *(XllnNetworkPacket::TYPE*)&data_buffer[iData];
	iData += packetSizeType;
	
	const char* packetName = XllnNetworkPacket::GetPacketTypeName(packetType);
	
	bool isUnknown = (packetType < XllnNetworkPacket::TYPE::XLLN_NPT_FIRST || packetType > XllnNetworkPacket::TYPE::XLLN_NPT_LAST);
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | (isUnknown ? XLLN_LOG_LEVEL_WARN : XLLN_LOG_LEVEL_DEBUG)
		, "%s Received %s packet of size (%zu)."
		, __func__
		, packetName
		, data_size
	);
	if (isUnknown) {
		return false;
	}
	
	switch (packetType) {
		case XllnNetworkPacket::TYPE::XLLN_NPT_TITLE_PACKET:
		case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_REQUEST:
		case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE:
		case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA:
		case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA_ACK: {
			if (data_size < iData + packetSizeTypeTitle) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeTitle
				);
				return false;
			}
			XllnNetworkPacket::TITLE_PACKET &packetTitle = *(XllnNetworkPacket::TITLE_PACKET*)&data_buffer[iData];
			iData += packetSizeTypeTitle;
			
			XllnNetEntityUpdateRemoteAddress(packetTitle.instanceId, sockaddr_external, packetName, via_hub);

					{
						EnterCriticalSection(&xlln_critsec_network_net_entity);
						XLLN_NET_ENTITY &netEntityTitle = xlln_net_entity_instance_id_to_remote_address[packetTitle.instanceId];
						if (packetTitle.titlePortSource && netEntityTitle.titlePortUdp != packetTitle.titlePortSource) {
							netEntityTitle.titlePortUdp = packetTitle.titlePortSource;
						}
						LeaveCriticalSection(&xlln_critsec_network_net_entity);
						if (packetTitle.titlePortDestination && xlln_title_port_udp_observed != packetTitle.titlePortDestination) {
							xlln_title_port_udp_observed = packetTitle.titlePortDestination;
						}
					}
			
			switch (packetType) {
				case XllnNetworkPacket::TYPE::XLLN_NPT_TITLE_PACKET: {
					XTS_RECV_PACKET* recvPacket = new XTS_RECV_PACKET(data_size - iData);
					memcpy(recvPacket->data, &data_buffer[iData], recvPacket->dataSize);
					recvPacket->dataFilledSize = recvPacket->dataSize;
					recvPacket->remoteInstanceId = packetTitle.instanceId;
					recvPacket->remoteInstanceTitlePort = packetTitle.titlePortSource;
					
					{
						EnterCriticalSection(&xlive_critsec_sockets);
						
						// Datagram delivery; an established stream resolves through the connection map.
						XLIVE_TITLE_SOCKET* titleSocket = XllnGetTitleSocketByTitlePort_(false, packetTitle.titlePortDestination);
						if (!titleSocket && xlln_title_bind_fallback) {
							// Opt-in: a socket whose requested port was taken and fell back to an ephemeral
							// port is reached through the port it asked for.
							for (auto itrTitleSocket : xlive_title_sockets) {
								if (
									itrTitleSocket.second->hasBinded
									&& itrTitleSocket.second->protocol == IPPROTO_UDP
									&& itrTitleSocket.second->portRequested == packetTitle.titlePortDestination
									&& itrTitleSocket.second->portActual != itrTitleSocket.second->portRequested
								) {
									titleSocket = itrTitleSocket.second;
									break;
								}
							}
						}
						if (!titleSocket) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
								, "%s %s packet not delivered to port (%hu) as no Title Socket is listening on it. Sent from instanceId (0x%08x) titlePort (%hu)."
								, __func__
								, packetName
								, packetTitle.titlePortDestination
								, packetTitle.instanceId
								, packetTitle.titlePortSource
							);
						}
						else {
							if (SubmitDataToTitleSocket_(titleSocket, recvPacket)) {
								recvPacket = 0;
							}
						}
						
						LeaveCriticalSection(&xlive_critsec_sockets);
					}
					
					if (recvPacket) {
						delete recvPacket;
					}
					else {
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
							, "%s %s packet delivered size (%zu) to local titlePort (%hu) from instanceId (0x%08x) titlePort (%hu)."
							, __func__
							, packetName
							, (size_t)(data_size - iData)
							, packetTitle.titlePortDestination
							, packetTitle.instanceId
							, packetTitle.titlePortSource
						);
					}
					
					return !recvPacket;
				}
				case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_REQUEST: {
					EnterCriticalSection(&xlive_critsec_sockets);
					
					XLIVE_TITLE_SOCKET* titleSocket = XllnGetTitleSocketByTitlePort_(true, packetTitle.titlePortDestination);
					
					if (!titleSocket) {
						XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
						sendPacket->destinationInstanceId = packetTitle.instanceId;
						{
							const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectResponse;
							
							uint8_t* packetBuffer = new uint8_t[packetSize];
							packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE;
							XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetBuffer[packetSizeType];
							packetTcpConnectResponse.titlePacket.instanceId = xlln_global_instance_id;
							packetTcpConnectResponse.titlePacket.titlePortDestination = packetTitle.titlePortSource;
							packetTcpConnectResponse.titlePacket.titlePortSource = packetTitle.titlePortDestination;
							packetTcpConnectResponse.responseCode = XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT;
							
							sendPacket->data = packetBuffer;
							sendPacket->dataSize = packetSize;
						}
						
						if (!SendPacketToRemoteInstance(sendPacket)) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s TCP failed to %s respond to %s packet from 0x%08x:%hu."
								, __func__
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT)
								, packetName
								, packetTitle.instanceId
								, packetTitle.titlePortSource
							);
							
							delete sendPacket;
						}
						else {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
								, "%s %s replying with %s to instanceId (0x%08x) titlePort (%hu)."
								, __func__
								, packetName
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT)
								, packetTitle.instanceId
								, packetTitle.titlePortSource
							);
						}
						sendPacket = 0;
						
						LeaveCriticalSection(&xlive_critsec_sockets);
						
						return false;
					}
					
					InstanceIdTitlePort instanceIdTitlePort = std::make_pair(packetTitle.instanceId, packetTitle.titlePortSource);
					
					__timeb64 timeCurrent;
					_ftime64_s(&timeCurrent);
					
					// Empty TCP backlog of old entries.
					for (auto itrPendingConnection = titleSocket->tcpConnectionBacklog.begin(); itrPendingConnection != titleSocket->tcpConnectionBacklog.end(); ) {
						if (TimeDiffMilliseconds(itrPendingConnection->second.lastResponse, timeCurrent) > (xlln_tcp_max_connection_attempts * xlln_tcp_packet_timeout)) {
							titleSocket->tcpConnectionBacklog.erase(itrPendingConnection++);
						}
						else {
							itrPendingConnection++;
						}
					}
					
					XTS_TCP_CONNECTION* pendingConnection = 0;
					bool alreadyAccepted = false;
					{
						auto itrPendingConnection = titleSocket->tcpConnectionBacklog.find(instanceIdTitlePort);
						if (itrPendingConnection != titleSocket->tcpConnectionBacklog.end()) {
							pendingConnection = &itrPendingConnection->second;
						}
						else if (xlive_title_socket_tcp_connected_sockets.find(instanceIdTitlePort) != xlive_title_socket_tcp_connected_sockets.end()) {
							alreadyAccepted = true;
						}
						else if (titleSocket->tcpConnectionBacklog.size() < titleSocket->tcpConnectionBacklogLimit) {
							pendingConnection = &titleSocket->tcpConnectionBacklog[instanceIdTitlePort];
							pendingConnection->remoteInstanceId = packetTitle.instanceId;
							pendingConnection->remoteInstanceTitlePort = packetTitle.titlePortSource;
						}
						if (pendingConnection) {
							pendingConnection->lastResponse = timeCurrent;
							
							SetEvent(titleSocket->tcpConnectNotify);
							titleSocket->selectNotifyReadPending = true;
							SetEvent(titleSocket->selectNotifyRead);
							
							if (titleSocket->wsaEventSelectRead != WSA_INVALID_EVENT) {
								SetEvent(titleSocket->wsaEventSelectRead);
							}
							if (titleSocket->wsaEventSelectAccept != WSA_INVALID_EVENT) {
								SetEvent(titleSocket->wsaEventSelectAccept);
							}
							
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s %s replying with %s to instanceId (0x%08x) titlePort (%hu)."
								, __func__
								, packetName
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE)
								, packetTitle.instanceId
								, packetTitle.titlePortSource
							);
						}
						else if (alreadyAccepted) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
								, "%s %s replying with %s (already accepted) to instanceId (0x%08x) titlePort (%hu)."
								, __func__
								, packetName
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_SUCCESS)
								, packetTitle.instanceId
								, packetTitle.titlePortSource
							);
						}
						else {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
								, "%s %s replying with %s to instanceId (0x%08x) titlePort (%hu) because the backlog limit (%u) has been reached."
								, __func__
								, packetName
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT)
								, packetTitle.instanceId
								, packetTitle.titlePortSource
								, titleSocket->tcpConnectionBacklogLimit
							);
						}
					}
					
					{
						XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
						sendPacket->destinationInstanceId = packetTitle.instanceId;
						{
							const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectResponse;
							
							uint8_t* packetBuffer = new uint8_t[packetSize];
							packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE;
							XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetBuffer[packetSizeType];
							packetTcpConnectResponse.titlePacket.instanceId = xlln_global_instance_id;
							packetTcpConnectResponse.titlePacket.titlePortDestination = packetTitle.titlePortSource;
							packetTcpConnectResponse.titlePacket.titlePortSource = titleSocket->portActual;
							packetTcpConnectResponse.responseCode = (pendingConnection ? XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE : (alreadyAccepted ? XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_SUCCESS : XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT));
							
							sendPacket->data = packetBuffer;
							sendPacket->dataSize = packetSize;
						}
						
						if (!SendPacketToRemoteInstance(sendPacket)) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s title_socket_handle (0x%zx) TCP failed to respond to connect request packet from 0x%08x:%hu."
								, __func__
								, titleSocket->handle
								, packetTitle.instanceId
								, packetTitle.titlePortSource
							);
							
							delete sendPacket;
						}
						sendPacket = 0;
					}
					
					LeaveCriticalSection(&xlive_critsec_sockets);
					
					return true;
				}
				case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE:
				case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA:
				case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA_ACK: {
					EnterCriticalSection(&xlive_critsec_sockets);
					
					InstanceIdTitlePort instanceIdTitlePort = std::make_pair(packetTitle.instanceId, packetTitle.titlePortSource);
					XLIVE_TITLE_SOCKET* titleSocket = 0;
					{
						auto itrConnectedSocket = xlive_title_socket_tcp_connected_sockets.find(instanceIdTitlePort);
						if (itrConnectedSocket != xlive_title_socket_tcp_connected_sockets.end()) {
							titleSocket = itrConnectedSocket->second;
						}
					}
					
					bool isConnectResponseReject = (
						packetType == XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE
						&& data_size >= iData + packetSizeTypeTcpConnectResponse - packetSizeTypeTitle
						&& ((XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetTitle)->responseCode == XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT
					);
					
					if (!titleSocket && !isConnectResponseReject) {
						XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
						sendPacket->destinationInstanceId = packetTitle.instanceId;
						{
							const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectResponse;
							
							uint8_t* packetBuffer = new uint8_t[packetSize];
							packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE;
							XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetBuffer[packetSizeType];
							packetTcpConnectResponse.titlePacket.instanceId = xlln_global_instance_id;
							packetTcpConnectResponse.titlePacket.titlePortDestination = packetTitle.titlePortSource;
							packetTcpConnectResponse.titlePacket.titlePortSource = packetTitle.titlePortDestination;
							packetTcpConnectResponse.responseCode = XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT;
							
							sendPacket->data = packetBuffer;
							sendPacket->dataSize = packetSize;
						}
						
						if (!SendPacketToRemoteInstance(sendPacket)) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s TCP failed to %s respond to %s packet from 0x%08x:%hu."
								, __func__
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT)
								, packetName
								, packetTitle.instanceId
								, packetTitle.titlePortSource
							);
							
							delete sendPacket;
						}
						else {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
								, "%s %s replying with %s to instanceId (0x%08x) titlePort (%hu)."
								, __func__
								, packetName
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT)
								, packetTitle.instanceId
								, packetTitle.titlePortSource
							);
						}
						sendPacket = 0;
						
						LeaveCriticalSection(&xlive_critsec_sockets);
						
						return false;
					}
					
					switch (packetType) {
						case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE: {
							if (data_size < iData + packetSizeTypeTcpConnectResponse - packetSizeTypeTitle) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
									, "%s received %s packet size (%zu) is too small (%zu)."
									, __func__
									, packetName
									, data_size
									, iData + packetSizeTypeTcpConnectResponse - packetSizeTypeTitle
								);
								
								LeaveCriticalSection(&xlive_critsec_sockets);
								
								return false;
							}
							
							XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetTitle;
							iData += packetSizeTypeTcpConnectResponse - packetSizeTypeTitle;
							
							if (!titleSocket) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
									, "%s %s packet responseCode (%s) (0x%hhx) dropped from instanceId (0x%08x) titlePort (%hu)."
									, __func__
									, packetName
									, XllnNetworkPacket::GetTcpResponseCodeName(packetTcpConnectResponse.responseCode)
									, packetTcpConnectResponse.responseCode
									, packetTcpConnectResponse.titlePacket.instanceId
									, packetTcpConnectResponse.titlePacket.titlePortSource
								);
								
								LeaveCriticalSection(&xlive_critsec_sockets);
								
								return false;
							}
							
							if (!isConnectResponseReject) {
								titleSocket->tcpConnectionAttempts = 0;
								SetTimeUnset(titleSocket->tcpConnectionAttemptLast);
								_ftime64_s(&titleSocket->tcpLastResponse);
							}
							
							switch (packetTcpConnectResponse.responseCode) {
								case XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_SUCCESS: {
									titleSocket->tcpIsConnected = true;
									titleSocket->tcpIsConnecting = false;
									titleSocket->socketOptions[SO_ERROR] = ERROR_SUCCESS;
									
									SetEvent(xlln_tcp_sockets_update);
									
									SetEvent(titleSocket->tcpConnectNotify);
									titleSocket->selectNotifyWritePending = true;
									SetEvent(titleSocket->selectNotifyWrite);
									
									if (titleSocket->wsaEventSelectWrite != WSA_INVALID_EVENT) {
										SetEvent(titleSocket->wsaEventSelectWrite);
									}
									
									if (titleSocket->wsaEventSelectConnect != WSA_INVALID_EVENT) {
										SetEvent(titleSocket->wsaEventSelectConnect);
									}
									
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_INFO
										, "%s %s packet %s from instanceId (0x%08x) titlePort (%hu)."
										, __func__
										, packetName
										, XllnNetworkPacket::GetTcpResponseCodeName(packetTcpConnectResponse.responseCode)
										, titleSocket->tcpRemoteInstanceId
										, titleSocket->tcpRemoteTitlePort
									);
									
									{
										XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
										sendPacket->destinationInstanceId = packetTitle.instanceId;
										{
											const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectResponse;
											
											uint8_t* packetBuffer = new uint8_t[packetSize];
											packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE;
											XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetBuffer[packetSizeType];
											packetTcpConnectResponse.titlePacket.instanceId = xlln_global_instance_id;
											packetTcpConnectResponse.titlePacket.titlePortDestination = packetTitle.titlePortSource;
											packetTcpConnectResponse.titlePacket.titlePortSource = titleSocket->portActual;
											packetTcpConnectResponse.responseCode = XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE;
											
											sendPacket->data = packetBuffer;
											sendPacket->dataSize = packetSize;
										}
										
										if (SendPacketToRemoteInstance(sendPacket)) {
											XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
												, "%s title_socket_handle (0x%zx) TCP sent %s %s packet to 0x%08x:%hu."
												, __func__
												, titleSocket->handle
												, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE)
												, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
												, packetTitle.instanceId
												, packetTitle.titlePortSource
											);
										}
										else {
											XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
												, "%s title_socket_handle (0x%zx) TCP failed to send %s %s packet to 0x%08x:%hu."
												, __func__
												, titleSocket->handle
												, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE)
												, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
												, packetTitle.instanceId
												, packetTitle.titlePortSource
											);
											
											delete sendPacket;
										}
										sendPacket = 0;
									}
									
									LeaveCriticalSection(&xlive_critsec_sockets);
									
									return true;
								}
								case XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT: {
									
									if (titleSocket->tcpIsConnected) {
										titleSocket->hasShutdown = (XTS_SHUTDOWN_STATE)(titleSocket->hasShutdown | XTS_SHUTDOWN_STATE::XTS_SS_NO_SEND);
										ShutdownTitleSocket_(titleSocket, titleSocket->hasShutdown);

										for (XTS_TCP_SEND_SEQUENCE_INFO* sendSequenceInfo : titleSocket->tcpSendQueue) {
											delete sendSequenceInfo;
										}
										titleSocket->tcpSendQueue.clear();

										titleSocket->selectNotifyReadPending = true;
										SetEvent(titleSocket->selectNotifyRead);

										SetEvent(titleSocket->recvNotify);
										if (titleSocket->recvPendingOverlapped.size()) {
											WSAOVERLAPPED* headOverlapped = titleSocket->recvPendingOverlapped.front().wsaOverlapped;
											if (headOverlapped->hEvent && headOverlapped->hEvent != INVALID_HANDLE_VALUE) {
												WSASetEvent(headOverlapped->hEvent);
											}
										}

										if (titleSocket->wsaEventSelectClose != WSA_INVALID_EVENT) {
											SetEvent(titleSocket->wsaEventSelectClose);
										}
									}
									else {
										titleSocket->tcpIsConnected = false;
										titleSocket->tcpIsConnecting = false;
										titleSocket->socketOptions[SO_ERROR] = WSAECONNREFUSED;

										SetEvent(titleSocket->tcpConnectNotify);
										titleSocket->selectNotifyExceptPending = true;
										SetEvent(titleSocket->selectNotifyExcept);
										if (titleSocket->wsaEventSelectConnect != WSA_INVALID_EVENT) {
											SetEvent(titleSocket->wsaEventSelectConnect);
										}

										XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
											, "%s %s packet %s from instanceId (0x%08x) titlePort (%hu)."
											, __func__
											, packetName
											, XllnNetworkPacket::GetTcpResponseCodeName(packetTcpConnectResponse.responseCode)
											, titleSocket->tcpRemoteInstanceId
											, titleSocket->tcpRemoteTitlePort
										);
									}
									
									LeaveCriticalSection(&xlive_critsec_sockets);
									
									return true;
								}
								case XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_NOT_ACCEPTING_DATA: {
									
									if (titleSocket->tcpIsConnected) {
										titleSocket->hasShutdown = (XTS_SHUTDOWN_STATE)(titleSocket->hasShutdown | XTS_SHUTDOWN_STATE::XTS_SS_NO_SEND);
										ShutdownTitleSocket_(titleSocket, titleSocket->hasShutdown);
										
										if (titleSocket->wsaEventSelectClose != WSA_INVALID_EVENT) {
											SetEvent(titleSocket->wsaEventSelectClose);
										}
									}
									
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
										, "%s %s packet %s from instanceId (0x%08x) titlePort (%hu)."
										, __func__
										, packetName
										, XllnNetworkPacket::GetTcpResponseCodeName(packetTcpConnectResponse.responseCode)
										, titleSocket->tcpRemoteInstanceId
										, titleSocket->tcpRemoteTitlePort
									);
									
									LeaveCriticalSection(&xlive_critsec_sockets);
									
									return true;
								}
								case XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE: {
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
										, "%s %s packet %s from instanceId (0x%08x) titlePort (%hu)."
										, __func__
										, packetName
										, XllnNetworkPacket::GetTcpResponseCodeName(packetTcpConnectResponse.responseCode)
										, titleSocket->tcpRemoteInstanceId
										, titleSocket->tcpRemoteTitlePort
									);
									
									LeaveCriticalSection(&xlive_critsec_sockets);
									
									return true;
								}
								case XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE_REQUEST: {
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
										, "%s %s packet %s from instanceId (0x%08x) titlePort (%hu)."
										, __func__
										, packetName
										, XllnNetworkPacket::GetTcpResponseCodeName(packetTcpConnectResponse.responseCode)
										, titleSocket->tcpRemoteInstanceId
										, titleSocket->tcpRemoteTitlePort
									);
									
									{
										XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
										sendPacket->destinationInstanceId = packetTitle.instanceId;
										{
											const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectResponse;
											
											uint8_t* packetBuffer = new uint8_t[packetSize];
											packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE;
											XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetBuffer[packetSizeType];
											packetTcpConnectResponse.titlePacket.instanceId = xlln_global_instance_id;
											packetTcpConnectResponse.titlePacket.titlePortDestination = packetTitle.titlePortSource;
											packetTcpConnectResponse.titlePacket.titlePortSource = titleSocket->portActual;
											packetTcpConnectResponse.responseCode = XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE;
											
											sendPacket->data = packetBuffer;
											sendPacket->dataSize = packetSize;
										}
										
										if (SendPacketToRemoteInstance(sendPacket)) {
											XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
												, "%s title_socket_handle (0x%zx) TCP sent %s %s packet to 0x%08x:%hu."
												, __func__
												, titleSocket->handle
												, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE)
												, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
												, packetTitle.instanceId
												, packetTitle.titlePortSource
											);
										}
										else {
											XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
												, "%s title_socket_handle (0x%zx) TCP failed to send %s %s packet to 0x%08x:%hu."
												, __func__
												, titleSocket->handle
												, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE)
												, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
												, packetTitle.instanceId
												, packetTitle.titlePortSource
											);
											
											delete sendPacket;
										}
										sendPacket = 0;
									}
									
									LeaveCriticalSection(&xlive_critsec_sockets);
									
									return true;
								}
							}
							
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s %s packet responseCode UNKNOWN (0x%hhx) from instanceId (0x%08x) titlePort (%hu)."
								, __func__
								, packetName
								, packetTcpConnectResponse.responseCode
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
							);
							
							LeaveCriticalSection(&xlive_critsec_sockets);
							
							return false;
						}
						case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA: {
							if (data_size < iData + packetSizeTypeTcpSequencedData - packetSizeTypeTitle) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
									, "%s received %s packet size (%zu) is too small (%zu)."
									, __func__
									, packetName
									, data_size
									, iData + packetSizeTypeTcpSequencedData - packetSizeTypeTitle
								);
								
								LeaveCriticalSection(&xlive_critsec_sockets);
								
								return false;
							}
							
							XllnNetworkPacket::TCP_SEQUENCED_DATA &packetTcpSequencedData = *(XllnNetworkPacket::TCP_SEQUENCED_DATA*)&packetTitle;
							iData += packetSizeTypeTcpSequencedData - packetSizeTypeTitle;
							
							if (data_size < iData + packetTcpSequencedData.dataSize) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
									, "%s received %s packet size (%zu) is too small (%zu), data was truncated."
									, __func__
									, packetName
									, data_size
									, iData + packetTcpSequencedData.dataSize
								);
								
								LeaveCriticalSection(&xlive_critsec_sockets);
								
								return false;
							}
							
							titleSocket->tcpConnectionAttempts = 0;
							SetTimeUnset(titleSocket->tcpConnectionAttemptLast);
							_ftime64_s(&titleSocket->tcpLastResponse);
							
							if (!packetTcpSequencedData.dataSize) {
								if (!titleSocket->tcpRecvQueue.size() && titleSocket->tcpRecvSequenceId == packetTcpSequencedData.sequenceId) {
									SetTimeUnset(titleSocket->tcpRecvLastAck);
									
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
										, "%s received empty %s packet and sequenceId matches."
										, __func__
										, packetName
									);
									
									LeaveCriticalSection(&xlive_critsec_sockets);
									
									return true;
								}
								
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
									, "%s received empty %s packet but we are still waiting on data."
									, __func__
									, packetName
								);
								
								LeaveCriticalSection(&xlive_critsec_sockets);
								
								return false;
							}
							
							if (titleSocket->hasShutdown & XTS_SHUTDOWN_STATE::XTS_SS_NO_RECV) {
								
								XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
								sendPacket->destinationInstanceId = titleSocket->tcpRemoteInstanceId;
								{
									const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectResponse;
									
									uint8_t* packetBuffer = new uint8_t[packetSize];
									packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE;
									XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetBuffer[packetSizeType];
									packetTcpConnectResponse.titlePacket.instanceId = xlln_global_instance_id;
									packetTcpConnectResponse.titlePacket.titlePortDestination = titleSocket->tcpRemoteTitlePort;
									packetTcpConnectResponse.titlePacket.titlePortSource = titleSocket->portActual;
									packetTcpConnectResponse.responseCode = XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_NOT_ACCEPTING_DATA;
									
									sendPacket->data = packetBuffer;
									sendPacket->dataSize = packetSize;
								}
								
								if (SendPacketToRemoteInstance(sendPacket)) {
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
										, "%s title_socket_handle (0x%zx) TCP sent %s %s to 0x%08x:%hu."
										, __func__
										, titleSocket->handle
										, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_NOT_ACCEPTING_DATA)
										, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
										, titleSocket->tcpRemoteInstanceId
										, titleSocket->tcpRemoteTitlePort
									);
								}
								else {
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
										, "%s title_socket_handle (0x%zx) TCP failed to send %s %s to 0x%08x:%hu."
										, __func__
										, titleSocket->handle
										, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_NOT_ACCEPTING_DATA)
										, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
										, titleSocket->tcpRemoteInstanceId
										, titleSocket->tcpRemoteTitlePort
									);
									
									delete sendPacket;
								}
								sendPacket = 0;
								
								LeaveCriticalSection(&xlive_critsec_sockets);
								
								return false;
							}
							
							if (IsTimeUnset(titleSocket->tcpRecvLastAck)) {
								// Set to a way out of date time.
								TimeAddMillis(titleSocket->tcpRecvLastAck, 1000);
								// The sending thread waits on a deadline computed before this packet arrived.
								SetEvent(xlln_tcp_sockets_update);
							}
							
							{
								XTS_RECV_PACKET* recvPacket = new XTS_RECV_PACKET(packetTcpSequencedData.dataSize);
								memcpy(recvPacket->data, &data_buffer[iData], recvPacket->dataSize);
								recvPacket->dataFilledSize = recvPacket->dataSize;
								recvPacket->remoteInstanceId = titleSocket->tcpRemoteInstanceId;
								recvPacket->remoteInstanceTitlePort = titleSocket->tcpRemoteTitlePort;
								
								size_t sequenceIndex = (packetTcpSequencedData.sequenceId < titleSocket->tcpRecvSequenceId ? ((0xFFFF - titleSocket->tcpRecvSequenceId) + 1 + packetTcpSequencedData.sequenceId) : (packetTcpSequencedData.sequenceId - titleSocket->tcpRecvSequenceId));
								if (sequenceIndex > xlln_tcp_send_queue_max) {
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
										, "%s received %s sequenceId (%hu) is too old and will be ignored. Current recv sequenceId (%hu)."
										, __func__
										, packetName
										, packetTcpSequencedData.sequenceId
										, titleSocket->tcpRecvSequenceId
									);
									
									LeaveCriticalSection(&xlive_critsec_sockets);
									
									delete recvPacket;
									recvPacket = 0;
									
									return false;
								}
								while (titleSocket->tcpRecvQueue.size() <= sequenceIndex) {
									titleSocket->tcpRecvQueue.push_back(0);
								}
								XTS_RECV_PACKET* &tcpRecvPacket = titleSocket->tcpRecvQueue[sequenceIndex];
								if (tcpRecvPacket) {
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
										, "%s received %s sequenceId (%hu) has already been received."
										, __func__
										, packetName
										, packetTcpSequencedData.sequenceId
									);
									
									LeaveCriticalSection(&xlive_critsec_sockets);
									
									delete recvPacket;
									recvPacket = 0;
									
									return false;
								}
								tcpRecvPacket = recvPacket;
							}
							
							while (1) {
								auto itrTcpRecvPacket = titleSocket->tcpRecvQueue.begin();
								if (itrTcpRecvPacket == titleSocket->tcpRecvQueue.end() || !*itrTcpRecvPacket) {
									break;
								}
								SubmitDataToTitleSocket_(titleSocket, *itrTcpRecvPacket);
								titleSocket->tcpRecvQueue.erase(itrTcpRecvPacket);
								titleSocket->tcpRecvSequenceId++;
							}
							
							LeaveCriticalSection(&xlive_critsec_sockets);
							
							return true;
						}
						case XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA_ACK: {
							if (data_size < iData + packetSizeTypeTcpSequencedDataAck - packetSizeTypeTitle) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
									, "%s received %s packet size (%zu) is too small (%zu)."
									, __func__
									, packetName
									, data_size
									, iData + packetSizeTypeTcpSequencedDataAck - packetSizeTypeTitle
								);
								
								LeaveCriticalSection(&xlive_critsec_sockets);
								
								return false;
							}
							
							XllnNetworkPacket::TCP_SEQUENCED_DATA_ACK &packetTcpSequencedDataAck = *(XllnNetworkPacket::TCP_SEQUENCED_DATA_ACK*)&packetTitle;
							iData += packetSizeTypeTcpSequencedDataAck - packetSizeTypeTitle;
							
							if (data_size < iData + (packetTcpSequencedDataAck.recvSequenceIdsCount * sizeof(uint16_t))) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
									, "%s received %s packet size (%zu) is too small (%zu), missing sequenceIds was truncated."
									, __func__
									, packetName
									, data_size
									, iData + (packetTcpSequencedDataAck.recvSequenceIdsCount * sizeof(uint16_t))
								);
								
								LeaveCriticalSection(&xlive_critsec_sockets);
								
								return false;
							}
							
							titleSocket->tcpConnectionAttempts = 0;
							SetTimeUnset(titleSocket->tcpConnectionAttemptLast);
							_ftime64_s(&titleSocket->tcpLastResponse);
							
							size_t previousSendQueueSize = titleSocket->tcpSendQueue.size();
							uint16_t* packetTcpSequencedDataAckSequences = (uint16_t*)&data_buffer[iData];
							
							for (auto itrSendSequenceInfo = titleSocket->tcpSendQueue.begin(); itrSendSequenceInfo != titleSocket->tcpSendQueue.end();) {
								XTS_TCP_SEND_SEQUENCE_INFO* sendSequenceInfo = *itrSendSequenceInfo;
								if (
									(titleSocket->tcpSendSequenceId >= packetTcpSequencedDataAck.recvSequenceAt)
									? (
										sendSequenceInfo->sequenceId >= titleSocket->tcpSendSequenceId
										|| sendSequenceInfo->sequenceId < packetTcpSequencedDataAck.recvSequenceAt
									)
									: (
										sendSequenceInfo->sequenceId >= titleSocket->tcpSendSequenceId
										&& sendSequenceInfo->sequenceId < packetTcpSequencedDataAck.recvSequenceAt
									)
								) {
									delete sendSequenceInfo;
									sendSequenceInfo = 0;
									itrSendSequenceInfo = titleSocket->tcpSendQueue.erase(itrSendSequenceInfo);
									continue;
								}
								size_t iAckSequenceId = 0;
								for (; iAckSequenceId < packetTcpSequencedDataAck.recvSequenceIdsCount; iAckSequenceId++) {
									if (sendSequenceInfo->sequenceId == packetTcpSequencedDataAckSequences[iAckSequenceId]) {
										break;
									}
								}
								if (iAckSequenceId < packetTcpSequencedDataAck.recvSequenceIdsCount) {
									delete sendSequenceInfo;
									sendSequenceInfo = 0;
									itrSendSequenceInfo = titleSocket->tcpSendQueue.erase(itrSendSequenceInfo);
									continue;
								}
								itrSendSequenceInfo++;
							}
							
							if (!titleSocket->tcpSendQueue.size() && packetTcpSequencedDataAck.recvSequenceAt == titleSocket->tcpSendSequenceId) {
								XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
								sendPacket->destinationInstanceId = packetTitle.instanceId;
								{
									const size_t packetSize = packetSizeType + packetSizeTypeTcpSequencedData;
									
									uint8_t* packetBuffer = new uint8_t[packetSize];
									packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA;
									XllnNetworkPacket::TCP_SEQUENCED_DATA &packetTcpSequencedData = *(XllnNetworkPacket::TCP_SEQUENCED_DATA*)&packetBuffer[packetSizeType];
									packetTcpSequencedData.titlePacket.instanceId = xlln_global_instance_id;
									packetTcpSequencedData.titlePacket.titlePortDestination = packetTitle.titlePortSource;
									packetTcpSequencedData.titlePacket.titlePortSource = titleSocket->portActual;
									packetTcpSequencedData.sequenceId = titleSocket->tcpSendSequenceId;
									packetTcpSequencedData.dataSize = 0;
									
									sendPacket->data = packetBuffer;
									sendPacket->dataSize = packetSize;
								}
								
								if (SendPacketToRemoteInstance(sendPacket)) {
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
										, "%s title_socket_handle (0x%zx) TCP sent empty %s packet to 0x%08x:%hu."
										, __func__
										, titleSocket->handle
										, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA)
										, packetTitle.instanceId
										, packetTitle.titlePortSource
									);
								}
								else {
									XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
										, "%s title_socket_handle (0x%zx) TCP failed to send empty %s packet to 0x%08x:%hu."
										, __func__
										, titleSocket->handle
										, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA)
										, packetTitle.instanceId
										, packetTitle.titlePortSource
									);
									
									delete sendPacket;
								}
								sendPacket = 0;
							}
							
							if (!titleSocket->tcpSendQueue.size() && titleSocket->tcpIsClosing) {
								SetEvent(xlln_tcp_sockets_update);
							}
							
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s received %s recvSequenceAt (%hu)."
								, __func__
								, packetName
								, packetTcpSequencedDataAck.recvSequenceAt
							);
							
							if (previousSendQueueSize != titleSocket->tcpSendQueue.size() && titleSocket->sendInCritSec) {
								uint16_t oldestSequenceId = 0;
								if (!(
									titleSocket->tcpSendQueue.size() >= xlln_tcp_send_queue_max
									|| (
										titleSocket->tcpSendQueue.size()
										&& ((oldestSequenceId = (*titleSocket->tcpSendQueue.begin())->sequenceId), true)
										&& (
											((titleSocket->tcpSendSequenceId - xlln_tcp_send_queue_max) > titleSocket->tcpSendSequenceId)
											? (
												oldestSequenceId <= (titleSocket->tcpSendSequenceId - xlln_tcp_send_queue_max)
												&& oldestSequenceId >= titleSocket->tcpSendSequenceId
											)
											: (
												oldestSequenceId <= (titleSocket->tcpSendSequenceId - xlln_tcp_send_queue_max)
												|| oldestSequenceId >= titleSocket->tcpSendSequenceId
											)
										)
									)
								)) {
									titleSocket->sendCompleted = true;
									
									SetEvent(titleSocket->sendNotify);
									if (titleSocket->sendWsaOverlapped && titleSocket->sendWsaOverlapped->hEvent && titleSocket->sendWsaOverlapped->hEvent != INVALID_HANDLE_VALUE) {
										WSASetEvent(titleSocket->sendWsaOverlapped->hEvent);
									}
									
									titleSocket->selectNotifyWritePending = true;
									SetEvent(titleSocket->selectNotifyWrite);
									
									if (titleSocket->wsaEventSelectWrite != WSA_INVALID_EVENT) {
										SetEvent(titleSocket->wsaEventSelectWrite);
									}
								}
							}
							
							LeaveCriticalSection(&xlive_critsec_sockets);
							
							return true;
						}
					}
					
					__debugbreak();
					return false;
				}
			}
			
			__debugbreak();
			return false;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_UNKNOWN_USER_ASK: {
			if (data_size < iData + packetSizeTypeUnknownUserAsk) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeUnknownUserAsk
				);
				return false;
			}
			
			XllnNetworkPacket::UNKNOWN_USER_ASK &packetUnknownUserAsk = *(XllnNetworkPacket::UNKNOWN_USER_ASK*)&data_buffer[iData];
			iData += packetSizeTypeUnknownUserAsk;
			
			if (packetUnknownUserAsk.instanceId && packetUnknownUserAsk.instanceId != xlln_global_instance_id) {
				// An ask says where the asker is, but it does not complete the exchange.
				XllnNetEntityUpdateRemoteAddress(packetUnknownUserAsk.instanceId, sockaddr_external, packetName, false, false);
			}
			
			if (packetUnknownUserAsk.instanceIdUnknown != xlln_global_instance_id) {
				return true;
			}
			
			if (!XllnNetAssociationAskReceived(packetUnknownUserAsk.instanceId)) {
				return true;
			}
			
			XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
			sendPacket->destinationAddress = *sockaddr_external;
			{
				const size_t newPacketBufferUnknownUserReplySize = packetSizeType + packetSizeTypeUnknownUserReply;
				uint8_t* newPacketBufferUnknownUserReply = new uint8_t[newPacketBufferUnknownUserReplySize];
				size_t iSendData = 0;
				
				XllnNetworkPacket::TYPE &newPacketUnknownUserReplyType = *(XllnNetworkPacket::TYPE*)&newPacketBufferUnknownUserReply[iSendData];
				iSendData += packetSizeType;
				newPacketUnknownUserReplyType = XllnNetworkPacket::TYPE::XLLN_NPT_UNKNOWN_USER_REPLY;
				
				XllnNetworkPacket::UNKNOWN_USER_REPLY &newPacketUnknownUserReply = *(XllnNetworkPacket::UNKNOWN_USER_REPLY*)&newPacketBufferUnknownUserReply[iSendData];
				iSendData += packetSizeTypeUnknownUserReply;
				newPacketUnknownUserReply.instanceId = xlln_global_instance_id;
				
				if (iSendData != newPacketBufferUnknownUserReplySize) {
					__debugbreak();
				}
				
				sendPacket->data = newPacketBufferUnknownUserReply;
				sendPacket->dataSize = newPacketBufferUnknownUserReplySize;
			}
			
			{
				char* sockAddrInfo = GET_SOCKADDR_INFO(sockaddr_external);
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
					, "%s Sending %s to %s."
					, __func__
					, XllnNetworkPacket::TYPE_NAMES[XllnNetworkPacket::TYPE::XLLN_NPT_UNKNOWN_USER_REPLY]
					, sockAddrInfo ? sockAddrInfo : ""
				);
				if (sockAddrInfo) {
					free(sockAddrInfo);
				}
			}
			
			if (!SendPacketToRemoteInstance(sendPacket)) {
				delete sendPacket;
			}
			sendPacket = 0;
			
			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_UNKNOWN_USER_REPLY: {
			if (data_size < iData + packetSizeTypeUnknownUserReply) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeUnknownUserReply
				);
				return false;
			}
			
			XllnNetworkPacket::UNKNOWN_USER_REPLY &packetUnknownUserReply = *(XllnNetworkPacket::UNKNOWN_USER_REPLY*)&data_buffer[iData];
			iData += packetSizeTypeUnknownUserReply;
			
			if (packetUnknownUserReply.instanceId && packetUnknownUserReply.instanceId != xlln_global_instance_id) {
				const bool completes = XllnNetAssociationReplyCompletesExchange(packetUnknownUserReply.instanceId);
				XllnNetEntityUpdateRemoteAddress(packetUnknownUserReply.instanceId, sockaddr_external, packetName, false, completes);
			}
			
			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_LIVE_OVER_LAN_ADVERTISE: {
			const uint8_t* liveSessionBuffer = &data_buffer[iData];
			const size_t liveSessionBufferSize = data_size - iData;
			LIVE_SESSION* liveSession;
			
			if (!LiveOverLanDeserialiseLiveSession(liveSessionBuffer, liveSessionBufferSize, &liveSession)) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s Invalid %s received."
					, __func__
					, packetName
				);
				
				return false;
			}
			
			// An advertise is broadcast to every instance running the Title, so it says where the
			// host is without finishing an exchange that instance never took part in.
			XllnNetEntityUpdateRemoteAddress(liveSession->instanceId, sockaddr_external, packetName, false, false);
			
			LiveOverLanAddRemoteLiveSession(liveSession->sessionType, liveSession);
			
			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_LIVE_OVER_LAN_UNADVERTISE: {
			// Only the fields an older instance also sends are required.
			const size_t packetSizeTypeLolUnadvertiseRequired = offsetof(XllnNetworkPacket::LIVE_OVER_LAN_UNADVERTISE, xnkid);
			if (data_size < iData + packetSizeTypeLolUnadvertiseRequired) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeLolUnadvertiseRequired
				);
				return false;
			}
			
			XllnNetworkPacket::LIVE_OVER_LAN_UNADVERTISE &packetLolUnadvertise = *(XllnNetworkPacket::LIVE_OVER_LAN_UNADVERTISE*)&data_buffer[iData];
			const uint64_t sessionIdUnadvertise = data_size >= iData + packetSizeTypeLolUnadvertise ? *(uint64_t*)&packetLolUnadvertise.xnkid : 0;
			iData += packetSizeTypeLolUnadvertise;
			
			LiveOverLanBroadcastRemoteSessionUnadvertise(packetLolUnadvertise.instanceId, packetLolUnadvertise.sessionType, packetLolUnadvertise.xuid, sessionIdUnadvertise);
			
			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_QOS_REQUEST: {
			if (data_size < iData + packetSizeTypeQosRequest) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeQosRequest
				);
				return false;
			}
			
			XllnNetworkPacket::QOS_REQUEST &packetQosRequest = *(XllnNetworkPacket::QOS_REQUEST*)&data_buffer[iData];
			iData += packetSizeTypeQosRequest;
			
			XLiveQosReceiveRequest(&packetQosRequest, sockaddr_external);
			
			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_QOS_RESPONSE: {
			if (data_size < iData + packetSizeTypeQosResponse) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeQosResponse
				);
				return false;
			}
			
			XllnNetworkPacket::QOS_RESPONSE &packetQosResponse = *(XllnNetworkPacket::QOS_RESPONSE*)&data_buffer[iData];
			iData += packetSizeTypeQosResponse;
			
			if (data_size < iData + packetQosResponse.sizeData) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu) (insufficient size with extra data payload)."
					, __func__
					, packetName
					, data_size
					, iData + packetQosResponse.sizeData
				);
				return false;
			}
			
			XLiveQosReceiveResponse(&packetQosResponse);
			
			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_DIRECT_IP_REQUEST: {
			if (data_size < iData + packetSizeTypeDirectIpRequest) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeDirectIpRequest
				);
				return false;
			}
			
			XllnNetworkPacket::DIRECT_IP_REQUEST &packetDirectIpRequest = *(XllnNetworkPacket::DIRECT_IP_REQUEST*)&data_buffer[iData];
			iData += packetSizeTypeDirectIpRequest;
			
			bool correctPassword = false;
			{
				char hostPassword[500];
				GetDlgItemTextA(xlln_window_hwnd, XLLNControlsMessageNumbers::MAIN_TBX_DIRECT_IP_CONNECT_PASSWORD, hostPassword, 500);
				
				uint8_t hostPasswordSha256[32];
				memset(hostPasswordSha256, 0, sizeof(hostPasswordSha256));
				
				size_t hostPasswordLen = strlen(hostPassword);
				if (hostPasswordLen) {
					mbedtls_sha256((uint8_t*)hostPassword, hostPasswordLen, hostPasswordSha256, 0);
				}
				
				if (memcmp(hostPasswordSha256, packetDirectIpRequest.passwordSha256, sizeof(hostPasswordSha256)) == 0) {
					correctPassword = true;
				}
			}
			
			{
				char* sockAddrInfo = GET_SOCKADDR_INFO(sockaddr_external);
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
					, "%s Sending %s to %s %s."
					, __func__
					, XllnNetworkPacket::TYPE_NAMES[XllnNetworkPacket::TYPE::XLLN_NPT_DIRECT_IP_RESPONSE]
					, sockAddrInfo ? sockAddrInfo : ""
					, correctPassword ? "having correct password" : "having incorrect password"
				);
				if (sockAddrInfo) {
					free(sockAddrInfo);
				}
			}
			
			XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
			sendPacket->destinationAddress = *sockaddr_external;
			{
				uint8_t* liveSessionSerialisedPacket = 0;
				size_t liveSessionSerialisedPacketSize = 0;
				
				if (correctPassword) {
					EnterCriticalSection(&xlln_critsec_liveoverlan_broadcast);
					// Guards the local sessions read below; XCloseHandle frees them from the Title's thread.
					EnterCriticalSection(&xlive_critsec_xsession);
					if (xlive_xlocator_local_session) {
						if (!LiveOverLanSerialiseLiveSessionIntoNetPacket(xlive_xlocator_local_session, &liveSessionSerialisedPacket, &liveSessionSerialisedPacketSize)) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
								, "%s Failed to serialise Live Session for %s packet."
								, __func__
								, XllnNetworkPacket::TYPE_NAMES[XllnNetworkPacket::TYPE::XLLN_NPT_DIRECT_IP_RESPONSE]
							);
						}
					}
					else {
						for (auto const &entry : xlive_xsession_local_sessions) {
							if (!(entry.second->liveSession->sessionType == XLLN_LIVEOVERLAN_SESSION_TYPE_XSESSION && entry.second->liveSession->sessionFlags & XSESSION_CREATE_HOST)) {
								continue;
							}
							
							if (!LiveOverLanSerialiseLiveSessionIntoNetPacket(entry.second->liveSession, &liveSessionSerialisedPacket, &liveSessionSerialisedPacketSize)) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
									, "%s Failed to serialise Live Session for %s packet."
									, __func__
									, XllnNetworkPacket::TYPE_NAMES[XllnNetworkPacket::TYPE::XLLN_NPT_DIRECT_IP_RESPONSE]
								);
							}
							
							break;
						}
					}
					LeaveCriticalSection(&xlive_critsec_xsession);
					LeaveCriticalSection(&xlln_critsec_liveoverlan_broadcast);
				}
				
				if (liveSessionSerialisedPacket) {
					const size_t packetSize = packetSizeType + packetSizeTypeDirectIpResponse + (liveSessionSerialisedPacketSize - packetSizeType);
					
					uint8_t* packetBuffer = new uint8_t[packetSize];
					packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_DIRECT_IP_RESPONSE;
					XllnNetworkPacket::DIRECT_IP_RESPONSE &directIpResponse = *(XllnNetworkPacket::DIRECT_IP_RESPONSE*)&packetBuffer[packetSizeType];
					directIpResponse.joinRequestSignature = packetDirectIpRequest.joinRequestSignature;
					directIpResponse.instanceId = xlln_global_instance_id;
					directIpResponse.titleId = xlive_title_id;
					
					memcpy(&packetBuffer[packetSizeType + packetSizeTypeDirectIpResponse], &liveSessionSerialisedPacket[packetSizeType], liveSessionSerialisedPacketSize - packetSizeType);
					
					delete[] liveSessionSerialisedPacket;
					liveSessionSerialisedPacket = 0;
					
					sendPacket->data = packetBuffer;
					sendPacket->dataSize = packetSize;
				}
				else {
					const int32_t packetSize = packetSizeType + packetSizeTypeDirectIpResponse;
					
					uint8_t* packetBuffer = new uint8_t[packetSize];
					packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_DIRECT_IP_RESPONSE;
					XllnNetworkPacket::DIRECT_IP_RESPONSE &directIpResponse = *(XllnNetworkPacket::DIRECT_IP_RESPONSE*)&packetBuffer[packetSizeType];
					directIpResponse.joinRequestSignature = packetDirectIpRequest.joinRequestSignature;
					directIpResponse.instanceId = 0;
					directIpResponse.titleId = 0;
					
					sendPacket->data = packetBuffer;
					sendPacket->dataSize = packetSize;
				}
			}
			
			if (!SendPacketToRemoteInstance(sendPacket)) {
				delete sendPacket;
			}
			sendPacket = 0;
			
			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_DIRECT_IP_RESPONSE: {
			if (data_size < iData + packetSizeTypeDirectIpResponse) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeDirectIpResponse
				);
				return false;
			}
			
			XllnNetworkPacket::DIRECT_IP_RESPONSE &packetDirectIpResponse = *(XllnNetworkPacket::DIRECT_IP_RESPONSE*)&data_buffer[iData];
			iData += packetSizeTypeDirectIpResponse;
			
			if (packetDirectIpResponse.joinRequestSignature != xlln_direct_ip_connect.joinRequestSignature) {
				return true;
			}
			if (!packetDirectIpResponse.instanceId) {
				char* sockAddrInfo = GET_SOCKADDR_INFO(sockaddr_external);
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
					, "%s Received %s from %s stating no available lobby or password was wrong."
					, __func__
					, packetName
					, sockAddrInfo ? sockAddrInfo : ""
				);
				if (sockAddrInfo) {
					free(sockAddrInfo);
				}
				
				XllnDirectIpConnectCancel();
				MessageBoxW(xlln_window_hwnd, L"There is no available lobby or the password was wrong.", L"XLLN Direct IP Connect Error", MB_OK);
				
				return true;
			}
			
			{
				char* sockAddrInfo = GET_SOCKADDR_INFO(sockaddr_external);
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
					, "%s Received %s from %s."
					, __func__
					, packetName
					, sockAddrInfo ? sockAddrInfo : ""
				);
				if (sockAddrInfo) {
					free(sockAddrInfo);
				}
			}
			
			xlln_direct_ip_connect.remoteInstanceId = packetDirectIpResponse.instanceId;
			xlln_direct_ip_connect.remoteTitleId = packetDirectIpResponse.titleId;
			
			const uint8_t* liveSessionBuffer = (uint8_t*)&data_buffer[iData];
			const size_t liveSessionBufferSize = data_size - iData;
			LIVE_SESSION* liveSession;
			
			if (!LiveOverLanDeserialiseLiveSession(liveSessionBuffer, liveSessionBufferSize, &liveSession)) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s Invalid %s received when parsing Live Session."
					, __func__
					, packetName
				);
				return false;
			}
			
			if (packetDirectIpResponse.instanceId != liveSession->instanceId) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s Invalid %s received packet instanceId (0x%08x) does not match parsed Live Session instanceId (0x%08x)."
					, __func__
					, packetName
					, packetDirectIpResponse.instanceId
					, liveSession->instanceId
				);
				return false;
			}
			
			XllnNetEntityUpdateRemoteAddress(packetDirectIpResponse.instanceId, sockaddr_external, packetName, via_hub);
			
			xlln_direct_ip_connect.remoteXuid = liveSession->xuid;
			xlln_direct_ip_connect.remoteSessionId = liveSession->xnkid;
			xlln_direct_ip_connect.remoteKeyExchangeKey = liveSession->xnkey;
			
			LiveOverLanAddRemoteLiveSession(liveSession->sessionType, liveSession);
			
			xlln_direct_ip_connect.timeoutAt = 0;
			
			XLiveNotifyAddEvent(XN_LIVE_INVITE_ACCEPTED, xlln_direct_ip_connect.localPlayerId);
			
			EnableWindow(GetDlgItem(xlln_window_hwnd, XLLNControlsMessageNumbers::MAIN_BTN_DIRECT_IP_CONNECT), true);

			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_FRIEND_INVITE_RESPONSE: {
			if (data_size < iData + packetSizeTypeFriendInviteResponse) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeFriendInviteResponse
				);
				return false;
			}

			XllnNetworkPacket::FRIEND_INVITE_RESPONSE &packetInviteResponse = *(XllnNetworkPacket::FRIEND_INVITE_RESPONSE*)&data_buffer[iData];
			iData += packetSizeTypeFriendInviteResponse;

			// Only meaningful when the addressed inviter is signed in on this instance.
			bool inviterIsLocal = false;
			EnterCriticalSection(&xlive_critsec_local_user);
			for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
				if (xlive_local_users[iUser].signin_state != eXUserSigninState_NotSignedIn
					&& xlive_local_users[iUser].xuid == packetInviteResponse.inviterXuid
				) {
					inviterIsLocal = true;
					break;
				}
			}
			LeaveCriticalSection(&xlive_critsec_local_user);
			if (!inviterIsLocal) {
				return true;
			}

			if (!XllnFriendsConsumeSentInviteToken(packetInviteResponse.inviteeXuid, packetInviteResponse.inviteToken)) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
					, "%s %s packet does not answer an invite this instance sent to 0x%016I64x; ignoring."
					, __func__
					, packetName
					, packetInviteResponse.inviteeXuid
				);
				return true;
			}

			char inviteeUsername[XUSER_NAME_SIZE];
			memcpy(inviteeUsername, packetInviteResponse.inviteeUsername, XUSER_NAME_SIZE);
			inviteeUsername[XUSER_NAME_SIZE - 1] = 0;

			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
				, "%s \"%s\" (instance 0x%08x) %s the game invite."
				, __func__
				, inviteeUsername
				, packetInviteResponse.inviteeInstanceId
				, packetInviteResponse.accepted ? "accepted" : "declined"
			);

			if (packetInviteResponse.accepted) {
				OverlayEventQueuePush(OverlayEventType::JOINING_SESSION, inviteeUsername, "Accepted your invite - joining");
			}
			else {
				OverlayEventQueuePush(OverlayEventType::INVITE_DECLINED, inviteeUsername, "Declined your invite");
			}

			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_HUB_REQUEST: {
			if (data_size < iData + packetSizeTypeHubRequest) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeHubRequest
				);
				return false;
			}
			
			XllnNetworkPacket::HUB_REQUEST_PACKET &packetHubRequest = *(XllnNetworkPacket::HUB_REQUEST_PACKET*)&data_buffer[iData];
			iData += packetSizeTypeHubRequest;
			
			{
				char* sockAddrInfo = GET_SOCKADDR_INFO(sockaddr_external);
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
					, "%s received %s from %s, xllnVersion (0x%08x), instanceId (0x%08x), titleId (%08x), titleVersion (0x%08x)."
					, __func__
					, packetName
					, sockAddrInfo ? sockAddrInfo : ""
					, packetHubRequest.xllnVersion
					, packetHubRequest.instanceId
					, packetHubRequest.titleId
					, packetHubRequest.titleVersion
				);
				if (sockAddrInfo) {
					free(sockAddrInfo);
				}
			}
			
			XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
			sendPacket->destinationAddress = *sockaddr_external;
			{
				const size_t newPacketBufferHubReplySize = packetSizeType + packetSizeTypeHubReply;
				uint8_t* newPacketBufferHubReply = new uint8_t[newPacketBufferHubReplySize];
				int32_t iSendData = 0;
				
				XllnNetworkPacket::TYPE &newPacketHubReplyType = *(XllnNetworkPacket::TYPE*)&newPacketBufferHubReply[iSendData];
				iSendData += packetSizeType;
				newPacketHubReplyType = XllnNetworkPacket::TYPE::XLLN_NPT_HUB_REPLY;
				
				XllnNetworkPacket::HUB_REPLY_PACKET &newPacketHubReplyReply = *(XllnNetworkPacket::HUB_REPLY_PACKET*)&newPacketBufferHubReply[iSendData];
				iSendData += packetSizeTypeHubReply;
				newPacketHubReplyReply.isHubServer = false;
				newPacketHubReplyReply.xllnVersion = (DLL_VERSION_MAJOR << 24) + (DLL_VERSION_MINOR << 16) + (DLL_VERSION_REVISION << 8) + DLL_VERSION_BUILD;
				newPacketHubReplyReply.recommendedInstanceId = 0;
				
				if (iSendData != newPacketBufferHubReplySize) {
					__debugbreak();
				}
				
				sendPacket->data = newPacketBufferHubReply;
				sendPacket->dataSize = newPacketBufferHubReplySize;
			}
			
			{
				char* sockAddrInfo = GET_SOCKADDR_INFO(sockaddr_external);
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
					, "%s Sending %s to %s."
					, __func__
					, XllnNetworkPacket::TYPE_NAMES[XllnNetworkPacket::TYPE::XLLN_NPT_HUB_REPLY]
					, sockAddrInfo ? sockAddrInfo : ""
				);
				if (sockAddrInfo) {
					free(sockAddrInfo);
				}
			}
			
			if (!SendPacketToRemoteInstance(sendPacket)) {
				delete sendPacket;
			}
			sendPacket = 0;
			
			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_HUB_REPLY: {
			if (data_size < iData + packetSizeTypeHubReply) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeHubReply
				);
				return false;
			}
			
			XllnNetworkPacket::HUB_REPLY_PACKET &packetHubReply = *(XllnNetworkPacket::HUB_REPLY_PACKET*)&data_buffer[iData];
			iData += packetSizeTypeHubReply;
			
			EnterCriticalSection(&xlln_critsec_network_broadcast_addresses);
			for (XllnNetworkBroadcastEntity::BROADCAST_ENTITY &broadcastEntity : xlln_network_broadcast_addresses) {
				if (SockAddrsMatch(&broadcastEntity.sockaddr, sockaddr_external)) {
					if (broadcastEntity.entityType != XllnNetworkBroadcastEntity::TYPE::XLLN_NBE_BROADCAST_ADDR) {
						broadcastEntity.entityType = (packetHubReply.isHubServer != 0 ? XllnNetworkBroadcastEntity::TYPE::XLLN_NBE_HUB_SERVER : XllnNetworkBroadcastEntity::TYPE::XLLN_NBE_OTHER_CLIENT);
					}
					_time64(&broadcastEntity.lastComm);
					
					broadcastEntity.xllnVersion = packetHubReply.xllnVersion;
					
					if (packetHubReply.recommendedInstanceId && packetHubReply.recommendedInstanceId != xlln_global_instance_id) {
						char* sockAddrInfo = GET_SOCKADDR_INFO(&broadcastEntity.sockaddr);
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
							, "%s %s from %s recommends a different Instance ID (from 0x%08x to 0x%08x)."
							, __func__
							, packetName
							, sockAddrInfo ? sockAddrInfo : "?"
							, xlln_global_instance_id
							, packetHubReply.recommendedInstanceId
						);
						if (sockAddrInfo) {
							free(sockAddrInfo);
						}
					}
					
					{
						char* sockAddrInfo = GET_SOCKADDR_INFO(&broadcastEntity.sockaddr);
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
							, "%s %s from %s entityType:%s."
							, __func__
							, packetName
							, sockAddrInfo ? sockAddrInfo : "?"
							, XllnNetworkBroadcastEntity::TYPE_NAMES[broadcastEntity.entityType]
						);
						if (sockAddrInfo) {
							free(sockAddrInfo);
						}
					}
					
					break;
				}
			}
			LeaveCriticalSection(&xlln_critsec_network_broadcast_addresses);
			
			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_USER_PRESENCE: {
			if (data_size < iData + packetSizeTypeUserPresence) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeUserPresence
				);
				return false;
			}

			XllnNetworkPacket::USER_PRESENCE &packetUserPresence = *(XllnNetworkPacket::USER_PRESENCE*)&data_buffer[iData];
			iData += packetSizeTypeUserPresence;

			// Our own broadcast can be looped back to us.
			if (!packetUserPresence.instanceId || packetUserPresence.instanceId == xlln_global_instance_id) {
				return true;
			}

			XllnNetEntityUpdateRemoteAddress(packetUserPresence.instanceId, sockaddr_external, packetName, false, false);

			char username[XUSER_NAME_SIZE];
			memcpy(username, packetUserPresence.username, XUSER_NAME_SIZE);
			username[XUSER_NAME_SIZE - 1] = 0;

			char titleName[sizeof(packetUserPresence.titleName)];
			memcpy(titleName, packetUserPresence.titleName, sizeof(titleName));
			titleName[sizeof(titleName) - 1] = 0;

			char richPresence[sizeof(packetUserPresence.richPresence)];
			memcpy(richPresence, packetUserPresence.richPresence, sizeof(richPresence));
			richPresence[sizeof(richPresence) - 1] = 0;

			XllnRemoteNameSanitise_(username);
			XllnRemoteNameSanitise_(titleName);
			XllnRemoteNameSanitise_(richPresence);

			XllnFriendsPresenceReceived(packetUserPresence.instanceId, packetUserPresence.xuid, username, packetUserPresence.titleId, titleName, richPresence, packetUserPresence.isOnline != 0);

			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_FRIEND_INVITE: {
			if (data_size < iData + packetSizeTypeFriendInvite) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeFriendInvite
				);
				return false;
			}

			XllnNetworkPacket::FRIEND_INVITE &packetFriendInvite = *(XllnNetworkPacket::FRIEND_INVITE*)&data_buffer[iData];
			iData += packetSizeTypeFriendInvite;

			const uint8_t* liveSessionBuffer = (uint8_t*)&data_buffer[iData];
			const size_t liveSessionBufferSize = data_size - iData;
			LIVE_SESSION* liveSession;

			if (!LiveOverLanDeserialiseLiveSession(liveSessionBuffer, liveSessionBufferSize, &liveSession)) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s Invalid %s received when parsing Live Session."
					, __func__
					, packetName
				);
				return false;
			}

			// The header names the inviter and the session names its host. They have to agree, or the
			// invite is describing someone else's session - the same check DIRECT_IP_RESPONSE makes.
			if (packetFriendInvite.inviterInstanceId != liveSession->instanceId) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s Invalid %s received packet inviterInstanceId (0x%08x) does not match parsed Live Session instanceId (0x%08x)."
					, __func__
					, packetName
					, packetFriendInvite.inviterInstanceId
					, liveSession->instanceId
				);
				LiveOverLanDestroyLiveSession(&liveSession);
				return false;
			}

			if (packetFriendInvite.inviterInstanceId) {
				XllnNetEntityUpdateRemoteAddress(packetFriendInvite.inviterInstanceId, sockaddr_external, packetName, via_hub);
			}

			// Record the host's advertised title port so the accept path can dial it. The advertise
			// path does this in LiveOverLanAddRemoteLiveSession; the invite path never reached it.
			if (liveSession->titlePortTcp) {
				EnterCriticalSection(&xlln_critsec_network_net_entity);
				xlln_net_entity_instance_id_to_remote_address[liveSession->instanceId].titlePortUdp = ntohs(liveSession->titlePortTcp);
				LeaveCriticalSection(&xlln_critsec_network_net_entity);
			}

			uint32_t userIndex = XUSER_INDEX_NONE;
			for (uint32_t iUser = 0; iUser < XLIVE_LOCAL_USER_COUNT; iUser++) {
				if (xlive_local_users[iUser].signin_state != eXUserSigninState_NotSignedIn && xlive_local_users[iUser].xuid == packetFriendInvite.inviteeXuid) {
					userIndex = iUser;
					break;
				}
			}
			if (userIndex == XUSER_INDEX_NONE) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
					, "%s %s invitee 0x%016I64x is not signed in on this instance."
					, __func__
					, packetName
					, packetFriendInvite.inviteeXuid
				);
				LiveOverLanDestroyLiveSession(&liveSession);
				return true;
			}

			char inviterUsername[XUSER_NAME_SIZE];
			memcpy(inviterUsername, packetFriendInvite.inviterUsername, XUSER_NAME_SIZE);
			inviterUsername[XUSER_NAME_SIZE - 1] = 0;
			XllnRemoteNameSanitise_(inviterUsername);

			wchar_t inviteMessage[sizeof(packetFriendInvite.inviteMessage) / sizeof(wchar_t)];
			memcpy(inviteMessage, packetFriendInvite.inviteMessage, sizeof(inviteMessage));
			inviteMessage[(sizeof(inviteMessage) / sizeof(wchar_t)) - 1] = 0;

			XllnFriendsInviteReceived(userIndex, packetFriendInvite.inviterInstanceId, packetFriendInvite.inviterXuid, inviterUsername, packetFriendInvite.titleId, packetFriendInvite.inviteToken, inviteMessage, liveSession);

			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_FRIEND_ADDED: {
			if (data_size < iData + packetSizeTypeFriendAdded) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeFriendAdded
				);
				return false;
			}

			XllnNetworkPacket::FRIEND_ADDED &packetFriendAdded = *(XllnNetworkPacket::FRIEND_ADDED*)&data_buffer[iData];
			iData += packetSizeTypeFriendAdded;

			if (packetFriendAdded.adderInstanceId) {
				XllnNetEntityUpdateRemoteAddress(packetFriendAdded.adderInstanceId, sockaddr_external, packetName, via_hub);
			}

			char adderUsername[XUSER_NAME_SIZE];
			memcpy(adderUsername, packetFriendAdded.adderUsername, XUSER_NAME_SIZE);
			adderUsername[XUSER_NAME_SIZE - 1] = 0;
			XllnRemoteNameSanitise_(adderUsername);

			XllnFriendsAddedReceived(packetFriendAdded.adderInstanceId, packetFriendAdded.adderXuid, adderUsername, packetFriendAdded.addedXuid);

			return true;
		}
		case XllnNetworkPacket::TYPE::XLLN_NPT_PACKET_FORWARDED: {
			if (data_size < iData + packetSizeTypeForwarded) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s received %s packet size (%zu) is too small (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeTypeForwarded
				);
				return false;
			}
			
			XllnNetworkPacket::PACKET_FORWARDED &packetForwarded = *(XllnNetworkPacket::PACKET_FORWARDED*)&data_buffer[iData];
			iData += packetSizeTypeForwarded;
			
			if (packetForwarded.originSockAddr.ss_family != AF_INET && packetForwarded.originSockAddr.ss_family != AF_INET6) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s Invalid %s received packet origin socket address is not IPv4 or IPv6."
					, __func__
					, packetName
				);
				return false;
			}
			
			if (data_size < iData + packetSizeType) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s Invalid %s packet size (%zu) is too small to contain a wrapped packet (%zu)."
					, __func__
					, packetName
					, data_size
					, iData + packetSizeType
				);
				return false;
			}
			
			XllnNetworkPacket::TYPE &packetTypeWrapped = *(XllnNetworkPacket::TYPE*)&data_buffer[iData];
			switch (packetTypeWrapped) {
				case XllnNetworkPacket::TYPE::XLLN_NPT_TITLE_PACKET:
				case XllnNetworkPacket::TYPE::XLLN_NPT_LIVE_OVER_LAN_ADVERTISE:
				case XllnNetworkPacket::TYPE::XLLN_NPT_LIVE_OVER_LAN_UNADVERTISE:
				case XllnNetworkPacket::TYPE::XLLN_NPT_USER_PRESENCE:
				case XllnNetworkPacket::TYPE::XLLN_NPT_UNKNOWN_USER_ASK: {
					break;
				}
				default: {
					XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
						, "%s Invalid %s packet wrapped packet type %s (%hhu) not allowed."
						, __func__
						, packetName
						, XllnNetworkPacket::GetPacketTypeName(packetTypeWrapped)
						, (uint8_t)packetTypeWrapped
					);
					return false;
				}
			}
			
			bool resultWrappedPacket = ParseNetworkData(&data_buffer[iData], data_size - iData, &packetForwarded.originSockAddr, true);
			
			return resultWrappedPacket;
		}
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
		, "%s %s packet not handled."
		, __func__
		, packetName
	);
	
	return false;
}

typedef struct _XLLN_THREAD_PARAMS_TCP_SOCKETS {
} XLLN_THREAD_PARAMS_TCP_SOCKETS;

static DWORD WINAPI XllnThreadTcpSockets(void* lpParam)
{
	XLLN_THREAD_PARAMS_TCP_SOCKETS* thread_params = (XLLN_THREAD_PARAMS_TCP_SOCKETS*)lpParam;
	
	delete thread_params;
	thread_params = 0;
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s starting."
		, __func__
	);
	
	const size_t packetSizeType = sizeof(XllnNetworkPacket::TYPE);
	const size_t packetSizeTypeTcpConnectRequest = sizeof(XllnNetworkPacket::TCP_CONNECT_REQUEST);
	const size_t packetSizeTypeTcpConnectResponse = sizeof(XllnNetworkPacket::TCP_CONNECT_RESPONSE);
	const size_t packetSizeTypeTcpSequencedDataAck = sizeof(XllnNetworkPacket::TCP_SEQUENCED_DATA_ACK);
	
	std::list<uint32_t> waitMillis;
	__timeb64 timeCurrent = {0, 0, 0, 0};
	while (1) {
		waitMillis.sort();
		uint32_t waitLength = (waitMillis.size() ? *waitMillis.begin() : INFINITE);
		//XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
		//	, "%s calling WaitForSingleObject with timeout (0x%x)."
		//	, __func__
		//	, waitLength
		//);
		DWORD resultWait = WaitForSingleObject(xlln_tcp_sockets_update, waitLength);
		if (resultWait != WAIT_OBJECT_0 && resultWait != STATUS_TIMEOUT) {
			uint32_t errorWait = GetLastError();
			XLLN_DEBUG_LOG_ECODE(errorWait, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
				, "%s WaitForSingleObject failed with result 0x%08x."
				, __func__
				, resultWait
			);
			break;
		}
		
		if (xlln_thread_shutdown) {
			break;
		}
		
		{
			__timeb64 timeUpdated;
			_ftime64_s(&timeUpdated);
			
			int64_t timeDifference = TimeDiffMilliseconds(timeCurrent, timeUpdated);
			// Treat a backward step of the wall clock, such as a Windows time sync, as no time passed.
			if (timeDifference < 0) {
				timeDifference = 0;
			}
			
			// Update the list of times to wait by subtracting the time already waited. Remove past entries.
			for (auto itrWaitTime = waitMillis.begin(); itrWaitTime != waitMillis.end(); ) {
				if (*itrWaitTime <= timeDifference || timeDifference >= INFINITE) {
					waitMillis.erase(itrWaitTime++);
				}
				else {
					*itrWaitTime -= (uint32_t)timeDifference;
					itrWaitTime++;
				}
			}
			
			timeCurrent = timeUpdated;
		}
		
		EnterCriticalSection(&xlive_critsec_sockets);
		
		ResetEvent(xlln_tcp_sockets_update);
		
		bool tcpActive = false;
		
		for (auto itrTitleSocket = xlive_title_sockets.begin(); itrTitleSocket != xlive_title_sockets.end(); ) {
			XLIVE_TITLE_SOCKET* titleSocket = itrTitleSocket->second;
			
			if (titleSocket->protocol != IPPROTO_TCP) {
				itrTitleSocket++;
				continue;
			}
			
			if (titleSocket->passthroughEnabled) {
				itrTitleSocket++;
				continue;
			}
			
			if (titleSocket->tcpIsConnecting) {
				if (TimeDiffMilliseconds(titleSocket->tcpConnectionAttemptLast, timeCurrent) >= xlln_tcp_packet_timeout) {
					if (titleSocket->tcpConnectionAttempts++ >= xlln_tcp_max_connection_attempts) {
						
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
							, "%s title_socket_handle (0x%zx) TCP failed to connect to 0x%08x:%hu."
							, __func__
							, titleSocket->handle
							, titleSocket->tcpRemoteInstanceId
							, titleSocket->tcpRemoteTitlePort
						);
						
						titleSocket->tcpIsConnecting = false;
						titleSocket->socketOptions[SO_ERROR] = WSAETIMEDOUT;
						
						xlive_title_socket_tcp_connected_sockets.erase(std::make_pair(titleSocket->tcpRemoteInstanceId, titleSocket->tcpRemoteTitlePort));
						
						SetEvent(titleSocket->tcpConnectNotify);
						titleSocket->selectNotifyExceptPending = true;
						SetEvent(titleSocket->selectNotifyExcept);
						if (titleSocket->wsaEventSelectConnect != WSA_INVALID_EVENT) {
							SetEvent(titleSocket->wsaEventSelectConnect);
						}
					}
					else {
						titleSocket->tcpConnectionAttemptLast = timeCurrent;
						
						waitMillis.push_back(xlln_tcp_packet_timeout);
						
						XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
						sendPacket->destinationInstanceId = titleSocket->tcpRemoteInstanceId;
						{
							const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectRequest;
							
							uint8_t* packetBuffer = new uint8_t[packetSize];
							packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_REQUEST;
							XllnNetworkPacket::TCP_CONNECT_REQUEST &packetTcpConnectRequest = *(XllnNetworkPacket::TCP_CONNECT_REQUEST*)&packetBuffer[packetSizeType];
							packetTcpConnectRequest.instanceId = xlln_global_instance_id;
							packetTcpConnectRequest.titlePortDestination = titleSocket->tcpRemoteTitlePort;
							packetTcpConnectRequest.titlePortSource = titleSocket->portActual;
							
							sendPacket->data = packetBuffer;
							sendPacket->dataSize = packetSize;
						}
						
						if (sendPacket->destinationInstanceId == xlln_global_instance_id) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s title_socket_handle (0x%zx) shortcutted %s destined for local socket."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_REQUEST)
							);
							
							SOCKADDR_STORAGE sockaddr = {AF_INET};
							(*(sockaddr_in*)&sockaddr).sin_addr.s_addr = htonl(INADDR_LOOPBACK);
							(*(sockaddr_in*)&sockaddr).sin_port = htons(xlln_network_instance_port);
							if (!ParseNetworkData(sendPacket->data, sendPacket->dataSize, &sockaddr)) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
									, "%s title_socket_handle (0x%zx) TCP failed to send connect packet to LOOPBACK."
									, __func__
									, titleSocket->handle
								);
							}
						}
						else if (SendPacketToRemoteInstance(sendPacket)) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s title_socket_handle (0x%zx) TCP sent %s to 0x%08x:%hu."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_REQUEST)
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
							);
						}
						else {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s title_socket_handle (0x%zx) TCP failed to send %s to 0x%08x:%hu."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_REQUEST)
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
							);
							
							delete sendPacket;
							
							titleSocket->tcpIsConnecting = false;
							titleSocket->socketOptions[SO_ERROR] = WSAENETUNREACH;
							
							SetEvent(titleSocket->tcpConnectNotify);
							titleSocket->selectNotifyExceptPending = true;
							SetEvent(titleSocket->selectNotifyExcept);
							if (titleSocket->wsaEventSelectConnect != WSA_INVALID_EVENT) {
								SetEvent(titleSocket->wsaEventSelectConnect);
							}
						}
						sendPacket = 0;
					}
				}
			}
			else if (titleSocket->tcpIsConnected && IsTimeUnset(titleSocket->tcpLastResponse)) {
				if (TimeDiffMilliseconds(titleSocket->tcpConnectionAttemptLast, timeCurrent) >= xlln_tcp_packet_timeout) {
					if (titleSocket->tcpConnectionAttempts++ >= xlln_tcp_max_connection_attempts) {
						
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
							, "%s title_socket_handle (0x%zx) TCP lost connection after no responses from 0x%08x:%hu."
							, __func__
							, titleSocket->handle
							, titleSocket->tcpRemoteInstanceId
							, titleSocket->tcpRemoteTitlePort
						);
						
						titleSocket->tcpIsConnected = false;
						titleSocket->tcpConnectionLost = true;
						titleSocket->socketOptions[SO_ERROR] = WSAETIMEDOUT;
						
						SetEvent(titleSocket->tcpConnectNotify);
						// A blocking recv watches this event only, not the select events below.
						SetEvent(titleSocket->recvNotify);
						titleSocket->selectNotifyExceptPending = true;
						SetEvent(titleSocket->selectNotifyExcept);
						if (titleSocket->wsaEventSelectClose != WSA_INVALID_EVENT) {
							SetEvent(titleSocket->wsaEventSelectClose);
						}
					}
					else {
						titleSocket->tcpConnectionAttemptLast = timeCurrent;
						
						waitMillis.push_back(xlln_tcp_packet_timeout);
						
						XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
						sendPacket->destinationInstanceId = titleSocket->tcpRemoteInstanceId;
						{
							const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectResponse;
							
							uint8_t* packetBuffer = new uint8_t[packetSize];
							packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE;
							XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetBuffer[packetSizeType];
							packetTcpConnectResponse.titlePacket.instanceId = xlln_global_instance_id;
							packetTcpConnectResponse.titlePacket.titlePortDestination = titleSocket->tcpRemoteTitlePort;
							packetTcpConnectResponse.titlePacket.titlePortSource = titleSocket->portActual;
							packetTcpConnectResponse.responseCode = XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_SUCCESS;
							
							sendPacket->data = packetBuffer;
							sendPacket->dataSize = packetSize;
						}
						
						if (sendPacket->destinationInstanceId == xlln_global_instance_id) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s title_socket_handle (0x%zx) shortcutted %s %s destined for local socket."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_SUCCESS)
							);
							
							SOCKADDR_STORAGE sockaddr = {AF_INET};
							(*(sockaddr_in*)&sockaddr).sin_addr.s_addr = htonl(INADDR_LOOPBACK);
							(*(sockaddr_in*)&sockaddr).sin_port = htons(xlln_network_instance_port);
							if (!ParseNetworkData(sendPacket->data, sendPacket->dataSize, &sockaddr)) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
									, "%s title_socket_handle (0x%zx) TCP failed to send connect packet to LOOPBACK."
									, __func__
									, titleSocket->handle
								);
							}
							
							delete sendPacket;
						}
						else if (SendPacketToRemoteInstance(sendPacket)) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s title_socket_handle (0x%zx) TCP sent %s %s to 0x%08x:%hu."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_SUCCESS)
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
							);
						}
						else {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s title_socket_handle (0x%zx) TCP failed to send %s %s to 0x%08x:%hu."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_SUCCESS)
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
							);
							
							delete sendPacket;
						}
						sendPacket = 0;
					}
				}
			}
			else if (titleSocket->tcpIsConnected) {
				size_t iSendSequenceInfo = 0;
				for (XTS_TCP_SEND_SEQUENCE_INFO* sendSequenceInfo : titleSocket->tcpSendQueue) {
					if (++iSendSequenceInfo > xlln_tcp_send_queue_max) {
						break;
					}
					if (TimeDiffMilliseconds(sendSequenceInfo->lastSent, timeCurrent) >= xlln_tcp_packet_timeout) {
						XLLN_NET_SEND_PACKET_INFO* sendPacket = sendSequenceInfo->sendPacket->Clone();
						
						if (!SendPacketToRemoteInstance(sendPacket)) {
							delete sendPacket;
							
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s title_socket_handle (0x%zx) SendPacketToRemoteInstance failed for %s of sequenceId (%hu)."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA)
								, sendSequenceInfo->sequenceId
							);
						}
						else if (!IsTimeUnset(sendSequenceInfo->lastSent)) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
								, "%s title_socket_handle (0x%zx) resending %s of sequenceId (%hu)."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA)
								, sendSequenceInfo->sequenceId
							);
						}
						else {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s title_socket_handle (0x%zx) sending %s of sequenceId (%hu)."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA)
								, sendSequenceInfo->sequenceId
							);
						}
						
						sendPacket = 0;
						
						sendSequenceInfo->lastSent = timeCurrent;
						waitMillis.push_back(xlln_tcp_packet_timeout);
					}
				}
				
				if (!IsTimeUnset(titleSocket->tcpRecvLastAck) && TimeDiffMilliseconds(titleSocket->tcpRecvLastAck, timeCurrent) >= xlln_tcp_ack_frequency) {
					XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
					sendPacket->destinationInstanceId = titleSocket->tcpRemoteInstanceId;
					{
						std::vector<uint16_t> recvSequenceIds;
						for (size_t iTcpRecvQueue = 0; iTcpRecvQueue < titleSocket->tcpRecvQueue.size(); iTcpRecvQueue++) {
							if (!titleSocket->tcpRecvQueue[iTcpRecvQueue]) {
								continue;
							}
							recvSequenceIds.push_back((titleSocket->tcpRecvSequenceId + iTcpRecvQueue) % 0x10000);
						}
						
						const size_t packetSize = packetSizeType + packetSizeTypeTcpSequencedDataAck + (recvSequenceIds.size() * sizeof(uint16_t));
						
						uint8_t* packetBuffer = new uint8_t[packetSize];
						packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA_ACK;
						XllnNetworkPacket::TCP_SEQUENCED_DATA_ACK &packetTcpSequencedDataAck = *(XllnNetworkPacket::TCP_SEQUENCED_DATA_ACK*)&packetBuffer[packetSizeType];
						packetTcpSequencedDataAck.titlePacket.instanceId = xlln_global_instance_id;
						packetTcpSequencedDataAck.titlePacket.titlePortDestination = titleSocket->tcpRemoteTitlePort;
						packetTcpSequencedDataAck.titlePacket.titlePortSource = titleSocket->portActual;
						packetTcpSequencedDataAck.recvSequenceAt = titleSocket->tcpRecvSequenceId;
						packetTcpSequencedDataAck.recvSequenceIdsCount = (uint16_t)recvSequenceIds.size();
						
						uint16_t* packetTcpSequencedDataAckSequences = (uint16_t*)&packetBuffer[packetSizeType + packetSizeTypeTcpSequencedDataAck];
						for (size_t iSequenceId = 0; iSequenceId < recvSequenceIds.size(); iSequenceId++) {
							packetTcpSequencedDataAckSequences[iSequenceId] = recvSequenceIds[iSequenceId];
						}
						
						sendPacket->data = packetBuffer;
						sendPacket->dataSize = packetSize;
					}
					
					if (!SendPacketToRemoteInstance(sendPacket)) {
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
							, "%s title_socket_handle (0x%zx) failed to send %s packet to 0x%08x:%hu."
							, __func__
							, titleSocket->handle
							, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA_ACK)
							, titleSocket->tcpRemoteInstanceId
							, titleSocket->tcpRemoteTitlePort
						);
						
						delete sendPacket;
					}
					else {
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
							, "%s title_socket_handle (0x%zx) sent %s packet with tcpRecvSequenceId (%hu) to 0x%08x:%hu."
							, __func__
							, titleSocket->handle
							, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_SEQUENCED_DATA_ACK)
							, titleSocket->tcpRecvSequenceId
							, titleSocket->tcpRemoteInstanceId
							, titleSocket->tcpRemoteTitlePort
						);
					}
					sendPacket = 0;
					
					titleSocket->tcpRecvLastAck = timeCurrent;
					waitMillis.push_back(xlln_tcp_ack_frequency);
				}
				else if (!IsTimeUnset(titleSocket->tcpRecvLastAck)) {
					// An owed ack has no other timer to ride on, so it schedules its own wake-up.
					const int64_t ackElapsedMs = TimeDiffMilliseconds(titleSocket->tcpRecvLastAck, timeCurrent);
					const int64_t ackRemainingMs = (int64_t)xlln_tcp_ack_frequency - (ackElapsedMs > 0 ? ackElapsedMs : 0);
					waitMillis.push_back(ackRemainingMs > 1 ? (uint32_t)ackRemainingMs : 1);
				}
				
				if (TimeDiffMilliseconds(titleSocket->tcpLastResponse, timeCurrent) >= xlln_tcp_packet_timeout && TimeDiffMilliseconds(titleSocket->tcpConnectionAttemptLast, timeCurrent) >= xlln_tcp_packet_timeout) {
					if (titleSocket->tcpConnectionAttempts++ >= xlln_tcp_max_connection_attempts) {
						titleSocket->hasShutdown = XTS_SHUTDOWN_STATE::XTS_SS_NO_SEND;
						if (titleSocket->recvPacketQueue.size() && titleSocket->tcpConnectionAttempts - xlln_tcp_max_connection_attempts <= xlln_tcp_lost_connection_drain_recv_queue_attempts) {
							// If the queue is actually being drained then allow further attempts.
							if (titleSocket->tcpRecvPacketQueueLastSize != titleSocket->recvPacketQueue.size()) {
								titleSocket->tcpConnectionAttempts--;
							}
							titleSocket->tcpRecvPacketQueueLastSize = titleSocket->recvPacketQueue.size();
							
							// Wait for the recv queue to be drained by the Title.
							titleSocket->tcpConnectionAttemptLast = timeCurrent;
							waitMillis.push_back(xlln_tcp_packet_timeout);
							
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s title_socket_handle (0x%zx) TCP lost connection to remote 0x%08x:%hu but still draining recvPacketQueue (%zu)."
								, __func__
								, titleSocket->handle
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
								, titleSocket->recvPacketQueue.size()
							);
						}
						else {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s title_socket_handle (0x%zx) TCP lost connection to remote 0x%08x:%hu%s."
								, __func__
								, titleSocket->handle
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
								, (titleSocket->recvPacketQueue.size() ? " failed to drain recvPacketQueue" : "")
							);
							
							titleSocket->tcpIsConnected = false;
							titleSocket->tcpConnectionLost = true;
							titleSocket->socketOptions[SO_ERROR] = WSAETIMEDOUT;
							
							SetEvent(titleSocket->tcpConnectNotify);
							// A blocking recv watches this event only, not the select events below.
							SetEvent(titleSocket->recvNotify);
							titleSocket->selectNotifyExceptPending = true;
							SetEvent(titleSocket->selectNotifyExcept);
							if (titleSocket->wsaEventSelectClose != WSA_INVALID_EVENT) {
								SetEvent(titleSocket->wsaEventSelectClose);
							}
						}
					}
					else {
						titleSocket->tcpConnectionAttemptLast = timeCurrent;
						
						waitMillis.push_back(xlln_tcp_packet_timeout);
						
						XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
						sendPacket->destinationInstanceId = titleSocket->tcpRemoteInstanceId;
						{
							const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectResponse;
							
							uint8_t* packetBuffer = new uint8_t[packetSize];
							packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE;
							XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetBuffer[packetSizeType];
							packetTcpConnectResponse.titlePacket.instanceId = xlln_global_instance_id;
							packetTcpConnectResponse.titlePacket.titlePortDestination = titleSocket->tcpRemoteTitlePort;
							packetTcpConnectResponse.titlePacket.titlePortSource = titleSocket->portActual;
							packetTcpConnectResponse.responseCode = XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE_REQUEST;
							
							sendPacket->data = packetBuffer;
							sendPacket->dataSize = packetSize;
						}
						
						if (sendPacket->destinationInstanceId == xlln_global_instance_id) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s title_socket_handle (0x%zx) shortcutted %s %s destined for local socket."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE_REQUEST)
							);
							
							SOCKADDR_STORAGE sockaddr = {AF_INET};
							(*(sockaddr_in*)&sockaddr).sin_addr.s_addr = htonl(INADDR_LOOPBACK);
							(*(sockaddr_in*)&sockaddr).sin_port = htons(xlln_network_instance_port);
							if (!ParseNetworkData(sendPacket->data, sendPacket->dataSize, &sockaddr)) {
								XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
									, "%s title_socket_handle (0x%zx) TCP failed to send %s packet to LOOPBACK."
									, __func__
									, titleSocket->handle
									, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE_REQUEST)
								);
							}
							
							delete sendPacket;
						}
						else if (SendPacketToRemoteInstance(sendPacket)) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s title_socket_handle (0x%zx) TCP sent %s %s to 0x%08x:%hu."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE_REQUEST)
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
							);
						}
						else {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s title_socket_handle (0x%zx) TCP failed to send %s %s to 0x%08x:%hu."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_KEEP_ALIVE_REQUEST)
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
							);
							
							delete sendPacket;
						}
						sendPacket = 0;
					}
				}
				else {
					// An idle connection submits no other deadline, and the peer's own probe refreshes
					// tcpLastResponse here, so without this its probing keeps this side silent.
					const int64_t responseElapsedMs = TimeDiffMilliseconds(titleSocket->tcpLastResponse, timeCurrent);
					int64_t keepAliveRemainingMs = (int64_t)xlln_tcp_packet_timeout - (responseElapsedMs > 0 ? responseElapsedMs : 0);
					if (!IsTimeUnset(titleSocket->tcpConnectionAttemptLast)) {
						const int64_t attemptElapsedMs = TimeDiffMilliseconds(titleSocket->tcpConnectionAttemptLast, timeCurrent);
						const int64_t attemptRemainingMs = (int64_t)xlln_tcp_packet_timeout - (attemptElapsedMs > 0 ? attemptElapsedMs : 0);
						if (attemptRemainingMs > keepAliveRemainingMs) {
							keepAliveRemainingMs = attemptRemainingMs;
						}
					}
					waitMillis.push_back(keepAliveRemainingMs > 1 ? (uint32_t)keepAliveRemainingMs : 1);
				}
				
			}
			
			if (titleSocket->tcpIsClosing) {
				if (IsTimeUnset(titleSocket->tcpCloseAfter)) {
					titleSocket->tcpCloseAfter = timeCurrent;
					TimeAddMillis(titleSocket->tcpCloseAfter, xlln_tcp_socket_close_timeout);
					waitMillis.push_back(xlln_tcp_socket_close_timeout);
				}
				else if (!titleSocket->tcpSendQueue.size() || IsTimeLessThan(titleSocket->tcpCloseAfter, timeCurrent)) {
					
					if (!(titleSocket->hasShutdown & XTS_SHUTDOWN_STATE::XTS_SS_NO_SEND)) {
						XLLN_NET_SEND_PACKET_INFO* sendPacket = new XLLN_NET_SEND_PACKET_INFO;
						sendPacket->destinationInstanceId = titleSocket->tcpRemoteInstanceId;
						{
							const size_t packetSize = packetSizeType + packetSizeTypeTcpConnectResponse;
							
							uint8_t* packetBuffer = new uint8_t[packetSize];
							packetBuffer[0] = XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE;
							XllnNetworkPacket::TCP_CONNECT_RESPONSE &packetTcpConnectResponse = *(XllnNetworkPacket::TCP_CONNECT_RESPONSE*)&packetBuffer[packetSizeType];
							packetTcpConnectResponse.titlePacket.instanceId = xlln_global_instance_id;
							packetTcpConnectResponse.titlePacket.titlePortDestination = titleSocket->tcpRemoteTitlePort;
							packetTcpConnectResponse.titlePacket.titlePortSource = titleSocket->portActual;
							packetTcpConnectResponse.responseCode = XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT;
							
							sendPacket->data = packetBuffer;
							sendPacket->dataSize = packetSize;
						}
						
						if (SendPacketToRemoteInstance(sendPacket)) {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
								, "%s title_socket_handle (0x%zx) TCP sent %s %s to 0x%08x:%hu."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT)
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
							);
						}
						else {
							XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
								, "%s title_socket_handle (0x%zx) TCP failed to send %s %s to 0x%08x:%hu."
								, __func__
								, titleSocket->handle
								, XllnNetworkPacket::GetTcpResponseCodeName(XllnNetworkPacket::TCP_RESPONSE_CODE::XLLN_NP_TRC_REJECT)
								, XllnNetworkPacket::GetPacketTypeName(XllnNetworkPacket::TYPE::XLLN_NPT_TCP_CONNECT_RESPONSE)
								, titleSocket->tcpRemoteInstanceId
								, titleSocket->tcpRemoteTitlePort
							);
							
							delete sendPacket;
						}
						sendPacket = 0;
					}
					
					if (titleSocket->wsaEventSelectClose != WSA_INVALID_EVENT) {
						SetEvent(titleSocket->wsaEventSelectClose);
					}
					
					xlive_title_sockets.erase(itrTitleSocket++);
					
					for (auto itrTitleSocketConnected = xlive_title_socket_tcp_connected_sockets.begin(); itrTitleSocketConnected != xlive_title_socket_tcp_connected_sockets.end(); ) {
						if (itrTitleSocketConnected->second == titleSocket) {
							xlive_title_socket_tcp_connected_sockets.erase(itrTitleSocketConnected++);
						}
						else {
							itrTitleSocketConnected++;
						}
					}
					
					if (titleSocket->tcpSendQueue.size()) {
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN
							, "%s title_socket_handle (0x%zx) has been forcefully closed with a tcpSendQueue of size (0x%zx)."
							, __func__
							, titleSocket->handle
							, titleSocket->tcpSendQueue.size()
						);
					}
					else {
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_INFO
							, "%s title_socket_handle (0x%zx) has been closed."
							, __func__
							, titleSocket->handle
						);
					}
					
					delete titleSocket;
					titleSocket = 0;
					
					continue;
				}
			}
			
			if (titleSocket->tcpIsConnecting || titleSocket->tcpIsAccepting || titleSocket->tcpIsConnected || titleSocket->tcpIsClosing) {
				tcpActive = true;
			}
			
			itrTitleSocket++;
		}
		
		if (tcpActive && !waitMillis.size()) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
				, "%s TCP socket(s) active but no timeouts submitted!"
				, __func__
			);
			waitMillis.push_back(6000);
		}
		
		LeaveCriticalSection(&xlive_critsec_sockets);
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s exiting."
		, __func__
	);
	
	SetEvent(xlln_thread_tcp_sockets_exited);
	
	return ERROR_SUCCESS;
}

bool SendPacketToRemoteInstance(XLLN_NET_SEND_PACKET_INFO* send_packet)
{
	if (!send_packet) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s send_packet is NULL."
			, __func__
		);
		return false;
	}
	if (!send_packet->data) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s send_packet->data is NULL."
			, __func__
		);
		return false;
	}
	if (!send_packet->dataSize) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s send_packet->dataSize is 0."
			, __func__
		);
		return false;
	}
	if (!send_packet->destinationInstanceId) {
		if (send_packet->destinationAddress.ss_family == AF_UNSPEC) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
				, "%s send_packet->destinationInstanceId is 0 and send_packet->destinationAddress.ss_family is UNSPECIFIED."
				, __func__
			);
			return false;
		}
		if (send_packet->destinationAddress.ss_family != AF_INET && send_packet->destinationAddress.ss_family != AF_INET6) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
				, "%s send_packet->destinationAddress.ss_family (0x%hu) is unsupported."
				, __func__
				, send_packet->destinationAddress.ss_family
			);
			return false;
		}
	}
	
	EnterCriticalSection(&xlln_critsec_network_send);
	
	xlln_network_send_queue.push_back(send_packet);
	SetEvent(xlln_network_send_queue_notify);
	
	LeaveCriticalSection(&xlln_critsec_network_send);
	
	return true;
}

bool SubmitDataToTitleSocket_(XLIVE_TITLE_SOCKET* title_socket, XTS_RECV_PACKET* recv_packet)
{
	if (title_socket->hasShutdown & XTS_SHUTDOWN_STATE::XTS_SS_NO_RECV) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s title_socket_handle (0x%zx) socket has been shutdown for receiving."
			, __func__
			, title_socket->handle
		);
		return false;
	}
	
	title_socket->recvPacketQueue.push_back(recv_packet);
	
	title_socket->socketIoctls[FIONREAD] = 0;
	for (XTS_RECV_PACKET* recvPacketNext : title_socket->recvPacketQueue) {
		title_socket->socketIoctls[FIONREAD] += recvPacketNext->dataFilledSize;
	}
	
	title_socket->selectNotifyReadPending = true;
	SetEvent(title_socket->selectNotifyRead);
	
	if (title_socket->wsaEventSelectRead != WSA_INVALID_EVENT) {
		SetEvent(title_socket->wsaEventSelectRead);
	}
	
	SetEvent(title_socket->recvNotify);
	// Signal the socket handle itself so it reflects "recv completed / data ready": a title
	// using event-less overlapped I/O (hEvent=NULL, no completion routine) may wait on the
	// socket handle for completion (Win32 signals the file/socket handle when hEvent is NULL).
	SetEvent((HANDLE)title_socket->handle);
	if (title_socket->recvPendingOverlapped.size()) {
		// Wake the OLDEST pending overlapped recv (completions happen in FIFO order).
		WSAOVERLAPPED* headOverlapped = title_socket->recvPendingOverlapped.front().wsaOverlapped;
		if (headOverlapped->hEvent && headOverlapped->hEvent != INVALID_HANDLE_VALUE) {
			WSASetEvent(headOverlapped->hEvent);
		}
	}

	return true;
}

bool SubmitDataToTitleSocketHandle_(SOCKET title_socket_handle, XTS_RECV_PACKET* recv_packet)
{
	auto itrTitleSocket = xlive_title_sockets.find(title_socket_handle);
	if (itrTitleSocket == xlive_title_sockets.end()) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s title_socket_handle (0x%zx) does not exist."
			, __func__
			, title_socket_handle
		);
		return false;
	}
	
	bool success = SubmitDataToTitleSocket_(itrTitleSocket->second, recv_packet);
	
	return success;
}

bool SubmitDataToTitleSocketHandle(SOCKET title_socket_handle, XTS_RECV_PACKET* recv_packet)
{
	EnterCriticalSection(&xlive_critsec_sockets);
	
	bool result = SubmitDataToTitleSocketHandle_(title_socket_handle, recv_packet);
	
	LeaveCriticalSection(&xlive_critsec_sockets);
	
	return result;
}

typedef struct _XLLN_THREAD_PARAMS_SOCKET_MAIN_SEND {
} XLLN_THREAD_PARAMS_SOCKET_MAIN_SEND;

static DWORD WINAPI XllnThreadSocketMainSend(void* lpParam)
{
	XLLN_THREAD_PARAMS_SOCKET_MAIN_SEND* thread_params = (XLLN_THREAD_PARAMS_SOCKET_MAIN_SEND*)lpParam;
	
	delete thread_params;
	thread_params = 0;
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s starting."
		, __func__
	);
	
	bool skipWait = false;
	while (1) {
		if (skipWait) {
			skipWait = false;
		}
		else {
			DWORD resultWait = WaitForSingleObject(xlln_network_send_queue_notify, INFINITE);
			if (resultWait != WAIT_OBJECT_0) {
				uint32_t errorWait = GetLastError();
				XLLN_DEBUG_LOG_ECODE(errorWait, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
					, "%s WaitForSingleObject failed with result 0x%08x."
					, __func__
					, resultWait
				);
				break;
			}
		}
		
		if (xlln_thread_shutdown) {
			break;
		}
		
		EnterCriticalSection(&xlln_critsec_network_send);
		
		auto itrSendPacket = xlln_network_send_queue.begin();
		XLLN_NET_SEND_PACKET_INFO* sendPacket = 0;
		if (itrSendPacket != xlln_network_send_queue.end()) {
			sendPacket = *itrSendPacket;
			xlln_network_send_queue.erase(itrSendPacket);
		}
		
		if (xlln_network_send_queue.size()) {
			skipWait = true;
			ResetEvent(xlln_network_send_queue_notify);
		}
		
		LeaveCriticalSection(&xlln_critsec_network_send);
		
		if (!sendPacket) {
			continue;
		}
		
		if (xlln_network_socket_main != INVALID_SOCKET) {
			if (sendPacket->destinationInstanceId == INADDR_BROADCAST) {
				std::vector<SOCKADDR_STORAGE> destinationAddresses;
				
				{
					EnterCriticalSection(&xlln_critsec_network_broadcast_addresses);
					
					for (const XllnNetworkBroadcastEntity::BROADCAST_ENTITY &broadcastEntity : xlln_network_broadcast_addresses) {
						if (sendPacket->skipHubServers && broadcastEntity.entityType == XllnNetworkBroadcastEntity::TYPE::XLLN_NBE_HUB_SERVER) {
							continue;
						}
						destinationAddresses.push_back(broadcastEntity.sockaddr);
						SOCKADDR_STORAGE &destinationAddress = destinationAddresses.back();
						if (GetSockAddrPort(&destinationAddress) == 0) {
							if (!SetSockAddrPort(&destinationAddress, xlln_network_instance_base_port)) {
								__debugbreak();
							}
						}
					}
					
					LeaveCriticalSection(&xlln_critsec_network_broadcast_addresses);
				}
				
				{
					EnterCriticalSection(&xlive_critsec_network_adapter);
					
					// TODO is deadcoded.
					for (SOCKADDR_STORAGE &multicastAddress : xlln_multicast_addresses) {
						destinationAddresses.push_back(multicastAddress);
					}
					
					for (ELIGIBLE_NETWORK_INTERFACE* eligibleNetworkInterface : xlive_eligible_network_adapters) {
						
						if (xlive_specific_network_adapter && eligibleNetworkInterface != xlive_specific_network_adapter) {
							continue;
						}
						
						// A broadcast on the loopback interface never leaves this machine, so the
						// peer only ever sees it as a 127.0.0.1 source address.
						if (
							eligibleNetworkInterface->addressUnicast.ss_family == AF_INET
							&& (ntohl(((sockaddr_in*)&eligibleNetworkInterface->addressUnicast)->sin_addr.s_addr) >> 24) == 127
						) {
							continue;
						}
						
						// TODO IPv6 support?
						// Ignore interfaces that have the same broadcast address as the unicast.
						if (
							eligibleNetworkInterface->addressUnicast.ss_family == AF_INET
							&& eligibleNetworkInterface->addressBroadcast.ss_family == AF_INET
							&& ((sockaddr_in*)&eligibleNetworkInterface->addressUnicast)->sin_addr.s_addr == ((sockaddr_in*)&eligibleNetworkInterface->addressBroadcast)->sin_addr.s_addr
						) {
							continue;
						}
						// Ignore interfaces that incorrectly have a broadcast address as the wildcard any address which occurs when the prefix length is 0.
						if (
							eligibleNetworkInterface->addressBroadcast.ss_family == AF_INET
							&& ((sockaddr_in*)&eligibleNetworkInterface->addressBroadcast)->sin_addr.s_addr == htonl(INADDR_ANY)
						) {
							continue;
						}
						
						bool skipAddingNetworkInterfaceBroadcast = false;
						
						for (auto itrDestinationAddresses = destinationAddresses.begin(); itrDestinationAddresses != destinationAddresses.end(); ) {
							// TODO IPv6 support.
							if (eligibleNetworkInterface->addressUnicast.ss_family == AF_INET && itrDestinationAddresses->ss_family == AF_INET && GetSockAddrPort(&*itrDestinationAddresses) == xlln_network_instance_base_port) {
								uint32_t dwMask = 0;
								DWORD dwRetVal = IPHLPAPI_ConvertLengthToIpv4Mask(eligibleNetworkInterface->prefixLength, &dwMask);
								if (dwRetVal != NO_ERROR) {
									++itrDestinationAddresses;
									continue;
								}
								
								// If the destination addresses include the network interface's network address then remove that and skip adding the broadcast address of this network interface.
								if (((sockaddr_in*)&*itrDestinationAddresses)->sin_addr.s_addr == (dwMask & (((sockaddr_in*)&eligibleNetworkInterface->addressUnicast)->sin_addr.s_addr))) {
									destinationAddresses.erase(itrDestinationAddresses++);
									skipAddingNetworkInterfaceBroadcast = true;
									break;
								}
								
								// If the destination addresses include an address within the subnet of this network interface, skip adding the broadcast address of this network interface.
								if ((dwMask & ((sockaddr_in*)&*itrDestinationAddresses)->sin_addr.s_addr) == (dwMask & (((sockaddr_in*)&eligibleNetworkInterface->addressUnicast)->sin_addr.s_addr))) {
									skipAddingNetworkInterfaceBroadcast = true;
									break;
								}
							}
							++itrDestinationAddresses;
						}
						
						if (skipAddingNetworkInterfaceBroadcast) {
							continue;
						}
						
						destinationAddresses.push_back(eligibleNetworkInterface->addressBroadcast);
						SOCKADDR_STORAGE &destinationAddress = destinationAddresses.back();
						if (!SetSockAddrPort(&destinationAddress, xlln_network_instance_base_port)) {
							__debugbreak();
						}
					}
					
					LeaveCriticalSection(&xlive_critsec_network_adapter);
				}
				
				if (!destinationAddresses.size()) {
					XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
						, "%s unable to broadcast when there are no known addresses to send to."
						, __func__
					);
				}
				
				for (SOCKADDR_STORAGE &destinationAddress : destinationAddresses) {
					int32_t resultSendTo = sendto(xlln_network_socket_main, (char*)sendPacket->data, sendPacket->dataSize, 0, (sockaddr*)&destinationAddress, sizeof(SOCKADDR_STORAGE));
					char* sockAddrInfo = 0;
					if (resultSendTo <= 0) {
						int32_t errorSendTo = WSAGetLastError();
						sockAddrInfo = GET_SOCKADDR_INFO(&destinationAddress);
						XLLN_DEBUG_LOG_ECODE(errorSendTo, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
							, "%s sendto %s broadcast with buffer size (0x%zx) failed with error"
							, __func__
							, sockAddrInfo ? sockAddrInfo : ""
							, sendPacket->dataSize
						);
					}
					else if (resultSendTo != sendPacket->dataSize) {
						sockAddrInfo = GET_SOCKADDR_INFO(&destinationAddress);
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
							, "%s broadcast truncated from (%zu) to (%d) on sendto %s."
							, __func__
							, sendPacket->dataSize
							, resultSendTo
							, sockAddrInfo ? sockAddrInfo : ""
						);
					}
					else {
						sockAddrInfo = GET_SOCKADDR_INFO(&destinationAddress);
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
							, "%s broadcast sendto %s."
							, __func__
							, sockAddrInfo ? sockAddrInfo : ""
						);
					}
					if (sockAddrInfo) {
						free(sockAddrInfo);
						sockAddrInfo = 0;
					}
				}
			}
			else {
				bool abortSend = false;
				uint32_t askUnknownInstanceId = 0;
				if (sendPacket->destinationInstanceId) {
					EnterCriticalSection(&xlln_critsec_network_net_entity);
					
					auto itrIiToRa = xlln_net_entity_instance_id_to_remote_address.find(sendPacket->destinationInstanceId);
					if (itrIiToRa == xlln_net_entity_instance_id_to_remote_address.end()) {
						abortSend = true;
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
							, "%s destinationInstanceId (0x%08x) has no remote address cached. Unable to send."
							, __func__
							, sendPacket->destinationInstanceId
						);
						
						__time64_t ltime;
						_time64(&ltime);
						auto itrIiToLa = xlln_net_entity_unknown_instance_id_last_ask.find(sendPacket->destinationInstanceId);
						if (itrIiToLa == xlln_net_entity_unknown_instance_id_last_ask.end() || itrIiToLa->second < ltime) {
							xlln_net_entity_unknown_instance_id_last_ask[sendPacket->destinationInstanceId] = ltime;
							askUnknownInstanceId = sendPacket->destinationInstanceId;
						}
					}
					else {
						sendPacket->destinationAddress = itrIiToRa->second.sockaddr;
					}
					
					LeaveCriticalSection(&xlln_critsec_network_net_entity);
				}
				
				// Queued outside the lock above: this enqueues onto the send queue, which is guarded by
				// a different critical section.
				if (askUnknownInstanceId) {
					XllnNetEntityBroadcastUnknownUserAsk(askUnknownInstanceId);
				}
				if (!abortSend) {
					int32_t resultSendTo = sendto(xlln_network_socket_main, (char*)sendPacket->data, sendPacket->dataSize, 0, (sockaddr*)&sendPacket->destinationAddress, sizeof(SOCKADDR_STORAGE));
					char* sockAddrInfo = 0;
					if (resultSendTo <= 0) {
						int32_t errorSendTo = WSAGetLastError();
						sockAddrInfo = GET_SOCKADDR_INFO(&sendPacket->destinationAddress);
						XLLN_DEBUG_LOG_ECODE(errorSendTo, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
							, "%s sendto 0x%08x %s with buffer size (0x%zx) failed with error"
							, __func__
							, sendPacket->destinationInstanceId
							, sockAddrInfo ? sockAddrInfo : ""
							, sendPacket->dataSize
						);
					}
					else if (resultSendTo != sendPacket->dataSize) {
						sockAddrInfo = GET_SOCKADDR_INFO(&sendPacket->destinationAddress);
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
							, "%s sendto 0x%08x %s truncated from (%zu) to (%d)."
							, __func__
							, sendPacket->destinationInstanceId
							, sockAddrInfo ? sockAddrInfo : ""
							, sendPacket->dataSize
							, resultSendTo
						);
					}
					else {
						sockAddrInfo = GET_SOCKADDR_INFO(&sendPacket->destinationAddress);
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
							, "%s sendto 0x%08x %s."
							, __func__
							, sendPacket->destinationInstanceId
							, sockAddrInfo ? sockAddrInfo : ""
						);
					}
					if (sockAddrInfo) {
						free(sockAddrInfo);
						sockAddrInfo = 0;
					}
				}
			}
		}
		
		if (sendPacket->sourceTitleSocketHandle != INVALID_SOCKET) {
			EnterCriticalSection(&xlive_critsec_sockets);
			
			auto itrTitleSocket = xlive_title_sockets.find(sendPacket->sourceTitleSocketHandle);
			if (itrTitleSocket == xlive_title_sockets.end()) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s sourceTitleSocketHandle (0x%zx) does not exist."
					, __func__
					, sendPacket->sourceTitleSocketHandle
				);
			}
			else {
				XLIVE_TITLE_SOCKET* titleSocket = itrTitleSocket->second;
				
				if (titleSocket->sendPacketCount) {
					titleSocket->sendPacketCount--;
				}
				
				if (!titleSocket->sendPacketCount) {
					titleSocket->sendCompleted = true;
					
					SetEvent(titleSocket->sendNotify);
					if (titleSocket->sendWsaOverlapped && titleSocket->sendWsaOverlapped->hEvent && titleSocket->sendWsaOverlapped->hEvent != INVALID_HANDLE_VALUE) {
						WSASetEvent(titleSocket->sendWsaOverlapped->hEvent);
					}
					
					titleSocket->selectNotifyWritePending = true;
					SetEvent(titleSocket->selectNotifyWrite);
					
					if (titleSocket->wsaEventSelectWrite != WSA_INVALID_EVENT) {
						SetEvent(titleSocket->wsaEventSelectWrite);
					}
				}
			}
			
			LeaveCriticalSection(&xlive_critsec_sockets);
		}
		
		delete sendPacket;
		sendPacket = 0;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s exiting."
		, __func__
	);
	
	SetEvent(xlln_thread_network_send_exited);
	
	return ERROR_SUCCESS;
}

typedef struct _XLLN_THREAD_PARAMS_SOCKET_MAIN_RECV {
	SOCKET networkSocket = INVALID_SOCKET;
	size_t networkRecvSize = 0;
} XLLN_THREAD_PARAMS_SOCKET_MAIN_RECV;

static DWORD WINAPI XllnThreadSocketMainRecv(void* lpParam)
{
	XLLN_THREAD_PARAMS_SOCKET_MAIN_RECV* thread_params = (XLLN_THREAD_PARAMS_SOCKET_MAIN_RECV*)lpParam;
	
	const SOCKET network_socket = thread_params->networkSocket;
	const size_t data_buffer_size = thread_params->networkRecvSize;
	
	delete thread_params;
	thread_params = 0;
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s starting."
		, __func__
	);
	
	uint8_t* dataBuffer = new uint8_t[data_buffer_size];
	SOCKADDR_STORAGE sockAddrExternal;
	int32_t sockAddrExternalSize = (int32_t)sizeof(sockAddrExternal);
	while (1) {
		int32_t resultRecvFromDataSize = SOCKET_ERROR;
		resultRecvFromDataSize = recvfrom(network_socket, (char*)dataBuffer, data_buffer_size, 0, (sockaddr*)&sockAddrExternal, &sockAddrExternalSize);
		if (resultRecvFromDataSize < 0) {
			int32_t resultErrorRecvFrom = WSAGetLastError();
			if (resultErrorRecvFrom == WSAEWOULDBLOCK) {
				// Normal error. No more data.
				break;
			}
			else if (resultErrorRecvFrom == WSAEMSGSIZE) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s recvfrom WSAEMSGSIZE buffer too small or message received too big. Discarding."
					, __func__
				);
				continue;
			}
			else if (resultErrorRecvFrom == WSAECONNRESET) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
					, "%s recvfrom WSAECONNRESET from a previous send to an unreachable destination. Ignoring."
					, __func__
				);
				continue;
			}
			
			XLLN_DEBUG_LOG_ECODE(resultErrorRecvFrom, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
				, "%s recvfrom failed."
				, __func__
			);
			
			break;
		}
		
		ParseNetworkData(dataBuffer, resultRecvFromDataSize, &sockAddrExternal);
	}
	
	delete[] dataBuffer;
	dataBuffer = 0;
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s exiting."
		, __func__
	);
	
	return ERROR_SUCCESS;
}

static bool SocketMainCreate(SOCKET* socket_main)
{
	*socket_main = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (*socket_main == INVALID_SOCKET) {
		int32_t errorSocketCreate = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSocketCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to create."
			, __func__
		);
		return false;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s (0x%zx) created."
		, __func__
		, *socket_main
	);
	
	uint32_t sockOptValue = 1;
	int32_t resultSetSockOpt = setsockopt(*socket_main, SOL_SOCKET, SO_BROADCAST, (const char*)&sockOptValue, sizeof(sockOptValue));
	if (resultSetSockOpt == SOCKET_ERROR) {
		int32_t errorSetSockOpt = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSetSockOpt, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to set SO_BROADCAST option."
			, __func__
		);
		return false;
	}
	
	uint32_t socketRecvSize = 0;
	int32_t sockOptValueSize = sizeof(socketRecvSize);
	int32_t resultGetSockOpt = getsockopt(*socket_main, SOL_SOCKET, SO_RCVBUF, (char*)&socketRecvSize, &sockOptValueSize);
	if (resultGetSockOpt == SOCKET_ERROR) {
		int32_t errorGetSockOpt = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorGetSockOpt, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to getsockopt SO_RCVBUF."
			, __func__
		);
		return false;
	}
	
	uint32_t socketSendSize = 0;
	sockOptValueSize = sizeof(socketSendSize);
	resultGetSockOpt = getsockopt(*socket_main, SOL_SOCKET, SO_SNDBUF, (char*)&socketSendSize, &sockOptValueSize);
	if (resultGetSockOpt == SOCKET_ERROR) {
		int32_t errorGetSockOpt = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorGetSockOpt, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to getsockopt SO_SNDBUF."
			, __func__
		);
		return false;
	}
	
	// If iMode = 0, blocking is enabled; 
	// If iMode != 0, non-blocking mode is enabled.
	unsigned long iMode = 0;
	int32_t resultIoCtlSocket = ioctlsocket(*socket_main, FIONBIO, &iMode);
	if (resultIoCtlSocket == SOCKET_ERROR) {
		int32_t errorIoCtlSocket = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorIoCtlSocket, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to set FIONBIO option."
			, __func__
		);
		return false;
	}
	
	sockaddr_in socketBindAddress;
	socketBindAddress.sin_family = AF_INET;
	socketBindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
	socketBindAddress.sin_port = htons(xlln_network_instance_port);
	SOCKET resultSocketBind = bind(*socket_main, (sockaddr*)&socketBindAddress, sizeof(socketBindAddress));
	if (resultSocketBind) {
		int32_t errorSocketBind = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSocketBind, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to bind to port %hu."
			, __func__
			, ntohs(socketBindAddress.sin_port)
		);
		return false;
	}
	
	// Stops an ICMP port unreachable from an earlier send failing the next recvfrom.
	BOOL sockOptConnReset = FALSE;
	DWORD bytesReturnedConnReset = 0;
	int32_t resultIoctlConnReset = WSAIoctl(*socket_main, SIO_UDP_CONNRESET, &sockOptConnReset, sizeof(sockOptConnReset), 0, 0, &bytesReturnedConnReset, 0, 0);
	if (resultIoctlConnReset == SOCKET_ERROR) {
		int32_t errorIoctlConnReset = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorIoctlConnReset, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
			, "%s Failed to disable SIO_UDP_CONNRESET."
			, __func__
		);
	}
	
	XLLN_THREAD_PARAMS_SOCKET_MAIN_RECV* threadParams = new XLLN_THREAD_PARAMS_SOCKET_MAIN_RECV;
	threadParams->networkSocket = *socket_main;
	threadParams->networkRecvSize = socketRecvSize;
	
	HANDLE resultCreateThread = CreateThread(0, 0, XllnThreadSocketMainRecv, (void*)threadParams, 0, 0);
	if (!resultCreateThread) {
		uint32_t errorSocketShutdown = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSocketShutdown, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
			, "%s Failed to create XllnThreadSocketMainRecv."
			, __func__
		);
		delete threadParams;
		threadParams = 0;
		
		return false;
	}
	
	xlive_network_rcvbuf = socketRecvSize;
	xlive_network_sndbuf = socketSendSize;
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG,
		"%s socket (0x%zx) created with recv buffer size (0x%x) and send buffer size (0x%x)."
		, __func__
		, *socket_main
		, socketRecvSize
		, socketSendSize
	);
	
	{
		EnterCriticalSection(&xlln_critsec_callbacks_network_socket_bind);
		for (const auto callback : xlln_callbacks_network_socket_bind) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLLN_MODULE | XLLN_LOG_LEVEL_TRACE,
				"%s xlln_callbacks_network_socket_bind invoking function (0x%zx) binding UDP port (%hu)."
				, __func__
				, (size_t)callback
				, xlln_network_instance_port
			);
			callback(true, IPPROTO_UDP, xlln_network_instance_port, xlln_network_instance_port, (int16_t)-1);
		}
		LeaveCriticalSection(&xlln_critsec_callbacks_network_socket_bind);
	}
	
	return true;
}

static bool SocketMainClose(SOCKET* socket_main)
{
	if (*socket_main == INVALID_SOCKET) {
		return true;
	}
	
	{
		EnterCriticalSection(&xlln_critsec_callbacks_network_socket_bind);
		for (const auto callback : xlln_callbacks_network_socket_bind) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLLN_MODULE | XLLN_LOG_LEVEL_TRACE,
				"%s xlln_callbacks_network_socket_bind invoking function (0x%zx) unbinding UDP port (%hu)."
				, __func__
				, (size_t)callback
				, xlln_network_instance_port
			);
			callback(false, IPPROTO_UDP, xlln_network_instance_port, xlln_network_instance_port, (int16_t)-1);
		}
		LeaveCriticalSection(&xlln_critsec_callbacks_network_socket_bind);
	}
	
	bool result = true;
	
	int32_t resultSocketShutdown = shutdown(*socket_main, SD_RECEIVE);
	if (resultSocketShutdown) {
		int32_t errorSocketShutdown = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSocketShutdown, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to shutdown."
			, __func__
		);
		result = false;
	}
	
	int32_t resultSocketClose = closesocket(*socket_main);
	if (resultSocketClose) {
		int32_t errorSocketClose = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSocketClose, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to close."
			, __func__
		);
		result = false;
	}
	
	if (result) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
			, "%s (0x%zx) closed."
			, __func__
			, *socket_main
		);
	}
	
	*socket_main = INVALID_SOCKET;
	
	return result;
}

bool XllnNetworkSocketSetSendBufferSize(uint32_t sockOptValue)
{
	int32_t resultSetSockOpt = setsockopt(xlln_network_socket_main, SOL_SOCKET, SO_SNDBUF, (const char*)&sockOptValue, sizeof(sockOptValue));
	if (resultSetSockOpt == SOCKET_ERROR) {
		int32_t errorSetSockOpt = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSetSockOpt, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to set SO_SNDBUF option."
			, __func__
		);
		return false;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s SO_SNDBUF has been increased to 0x%08x."
		, __func__
		, sockOptValue
	);
	
	xlive_network_sndbuf = sockOptValue;
	return true;
}

static bool SocketMulticastCreate(SOCKET* socket_multicast)
{
	*socket_multicast = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (*socket_multicast == INVALID_SOCKET) {
		int32_t errorSocketCreate = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSocketCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to create."
			, __func__
		);
		return false;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s (0x%zx) created."
		, __func__
		, *socket_multicast
	);
	
	uint32_t sockOptValue = 1;
	int32_t resultSetSockOpt = setsockopt(*socket_multicast, SOL_SOCKET, SO_BROADCAST, (const char*)&sockOptValue, sizeof(sockOptValue));
	if (resultSetSockOpt == SOCKET_ERROR) {
		int32_t errorSetSockOpt = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSetSockOpt, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to set SO_BROADCAST option."
			, __func__
		);
		return false;
	}
	
	sockOptValue = 1;
	resultSetSockOpt = setsockopt(*socket_multicast, SOL_SOCKET, SO_REUSEADDR, (const char*)&sockOptValue, sizeof(sockOptValue));
	if (resultSetSockOpt == SOCKET_ERROR) {
		int32_t errorSetSockOpt = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSetSockOpt, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to set SO_REUSEADDR option."
			, __func__
		);
		return false;
	}
	
	uint32_t socketRecvSize = 0;
	int32_t sockOptValueSize = sizeof(socketRecvSize);
	int32_t resultGetSockOpt = getsockopt(*socket_multicast, SOL_SOCKET, SO_RCVBUF, (char*)&socketRecvSize, &sockOptValueSize);
	if (resultGetSockOpt == SOCKET_ERROR) {
		int32_t errorGetSockOpt = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorGetSockOpt, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to getsockopt SO_RCVBUF."
			, __func__
		);
		return false;
	}
	
	// If iMode = 0, blocking is enabled; 
	// If iMode != 0, non-blocking mode is enabled.
	unsigned long iMode = 0;
	int32_t resultIoCtlSocket = ioctlsocket(*socket_multicast, FIONBIO, &iMode);
	if (resultIoCtlSocket == SOCKET_ERROR) {
		int32_t errorIoCtlSocket = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorIoCtlSocket, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to set FIONBIO option."
			, __func__
		);
		return false;
	}
	
	sockaddr_in socketBindAddress;
	socketBindAddress.sin_family = AF_INET;
	socketBindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
	socketBindAddress.sin_port = htons(xlln_network_instance_base_port);
	SOCKET resultSocketBind = bind(*socket_multicast, (sockaddr*)&socketBindAddress, sizeof(socketBindAddress));
	if (resultSocketBind) {
		int32_t errorSocketBind = WSAGetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSocketBind, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to bind to port %hu."
			, __func__
			, ntohs(socketBindAddress.sin_port)
		);
		return false;
	}
	
	// TODO Deadcoded because WINE GetAdaptersAddresses doesn't seem to give any multicast addresses.
	if (false)
	{
		std::vector<SOCKADDR_STORAGE> multicastAddresses;
		
		{
			EnterCriticalSection(&xlive_critsec_network_adapter);
			
			for (ELIGIBLE_NETWORK_INTERFACE* eligibleNetworkInterface : xlive_eligible_network_adapters) {
				char* sockAddrInfo = GET_SOCKADDR_INFO(&eligibleNetworkInterface->addressMulticast);
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
					, "%s Network Interface (%s) multicast address (%s)."
					, __func__
					, eligibleNetworkInterface->name ? eligibleNetworkInterface->name : ""
					, sockAddrInfo ? sockAddrInfo : ""
				);
				if (sockAddrInfo) {
					free(sockAddrInfo);
					sockAddrInfo = 0;
				}
				if (eligibleNetworkInterface->addressMulticast.ss_family == AF_INET || eligibleNetworkInterface->addressMulticast.ss_family == AF_INET6) {
					multicastAddresses.push_back(eligibleNetworkInterface->addressMulticast);
				}
			}
			
			LeaveCriticalSection(&xlive_critsec_network_adapter);
		}
		
		for (SOCKADDR_STORAGE &multicastAddress : multicastAddresses) {
			if (multicastAddress.ss_family != AF_INET) {
				// TODO IPv6.
				continue;
			}
			
			IP_MREQ multicastMembershipRequest;
			multicastMembershipRequest.imr_multiaddr.s_addr = ((sockaddr_in*)&multicastAddress)->sin_addr.s_addr;
			multicastMembershipRequest.imr_interface.s_addr = htonl(INADDR_ANY);
			int32_t resultSetSockOpt = setsockopt(*socket_multicast, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&multicastMembershipRequest, sizeof(multicastMembershipRequest));
			char* sockAddrInfo = GET_SOCKADDR_INFO(&multicastAddress);
			if (resultSetSockOpt == SOCKET_ERROR) {
				int32_t errorSetSockOpt = WSAGetLastError();
				XLLN_DEBUG_LOG_ECODE(errorSetSockOpt, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "%s Failed to add multicast membership for %s."
					, __func__
					, sockAddrInfo ? sockAddrInfo : ""
				);
			}
			else {
				if (!SetSockAddrPort(&multicastAddress, xlln_network_instance_base_port)) {
					__debugbreak();
				}
				else {
					XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
						, "%s added multicast membership to %s."
						, __func__
						, sockAddrInfo ? sockAddrInfo : ""
					);
					
					EnterCriticalSection(&xlive_critsec_network_adapter);
					
					xlln_multicast_addresses.push_back(multicastAddress);
					
					LeaveCriticalSection(&xlive_critsec_network_adapter);
				}
			}
			if (sockAddrInfo) {
				free(sockAddrInfo);
				sockAddrInfo = 0;
			}
		}
		
		multicastAddresses.clear();
	}
	
	XLLN_THREAD_PARAMS_SOCKET_MAIN_RECV* threadParams = new XLLN_THREAD_PARAMS_SOCKET_MAIN_RECV;
	threadParams->networkSocket = *socket_multicast;
	threadParams->networkRecvSize = socketRecvSize;
	
	HANDLE resultCreateThread = CreateThread(0, 0, XllnThreadSocketMainRecv, (void*)threadParams, 0, 0);
	if (!resultCreateThread) {
		uint32_t errorSocketShutdown = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorSocketShutdown, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
			, "%s Failed to create XllnThreadSocketMainRecv."
			, __func__
		);
		delete threadParams;
		threadParams = 0;
		
		return false;
	}
	
	return true;
}

static bool CloseEvents()
{
	bool success = true;
	
	if (xlln_network_send_queue_notify != INVALID_HANDLE_VALUE && !CloseHandle(xlln_network_send_queue_notify)) {
		uint32_t errorCloseHandle = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorCloseHandle, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s CloseHandle xlln_network_send_queue_notify failed."
			, __func__
		);
		success = false;
	}
	xlln_network_send_queue_notify = INVALID_HANDLE_VALUE;
	
	if (xlln_tcp_sockets_update != INVALID_HANDLE_VALUE && !CloseHandle(xlln_tcp_sockets_update)) {
		uint32_t errorCloseHandle = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorCloseHandle, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s CloseHandle xlln_tcp_sockets_update failed."
			, __func__
		);
		success = false;
	}
	xlln_tcp_sockets_update = INVALID_HANDLE_VALUE;
	
	if (xlln_thread_network_send_exited != INVALID_HANDLE_VALUE && !CloseHandle(xlln_thread_network_send_exited)) {
		uint32_t errorCloseHandle = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorCloseHandle, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s CloseHandle xlln_thread_network_send_exited failed."
			, __func__
		);
		success = false;
	}
	xlln_thread_network_send_exited = INVALID_HANDLE_VALUE;
	
	if (xlln_thread_tcp_sockets_exited != INVALID_HANDLE_VALUE && !CloseHandle(xlln_thread_tcp_sockets_exited)) {
		uint32_t errorCloseHandle = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorCloseHandle, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s CloseHandle xlln_thread_tcp_sockets_exited failed."
			, __func__
		);
		success = false;
	}
	xlln_thread_tcp_sockets_exited = INVALID_HANDLE_VALUE;
	
	return success;
}

static bool ShutdownThreads()
{
	xlln_thread_shutdown = true;
	SetEvent(xlln_network_send_queue_notify);
	SetEvent(xlln_tcp_sockets_update);
	
	HANDLE events[] = {xlln_thread_network_send_exited, xlln_thread_tcp_sockets_exited};
	size_t eventsCount = sizeof(events) / sizeof(events[0]);
	
	uint32_t resultWait = WaitForMultipleObjects(eventsCount, events, TRUE, INFINITE);
	if (resultWait < WAIT_OBJECT_0 || resultWait > WAIT_OBJECT_0 + eventsCount - 1) {
		uint32_t errorWait = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorWait, XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s WaitForMultipleObjects failed with result 0x%08x."
			, __func__
			, resultWait
		);
		return false;
	}
	
	return true;
}

bool InitXllnNetwork()
{
	TRACE_FX();
	
	xlln_network_send_queue_notify = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (xlln_network_send_queue_notify == INVALID_HANDLE_VALUE) {
		uint32_t errorCreateEvent = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorCreateEvent, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
			, "%s CreateEventA xlln_network_send_queue_notify failed."
			, __func__
		);
		return false;
	}
	
	xlln_tcp_sockets_update = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (xlln_tcp_sockets_update == INVALID_HANDLE_VALUE) {
		uint32_t errorCreateEvent = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorCreateEvent, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
			, "%s CreateEventA xlln_tcp_sockets_update failed."
			, __func__
		);
		bool resultCloseEvents = CloseEvents();
		return false;
	}
	
	xlln_thread_network_send_exited = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (xlln_thread_network_send_exited == INVALID_HANDLE_VALUE) {
		uint32_t errorCreateEvent = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorCreateEvent, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
			, "%s CreateEventA xlln_thread_network_send_exited failed."
			, __func__
		);
		bool resultCloseEvents = CloseEvents();
		return false;
	}
	
	xlln_thread_tcp_sockets_exited = CreateEventA(NULL, FALSE, FALSE, NULL);
	if (xlln_thread_tcp_sockets_exited == INVALID_HANDLE_VALUE) {
		uint32_t errorCreateEvent = GetLastError();
		XLLN_DEBUG_LOG_ECODE(errorCreateEvent, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
			, "%s CreateEventA xlln_thread_tcp_sockets_exited failed."
			, __func__
		);
		bool resultCloseEvents = CloseEvents();
		return false;
	}
	
	xlln_thread_shutdown = false;
	
	{
		XLLN_THREAD_PARAMS_SOCKET_MAIN_SEND* threadParams = new XLLN_THREAD_PARAMS_SOCKET_MAIN_SEND;
		
		HANDLE resultCreateThread = CreateThread(0, 0, XllnThreadSocketMainSend, (void*)threadParams, 0, 0);
		if (!resultCreateThread) {
			uint32_t errorSocketShutdown = GetLastError();
			XLLN_DEBUG_LOG_ECODE(errorSocketShutdown, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
				, "%s Failed to create XllnThreadSocketMainSend."
				, __func__
			);
			delete threadParams;
			threadParams = 0;
			
			xlln_thread_shutdown = true;
			bool resultCloseEvents = CloseEvents();
			return false;
		}
	}
	
	{
		XLLN_THREAD_PARAMS_TCP_SOCKETS* threadParams = new XLLN_THREAD_PARAMS_TCP_SOCKETS;
		
		HANDLE resultCreateThread = CreateThread(0, 0, XllnThreadTcpSockets, (void*)threadParams, 0, 0);
		if (!resultCreateThread) {
			uint32_t errorSocketShutdown = GetLastError();
			XLLN_DEBUG_LOG_ECODE(errorSocketShutdown, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
				, "%s Failed to create XllnThreadTcpSockets."
				, __func__
			);
			delete threadParams;
			threadParams = 0;
			
			SetEvent(xlln_thread_tcp_sockets_exited);
			bool resultShutdownThreads = ShutdownThreads();
			bool resultCloseEvents = CloseEvents();
			return false;
		}
	}
	
	if (xlln_network_instance_port) {
		// Request version 2.0.
		WORD wVersionRequested = MAKEWORD(2, 0);
		WSADATA wsaData;
		int resultWSAStartup = WSAStartup(wVersionRequested, &wsaData);
		if (resultWSAStartup) {
			XLLN_DEBUG_LOG_ECODE(resultWSAStartup, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_FATAL
				, "%s WSAStartup failed. Requested version (%hhu.%hhu)."
				, __func__
				, (uint8_t)(wVersionRequested & 0xFF)
				, (uint8_t)((wVersionRequested >> 8) & 0xFF)
			);
			
			bool resultShutdownThreads = ShutdownThreads();
			bool resultCloseEvents = CloseEvents();
			return false;
		}
		
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
			, "%s WSAStartup succeeded. Requested version (%hhu.%hhu). Result support for version (%hhu.%hhu)."
			, __func__
			, (uint8_t)(wVersionRequested & 0xFF)
			, (uint8_t)((wVersionRequested >> 8) & 0xFF)
			, (uint8_t)(wsaData.wVersion & 0xFF)
			, (uint8_t)((wsaData.wVersion >> 8) & 0xFF)
		);
		
		bool resultSocketMain = SocketMainCreate(&xlln_network_socket_main);
		if (!resultSocketMain) {
			resultSocketMain = SocketMainClose(&xlln_network_socket_main);
			
			bool resultShutdownThreads = ShutdownThreads();
			bool resultCloseEvents = CloseEvents();
			return false;
		}
		
		bool resultSocketMulticast = SocketMulticastCreate(&xlln_network_socket_multicast);
		if (!resultSocketMulticast) {
			resultSocketMulticast = SocketMainClose(&xlln_network_socket_multicast);
			resultSocketMain = SocketMainClose(&xlln_network_socket_main);
			
			bool resultShutdownThreads = ShutdownThreads();
			bool resultCloseEvents = CloseEvents();
			return false;
		}
	}
	
	return true;
}

bool UninitXllnNetwork()
{
	TRACE_FX();
	
	xlln_thread_shutdown = true;
	
	bool success = true;
	
	if (xlln_network_instance_port) {
		success = SocketMainClose(&xlln_network_socket_multicast);
		success &= SocketMainClose(&xlln_network_socket_main);
		
		int resultWSACleanup = WSACleanup();
		if (resultWSACleanup) {
			int32_t errorWsaCleanup = WSAGetLastError();
			XLLN_DEBUG_LOG_ECODE(errorWsaCleanup, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
				, "%s WSACleanup failed."
				, __func__
			);
			success = false;
		}
	}
	
	bool resultShutdownThreads = ShutdownThreads();
	success = success && resultShutdownThreads;
	bool resultCloseEvents = CloseEvents();
	success = success && resultCloseEvents;
	
	{
		EnterCriticalSection(&xlln_critsec_network_send);
		
		for (XLLN_NET_SEND_PACKET_INFO* sendPacket : xlln_network_send_queue) {
			delete sendPacket;
		}
		xlln_network_send_queue.clear();
		
		LeaveCriticalSection(&xlln_critsec_network_send);
	}
	
	if (success) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
			, "%s succeeded."
			, __func__
		);
	}
	
	return success;
}
