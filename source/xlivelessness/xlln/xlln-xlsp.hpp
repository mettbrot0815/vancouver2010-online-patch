#pragma once
#include <winsock2.h>
#include <stdint.h>

// Per-title XLSP configuration, read from XLiveLessNess/xlln-xlsp/*.json: which secure-gateway ports
// the title dials and where each bridges to. Service labels are opaque strings to XLLN.

// Reads the config and matches it against the running title. Must run once the title id is known and
// off the loader lock: it enumerates files, parses JSON and resolves hostnames.
void XllnXlspLoadConfig();

// Whether the running title matched an entry declaring at least one service.
bool XllnXlspIsConfigured();

// Resolves which backend a title-server connection to the given secure-gateway port bridges to.
// Returns false for an undeclared port, which is not bridged. out_service may be null.
bool XllnXlspGetPassthroughEndpoint(uint16_t title_port_hbo, SOCKADDR_STORAGE* out_endpoint, const char** out_service);

// Drops the loaded configuration.
void XllnXlspReset();
