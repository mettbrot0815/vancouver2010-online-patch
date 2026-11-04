#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <stdlib.h>
#include <map>
#include <string>
#include <vector>
#include "xlln-xlsp.hpp"
#include "xlln.hpp"
#include "./debug-log.hpp"
#include "xlln-config.hpp"
#include "../utils/utils.hpp"
#include "../utils/util-socket.hpp"
#include "../xlive/xlive.hpp"
#include "../xlive/xdefs.hpp"
#include <rapidjson/document.h>

typedef struct _XLSP_SERVICE_ENDPOINT {
	std::string service;
	std::string host;     // address literal or hostname, as written in the config
	IN_ADDR addr = {};    // backend address (network byte order)
	uint16_t portHbo = 0; // backend port (host byte order)
} XLSP_SERVICE_ENDPOINT;

typedef struct _XLSP_TITLE_CONFIG {
	bool valid = false; // a matching title entry with at least one resolved service
	std::map<uint16_t, XLSP_SERVICE_ENDPOINT> servicesByPort;
} XLSP_TITLE_CONFIG;

static XLSP_TITLE_CONFIG xlln_xlsp_config;
static bool xlln_xlsp_config_loaded = false;

// Converts an IPv4 address literal, and nothing else.
static bool ParseIpv4Literal(const char* host, IN_ADDR* out_addr)
{
	if (!host || !host[0] || !out_addr) {
		return false;
	}
	if (strnlen(host, INET6_ADDRSTRLEN + 1) > INET6_ADDRSTRLEN) {
		return false;
	}
	IN_ADDR literalAddr;
	if (WS2_32_inet_pton(AF_INET, host, &literalAddr) != 1) {
		return false;
	}
	*out_addr = literalAddr;
	return true;
}

// Resolves an IPv4 literal or a hostname to an IN_ADDR (network byte order). AF_INET only: XLSP
// backend addresses are all carried as 4-byte IN_ADDRs. Must not run under the loader lock - a name
// lookup can have ws2_32 load a namespace provider - which is why the config is read from
// XLiveInitialize and not from InitXLLN.
static bool ResolveIpv4(const char* host, IN_ADDR* out_addr)
{
	if (!host || !host[0] || !out_addr) {
		return false;
	}
	// WS2_32_inet_pton copies into a fixed INET6_ADDRSTRLEN buffer using a count equal to that buffer's
	// size, which the CRT treats as an error rather than a truncation - and the release handler fails
	// the process rather than returning.
	if (strnlen(host, INET6_ADDRSTRLEN + 1) > INET6_ADDRSTRLEN) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s host is longer than %d characters."
			, __func__
			, INET6_ADDRSTRLEN
		);
		return false;
	}

	if (ParseIpv4Literal(host, out_addr)) {
		return true;
	}

	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo* res = 0;
	int error = getaddrinfo(host, NULL, &hints, &res);
	if (error) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s Unable to resolve XLSP address (%s) error (%d)."
			, __func__
			, host
			, error
		);
		return false;
	}

	bool resolved = false;
	for (addrinfo* nextRes = res; nextRes; nextRes = nextRes->ai_next) {
		if (nextRes->ai_family == AF_INET) {
			*out_addr = (*(sockaddr_in*)nextRes->ai_addr).sin_addr;
			resolved = true;
			break;
		}
	}

	freeaddrinfo(res);
	return resolved;
}

// "host:port" -> host text + host-order port. Splits on the last colon; the port is mandatory. The
// host is not resolved here, so this is safe on the config-load path.
static bool ParseEndpoint(const char* endpoint, std::string* out_host, uint16_t* out_port_hbo)
{
	if (!endpoint || !out_host || !out_port_hbo) {
		return false;
	}
	std::string ep = endpoint;
	size_t colon = ep.rfind(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 >= ep.size()) {
		return false;
	}
	std::string host = ep.substr(0, colon);
	int port = atoi(ep.c_str() + colon + 1);
	if (port <= 0 || port > 0xFFFF) {
		return false;
	}
	*out_host = host;
	*out_port_hbo = (uint16_t)port;
	return true;
}

static rapidjson::Value* FindMember(rapidjson::Value* value, const char* name)
{
	auto itrMember = value->FindMember(name);
	if (itrMember == value->MemberEnd()) {
		return 0;
	}
	return &(*itrMember).value;
}

// Parse one Title entry. A non-matching titleId is not an error (other titles share the file), so it
// returns true without touching out_config; out_config->valid is set only for the matched title once
// it has at least one resolved service.
static bool ParseTitleEntry(rapidjson::Value* entry, uint32_t running_title_id, XLSP_TITLE_CONFIG* out_config)
{
	if (!entry->IsObject()) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "[XLSP] %s config Title entry is not an object."
			, __func__
		);
		return false;
	}

	// Title ids are hexadecimal everywhere else - the title's own cfg is read with "%x" and profile
	// folders are named "%08X" - so a bare "544707D4" is the natural thing to copy into this file.
	// Base 16 accepts that and an explicit 0x prefix; base 0 would read the bare form as decimal and
	// stop at the first hex letter, silently matching no title.
	rapidjson::Value* jTitleId = FindMember(entry, "titleId");
	uint32_t titleId = 0;
	if (jTitleId && jTitleId->IsString()) {
		const char* titleIdText = jTitleId->GetString();
		char* titleIdEnd = 0;
		titleId = (uint32_t)strtoul(titleIdText, &titleIdEnd, 16);
		if (titleIdEnd == titleIdText || *titleIdEnd) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
				, "[XLSP] %s config Title entry 'titleId' (%s) is not a hexadecimal title id."
				, __func__
				, titleIdText
			);
			return false;
		}
	}
	else if (jTitleId && jTitleId->IsUint()) {
		titleId = jTitleId->GetUint();
	}
	else {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "[XLSP] %s config Title entry 'titleId' must be a hex/decimal string or a number."
			, __func__
		);
		return false;
	}
	if (titleId != running_title_id) {
		return true;
	}

	rapidjson::Value* jServices = FindMember(entry, "services");
	if (!jServices || !jServices->IsArray()) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "[XLSP] %s matched title (0x%08x) declares no 'services' array."
			, __func__
			, titleId
		);
		return false;
	}

	if (jServices && jServices->IsArray()) {
		for (auto &jService : jServices->GetArray()) {
			if (!jService.IsObject()) {
				continue;
			}
			rapidjson::Value* jPort = FindMember(&jService, "port");
			rapidjson::Value* jEndpoint = FindMember(&jService, "endpoint");
			if (!jPort || !jPort->IsUint() || !jEndpoint || !jEndpoint->IsString()) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "[XLSP] %s a 'services' entry lacks a numeric 'port' or string 'endpoint'."
					, __func__
				);
				continue;
			}
			uint32_t port = jPort->GetUint();
			if (port == 0 || port > 0xFFFF) {
				continue;
			}

			XLSP_SERVICE_ENDPOINT serviceEndpoint;
			if (!ParseEndpoint(jEndpoint->GetString(), &serviceEndpoint.host, &serviceEndpoint.portHbo)) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "[XLSP] %s could not parse service 'endpoint' (%s)."
					, __func__
					, jEndpoint->GetString()
				);
				continue;
			}
			rapidjson::Value* jServiceName = FindMember(&jService, "service");
			if (jServiceName && jServiceName->IsString()) {
				serviceEndpoint.service = jServiceName->GetString();
			}

			if (!ResolveIpv4(serviceEndpoint.host.c_str(), &serviceEndpoint.addr)) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
					, "[XLSP] %s could not resolve service 'endpoint' host (%s)."
					, __func__
					, serviceEndpoint.host.c_str()
				);
				continue;
			}
			out_config->servicesByPort[(uint16_t)port] = serviceEndpoint;

			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
				, "[XLSP] service (%s) titlePort (%u) -> %s:%hu."
				, serviceEndpoint.service.size() ? serviceEndpoint.service.c_str() : "?"
				, port
				, serviceEndpoint.host.c_str()
				, serviceEndpoint.portHbo
			);
		}
	}

	out_config->valid = !out_config->servicesByPort.empty();
	return true;
}

static void ParseConfigFile(uint8_t* config_buffer, size_t config_buffer_size, uint32_t running_title_id, XLSP_TITLE_CONFIG* out_config)
{
	rapidjson::Document document;
	if (document.Parse< rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag >((char*)config_buffer, config_buffer_size).HasParseError()) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "[XLSP] %s failed to parse JSON config with error 0x%08x at offset %zu."
			, __func__
			, document.GetParseError()
			, document.GetErrorOffset()
		);
		return;
	}
	if (!document.IsArray()) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "[XLSP] %s config is not an array of Title entries."
			, __func__
		);
		return;
	}

	for (auto &titleEntry : document.GetArray()) {
		ParseTitleEntry(&titleEntry, running_title_id, out_config);
		if (out_config->valid) {
			break;
		}
	}
}

static void LoadConfigForCurrentTitle(XLSP_TITLE_CONFIG* out_config)
{
	uint32_t titleId = xlive_title_id;
	if (!titleId) {
		return;
	}
	if (!xlln_file_config_path) {
		return;
	}

	// InitXLLN made xlln_file_config_path absolute before the title could change the working directory,
	// so this derivation holds however late it runs.
	wchar_t* pathConfig = PathFromFilename(xlln_file_config_path);
	if (!pathConfig) {
		return;
	}
	wchar_t* xlspConfigPath = FormMallocString(L"%wsxlln-xlsp\\", pathConfig);
	delete[] pathConfig;
	pathConfig = 0;
	if (!xlspConfigPath) {
		return;
	}

	wchar_t* configSearch = FormMallocString(L"%ws*.json", xlspConfigPath);
	WIN32_FIND_DATAW findData;
	HANDLE hFind = FindFirstFileW(configSearch, &findData);
	if (hFind == INVALID_HANDLE_VALUE) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
			, "[XLSP] no config file found (%ls) lastErr (%u)."
			, configSearch
			, (uint32_t)GetLastError()
		);
	}
	free(configSearch);
	configSearch = 0;
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (!EndsWithCaseInsensitive(findData.cFileName, L".json")) {
				continue;
			}
			wchar_t* configFilePath = FormMallocString(L"%ws%ws", xlspConfigPath, findData.cFileName);

			FILE* fileHandle = 0;
			errno_t errorFopen = _wfopen_s(&fileHandle, configFilePath, L"rb");
			if (!errorFopen && fileHandle) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_DEBUG
					, "[XLSP] Loading: \"%ls\"."
					, configFilePath
				);
				fseek(fileHandle, (long)0, SEEK_END);
				long fileSizeSigned = ftell(fileHandle);
				fseek(fileHandle, (long)0, SEEK_SET);
				if (fileSizeSigned < 0) {
					fclose(fileHandle);
					fileHandle = 0;
					free(configFilePath);
					configFilePath = 0;
					continue;
				}
				size_t fileSize = (size_t)fileSizeSigned;
				uint8_t* buffer = new uint8_t[fileSize + 1];
				size_t readC = fread(buffer, sizeof(uint8_t), fileSize / sizeof(uint8_t), fileHandle);
				buffer[readC] = 0;
				fclose(fileHandle);
				fileHandle = 0;

				ParseConfigFile(buffer, readC, titleId, out_config);

				delete[] buffer;
				buffer = 0;
			}

			free(configFilePath);
			configFilePath = 0;
		} while (!out_config->valid && FindNextFileW(hFind, &findData));
		FindClose(hFind);
	}

	free(xlspConfigPath);
	xlspConfigPath = 0;

	if (out_config->valid) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
			, "[XLSP] configured for title (0x%08x): %zu declared service(s)."
			, titleId
			, out_config->servicesByPort.size()
		);
	}
	else {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
			, "[XLSP] no config Title entry matched the running title (0x%08x); XLSP is inactive."
			, titleId
		);
	}
}

void XllnXlspLoadConfig()
{
	if (xlln_xlsp_config_loaded) {
		return;
	}
	xlln_xlsp_config_loaded = true;

	LoadConfigForCurrentTitle(&xlln_xlsp_config);
}

bool XllnXlspIsConfigured()
{
	return xlln_xlsp_config.valid;
}

bool XllnXlspGetPassthroughEndpoint(uint16_t title_port_hbo, SOCKADDR_STORAGE* out_endpoint, const char** out_service)
{
	if (!out_endpoint) {
		return false;
	}
	XLSP_TITLE_CONFIG* config = &xlln_xlsp_config;
	if (!config->valid) {
		return false;
	}
	auto itrService = config->servicesByPort.find(title_port_hbo);
	if (itrService == config->servicesByPort.end()) {
		return false;
	}

	memset(out_endpoint, 0, sizeof(*out_endpoint));
	sockaddr_in* backendSockAddr = (sockaddr_in*)out_endpoint;
	backendSockAddr->sin_family = AF_INET;
	backendSockAddr->sin_addr = itrService->second.addr;
	backendSockAddr->sin_port = htons(itrService->second.portHbo);
	if (out_service) {
		*out_service = itrService->second.service.c_str();
	}
	return true;
}

void XllnXlspReset()
{
	xlln_xlsp_config.valid = false;
	xlln_xlsp_config.servicesByPort.clear();
	xlln_xlsp_config_loaded = false;
}
