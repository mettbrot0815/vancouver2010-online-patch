#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

typedef struct _CONTENT_OFFER_REGIONS_DETAILS {
	uint32_t unkn1 = 0;
	uint32_t unkn2 = 0;
	uint32_t unkn3 = 0;
	// Probably an enum: "TaxNotApplicable", "TaxNotIncluded".
	uint8_t tax = 0;
} CONTENT_OFFER_REGIONS_DETAILS;

typedef struct _CONTENT_DETAILS {
	wchar_t* xcontentPath = 0;
	bool premium = false;
	uint32_t titleId = 0;
	// XCONTENTTYPE_
	uint32_t contentPackageType = 0;
	uint32_t licenseBits = 0;
	uint32_t baseVersion = 0;
	uint32_t updateVersion = 0;
	wchar_t* thumbnailImage = 0;
	uint64_t offeringId = 0;
	wchar_t* adminFriendlyName = 0;
	wchar_t* titleName = 0;
	bool purchaseOnceOnly = false;
	uint8_t contentFlags = 0;
	uint8_t contentId[XLIVE_CONTENT_ID_SIZE];
	uint8_t platform = 0;
	uint32_t bitFlags = 0;
	tm activationDate;
	wchar_t* defaultDisplayName = 0;
	// OfferRegions.<KEY>
	std::map<uint8_t, CONTENT_OFFER_REGIONS_DETAILS> offerRegions;
	XMARKETPLACE_OFFERING_TYPE offeringType = XMARKETPLACE_OFFERING_TYPE::XMARKETPLACE_OFFERING_TYPE_CONTENT;
	tm endDate;
	bool displayInDash = false;
	uint32_t version = 0;
	// DashDisplayName.<KEY>
	std::map<uint8_t, wchar_t*> dashDisplayName;
	// DashDescription.<KEY>
	std::map<uint8_t, wchar_t*> dashDescription;
	// From the pack's .LIC file.
	wchar_t* licensePath = 0;
	uint64_t licenseeXuid = 0;
	uint32_t licenseMask = 0;
	// The pack has a Content folder.
	bool hasPayload = false;
	
	_CONTENT_DETAILS()
	{
		memset(this->contentId, 0, sizeof(this->contentId));
		memset(&this->activationDate, 0, sizeof(this->activationDate));
		memset(&this->endDate, 0, sizeof(this->endDate));
	}
	
	~_CONTENT_DETAILS()
	{
		if (this->xcontentPath) {
			delete[] this->xcontentPath;
			this->xcontentPath = 0;
		}
		if (this->thumbnailImage) {
			delete[] this->thumbnailImage;
			this->thumbnailImage = 0;
		}
		if (this->adminFriendlyName) {
			delete[] this->adminFriendlyName;
			this->adminFriendlyName = 0;
		}
		if (this->titleName) {
			delete[] this->titleName;
			this->titleName = 0;
		}
		if (this->defaultDisplayName) {
			delete[] this->defaultDisplayName;
			this->defaultDisplayName = 0;
		}
		if (this->licensePath) {
			delete[] this->licensePath;
			this->licensePath = 0;
		}
		for (auto &dashDisplayName : this->dashDisplayName) {
			delete[] dashDisplayName.second;
		}
		this->dashDisplayName.clear();
		for (auto &dashDescription : this->dashDescription) {
			delete[] dashDescription.second;
		}
		this->dashDescription.clear();
	}
} CONTENT_DETAILS;

typedef struct _XCONTENT_ENUMERATION_DETAILS {
	HANDLE enumeratorHandle = 0;
	uint32_t titleId = 0;
	// Key: the 20-byte ContentID as raw bytes.
	std::map<std::string, CONTENT_DETAILS*> contentDetails;
} XCONTENT_ENUMERATION_DETAILS;

typedef struct _XLLN_OFFER_DETAILS {
	uint64_t offeringId = 0;
	uint32_t titleId = 0;
	uint32_t contentPackageType = 0;
	uint8_t contentId[XLIVE_CONTENT_ID_SIZE];
	uint32_t licenseMask = 0;
	bool hasPayload = false;
	std::wstring name;
	std::wstring titleName;
	std::wstring sellText;
	_XLLN_OFFER_DETAILS()
	{
		memset(this->contentId, 0, sizeof(this->contentId));
	}
} XLLN_OFFER_DETAILS;

uint32_t XllnContentLicenseMask(const CONTENT_DETAILS* content_details);
wchar_t* XllnContentReadDisplayName(const wchar_t* pack_path);
void XllnContentCollectOffers(std::vector<XLLN_OFFER_DETAILS>& result_offers);

extern CRITICAL_SECTION xlive_critsec_xcontent;
extern std::map<HANDLE, XCONTENT_ENUMERATION_DETAILS*> xlive_xcontent_enumerators;
// Key: content access handle. Value: the root path of its pack.
extern std::map<HANDLE, wchar_t*> xlive_xcontent_access_handles;

wchar_t* XllnContentResolveProtectedPath(HANDLE content_access_handle, const wchar_t* file_pathname);
