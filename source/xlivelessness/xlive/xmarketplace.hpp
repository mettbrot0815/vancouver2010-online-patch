#pragma once
#include <map>
#include <vector>
#include "xcontent.hpp"

typedef struct _MARKETPLACE_ENUMERATION_DETAILS {
	uint32_t userIndex = 0;
	size_t batchSize = 0;
	size_t cursor = 0;
	std::vector<XLLN_OFFER_DETAILS> offers;
} MARKETPLACE_ENUMERATION_DETAILS;

extern CRITICAL_SECTION xlive_critsec_xmarketplace;
// Key: enumerator handle. Value: 0 for an asset enumerator.
extern std::map<HANDLE, MARKETPLACE_ENUMERATION_DETAILS*> xlive_xmarketplace_enumerators;
