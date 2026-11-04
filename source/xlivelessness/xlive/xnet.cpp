#include <winsock2.h>
#include "xdefs.hpp"
#include "xnet.hpp"
#include "xlive.hpp"
#include "../xlln/xlln.hpp"
#include "../xlln/xlln-xlsp.hpp"
#include "../xlln/xlln-network.hpp"
#include <map>
#include <mutex>
#include "../xlln/debug-log.hpp"
#include "xsocket.hpp"
#include "xnetqos.hpp"
#include "live-over-lan.hpp"
#include <WS2tcpip.h>
#include <iptypes.h>

bool xlive_net_initialized = false;

XNetStartupParams xlive_net_startup_params;

CRITICAL_SECTION xlive_critsec_xnet_session_keys;

// Key: sessionId / xnkid.
static std::map<uint64_t, XNKEY*> xlive_xnet_session_keys;

static void ResetXNetStartupParams()
{
	xlive_net_startup_params.cfgSizeOfStruct = sizeof(XNetStartupParams);
	xlive_net_startup_params.cfgFlags = 0;
	xlive_net_startup_params.cfgSockMaxDgramSockets = 8;
	xlive_net_startup_params.cfgSockMaxStreamSockets = 32;
	xlive_net_startup_params.cfgSockDefaultRecvBufsizeInK = 16;
	xlive_net_startup_params.cfgSockDefaultSendBufsizeInK = 16;
	xlive_net_startup_params.cfgKeyRegMax = 8;
	xlive_net_startup_params.cfgSecRegMax = 32;
	xlive_net_startup_params.cfgQosDataLimitDiv4 = 64;
	xlive_net_startup_params.cfgQosProbeTimeoutInSeconds = 2;
	xlive_net_startup_params.cfgQosProbeRetries = 3;
	xlive_net_startup_params.cfgQosSrvMaxSimultaneousResponses = 8;
	xlive_net_startup_params.cfgQosPairWaitTimeInSeconds = 2;
}

static void XllnXNetConnectStatusForget_(uint32_t instance_id);
static void XllnXNetConnectStatusForgetAll_();

static void UnregisterSecureAddr(const IN_ADDR ina)
{
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
}

// XNetStartup is called to load the Xbox Secure Network Library.  It takes an
// optional pointer to a parameter structure.  To initialize the library with
// the default set of parameters, simply pass NULL for this argument.  To
// initialize the library with a different set of parameters, place an
// XNetStartupParams on the stack, zero it out, set the cfgSizeOfStruct to
// sizeof(XNetStartupParams), set any of the parameters you want to configure
// (leaving the remaining ones zeroed), and pass a pointer to this structure to
// XNetStartup.
//
// By default the Xbox Secure Network Library operates in secure mode, which
// means that communication to untrusted hosts (such as a PC) is disallowed.
// However, the devkit version of the library allows you to bypass this
// security by specifying the XNET_STARTUP_BYPASS_SECURITY flag in the cfgFlags
// parameter.  Here is an example of how to do this:
//
//      XNetStartupParams xnsp;
//      memset(&xnsp, 0, sizeof(xnsp));
//      xnsp.cfgSizeOfStruct = sizeof(XNetStartupParams);
//      xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
//      INT err = XNetStartup(&xnsp);
//
// Attempting to bypass security when not using the devkit version of the
// library does not return an error, it is simply ignored.  Attempts to send or
// receive packets from untrusted hosts will fail.
//
// #51
int32_t WINAPI XNetStartup(const XNetStartupParams* xnet_startup_parameters)
{
	TRACE_FX();
	
	ResetXNetStartupParams();
	
	if (xnet_startup_parameters && xnet_startup_parameters->cfgSizeOfStruct != sizeof(XNetStartupParams)) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (xnet_startup_parameters->cfgSizeOfStruct != sizeof(XNetStartupParams)) (%u != %u).", __func__, xnet_startup_parameters->cfgSizeOfStruct, sizeof(XNetStartupParams));
		return ERROR_INVALID_PARAMETER;
	}
	if (xlive_netsocket_abort) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive NetSocket is disabled.", __func__);
		return ERROR_FUNCTION_FAILED;
	}
	
	if (xnet_startup_parameters) {
		if (xnet_startup_parameters->cfgFlags) {
			xlive_net_startup_params.cfgFlags = xnet_startup_parameters->cfgFlags;
		}
		if (xnet_startup_parameters->cfgSockMaxDgramSockets) {
			xlive_net_startup_params.cfgSockMaxDgramSockets = xnet_startup_parameters->cfgSockMaxDgramSockets;
		}
		if (xnet_startup_parameters->cfgSockMaxStreamSockets) {
			xlive_net_startup_params.cfgSockMaxStreamSockets = xnet_startup_parameters->cfgSockMaxStreamSockets;
		}
		if (xnet_startup_parameters->cfgSockDefaultRecvBufsizeInK) {
			xlive_net_startup_params.cfgSockDefaultRecvBufsizeInK = xnet_startup_parameters->cfgSockDefaultRecvBufsizeInK;
		}
		if (xnet_startup_parameters->cfgSockDefaultSendBufsizeInK) {
			xlive_net_startup_params.cfgSockDefaultSendBufsizeInK = xnet_startup_parameters->cfgSockDefaultSendBufsizeInK;
		}
		if (xnet_startup_parameters->cfgKeyRegMax) {
			xlive_net_startup_params.cfgKeyRegMax = xnet_startup_parameters->cfgKeyRegMax;
		}
		if (xnet_startup_parameters->cfgSecRegMax) {
			xlive_net_startup_params.cfgSecRegMax = xnet_startup_parameters->cfgSecRegMax;
		}
		if (xnet_startup_parameters->cfgQosDataLimitDiv4) {
			xlive_net_startup_params.cfgQosDataLimitDiv4 = xnet_startup_parameters->cfgQosDataLimitDiv4;
		}
		if (xnet_startup_parameters->cfgQosProbeTimeoutInSeconds) {
			xlive_net_startup_params.cfgQosProbeTimeoutInSeconds = xnet_startup_parameters->cfgQosProbeTimeoutInSeconds;
		}
		if (xnet_startup_parameters->cfgQosProbeRetries) {
			xlive_net_startup_params.cfgQosProbeRetries = xnet_startup_parameters->cfgQosProbeRetries;
		}
		if (xnet_startup_parameters->cfgQosSrvMaxSimultaneousResponses) {
			xlive_net_startup_params.cfgQosSrvMaxSimultaneousResponses = xnet_startup_parameters->cfgQosSrvMaxSimultaneousResponses;
		}
		if (xnet_startup_parameters->cfgQosPairWaitTimeInSeconds) {
			xlive_net_startup_params.cfgQosPairWaitTimeInSeconds = xnet_startup_parameters->cfgQosPairWaitTimeInSeconds;
		}
	}
	
	xlive_net_initialized = true;
	
	return ERROR_SUCCESS;
}

// #52
int32_t WINAPI XNetCleanup()
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNet is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	
	ResetXNetStartupParams();
	XllnXNetConnectStatusForgetAll_();
	xlive_net_initialized = false;
	
	return ERROR_SUCCESS;
}

// #53
int32_t WINAPI XNetRandom(uint8_t* buffer, size_t buffer_size)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!buffer) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s buffer is NULL.", __func__);
		return WSAEFAULT;
	}

	for (size_t iBuffer = 0; iBuffer < buffer_size; iBuffer++) {
		buffer[iBuffer] = static_cast<uint8_t>(rand());
	}

	return S_OK;
}

// #54
int32_t WINAPI XNetCreateKey(XNKID* xnkid, XNKEY* xnkey)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!xnkid) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnkid is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!xnkey) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnkey is NULL.", __func__);
		return WSAEFAULT;
	}

	XllnGenerateSessionXnkid(xnkid);
	memset(xnkey, 0XAA, sizeof(XNKEY));

	xnkid->ab[0] &= ~XNET_XNKID_MASK;
	xnkid->ab[0] |= XNET_XNKID_SYSTEM_LINK_XPLAT;
	
	return S_OK;
}

// #55
int32_t WINAPI XNetRegisterKey(const XNKID* xnkid, const XNKEY* xnkey)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!xnkid) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnkid is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!xnkey) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnkey is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!XNetXnKidIsOnlinePeer(xnkid) && !XNetXnKidIsOnlineTitleServer(xnkid) && !XNetXnKidIsSystemLinkXPlat(xnkid)) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNetXnKidFlag(xnkid) (0x%02x) is not XNET_XNKID_ONLINE_PEER, XNET_XNKID_ONLINE_TITLESERVER or XNET_XNKID_SYSTEM_LINK_XPLAT.", __func__, XNetXnKidFlag(xnkid));
		return E_INVALIDARG;
	}
	
	const uint64_t &sessionId = *(uint64_t*)xnkid;
	
	{
		EnterCriticalSection(&xlive_critsec_xnet_session_keys);
		
		if (xlive_xnet_session_keys.count(sessionId)) {
			LeaveCriticalSection(&xlive_critsec_xnet_session_keys);
			XLLN_DEBUG_LOG(
				XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnkid (0x%016I64x) is already registered."
				, __func__
				, sessionId
			);
			return ERROR_ALREADY_REGISTERED;
		}

		*(xlive_xnet_session_keys[sessionId] = new XNKEY) = *xnkey;
		
		LeaveCriticalSection(&xlive_critsec_xnet_session_keys);
	}
	
	return S_OK;
}

// #56
int32_t WINAPI XNetUnregisterKey(const XNKID* xnkid)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!xnkid) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnkid is NULL.", __func__);
		return WSAEFAULT;
	}
	
	const uint64_t &sessionId = *(uint64_t*)xnkid;
	
	{
		EnterCriticalSection(&xlive_critsec_qos_listeners);
		EnterCriticalSection(&xlive_critsec_xnet_session_keys);
		
		if (!xlive_xnet_session_keys.count(sessionId)) {
			LeaveCriticalSection(&xlive_critsec_xnet_session_keys);
			LeaveCriticalSection(&xlive_critsec_qos_listeners);
			XLLN_DEBUG_LOG(
				XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnkid (0x%016I64x) does not exist."
				, __func__
				, sessionId
			);
			return ERROR_NOT_FOUND;
		}
		
		XNKEY* xnkey = xlive_xnet_session_keys[sessionId];
		xlive_qos_listeners.erase(sessionId);
		xlive_xnet_session_keys.erase(sessionId);
		delete xnkey;
		
		LeaveCriticalSection(&xlive_critsec_xnet_session_keys);
		LeaveCriticalSection(&xlive_critsec_qos_listeners);
	}
	
	return S_OK;
}

// #57
int32_t WINAPI XNetXnAddrToInAddr(XNADDR* xnaddr, XNKID* xnkid, IN_ADDR* in_addr)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!xnaddr) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnaddr is NULL.", __func__);
		return WSAEINVAL;
	}
	if (!xnkid) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnkid is NULL.", __func__);
		return WSAEINVAL;
	}
	if (!in_addr) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s in_addr is NULL.", __func__);
		return WSAEINVAL;
	}
	
	uint32_t instanceId = ntohl(xnaddr->inaOnline.s_addr);
	uint16_t portBaseHBO = ntohs(xnaddr->wPortOnline);
	uint32_t ipv4HBO = ntohl(xnaddr->ina.s_addr);
	
	in_addr->s_addr = htonl(instanceId);
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
		, "%s instanceId (0x%08x), portBaseHBO (%hu), ipv4HBO (0x%08x)."
		, __func__
		, instanceId
		, portBaseHBO
		, ipv4HBO
	);
	
	if (instanceId == INADDR_ANY || instanceId == INADDR_BROADCAST || instanceId == INADDR_LOOPBACK) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s attempting to connect to invalid address (0x%08x)."
			, __func__
			, instanceId
		);
		return WSAEINVAL;
	}
	
	// Dropping a lost association here is what lets the title reconnect.
	XllnNetAssociationForgetLost(instanceId);
	
	return S_OK;
}

// #58
int32_t WINAPI XNetServerToInAddr(const IN_ADDR in_addr_server, uint32_t service_id, IN_ADDR* in_addr)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!in_addr) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s in_addr is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!in_addr_server.s_addr) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s in_addr_server is 0.", __func__);
		in_addr->s_addr = 0;
		return WSAEHOSTUNREACH;
	}
	
	// With no XLSP backend configured no title server was ever handed out, so decline exactly as the
	// stub always has.
	if (!XllnXlspIsConfigured()) {
		in_addr->s_addr = 0;
		return WSAEHOSTUNREACH;
	}
	
	// The enumerator handed out this XLLN instance's id as the server address, so the mapping is the
	// identity: the id has to survive the round trip unchanged for XSocketConnect to recognise the dial
	// as self and bridge it to the configured backend.
	*in_addr = in_addr_server;
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
		, "%s service_id (0x%08x) resolved to ipv4HBO (0x%08x)."
		, __func__
		, service_id
		, ntohl(in_addr->s_addr)
	);
	
	return ERROR_SUCCESS;
}

// #59
int32_t WINAPI XNetTsAddrToInAddr(const TSADDR* ts_addr, uint32_t service_id, const XNKID* xnkid, IN_ADDR* in_addr)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!in_addr) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s in_addr is NULL.", __func__);
		return WSAEFAULT;
	}
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	// actually look up the xlln instance / title server.
	in_addr->s_addr = 0;
	return WSAEHOSTUNREACH;
	return ERROR_SUCCESS;
}

// #60
int32_t WINAPI XNetInAddrToXnAddr(const IN_ADDR in_addr, XNADDR* xnaddr, XNKID* xnkid)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	
	uint32_t instanceId = ntohl(in_addr.s_addr);
	
	if (!XllnNetEntityGetXnaddrXnkidByInstanceId(instanceId, xnaddr, xnkid)) {
		XLLN_DEBUG_LOG( XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s XllnNetEntityGetXnaddrXnkidByInstanceId failed."
			, __func__
			, instanceId
		);
		return E_FAIL;
	}
	
	// A title round-trips the peer address it stored through XNetXnAddrToInAddr and
	// XNetInAddrToXnAddr before it connects, and abandons the join when the XNKID it stored is not
	// the one that comes back. The XNKID it stored is the peer's advertised session id, not one
	// derived from the instance id, so answer with the advertised id where it is known.
	uint16_t advertisedTitlePort = 0;
	if (xnkid) {
		XNKID advertisedXnkid;
		if (LiveOverLanGetInstanceAdvertisedSessionId(instanceId, &advertisedXnkid, &advertisedTitlePort)) {
			*xnkid = advertisedXnkid;
		}
	}
	// The port the game stored came from the advertise; packets observed from the peer can carry a
	// different source port, so the round trip returns the advertised one.
	if (xnaddr && advertisedTitlePort) {
		xnaddr->wPortOnline = advertisedTitlePort;
	}
	
	
	return S_OK;
}

// #61
int32_t WINAPI XNetInAddrToServer(const IN_ADDR in_addr, IN_ADDR* in_addr_server)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!in_addr_server) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s in_addr is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!in_addr.s_addr) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s in_addr is 0.", __func__);
		in_addr_server->s_addr = 0;
		return WSAEHOSTUNREACH;
	}
	
	if (!XllnXlspIsConfigured()) {
		in_addr_server->s_addr = 0;
		return WSAEHOSTUNREACH;
	}
	
	// Inverse of XNetServerToInAddr, and the identity for the same reason.
	*in_addr_server = in_addr;
	return ERROR_SUCCESS;
}

// #62
int32_t WINAPI XNetInAddrToString(const IN_ADDR in_addr, char* address, size_t address_size)
{
	TRACE_FX();
	if (!address) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s pchBuf is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!address_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s cchBuf is 0.", __func__);
		return WSAEINVAL;
	}
	
	address[0] = 0;
	
	SOCKADDR_IN sockAddr;
	sockAddr.sin_family = AF_INET;
	sockAddr.sin_port = 0;
	sockAddr.sin_addr = in_addr;
	
	// Maximum length of full domain name + sentinel.
	char namebuf[253 + 1];
	int errorGetNameInfo = getnameinfo((const sockaddr*)&sockAddr, sizeof(SOCKADDR_IN), namebuf, sizeof(namebuf), NULL, 0, NI_NUMERICHOST);
	if (errorGetNameInfo) {
		XLLN_DEBUG_LOG_ECODE(errorGetNameInfo, XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s getnameinfo error:", __func__);
		return errorGetNameInfo;
	}
	size_t nameLen = strnlen(namebuf, sizeof(namebuf));
	if ((int32_t)nameLen >= address_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (nameLen >= cchBuf) (%zu >= %d).", __func__, nameLen, address_size);
		return WSAEMSGSIZE;
	}
	memcpy(address, namebuf, nameLen);
	address[nameLen] = 0;
	
	return ERROR_SUCCESS;
}

// #63
int32_t WINAPI XNetUnregisterInAddr(const IN_ADDR in_addr)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	UnregisterSecureAddr(in_addr);
	XllnXNetConnectStatusForget_(ntohl(in_addr.s_addr));
	return S_OK;
}

// #64
int32_t WINAPI XNetXnAddrToMachineId(const XNADDR* xnaddr, uint64_t* machine_id)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!xnaddr) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnaddr is NULL.", __func__);
		return E_POINTER;
	}
	if (!machine_id) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s machine_id is NULL.", __func__);
		return E_POINTER;
	}
	//TODO  !CIpAddr::IsValidUnicast(xnaddr->ina.s_addr) || !CIpAddr::IsValidUnicast(xnaddr->inaOnline.s_addr) || 
	if (xnaddr->ina.s_addr == 0) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnaddr->ina.s_addr is 0.", __func__);
		*machine_id = 0;
		return E_INVALIDARG;
	}
	if (xnaddr->inaOnline.s_addr == 0) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnaddr->inaOnline.s_addr is 0.", __func__);
		*machine_id = 0;
		return E_INVALIDARG;
	}
	// TODO should this be there?
	//if (xnaddr->wPortOnline == 0) {
	//	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnaddr->wPortOnline is 0.", __func__);
	//	*machine_id = 0;
	//	return E_INVALIDARG;
	//}
	
	*machine_id = *(uint64_t*)&xnaddr->abOnline[8];
	// An XNADDR from an older XLLN build has no machine id stored in it.
	if (!*machine_id) {
		*machine_id = XllnNetMachineIdFromInstanceId(ntohl(xnaddr->inaOnline.s_addr));
	}
	return S_OK;
}

static std::mutex xlive_mutex_xnet_connect_status;
static std::map<uint32_t, int32_t> xlive_xnet_connect_status_reported;

// A reconnect needs a fresh exchange, because the old association reports LOST at once.
static void XllnXNetConnectStatusForget_(uint32_t instance_id)
{
	XllnNetAssociationForget(instance_id);
	
	std::lock_guard<std::mutex> lock(xlive_mutex_xnet_connect_status);
	xlive_xnet_connect_status_reported.erase(instance_id);
}

static void XllnXNetConnectStatusForgetAll_()
{
	XllnNetAssociationForgetAll();
	
	std::lock_guard<std::mutex> lock(xlive_mutex_xnet_connect_status);
	xlive_xnet_connect_status_reported.clear();
}

// Titles poll this every frame, so only log when the answer changes.
static int32_t XllnXNetReportConnectStatus_(uint32_t instance_id, int32_t status)
{
	{
		std::lock_guard<std::mutex> lock(xlive_mutex_xnet_connect_status);
		
		auto itrReported = xlive_xnet_connect_status_reported.find(instance_id);
		if (itrReported != xlive_xnet_connect_status_reported.end() && itrReported->second == status) {
			return status;
		}
		xlive_xnet_connect_status_reported[instance_id] = status;
	}
	
	const char* statusName = status == XNET_CONNECT_STATUS_CONNECTED ? "CONNECTED"
		: (status == XNET_CONNECT_STATUS_PENDING ? "PENDING"
		: (status == XNET_CONNECT_STATUS_LOST ? "LOST" : "IDLE"));
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_INFO
		, "%s instanceId (0x%08x) is now %s."
		, __func__
		, instance_id
		, statusName
	);
	
	return status;
}

// #65
int32_t WINAPI XNetConnect(const IN_ADDR in_addr)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	
	uint32_t instanceId = ntohl(in_addr.s_addr);
	if (instanceId == INADDR_ANY || instanceId == INADDR_BROADCAST || instanceId == INADDR_LOOPBACK) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s attempting to connect to invalid address (0x%08x)."
			, __func__
			, instanceId
		);
		return WSAEINVAL;
	}
	
	// A lost association is not reestablished here, the title has to resolve the XNADDR again.
	if (XllnNetAssociationPoll(instanceId) == XLLN_NET_ASSOCIATION_LOST) {
		return WSAEINVAL;
	}
	
	return S_OK;
}

// #66
int32_t WINAPI XNetGetConnectStatus(const IN_ADDR in_addr)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return XNET_CONNECT_STATUS_PENDING;
	}
	
	uint32_t instanceId = ntohl(in_addr.s_addr);
	
	int32_t status;
	switch (XllnNetAssociationPoll(instanceId)) {
		case XLLN_NET_ASSOCIATION_CONNECTED: {
			status = XNET_CONNECT_STATUS_CONNECTED;
			break;
		}
		case XLLN_NET_ASSOCIATION_LOST: {
			status = XNET_CONNECT_STATUS_LOST;
			break;
		}
		default: {
			status = XNET_CONNECT_STATUS_PENDING;
			break;
		}
	}
	
	return XllnXNetReportConnectStatus_(instanceId, status);
}

// #67
// Resolves a hostname into a freshly allocated XNDNS that the title owns until XNetDnsRelease.
// The title's XNDNS* is uninitialised on entry, so nothing may be read through *xndns.
int32_t WINAPI XNetDnsLookup(const char* hostname, WSAEVENT wsa_event, XNDNS** xndns)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!xndns) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xndns is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	
	XNDNS* lookup = new XNDNS{};
	*xndns = lookup;
	
	// TODO Improper Implementation. Resolves synchronously; iStatus is final on return.
	hostent* he = gethostbyname(hostname);
	if (!he) {
		lookup->iStatus = WSAGetLastError();
	}
	else if (he->h_addrtype != AF_INET || he->h_length != sizeof(in_addr)) {
		lookup->iStatus = WSAEAFNOSUPPORT;
	}
	else {
		in_addr** addr_list = (in_addr**)he->h_addr_list;
		const uint32_t maxAddresses = sizeof(lookup->aina) / sizeof(lookup->aina[0]);
		while (lookup->cina < maxAddresses && addr_list[lookup->cina]) {
			lookup->aina[lookup->cina] = *addr_list[lookup->cina];
			lookup->cina++;
		}
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
		, "%s hostname (%s) resolved (%u) address(es) with status (0x%08x)."
		, __func__
		, hostname ? hostname : "NULL"
		, lookup->cina
		, lookup->iStatus
	);
	
	if (wsa_event) {
		WSASetEvent(wsa_event);
	}
	
	return ERROR_SUCCESS;
}

// #68
int32_t WINAPI XNetDnsRelease(XNDNS* xndns)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!xndns) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xndns is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	
	delete xndns;
	
	return ERROR_SUCCESS;
}

// #73
uint32_t WINAPI XNetGetTitleXnAddr(XNADDR* xnaddr)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return XNET_GET_XNADDR_NONE;
	}
	if (!xnaddr) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnaddr is NULL.", __func__);
		return XNET_GET_XNADDR_NONE;
	}
	
	XllnNetEntityGetXnaddrXnkidByInstanceId(xlln_global_instance_id, xnaddr, 0);

	// A title that advertises its own XNADDR has to name a port a peer can dial, otherwise it
	// publishes port 0 and the peer dials nothing.
	if (xnaddr->wPortOnline == 0) {
		// The port the game really bound, not the boot-time reservation: a title that binds its
		// session socket to an explicit port would otherwise publish a port nothing listens on.
		xnaddr->wPortOnline = htons(XllnGetTitleDialPort_());
	}
	
	uint32_t instanceId = ntohl(xnaddr->inaOnline.s_addr);
	uint16_t portBaseHBO = ntohs(xnaddr->wPortOnline);
	uint32_t ipv4HBO = ntohl(xnaddr->ina.s_addr);
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
		, "%s instanceId (0x%08x), portBaseHBO (%hu), ipv4HBO (0x%08x)."
		, __func__
		, instanceId
		, portBaseHBO
		, ipv4HBO
	);
	
	return XNET_GET_XNADDR_STATIC | XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_ONLINE;
}

// #74
uint32_t WINAPI XNetGetDebugXnAddr(XNADDR* xnaddr)
{
	TRACE_FX();
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO verify that this works.", __func__);
	return XNetGetTitleXnAddr(xnaddr);
}

// #75
uint32_t WINAPI XNetGetEthernetLinkStatus()
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return XNET_ETHERNET_LINK_INACTIVE;
	}
	return XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | XNET_ETHERNET_LINK_FULL_DUPLEX;
}

//TODO XNetGetBroadcastVersionStatus
static uint32_t xlive_broadcast_incompatible_version = 0;
//xlive_broadcast_incompatible_version = XNET_BROADCAST_VERSION_OLDER;
//xlive_broadcast_incompatible_version = XNET_BROADCAST_VERSION_NEWER;

// #76
uint32_t WINAPI XNetGetBroadcastVersionStatus(BOOL reset_status)
{
	TRACE_FX();
	uint32_t result = xlive_broadcast_incompatible_version;
	if (reset_status) {
		xlive_broadcast_incompatible_version = 0;
	}
	return result;
}

// #78
int32_t WINAPI XNetGetOpt(uint32_t option_id, uint8_t* option_value, size_t* option_value_size)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!option_value) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s option_value is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!option_value_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s option_value_size is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!*option_value_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s *option_value_size is 0.", __func__);
		return WSAEINVAL;
	}
	
	size_t requiredSize = 0;
	switch (option_id) {
		default: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s unknown option_id (0x%08x).", __func__, option_id);
			return WSAEINVAL;
		}
		case XNET_OPTID_STARTUP_PARAMS: {
			requiredSize = sizeof(XNetStartupParams);
			break;
		}
		case XNET_OPTID_NIC_XMIT_BYTES:
		case XNET_OPTID_NIC_RECV_BYTES:
		case XNET_OPTID_CALLER_XMIT_BYTES:
		case XNET_OPTID_CALLER_RECV_BYTES: {
			requiredSize = sizeof(uint64_t);
			break;
		}
		case XNET_OPTID_NIC_XMIT_FRAMES:
		case XNET_OPTID_NIC_RECV_FRAMES:
		case XNET_OPTID_CALLER_XMIT_FRAMES:
		case XNET_OPTID_CALLER_RECV_FRAMES: {
			requiredSize = sizeof(uint32_t);
			break;
		}
	}
	
	if (requiredSize > *option_value_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (requiredSize > *option_value_size) (%zu > %zu).", __func__, requiredSize, *option_value_size);
		*option_value_size = requiredSize;
		return WSAEMSGSIZE;
	}
	
	*option_value_size = requiredSize;
	
	switch (option_id) {
		case XNET_OPTID_STARTUP_PARAMS: {
			memcpy_s(option_value, *option_value_size, &xlive_net_startup_params, sizeof(xlive_net_startup_params));
			break;
		}
		default: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
			break;
		}
	}
	
	switch (option_id) {
		case XNET_OPTID_NIC_XMIT_BYTES: {
			*(uint64_t*)option_value = 0;
			break;
		}
		case XNET_OPTID_NIC_XMIT_FRAMES: {
			*(uint32_t*)option_value = 0;
			break;
		}
		case XNET_OPTID_NIC_RECV_BYTES: {
			*(uint64_t*)option_value = 0;
			break;
		}
		case XNET_OPTID_NIC_RECV_FRAMES: {
			*(uint32_t*)option_value = 0;
			break;
		}
		case XNET_OPTID_CALLER_XMIT_BYTES: {
			*(uint64_t*)option_value = 0;
			break;
		}
		case XNET_OPTID_CALLER_XMIT_FRAMES: {
			*(uint32_t*)option_value = 0;
			break;
		}
		case XNET_OPTID_CALLER_RECV_BYTES: {
			*(uint64_t*)option_value = 0;
			break;
		}
		case XNET_OPTID_CALLER_RECV_FRAMES: {
			*(uint32_t*)option_value = 0;
			break;
		}
	}
	
	return ERROR_SUCCESS;
}

// #79
int32_t WINAPI XNetSetOpt(uint32_t option_id, uint8_t* option_value, size_t* option_value_size)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!option_value) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s option_value is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!option_value_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s option_value_size is NULL.", __func__);
		return WSAEFAULT;
	}
	if (!*option_value_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s *option_value_size is 0.", __func__);
		return WSAEINVAL;
	}
	
	switch (option_id) {
		default: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s unknown option_id (0x%08x).", __func__, option_id);
			return WSAEINVAL;
		}
		case XNET_OPTID_STARTUP_PARAMS: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNET_OPTID_STARTUP_PARAMS cannot be set.", __func__);
			return WSAEINVAL;
		}
		case XNET_OPTID_NIC_XMIT_BYTES: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNET_OPTID_NIC_XMIT_BYTES cannot be set.", __func__);
			return WSAEINVAL;
		}
		case XNET_OPTID_NIC_XMIT_FRAMES: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNET_OPTID_NIC_XMIT_FRAMES cannot be set.", __func__);
			return WSAEINVAL;
		}
		case XNET_OPTID_NIC_RECV_BYTES: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNET_OPTID_NIC_RECV_BYTES cannot be set.", __func__);
			return WSAEINVAL;
		}
		case XNET_OPTID_NIC_RECV_FRAMES: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNET_OPTID_NIC_RECV_FRAMES cannot be set.", __func__);
			return WSAEINVAL;
		}
		case XNET_OPTID_CALLER_XMIT_BYTES: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNET_OPTID_CALLER_XMIT_BYTES cannot be set.", __func__);
			return WSAEINVAL;
		}
		case XNET_OPTID_CALLER_XMIT_FRAMES: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNET_OPTID_CALLER_XMIT_FRAMES cannot be set.", __func__);
			return WSAEINVAL;
		}
		case XNET_OPTID_CALLER_RECV_BYTES: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNET_OPTID_CALLER_RECV_BYTES cannot be set.", __func__);
			return WSAEINVAL;
		}
		case XNET_OPTID_CALLER_RECV_FRAMES: {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XNET_OPTID_CALLER_RECV_FRAMES cannot be set.", __func__);
			return WSAEINVAL;
		}
	}
	
	return ERROR_SUCCESS;
}

// #80
int32_t WINAPI XNetStartupEx(const XNetStartupParams* xnet_startup_parameters, uint32_t version_required)
{
	TRACE_FX();
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_INFO
		, "%s version_required (0x%08x)."
		, __func__
		, version_required
	);
	int32_t result = XNetStartup(xnet_startup_parameters);
	return result;
}

// #81
int32_t WINAPI XNetReplaceKey(const XNKID* xnkid_unregister, const XNKID* pxnkid_new)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!xnkid_unregister) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnkid_unregister is NULL.", __func__);
		return WSAEFAULT;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	return ERROR_SUCCESS;
}

// #82
int32_t WINAPI XNetGetXnAddrPlatform(const XNADDR* xnaddr, uint32_t* platform)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!platform) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s platform is NULL.", __func__);
		return WSAEFAULT;
	}
	*platform = XNET_XNADDR_PLATFORM_XBOX1;
	if (*(uint32_t*)&xnaddr->abEnet[0] == 0 && *(uint16_t*)&xnaddr->abEnet[4] == 0) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xnaddr->abEnet is all 0.", __func__);
		return WSAEINVAL;
	}
	*platform = XNET_XNADDR_PLATFORM_WINPC;
	return ERROR_SUCCESS;
}

// #83
// system_link_port_NBO - network byte (big-endian) order
int32_t WINAPI XNetGetSystemLinkPort(uint16_t* system_link_port_NBO)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	if (!system_link_port_NBO) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s system_link_port_NBO is NULL.", __func__);
		return E_POINTER;
	}
	// TODO XNetGetSystemLinkPort XEX_PRIVILEGE_CROSSPLATFORM_SYSTEM_LINK.
	
	*system_link_port_NBO = htons(xlive_port_system_link);
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
		, "%s port (%hu)."
		, __func__
		, xlive_port_system_link
	);
	
	return S_OK;
}

// #84
// system_link_port_NBO - network byte (big-endian) order
int32_t WINAPI XNetSetSystemLinkPort(uint16_t system_link_port_NBO)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return WSANOTINITIALISED;
	}
	// network byte (big-endian) to little-endian host.
	uint16_t portHBO = ntohs(system_link_port_NBO);
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_INFO
		, "%s set to (%hu)."
		, __func__
		, portHBO
	);
	
	xlive_port_system_link = portHBO;
	
	return ERROR_SUCCESS;
	return WSAEADDRINUSE;
}

// #5023
HRESULT WINAPI XNetGetCurrentAdapter(char* network_adapter, size_t* network_adapter_size)
{
	TRACE_FX();
	if (!network_adapter_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s network_adapter_size is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		*network_adapter_size = 0;
		return E_UNEXPECTED;
	}
	
	if (network_adapter) {
		network_adapter[0] = 0;
	}
	
	{
		EnterCriticalSection(&xlive_critsec_network_adapter);
		
		if (!xlive_specific_network_adapter || !xlive_specific_network_adapter->name) {
			LeaveCriticalSection(&xlive_critsec_network_adapter);
			*network_adapter_size = 0;
			return HRESULT_FROM_WIN32(ERROR_ADAP_HDW_ERR);
		}
		
		size_t nameLen = strnlen(xlive_specific_network_adapter->name, MAX_ADAPTER_NAME_LENGTH + 4);
		if (nameLen >= *network_adapter_size) {
			*network_adapter_size = nameLen + 1;
		}
		else if (network_adapter) {
			memcpy(network_adapter, xlive_specific_network_adapter->name, nameLen);
			network_adapter[nameLen] = 0;
		}
		
		LeaveCriticalSection(&xlive_critsec_network_adapter);
	}
	
	return S_OK;
}

// #5296
// assuming online_port_NBO - network byte (big-endian) order
HRESULT WINAPI XLiveGetLocalOnlinePort(uint16_t* online_port_NBO)
{
	TRACE_FX();
	if (!xlive_net_initialized) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s XLive Net is not initialised.", __func__);
		return E_UNEXPECTED;
	}
	if (!online_port_NBO) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s online_port_NBO is NULL.", __func__);
		return E_INVALIDARG;
	}
	
	*online_port_NBO = htons(xlive_port_online);
	
	return S_OK;
}

bool InitXNet()
{
	ResetXNetStartupParams();
	
	return true;
}

bool UninitXNet()
{
	return true;
}
