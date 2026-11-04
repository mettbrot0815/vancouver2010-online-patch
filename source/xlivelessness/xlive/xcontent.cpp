#include <winsock2.h>
#include <algorithm>
#include "xdefs.hpp"
#include "xfuncs.hpp"
#include "xcontent.hpp"
#include "xlive.hpp"
#include "../xlln/debug-log.hpp"
#include "../xlln/xlln.hpp"
#include "../xlln/xlln-config.hpp"
#include "../utils/utils.hpp"

CRITICAL_SECTION xlive_critsec_xcontent;

// Key: enumerator handle (id).
std::map<HANDLE, XCONTENT_ENUMERATION_DETAILS*> xlive_xcontent_enumerators;
// Key: content_access_handle.
// Value: None.
std::map<HANDLE, wchar_t*> xlive_xcontent_access_handles;

wchar_t* XllnContentResolveProtectedPath(HANDLE content_access_handle, const wchar_t* file_pathname)
{
	bool isAbsolute = file_pathname[0] == L'\\' || file_pathname[0] == L'/' || (iswalpha(file_pathname[0]) && file_pathname[1] == L':');
	wchar_t* packPath = 0;
	if (!isAbsolute) {
		EnterCriticalSection(&xlive_critsec_xcontent);
		auto const &itrAccessHandle = xlive_xcontent_access_handles.find(content_access_handle);
		if (itrAccessHandle != xlive_xcontent_access_handles.end()) {
			packPath = CloneString(itrAccessHandle->second);
		}
		LeaveCriticalSection(&xlive_critsec_xcontent);
	}
	if (!packPath) {
		return CloneString(file_pathname);
	}
	wchar_t* resolvedPath = FormNewString(L"%s\\Content\\%s", packPath, file_pathname);
	if (GetFileAttributesW(resolvedPath) == INVALID_FILE_ATTRIBUTES) {
		wchar_t* rootPath = FormNewString(L"%s\\%s", packPath, file_pathname);
		if (GetFileAttributesW(rootPath) != INVALID_FILE_ATTRIBUTES) {
			delete[] resolvedPath;
			resolvedPath = rootPath;
			rootPath = 0;
		}
		else {
			delete[] rootPath;
			rootPath = 0;
		}
	}
	delete[] packPath;
	packPath = 0;
	for (wchar_t* separator = resolvedPath; *separator; separator++) {
		if (*separator == L'/') {
			*separator = L'\\';
		}
	}
	return resolvedPath;
}

static wchar_t* XllnContentUnquote_(const wchar_t* value)
{
	size_t valueLength = wcslen(value);
	if (valueLength >= 2 && value[0] == L'"' && value[valueLength - 1] == L'"') {
		wchar_t* text = new wchar_t[valueLength - 1];
		memcpy(text, value + 1, (valueLength - 2) * sizeof(wchar_t));
		text[valueLength - 2] = 0;
		return text;
	}
	return CloneString(value);
}

static void XllnContentSetLocalisedText_(std::map<uint8_t, wchar_t*>& texts, uint8_t region, const wchar_t* value)
{
	const wchar_t* comma = wcschr(value, L',');
	auto const &itrText = texts.find(region);
	if (itrText != texts.end()) {
		delete[] itrText->second;
		texts.erase(itrText);
	}
	texts[region] = XllnContentUnquote_(comma ? comma + 1 : value);
}

uint32_t XllnContentLicenseMask(const CONTENT_DETAILS* content_details)
{
	if (content_details->licensePath) {
		return content_details->licenseMask;
	}
	// Every bit set passes any bit test a title makes on the mask.
	return content_details->licenseBits ? content_details->licenseBits : 0xFFFFFFFF;
}

void XllnContentCollectOffers(std::vector<XLLN_OFFER_DETAILS>& result_offers)
{
	XLIVE_CONTENT_RETRIEVAL_INFO contentRetrievalInfo;
	contentRetrievalInfo.dwContentAPIVersion = XLIVE_CONTENT_API_VERSION;
	contentRetrievalInfo.dwRetrievalMask = XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_USERS;
	contentRetrievalInfo.dwUserIndex = 0;
	contentRetrievalInfo.xuidUser = INVALID_XUID;
	contentRetrievalInfo.dwTitleID = xlive_title_id;
	contentRetrievalInfo.dwContentType = 0;
	memset(contentRetrievalInfo.abContentID, 0, sizeof(contentRetrievalInfo.abContentID));
	size_t bufferSizeRequired = 0;
	HANDLE enumeratorHandle = INVALID_HANDLE_VALUE;
	uint32_t resultCreateEnumerator = XLiveContentCreateEnumerator(1, &contentRetrievalInfo, &bufferSizeRequired, &enumeratorHandle);
	if (resultCreateEnumerator != ERROR_SUCCESS) {
		XLLN_DEBUG_LOG_ECODE(resultCreateEnumerator, XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s Failed to create XContent enumerator:", __func__);
		return;
	}
	EnterCriticalSection(&xlive_critsec_xcontent);
	auto const &itrXContentEnumerator = xlive_xcontent_enumerators.find(enumeratorHandle);
	if (itrXContentEnumerator != xlive_xcontent_enumerators.end()) {
		for (auto const &itrContentDetail : itrXContentEnumerator->second->contentDetails) {
			const CONTENT_DETAILS* contentDetails = itrContentDetail.second;
			XLLN_OFFER_DETAILS* offer = 0;
			for (auto &existingOffer : result_offers) {
				if (existingOffer.offeringId == contentDetails->offeringId) {
					offer = &existingOffer;
					break;
				}
			}
			// When several packs share one OfferingID, the pack with files stands for the offer.
			if (offer && (offer->hasPayload || !contentDetails->hasPayload)) {
				continue;
			}
			if (!offer) {
				result_offers.push_back(XLLN_OFFER_DETAILS());
				offer = &result_offers.back();
			}
			offer->offeringId = contentDetails->offeringId;
			offer->titleId = contentDetails->titleId;
			offer->contentPackageType = contentDetails->contentPackageType;
			memcpy(offer->contentId, contentDetails->contentId, sizeof(offer->contentId));
			offer->licenseMask = XllnContentLicenseMask(contentDetails);
			offer->hasPayload = contentDetails->hasPayload;
			auto const &itrDashName = contentDetails->dashDisplayName.find(0);
			offer->name = itrDashName != contentDetails->dashDisplayName.end() ? itrDashName->second : (contentDetails->defaultDisplayName ? contentDetails->defaultDisplayName : L"");
			offer->titleName = contentDetails->titleName ? contentDetails->titleName : L"";
			auto const &itrDashDescription = contentDetails->dashDescription.find(0);
			offer->sellText = itrDashDescription != contentDetails->dashDescription.end() ? itrDashDescription->second : L"";
		}
	}
	LeaveCriticalSection(&xlive_critsec_xcontent);
	XCloseHandle(enumeratorHandle);
	std::sort(result_offers.begin(), result_offers.end(), [](const XLLN_OFFER_DETAILS &a, const XLLN_OFFER_DETAILS &b) { return a.offeringId < b.offeringId; });
}

// #5350
HRESULT WINAPI XLiveContentCreateAccessHandle(uint32_t user_index, XLIVE_CONTENT_INFO* content_info, uint32_t license_info_version, XLIVE_PROTECTED_BUFFER* protected_buffer, size_t protected_buffer_offset, HANDLE* content_access_handle, XOVERLAPPED* xoverlapped)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (!content_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (license_info_version != XLIVE_LICENSE_INFO_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (license_info_version != XLIVE_LICENSE_INFO_VERSION) (%u != %u).", __func__, license_info_version, XLIVE_LICENSE_INFO_VERSION);
		return E_INVALIDARG;
	}
	if (!protected_buffer) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s protected_buffer is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (protected_buffer->dwSize - protected_buffer_offset < sizeof(XLIVE_LICENSE_INFO)) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s protected_buffer->dwSize (0x%x) - protected_buffer_offset (0x%x) is not big enough to hold sizeof(XLIVE_LICENSE_INFO) (0x%zx).", __func__, protected_buffer->dwSize, protected_buffer_offset, sizeof(XLIVE_LICENSE_INFO));
		return E_INVALIDARG;
	}
	if (!content_access_handle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_access_handle is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (xoverlapped) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s xoverlapped is not NULL.", __func__);
		return E_NOTIMPL;
	}
	
	XLIVE_CONTENT_RETRIEVAL_INFO contentRetrievalInfo;
	contentRetrievalInfo.dwContentAPIVersion = XLIVE_CONTENT_API_VERSION;
	contentRetrievalInfo.dwRetrievalMask = XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_TITLES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_USERS;
	contentRetrievalInfo.dwUserIndex = 0;
	contentRetrievalInfo.xuidUser = INVALID_XUID;
	contentRetrievalInfo.dwTitleID = 0;
	contentRetrievalInfo.dwContentType = 0;
	memset(contentRetrievalInfo.abContentID, 0, sizeof(contentRetrievalInfo.abContentID));
	size_t bufferSizeRequired = 0;
	HANDLE enumeratorHandle = INVALID_HANDLE_VALUE;
	uint32_t resultCreateEnumerator = XLiveContentCreateEnumerator(1, &contentRetrievalInfo, &bufferSizeRequired, &enumeratorHandle);
	if (resultCreateEnumerator != ERROR_SUCCESS) {
		XLLN_DEBUG_LOG_ECODE(resultCreateEnumerator, XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to create XContent enumerator:"
			, __func__
		);
		return HRESULT_FROM_WIN32(resultCreateEnumerator);
	}
	
	HRESULT result = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	bool foundContent = false;
	
	EnterCriticalSection(&xlive_critsec_xcontent);
	
	auto const &itrXContentEnumerator = xlive_xcontent_enumerators.find(enumeratorHandle);
	if (itrXContentEnumerator != xlive_xcontent_enumerators.end()) {
		XCONTENT_ENUMERATION_DETAILS* xcontentEnumerationDetails = itrXContentEnumerator->second;
		for (auto itrContentDetail : xcontentEnumerationDetails->contentDetails) {
			if (
				itrContentDetail.second->titleId == content_info->dwTitleID
				&& itrContentDetail.second->contentPackageType == content_info->dwContentType
				&& memcmp(itrContentDetail.second->contentId, content_info->abContentID, sizeof(itrContentDetail.second->contentId)) == 0
			) {
				foundContent = true;
				result = S_OK;
				
				XLIVE_LICENSE_INFO &xcontentLicenceInfo = *(XLIVE_LICENSE_INFO*)&((uint8_t*)&protected_buffer->bData)[protected_buffer_offset];
				xcontentLicenceInfo.dwContentAPIVersion = XLIVE_LICENSE_INFO_VERSION;
				memcpy(xcontentLicenceInfo.abContentID, itrContentDetail.second->contentId, sizeof(itrContentDetail.second->contentId));
				xcontentLicenceInfo.dwLicenseMask = XllnContentLicenseMask(itrContentDetail.second);
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_INFO, "%s \"%ls\" license mask 0x%08x (%s).", __func__, itrContentDetail.second->xcontentPath, xcontentLicenceInfo.dwLicenseMask, itrContentDetail.second->licensePath ? "from the .LIC file" : "no .LIC file, default");
				
				*content_access_handle = CreateMutex(NULL, NULL, NULL);
				xlive_xcontent_access_handles[*content_access_handle] = CloneString(itrContentDetail.second->xcontentPath);
				
				break;
			}
		}
	}
	else {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to find XContent enumerator (0x%zx)."
			, __func__
			, enumeratorHandle
		);
	}
	
	LeaveCriticalSection(&xlive_critsec_xcontent);
	
	BOOL resultCloseHandle = XCloseHandle(enumeratorHandle);
	if (!resultCloseHandle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to close XContent enumerator handle."
			, __func__
		);
	}
	
	if (!foundContent) {
		char contentIdAsText[(XLIVE_CONTENT_ID_SIZE * 2) + sizeof(char)];
		ByteArrayToHexStr(contentIdAsText, content_info->abContentID, XLIVE_CONTENT_ID_SIZE);
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to find XContent for TitleId (%X), ContentType (0x%08x), ContentID (%s)."
			, __func__
			, content_info->dwTitleID
			, content_info->dwContentType
			, contentIdAsText
		);
	}
	
	return result;
}

// #5351
HRESULT WINAPI XLiveContentInstallPackage(XLIVE_CONTENT_INFO* content_info, const wchar_t* cab_file_pathname, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* callback_install_params)
{
	TRACE_FX();
	if (!content_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (content_info->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info->dwContentAPIVersion (%u) != XLIVE_CONTENT_API_VERSION (%u).", __func__, content_info->dwContentAPIVersion, XLIVE_CONTENT_API_VERSION);
		return E_INVALIDARG;
	}
	if (content_info->dwContentType != XCONTENTTYPE_MARKETPLACE) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (content_info->dwContentType != XCONTENTTYPE_MARKETPLACE) (%u != %u).", __func__, content_info->dwContentType, XCONTENTTYPE_MARKETPLACE);
		return E_INVALIDARG;
	}
	if (!cab_file_pathname) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s cab_file_pathname is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (callback_install_params) {
		if (callback_install_params->cbSize != sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V1) && callback_install_params->cbSize != sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V2)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->cbSize (%u) is not sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_(V1/V2)) (12/16).", __func__, callback_install_params->cbSize);
			return E_INVALIDARG;
		}
		if (!callback_install_params->pInstallCallback && callback_install_params->cbSize == sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V1)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->pInstallCallback is NULL.", __func__);
			return E_INVALIDARG;
		}
		if (!callback_install_params->pInstallCallback && !callback_install_params->pInstallCallbackEx && callback_install_params->cbSize == sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V2)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->pInstallCallback and callback_install_params->pInstallCallbackEx is NULL.", __func__);
			return E_INVALIDARG;
		}
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	return E_UNEXPECTED;
	return E_ACCESSDENIED;
	return S_OK;
}

// #5352
HRESULT WINAPI XLiveContentUninstall(XLIVE_CONTENT_INFO* content_info, XUID* uninstall_for_xuid, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* callback_install_params)
{
	TRACE_FX();
	if (!content_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (content_info->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info->dwContentAPIVersion (%u) != XLIVE_CONTENT_API_VERSION (%u).", __func__, content_info->dwContentAPIVersion, XLIVE_CONTENT_API_VERSION);
		return E_INVALIDARG;
	}
	if (content_info->dwContentType != XCONTENTTYPE_MARKETPLACE) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (content_info->dwContentType != XCONTENTTYPE_MARKETPLACE) (%u != %u).", __func__, content_info->dwContentType, XCONTENTTYPE_MARKETPLACE);
		return E_INVALIDARG;
	}
	if (uninstall_for_xuid && !*uninstall_for_xuid) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s *uninstall_for_xuid is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (uninstall_for_xuid && !IsOnlineXUID(*uninstall_for_xuid)) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s uninstall_for_xuid is not an online account.", __func__);
		return E_INVALIDARG;
	}
	if (callback_install_params) {
		if (callback_install_params->cbSize != sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V1) && callback_install_params->cbSize != sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V2)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->cbSize (%u) is not sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_(V1/V2)) (12/16).", __func__, callback_install_params->cbSize);
			return E_INVALIDARG;
		}
		if (!callback_install_params->pInstallCallback && callback_install_params->cbSize == sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V1)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->pInstallCallback is NULL.", __func__);
			return E_INVALIDARG;
		}
		if (!callback_install_params->pInstallCallback && !callback_install_params->pInstallCallbackEx && callback_install_params->cbSize == sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V2)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->pInstallCallback and callback_install_params->pInstallCallbackEx is NULL.", __func__);
			return E_INVALIDARG;
		}
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	return E_UNEXPECTED;
	return E_ACCESSDENIED;
	return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	return S_OK;
}

static HRESULT XllnContentInstallCallbackInvoke_(const XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* callback_params, XLIVE_CONTENT_INSTALL_NOTIFICATION notification, uint32_t current_tick, UINT_PTR param1, UINT_PTR param2, HRESULT current_status)
{
	if (!callback_params || !callback_params->pInstallCallback) {
		return S_OK;
	}
	return callback_params->pInstallCallback(callback_params->pInstallCallbackContext, notification, current_tick, param1, param2, current_status);
}

static void XllnContentListFiles_(const wchar_t* directory, std::vector<std::wstring>& result_files, uint32_t depth)
{
	// A junction inside a pack that points back up the tree would recurse without end.
	if (depth > 32) {
		return;
	}
	wchar_t* searchPattern = FormNewString(L"%s\\*", directory);
	WIN32_FIND_DATAW findData;
	HANDLE hFind = FindFirstFileW(searchPattern, &findData);
	delete[] searchPattern;
	searchPattern = 0;
	if (hFind == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) {
			continue;
		}
		wchar_t* entryPath = FormNewString(L"%s\\%s", directory, findData.cFileName);
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			XllnContentListFiles_(entryPath, result_files, depth + 1);
		}
		else {
			result_files.push_back(entryPath);
		}
		delete[] entryPath;
		entryPath = 0;
	} while (FindNextFileW(hFind, &findData));
	FindClose(hFind);
}

// The files are listed, not hashed: a pack is accepted as the user installed it.
static HRESULT XllnContentVerifyPack_(const CONTENT_DETAILS* content_details, const XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* callback_params)
{
	wchar_t* packPath = CloneString(content_details->xcontentPath);
	for (wchar_t* separator = packPath; *separator; separator++) {
		if (*separator == L'/') {
			*separator = L'\\';
		}
	}
	std::vector<std::wstring> files;
	XllnContentListFiles_(packPath, files, 0);
	delete[] packPath;
	packPath = 0;
	HRESULT result = XllnContentInstallCallbackInvoke_(callback_params, XLIVE_CONTENT_INSTALL_NOTIFY_TICKS_REQUIRED, 0, files.size(), 0, S_OK);
	if (FAILED(result)) {
		return E_ABORT;
	}
	uint32_t tick = 0;
	for (auto const &file : files) {
		result = XllnContentInstallCallbackInvoke_(callback_params, XLIVE_CONTENT_INSTALL_NOTIFY_STARTFILEVERIFY, tick, (UINT_PTR)file.c_str(), 0, S_OK);
		if (FAILED(result)) {
			return E_ABORT;
		}
		tick++;
		result = XllnContentInstallCallbackInvoke_(callback_params, XLIVE_CONTENT_INSTALL_NOTIFY_ENDFILEVERIFY, tick, (UINT_PTR)file.c_str(), 0, S_OK);
		if (FAILED(result)) {
			return E_ABORT;
		}
	}
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s \"%ls\" reported %zu file(s) to the title.", __func__, content_details->xcontentPath, files.size());
	return S_OK;
}

// #5354
HRESULT WINAPI XLiveContentVerifyInstalledPackage(XLIVE_CONTENT_INFO* content_info, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* callback_install_params)
{
	TRACE_FX();
	if (!content_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (content_info->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info->dwContentAPIVersion (%u) != XLIVE_CONTENT_API_VERSION (%u).", __func__, content_info->dwContentAPIVersion, XLIVE_CONTENT_API_VERSION);
		return E_INVALIDARG;
	}
	if (content_info->dwContentType != XCONTENTTYPE_MARKETPLACE) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (content_info->dwContentType != XCONTENTTYPE_MARKETPLACE) (%u != %u).", __func__, content_info->dwContentType, XCONTENTTYPE_MARKETPLACE);
		return E_INVALIDARG;
	}
	if (callback_install_params) {
		if (callback_install_params->cbSize != sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V1) && callback_install_params->cbSize != sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V2)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->cbSize (%u) is not sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_(V1/V2)) (12/16).", __func__, callback_install_params->cbSize);
			return E_INVALIDARG;
		}
		if (!callback_install_params->pInstallCallback && callback_install_params->cbSize == sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V1)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->pInstallCallback is NULL.", __func__);
			return E_INVALIDARG;
		}
		if (!callback_install_params->pInstallCallback && !callback_install_params->pInstallCallbackEx && callback_install_params->cbSize == sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V2)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->pInstallCallback and callback_install_params->pInstallCallbackEx is NULL.", __func__);
			return E_INVALIDARG;
		}
	}
	
	
	XLIVE_CONTENT_RETRIEVAL_INFO contentRetrievalInfo;
	contentRetrievalInfo.dwContentAPIVersion = XLIVE_CONTENT_API_VERSION;
	contentRetrievalInfo.dwRetrievalMask = XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_TITLES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_USERS;
	contentRetrievalInfo.dwUserIndex = 0;
	contentRetrievalInfo.xuidUser = INVALID_XUID;
	contentRetrievalInfo.dwTitleID = 0;
	contentRetrievalInfo.dwContentType = 0;
	memset(contentRetrievalInfo.abContentID, 0, sizeof(contentRetrievalInfo.abContentID));
	size_t bufferSizeRequired = 0;
	HANDLE enumeratorHandle = INVALID_HANDLE_VALUE;
	uint32_t resultCreateEnumerator = XLiveContentCreateEnumerator(1, &contentRetrievalInfo, &bufferSizeRequired, &enumeratorHandle);
	if (resultCreateEnumerator != ERROR_SUCCESS) {
		XLLN_DEBUG_LOG_ECODE(resultCreateEnumerator, XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to create XContent enumerator:"
			, __func__
		);
		return HRESULT_FROM_WIN32(resultCreateEnumerator);
	}
	
	HRESULT result = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	bool foundContent = false;
	CONTENT_DETAILS* foundDetails = 0;
	
	EnterCriticalSection(&xlive_critsec_xcontent);
	
	auto const &itrXContentEnumerator = xlive_xcontent_enumerators.find(enumeratorHandle);
	if (itrXContentEnumerator != xlive_xcontent_enumerators.end()) {
		XCONTENT_ENUMERATION_DETAILS* xcontentEnumerationDetails = itrXContentEnumerator->second;
		for (auto itrContentDetail : xcontentEnumerationDetails->contentDetails) {
			if (
				itrContentDetail.second->titleId == content_info->dwTitleID
				&& itrContentDetail.second->contentPackageType == content_info->dwContentType
				&& memcmp(itrContentDetail.second->contentId, content_info->abContentID, sizeof(itrContentDetail.second->contentId)) == 0
			) {
				foundContent = true;
				foundDetails = itrContentDetail.second;
				result = S_OK;
				
				break;
			}
		}
	}
	else {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to find XContent enumerator (0x%zx)."
			, __func__
			, enumeratorHandle
		);
	}
	
	LeaveCriticalSection(&xlive_critsec_xcontent);
	
	// The enumerator is this call's own, so its details stay valid until the close below.
	if (foundDetails) {
		result = XllnContentVerifyPack_(foundDetails, callback_install_params);
	}

	BOOL resultCloseHandle = XCloseHandle(enumeratorHandle);
	if (!resultCloseHandle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to close XContent enumerator handle."
			, __func__
		);
	}
	
	if (!foundContent) {
		char contentIdAsText[(XLIVE_CONTENT_ID_SIZE * 2) + sizeof(char)];
		ByteArrayToHexStr(contentIdAsText, content_info->abContentID, XLIVE_CONTENT_ID_SIZE);
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to find XContent for TitleId (%X), ContentType (0x%08x), ContentID (%s)."
			, __func__
			, content_info->dwTitleID
			, content_info->dwContentType
			, contentIdAsText
		);
	}
	
	return result;
}

// #5355
HRESULT WINAPI XLiveContentGetPath(uint32_t user_index, XLIVE_CONTENT_INFO* content_info, wchar_t* result_content_pathname, size_t* result_content_pathname_length)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (!content_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (content_info->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info->dwContentAPIVersion (%u) != XLIVE_CONTENT_API_VERSION (%u).", __func__, content_info->dwContentAPIVersion, XLIVE_CONTENT_API_VERSION);
		return E_INVALIDARG;
	}
	if (!result_content_pathname_length) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_content_pathname_length is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (!*result_content_pathname_length && result_content_pathname) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_content_pathname is not NULL when *result_content_pathname_length is 0.", __func__);
		return E_INVALIDARG;
	}
	
	XLIVE_CONTENT_RETRIEVAL_INFO contentRetrievalInfo;
	contentRetrievalInfo.dwContentAPIVersion = XLIVE_CONTENT_API_VERSION;
	contentRetrievalInfo.dwRetrievalMask = XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_TITLES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_USERS;
	contentRetrievalInfo.dwUserIndex = 0;
	contentRetrievalInfo.xuidUser = INVALID_XUID;
	contentRetrievalInfo.dwTitleID = 0;
	contentRetrievalInfo.dwContentType = 0;
	memset(contentRetrievalInfo.abContentID, 0, sizeof(contentRetrievalInfo.abContentID));
	size_t bufferSizeRequired = 0;
	HANDLE enumeratorHandle = INVALID_HANDLE_VALUE;
	uint32_t resultCreateEnumerator = XLiveContentCreateEnumerator(1, &contentRetrievalInfo, &bufferSizeRequired, &enumeratorHandle);
	if (resultCreateEnumerator != ERROR_SUCCESS) {
		XLLN_DEBUG_LOG_ECODE(resultCreateEnumerator, XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to create XContent enumerator:"
			, __func__
		);
		return HRESULT_FROM_WIN32(resultCreateEnumerator);
	}
	
	HRESULT result = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	bool foundContent = false;
	
	EnterCriticalSection(&xlive_critsec_xcontent);
	
	auto const &itrXContentEnumerator = xlive_xcontent_enumerators.find(enumeratorHandle);
	if (itrXContentEnumerator != xlive_xcontent_enumerators.end()) {
		XCONTENT_ENUMERATION_DETAILS* xcontentEnumerationDetails = itrXContentEnumerator->second;
		for (auto itrContentDetail : xcontentEnumerationDetails->contentDetails) {
			if (
				itrContentDetail.second->titleId == content_info->dwTitleID
				&& itrContentDetail.second->contentPackageType == content_info->dwContentType
				&& memcmp(itrContentDetail.second->contentId, content_info->abContentID, sizeof(itrContentDetail.second->contentId)) == 0
			) {
				foundContent = true;
				// A title appends its own backslash paths to this, and its file system can reject a mixed path.
				wchar_t* contentPath = FormNewString(L"%s", itrContentDetail.second->xcontentPath);
				for (wchar_t* separator = contentPath; *separator; separator++) {
					if (*separator == L'/') {
						*separator = L'\\';
					}
				}
				size_t contentPathLength = wcslen(contentPath) + 1;
				
				if (*result_content_pathname_length < contentPathLength) {
					*result_content_pathname_length = contentPathLength;
					result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
					delete[] contentPath;
					contentPath = 0;
				}
				else {
					memcpy(result_content_pathname, contentPath, contentPathLength * sizeof(wchar_t));
					XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s \"%ls\".", __func__, contentPath);
					delete[] contentPath;
					contentPath = 0;
					*result_content_pathname_length = contentPathLength;
					result = S_OK;
				}
				
				break;
			}
		}
	}
	else {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to find XContent enumerator (0x%zx)."
			, __func__
			, enumeratorHandle
		);
	}
	
	LeaveCriticalSection(&xlive_critsec_xcontent);
	
	BOOL resultCloseHandle = XCloseHandle(enumeratorHandle);
	if (!resultCloseHandle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to close XContent enumerator handle."
			, __func__
		);
	}
	
	if (!foundContent) {
		char contentIdAsText[(XLIVE_CONTENT_ID_SIZE * 2) + sizeof(char)];
		ByteArrayToHexStr(contentIdAsText, content_info->abContentID, XLIVE_CONTENT_ID_SIZE);
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to find XContent for TitleId (%X), ContentType (0x%08x), ContentID (%s)."
			, __func__
			, content_info->dwTitleID
			, content_info->dwContentType
			, contentIdAsText
		);
	}
	
	return result;
}

// #5356
HRESULT WINAPI XLiveContentGetDisplayName(uint32_t user_index, XLIVE_CONTENT_INFO* content_info, wchar_t* result_display_name, size_t* result_display_name_length)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (!content_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (content_info->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info->dwContentAPIVersion (%u) != XLIVE_CONTENT_API_VERSION (%u).", __func__, content_info->dwContentAPIVersion, XLIVE_CONTENT_API_VERSION);
		return E_INVALIDARG;
	}
	if (!result_display_name_length) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_display_name_length is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (!*result_display_name_length && result_display_name) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_display_name is not NULL when *result_display_name_length is 0.", __func__);
		return E_INVALIDARG;
	}
	
	XLIVE_CONTENT_RETRIEVAL_INFO contentRetrievalInfo;
	contentRetrievalInfo.dwContentAPIVersion = XLIVE_CONTENT_API_VERSION;
	contentRetrievalInfo.dwRetrievalMask = XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_TITLES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_USERS;
	contentRetrievalInfo.dwUserIndex = 0;
	contentRetrievalInfo.xuidUser = INVALID_XUID;
	contentRetrievalInfo.dwTitleID = 0;
	contentRetrievalInfo.dwContentType = 0;
	memset(contentRetrievalInfo.abContentID, 0, sizeof(contentRetrievalInfo.abContentID));
	size_t bufferSizeRequired = 0;
	HANDLE enumeratorHandle = INVALID_HANDLE_VALUE;
	uint32_t resultCreateEnumerator = XLiveContentCreateEnumerator(1, &contentRetrievalInfo, &bufferSizeRequired, &enumeratorHandle);
	if (resultCreateEnumerator != ERROR_SUCCESS) {
		XLLN_DEBUG_LOG_ECODE(resultCreateEnumerator, XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to create XContent enumerator:"
			, __func__
		);
		return HRESULT_FROM_WIN32(resultCreateEnumerator);
	}
	
	HRESULT result = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	bool foundContent = false;
	
	EnterCriticalSection(&xlive_critsec_xcontent);
	
	auto const &itrXContentEnumerator = xlive_xcontent_enumerators.find(enumeratorHandle);
	if (itrXContentEnumerator != xlive_xcontent_enumerators.end()) {
		XCONTENT_ENUMERATION_DETAILS* xcontentEnumerationDetails = itrXContentEnumerator->second;
		for (auto itrContentDetail : xcontentEnumerationDetails->contentDetails) {
			if (
				itrContentDetail.second->titleId == content_info->dwTitleID
				&& itrContentDetail.second->contentPackageType == content_info->dwContentType
				&& memcmp(itrContentDetail.second->contentId, content_info->abContentID, sizeof(itrContentDetail.second->contentId)) == 0
			) {
				foundContent = true;
				size_t contentNameLength = wcslen(itrContentDetail.second->defaultDisplayName) + 1;
				
				if (*result_display_name_length < contentNameLength) {
					*result_display_name_length = contentNameLength;
					result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
				}
				else {
					memcpy(result_display_name, itrContentDetail.second->defaultDisplayName, contentNameLength * sizeof(wchar_t));
					XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s \"%ls\" -> \"%ls\".", __func__, itrContentDetail.second->xcontentPath, itrContentDetail.second->defaultDisplayName);
					*result_display_name_length = contentNameLength;
					result = S_OK;
				}
				
				break;
			}
		}
	}
	else {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to find XContent enumerator (0x%zx)."
			, __func__
			, enumeratorHandle
		);
	}
	
	LeaveCriticalSection(&xlive_critsec_xcontent);
	
	BOOL resultCloseHandle = XCloseHandle(enumeratorHandle);
	if (!resultCloseHandle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to close XContent enumerator handle."
			, __func__
		);
	}
	
	if (!foundContent) {
		char contentIdAsText[(XLIVE_CONTENT_ID_SIZE * 2) + sizeof(char)];
		ByteArrayToHexStr(contentIdAsText, content_info->abContentID, XLIVE_CONTENT_ID_SIZE);
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s Failed to find XContent for TitleId (%X), ContentType (0x%08x), ContentID (%s)."
			, __func__
			, content_info->dwTitleID
			, content_info->dwContentType
			, contentIdAsText
		);
	}
	
	return result;
}

// #5357
HRESULT WINAPI XLiveContentGetThumbnail(uint32_t user_index, XLIVE_CONTENT_INFO* content_info, uint8_t* result_thumbnail, size_t* result_thumbnail_size)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (!content_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (content_info->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info->dwContentAPIVersion (%u) != XLIVE_CONTENT_API_VERSION (%u).", __func__, content_info->dwContentAPIVersion, XLIVE_CONTENT_API_VERSION);
		return E_INVALIDARG;
	}
	if (!result_thumbnail_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_thumbnail_size is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (!*result_thumbnail_size && result_thumbnail) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_thumbnail is not NULL when *result_thumbnail_size is 0.", __func__);
		return E_INVALIDARG;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	return E_UNEXPECTED;
	return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	return S_OK;
}

// #5358
HRESULT WINAPI XLiveContentInstallLicense(XLIVE_CONTENT_INFO* content_info, const wchar_t* license_file_pathname, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* callback_install_params)
{
	TRACE_FX();
	if (!content_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (content_info->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info->dwContentAPIVersion (%u) != XLIVE_CONTENT_API_VERSION (%u).", __func__, content_info->dwContentAPIVersion, XLIVE_CONTENT_API_VERSION);
		return E_INVALIDARG;
	}
	if (content_info->dwContentType != XCONTENTTYPE_MARKETPLACE) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (content_info->dwContentType != XCONTENTTYPE_MARKETPLACE) (%u != %u).", __func__, content_info->dwContentType, XCONTENTTYPE_MARKETPLACE);
		return E_INVALIDARG;
	}
	if (!license_file_pathname) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s license_file_pathname is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (callback_install_params) {
		if (callback_install_params->cbSize != sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V1) && callback_install_params->cbSize != sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V2)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->cbSize (%u) is not sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_(V1/V2)) (12/16).", __func__, callback_install_params->cbSize);
			return E_INVALIDARG;
		}
		if (!callback_install_params->pInstallCallback && callback_install_params->cbSize == sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V1)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->pInstallCallback is NULL.", __func__);
			return E_INVALIDARG;
		}
		if (!callback_install_params->pInstallCallback && !callback_install_params->pInstallCallbackEx && callback_install_params->cbSize == sizeof(XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS_V2)) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s callback_install_params->pInstallCallback and callback_install_params->pInstallCallbackEx is NULL.", __func__);
			return E_INVALIDARG;
		}
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	return E_UNEXPECTED;
	return E_ACCESSDENIED;
	return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	return S_OK;
}

static bool ParseDateString(const wchar_t* date_str, struct tm* out_tm)
{
	int m, d, y;
	if (swscanf_s(date_str, L"%d/%d/%d", &m, &d, &y) != 3) {
		return false;
	}
	memset(out_tm, 0, sizeof(*out_tm));
	out_tm->tm_year = y - 1900;
	out_tm->tm_mon = m - 1;
	out_tm->tm_mday = d;
	return true;
}

typedef struct _INTERPRET_XCONTENT_CONTEXT {
	bool foundHeader = false;
	std::vector<wchar_t*> badConfigEntries;
	std::vector<wchar_t*> readSettings;
	std::vector<const wchar_t*> missingSettings;
	CONTENT_DETAILS* contentDetails = 0;
	
	~_INTERPRET_XCONTENT_CONTEXT()
	{
		if (this->contentDetails) {
			delete this->contentDetails;
			this->contentDetails = 0;
		}
		
		for (wchar_t* configEntry : this->badConfigEntries) {
			delete[] configEntry;
		}
		this->badConfigEntries.clear();
		
		for (wchar_t* readSetting : this->readSettings) {
			delete[] readSetting;
		}
		this->readSettings.clear();
		
		this->missingSettings.clear();
	}
	
} INTERPRET_XCONTENT_CONTEXT;

static int interpretDlcContent(const wchar_t* fileLine, const wchar_t* version, size_t lineNumber, void* interpretationContext)
{
	INTERPRET_XCONTENT_CONTEXT &configContext = *(INTERPRET_XCONTENT_CONTEXT*)interpretationContext;
	
	if (!configContext.foundHeader) {
		const wchar_t xcontentHeader[] = L"[All]";
		
		if (wcsncmp(fileLine, xcontentHeader, sizeof(xcontentHeader) - sizeof(xcontentHeader[0])) == 0) {
			configContext.foundHeader = true;
		}
		
		return 0;
	}
	
	bool unrecognised = false;
	bool duplicated = false;
	bool incorrect = false;
	size_t fileLineLen = wcslen(fileLine);
	
	if (fileLine[0] == L'[' || fileLine[0] == L'#' || fileLine[0] == L';' || fileLineLen <= 2) {
		unrecognised = true;
	}
	else {
		const wchar_t* equals = wcschr(fileLine, L'=');
		
		if (!equals || equals == fileLine) {
			unrecognised = true;
		}
		else {
			size_t iValueSearch = 0;
			const wchar_t* value = equals + 1;
			while (*value == L'\t' || *value == L' ') {
				value++;
			}
			
			const wchar_t* whitespace = equals;
			while (&whitespace[-1] != fileLine && (whitespace[-1] == L'\t' || whitespace[-1] == L' ')) {
				whitespace--;
			}
			size_t settingStrLen = whitespace - fileLine;
			
			wchar_t* settingName = new wchar_t[settingStrLen + 1];
			memcpy(settingName, fileLine, settingStrLen * sizeof(wchar_t));
			settingName[settingStrLen] = 0;
			
			for (wchar_t* &readSettingName : configContext.readSettings) {
				if (_wcsicmp(readSettingName, settingName) == 0) {
					duplicated = true;
					break;
				}
			}
			
#define SettingNameMatches(x) _wcsicmp(x, settingName) == 0
			
			if (!unrecognised && !duplicated && !incorrect) {
				
				if (SettingNameMatches(L"Premium")) {
					if (wcscmp(value, L"Yes") == 0) {
						configContext.contentDetails->premium = true;
					}
					else if (wcscmp(value, L"No") == 0) {
						configContext.contentDetails->premium = false;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"TitleID")) {
					uint32_t tempuint32;
					if (swscanf_s(value, L"0x%x", &tempuint32) == 1) {
						configContext.contentDetails->titleId = tempuint32;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"ContentPackageType")) {
					uint32_t tempuint32;
					if (swscanf_s(value, L"0x%x", &tempuint32) == 1) {
						configContext.contentDetails->contentPackageType = tempuint32;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"LicenseBits")) {
					uint32_t tempuint32;
					if (swscanf_s(value, L"0x%x", &tempuint32) == 1) {
						configContext.contentDetails->licenseBits = tempuint32;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"BaseVersion")) {
					uint32_t tempuint32;
					if (swscanf_s(value, L"0x%x", &tempuint32) == 1) {
						configContext.contentDetails->baseVersion = tempuint32;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"UpdateVersion")) {
					uint32_t tempuint32;
					if (swscanf_s(value, L"0x%x", &tempuint32) == 1) {
						configContext.contentDetails->updateVersion = tempuint32;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"ThumbnailImage")) {
					if (configContext.contentDetails->thumbnailImage) {
						delete[] configContext.contentDetails->thumbnailImage;
					}
					configContext.contentDetails->thumbnailImage = CloneString(value);
				}
				else if (SettingNameMatches(L"OfferingID")) {
					uint64_t tempuint64;
					if (swscanf_s(value, L"0x%I64x", &tempuint64) == 1) {
						configContext.contentDetails->offeringId = tempuint64;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"AdminFriendlyName")) {
					if (configContext.contentDetails->adminFriendlyName) {
						delete[] configContext.contentDetails->adminFriendlyName;
					}
					configContext.contentDetails->adminFriendlyName = XllnContentUnquote_(value);
				}
				else if (SettingNameMatches(L"TitleName")) {
					if (configContext.contentDetails->titleName) {
						delete[] configContext.contentDetails->titleName;
					}
					configContext.contentDetails->titleName = XllnContentUnquote_(value);
				}
				else if (SettingNameMatches(L"PurchaseOnceOnly")) {
					if (wcscmp(value, L"Yes") == 0) {
						configContext.contentDetails->purchaseOnceOnly = true;
					}
					else if (wcscmp(value, L"No") == 0) {
						configContext.contentDetails->purchaseOnceOnly = false;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"ContentFlags")) {
					uint8_t tempuint8;
					if (swscanf_s(value, L"0x%hhx", &tempuint8) == 1) {
						configContext.contentDetails->contentFlags = tempuint8;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"ContentID")) {
					if (!HexStrToByteArray(configContext.contentDetails->contentId, XLIVE_CONTENT_ID_SIZE, value)) {
						memset(configContext.contentDetails->contentId, 0, XLIVE_CONTENT_ID_SIZE);
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"Platform")) {
					uint8_t tempuint8;
					if (swscanf_s(value, L"0x%hhx", &tempuint8) == 1) {
						configContext.contentDetails->platform = tempuint8;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"BitFlags")) {
					uint32_t tempuint32;
					if (swscanf_s(value, L"0x%x", &tempuint32) == 1) {
						configContext.contentDetails->bitFlags = tempuint32;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"ActivationDate")) {
					if (!ParseDateString(value, &configContext.contentDetails->activationDate)) {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"DefaultDisplayName")) {
					if (configContext.contentDetails->defaultDisplayName) {
						delete[] configContext.contentDetails->defaultDisplayName;
					}
					configContext.contentDetails->defaultDisplayName = XllnContentUnquote_(value);
				}
				else if (_wcsnicmp(L"OfferRegions.", settingName, 13) == 0) {
					uint32_t iRegion;
					if (swscanf_s(&settingName[13], L"%u", &iRegion) == 1) {
						// TODO OfferRegions
						//iRegion;
						//value;
					}
					else {
						unrecognised = true;
					}
				}
				else if (SettingNameMatches(L"OfferingType")) {
					if (wcscmp(value, L"Content") == 0) {
						configContext.contentDetails->offeringType = XMARKETPLACE_OFFERING_TYPE::XMARKETPLACE_OFFERING_TYPE_CONTENT;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"EndDate")) {
					if (!ParseDateString(value, &configContext.contentDetails->endDate)) {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"DisplayInDash")) {
					if (wcscmp(value, L"Yes") == 0) {
						configContext.contentDetails->displayInDash = true;
					}
					else if (wcscmp(value, L"No") == 0) {
						configContext.contentDetails->displayInDash = false;
					}
					else {
						incorrect = true;
					}
				}
				else if (SettingNameMatches(L"Version")) {
					uint32_t tempuint32;
					if (swscanf_s(value, L"0x%x", &tempuint32) == 1) {
						configContext.contentDetails->version = tempuint32;
					}
					else {
						incorrect = true;
					}
				}
				else if (_wcsnicmp(L"DashDisplayName.", settingName, 16) == 0) {
					uint32_t iRegion;
					if (swscanf_s(&settingName[16], L"%u", &iRegion) == 1) {
						XllnContentSetLocalisedText_(configContext.contentDetails->dashDisplayName, (uint8_t)iRegion, value);
					}
					else {
						unrecognised = true;
					}
				}
				else if (_wcsnicmp(L"DashDescription.", settingName, 16) == 0) {
					uint32_t iRegion;
					if (swscanf_s(&settingName[16], L"%u", &iRegion) == 1) {
						XllnContentSetLocalisedText_(configContext.contentDetails->dashDescription, (uint8_t)iRegion, value);
					}
					else {
						unrecognised = true;
					}
				}
				else {
					unrecognised = true;
				}
				
				if (!unrecognised && !duplicated && !incorrect) {
					configContext.readSettings.push_back(CloneString(settingName));
					
					for (auto itrMissingSetting = configContext.missingSettings.begin(); itrMissingSetting != configContext.missingSettings.end(); itrMissingSetting++) {
						if (wcscmp(*itrMissingSetting, settingName) == 0) {
							configContext.missingSettings.erase(itrMissingSetting);
							break;
						}
					}
				}
			}
#undef SettingNameMatches
			delete[] settingName;
			settingName = 0;
		}
	}
	
	if (unrecognised || duplicated || incorrect) {
		configContext.badConfigEntries.push_back(CloneString(fileLine));
	}
	return 0;
}

// A .LIC file is an XContent package header cut after its content id.
wchar_t* XllnContentReadDisplayName(const wchar_t* pack_path)
{
	wchar_t* contentPathname = FormNewString(L"%s/content.xbx", pack_path);
	FILE* fileContent = 0;
	_wfopen_s(&fileContent, contentPathname, L"rb");
	delete[] contentPathname;
	contentPathname = 0;
	if (!fileContent) {
		return 0;
	}
	INTERPRET_XCONTENT_CONTEXT interpretationContext;
	interpretationContext.contentDetails = new CONTENT_DETAILS;
	ReadIniFile(fileContent, true, 0, 0, interpretDlcContent, (void*)&interpretationContext);
	fclose(fileContent);
	fileContent = 0;
	return interpretationContext.contentDetails->defaultDisplayName ? CloneString(interpretationContext.contentDetails->defaultDisplayName) : 0;
}

static void XllnContentReadLicense_(CONTENT_DETAILS* content_details)
{
	const size_t licenseHeaderSize = 0x344;
	const size_t licenseEntriesOffset = 0x22C;
	const size_t licenseEntryCount = 16;
	const size_t licenseContentIdOffset = 0x32C;

	wchar_t* searchPattern = FormNewString(L"%s/*.LIC", content_details->xcontentPath);
	WIN32_FIND_DATAW findData;
	HANDLE hFind = FindFirstFileW(searchPattern, &findData);
	delete[] searchPattern;
	searchPattern = 0;
	if (hFind == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			continue;
		}
		wchar_t* licensePath = FormNewString(L"%s/%s", content_details->xcontentPath, findData.cFileName);
		uint8_t header[licenseHeaderSize];
		size_t readBytes = 0;
		FILE* fpLicense = 0;
		if (!_wfopen_s(&fpLicense, licensePath, L"rb") && fpLicense) {
			readBytes = fread(header, 1, sizeof(header), fpLicense);
			fclose(fpLicense);
			fpLicense = 0;
		}
		// 'LIVE', 'PIRS' or 'CON ' as the file spells them.
		uint32_t magic = readBytes == sizeof(header) ? _byteswap_ulong(*(const uint32_t*)header) : 0;
		if (magic != 0x4C495645 && magic != 0x50495253 && magic != 0x434F4E20) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN, "%s \"%ls\" is not an XContent license (read %zu byte(s), magic 0x%08x).", __func__, licensePath, readBytes, magic);
			delete[] licensePath;
			licensePath = 0;
			continue;
		}
		if (memcmp(header + licenseContentIdOffset, content_details->contentId, XLIVE_CONTENT_ID_SIZE) != 0) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN, "%s \"%ls\" is a license for another content id.", __func__, licensePath);
			delete[] licensePath;
			licensePath = 0;
			continue;
		}
		uint64_t licensee = 0;
		uint32_t licenseBits = 0;
		uint32_t licenseFlags = 0;
		for (size_t iEntry = 0; iEntry < licenseEntryCount; iEntry++) {
			const uint8_t* entry = header + licenseEntriesOffset + iEntry * 16;
			licensee = _byteswap_uint64(*(const uint64_t*)entry);
			if (licensee) {
				licenseBits = _byteswap_ulong(*(const uint32_t*)(entry + 8));
				licenseFlags = _byteswap_ulong(*(const uint32_t*)(entry + 12));
				break;
			}
		}
		if (!licensee) {
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN, "%s \"%ls\" names no licensee.", __func__, licensePath);
			delete[] licensePath;
			licensePath = 0;
			continue;
		}
		content_details->licensePath = licensePath;
		content_details->licenseeXuid = licensee;
		content_details->licenseMask = licenseBits;
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s \"%ls\" licensee 0x%016I64x mask 0x%08x flags 0x%08x.", __func__, licensePath, licensee, licenseBits, licenseFlags);
		break;
	} while (FindNextFileW(hFind, &findData));
	FindClose(hFind);
}

// #5360
uint32_t WINAPI XLiveContentCreateEnumerator(size_t item_count, XLIVE_CONTENT_RETRIEVAL_INFO* content_retrieval_info, size_t* result_buffer_size, HANDLE* enumerator_handle)
{
	TRACE_FX();
	if (!item_count) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s item_count is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (item_count > XMARKETPLACE_ASSET_MAX_ENUM_SIZE) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s item_count (%zu) is greater than XMARKETPLACE_ASSET_MAX_ENUM_SIZE (%u).", __func__, item_count, XMARKETPLACE_ASSET_MAX_ENUM_SIZE);
		return ERROR_INVALID_PARAMETER;
	}
	if (!result_buffer_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_buffer_size is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (!enumerator_handle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s enumerator_handle is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (!content_retrieval_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_retrieval_info is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (content_retrieval_info->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_retrieval_info->dwContentAPIVersion (%u) != XLIVE_CONTENT_API_VERSION (%u).", __func__, content_retrieval_info->dwContentAPIVersion, XLIVE_CONTENT_API_VERSION);
		return ERROR_INVALID_PARAMETER;
	}
	if (content_retrieval_info->dwUserIndex >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, content_retrieval_info->dwUserIndex);
		return ERROR_NO_SUCH_USER;
	}
	if (xlive_local_users[content_retrieval_info->dwUserIndex].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, content_retrieval_info->dwUserIndex);
		return ERROR_NOT_LOGGED_ON;
	}
	if ((content_retrieval_info->dwRetrievalMask & XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_USERS) && (content_retrieval_info->dwRetrievalMask & XLIVE_CONTENT_FLAG_RETRIEVE_BY_XUID)) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s content_retrieval_info->dwRetrievalMask (0x%x) must not include both XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_USERS (0x%x) and XLIVE_CONTENT_FLAG_RETRIEVE_BY_XUID (0x%x) simultaneously."
			, __func__
			, content_retrieval_info->dwRetrievalMask
			, XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_USERS
			, XLIVE_CONTENT_FLAG_RETRIEVE_BY_XUID
		);
		return ERROR_INVALID_PARAMETER;
	}
	if (!(content_retrieval_info->dwRetrievalMask & XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES) && content_retrieval_info->dwContentType != XCONTENTTYPE_MARKETPLACE) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s content_retrieval_info->dwRetrievalMask (0x%x) must include XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES (0x%x) or content_retrieval_info->dwContentType (0x%x) must be XCONTENTTYPE_MARKETPLACE (0x%x)."
			, __func__
			, content_retrieval_info->dwRetrievalMask
			, XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES
			, content_retrieval_info->dwContentType
			, XCONTENTTYPE_MARKETPLACE
		);
		return ERROR_INVALID_PARAMETER;
	}
	
	*result_buffer_size = item_count * sizeof(XLIVE_CONTENT_INFO);
	XCONTENT_ENUMERATION_DETAILS* xcontentEnumerationDetails = new XCONTENT_ENUMERATION_DETAILS;
	xcontentEnumerationDetails->enumeratorHandle = CreateMutex(NULL, NULL, NULL);
	XllnHandleReused(xcontentEnumerationDetails->enumeratorHandle);
	xcontentEnumerationDetails->titleId = content_retrieval_info->dwTitleID;
	if (!xcontentEnumerationDetails->titleId) {
		xcontentEnumerationDetails->titleId = xlive_title_id;
	}
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s mask 0x%08x title 0x%08x type 0x%08x xuid 0x%016I64x.", __func__, content_retrieval_info->dwRetrievalMask, xcontentEnumerationDetails->titleId, content_retrieval_info->dwContentType, content_retrieval_info->xuidUser);
	
	uint32_t resultCode = ERROR_FUNCTION_FAILED;
	size_t numberOfResults = 0;
	
	wchar_t* pathDlc = 0;
	{
		wchar_t* pathConfig = PathFromFilename(xlln_file_config_path);
		
		wchar_t fullPath[MAX_PATH];
		DWORD result = GetFullPathNameW(pathConfig, MAX_PATH, fullPath, NULL);
		if (result != 0 && result < MAX_PATH) {
			pathDlc = FormNewString(L"%sdlc/", fullPath);
		}
		
		delete[] pathConfig;
		pathConfig = 0;
	}
	if (!pathDlc) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
			, "%s failed to get DLC base path."
			, __func__
		);
		return ERROR_FUNCTION_FAILED;
	}
	// TODO add another FindFirstFileW iteration on all title folders: (content_retrieval_info->dwRetrievalMask & XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_TITLES)
	wchar_t* pathDlcTitle = FormNewString(L"%s%08X/", pathDlc, xcontentEnumerationDetails->titleId);
	uint32_t errorMkdir = EnsureDirectoryExists(pathDlcTitle);
	if (errorMkdir) {
		XLLN_DEBUG_LOG_ECODE(errorMkdir, XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_WARN, "%s EnsureDirectoryExists(...) error on path \"%ls\".", __func__, pathDlcTitle);
		resultCode = errorMkdir;
	}
	else {
		wchar_t* pathDlcTitleSearch = FormNewString(L"%s*", pathDlcTitle);
		WIN32_FIND_DATAW data;
		HANDLE hFind = FindFirstFileW(pathDlcTitleSearch, &data);
		delete[] pathDlcTitleSearch;
		pathDlcTitleSearch = 0;
		if (hFind != INVALID_HANDLE_VALUE) {
			do {
				if (
					!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					|| wcscmp(data.cFileName, L".") == 0
					|| wcscmp(data.cFileName, L"..") == 0
				) {
					continue;
				}
				
				wchar_t* dlcContentPathname = FormNewString(L"%s%s%s", pathDlcTitle, data.cFileName, L"/content.xbx");
				
				FILE* fileDlcContent = 0;
				errno_t errorFopen = _wfopen_s(&fileDlcContent, dlcContentPathname, L"rb");
				if (!fileDlcContent) {
					if (errorFopen != ENOENT) {
						XLLN_DEBUG_LOG_ECODE(errorFopen, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN, "%s XContent file \"%ls\" read error:", __func__, dlcContentPathname);
					}
					
					delete[] dlcContentPathname;
					dlcContentPathname = 0;
					
					continue;
				}
				
				delete[] dlcContentPathname;
				dlcContentPathname = 0;
				
				INTERPRET_XCONTENT_CONTEXT interpretationContext;
				interpretationContext.missingSettings.push_back(L"TitleID");
				interpretationContext.missingSettings.push_back(L"ContentPackageType");
				interpretationContext.missingSettings.push_back(L"OfferingID");
				interpretationContext.missingSettings.push_back(L"ContentID");
				interpretationContext.missingSettings.push_back(L"DefaultDisplayName");
				interpretationContext.missingSettings.push_back(L"OfferingType");
				
				interpretationContext.contentDetails = new CONTENT_DETAILS;
				interpretationContext.contentDetails->xcontentPath = FormNewString(L"%s%s", pathDlcTitle, data.cFileName);
				
				ReadIniFile(fileDlcContent, true, 0, 0, interpretDlcContent, (void*)&interpretationContext);
				fclose(fileDlcContent);
				fileDlcContent = 0;
				
				if (interpretationContext.missingSettings.size()) {
					for (auto itrMissingSetting : interpretationContext.missingSettings) {
						XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
							, "%s XContent file is missing the setting \"%ls\" in \"%ls/content.xbx\"."
							, __func__
							, itrMissingSetting
							, interpretationContext.contentDetails->xcontentPath
						);
					}
					continue;
				}
				
				XllnContentReadLicense_(interpretationContext.contentDetails);
				wchar_t* contentFolder = FormNewString(L"%s/Content", interpretationContext.contentDetails->xcontentPath);
				uint32_t contentFolderAttributes = GetFileAttributesW(contentFolder);
				interpretationContext.contentDetails->hasPayload = contentFolderAttributes != INVALID_FILE_ATTRIBUTES && (contentFolderAttributes & FILE_ATTRIBUTE_DIRECTORY);
				delete[] contentFolder;
				contentFolder = 0;
				std::string contentIdKey((const char*)interpretationContext.contentDetails->contentId, XLIVE_CONTENT_ID_SIZE);
				auto itrPreviousContentOfferingId = xcontentEnumerationDetails->contentDetails.find(contentIdKey);
				if (itrPreviousContentOfferingId != xcontentEnumerationDetails->contentDetails.end()) {
					CONTENT_DETAILS* previousContentDetails = itrPreviousContentOfferingId->second;
					XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR
						, "%s XContent file ContentID collision between \"%ls/content.xbx\" and the following which will be ignored \"%ls/content.xbx\"."
						, __func__
						, previousContentDetails->xcontentPath
						, interpretationContext.contentDetails->xcontentPath
					);
				}
				else if (!(content_retrieval_info->dwRetrievalMask & XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_TITLES) && interpretationContext.contentDetails->titleId != xcontentEnumerationDetails->titleId) {
					XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_INFO
						, "%s Ignored XContent as the required TitleID value (%X) does not match the value (%X) from \"%ls/content.xbx\"."
						, __func__
						, xcontentEnumerationDetails->titleId
						, interpretationContext.contentDetails->titleId
						, interpretationContext.contentDetails->xcontentPath
					);
				}
				else if (!(content_retrieval_info->dwRetrievalMask & XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES) && interpretationContext.contentDetails->contentPackageType != content_retrieval_info->dwContentType) {
					XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_INFO
						, "%s Ignored XContent as the required ContentPackageType value (%X) does not match the value (%X) from \"%ls/content.xbx\"."
						, __func__
						, content_retrieval_info->dwContentType
						, interpretationContext.contentDetails->contentPackageType
						, interpretationContext.contentDetails->xcontentPath
					);
				}
				else {
					XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG
						, "%s Found XContent with OfferingID (0x%I64x) at \"%ls/content.xbx\"."
						, __func__
						, interpretationContext.contentDetails->offeringId
						, interpretationContext.contentDetails->xcontentPath
					);
					
					xcontentEnumerationDetails->contentDetails[contentIdKey] = interpretationContext.contentDetails;
					interpretationContext.contentDetails = 0;
				}
			} while (FindNextFileW(hFind, &data));
			FindClose(hFind);
		}
	}
	
	delete[] pathDlcTitle;
	pathDlcTitle = 0;
	
	delete[] pathDlc;
	pathDlc = 0;
	
	*enumerator_handle = xcontentEnumerationDetails->enumeratorHandle;
	
	EnterCriticalSection(&xlive_critsec_xcontent);
	xlive_xcontent_enumerators[xcontentEnumerationDetails->enumeratorHandle] = xcontentEnumerationDetails;
	LeaveCriticalSection(&xlive_critsec_xcontent);
	
	return ERROR_SUCCESS;
}

// #5361
HRESULT WINAPI XLiveContentRetrieveOffersByDate(uint32_t user_index, uint32_t offer_info_version, SYSTEMTIME* offers_start_date, XLIVE_OFFER_INFO* result_offer_infos, size_t* result_offer_info_count, XOVERLAPPED* xoverlapped)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (offer_info_version > XLIVE_OFFER_INFO_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (offer_info_version > XLIVE_OFFER_INFO_VERSION) (%u > %u).", __func__, offer_info_version, XLIVE_OFFER_INFO_VERSION);
		return E_INVALIDARG;
	}
	if (!result_offer_infos) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_offer_infos is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (!result_offer_info_count) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_offer_info_count is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (*result_offer_info_count != XLIVE_MAX_RETURNED_OFFER_INFO) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (*result_offer_info_count != XLIVE_MAX_RETURNED_OFFER_INFO) (%zu != %u).", __func__, *result_offer_info_count, XLIVE_MAX_RETURNED_OFFER_INFO);
		return E_INVALIDARG;
	}
	
	uint32_t result = HRESULT_FROM_WIN32(ERROR_FUNCTION_FAILED);
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	
	if (xoverlapped) {
		//asynchronous
		
		xoverlapped->InternalLow = result;
		xoverlapped->InternalHigh = result;
		xoverlapped->dwExtendedError = result;
		
		Check_Overlapped(xoverlapped);
		
		return HRESULT_FROM_WIN32(ERROR_IO_PENDING);
	}
	
	//synchronous
	return result;
}

// #5362
BOOL WINAPI XLiveMarketplaceDoesContentIdMatch(const uint8_t* content_id, const XLIVE_CONTENT_INFO* content_data)
{
	TRACE_FX();
	if (!content_id) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_id is NULL.", __func__);
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!content_data) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_data is NULL.", __func__);
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	
	bool result = memcmp(content_id, &(content_data->abContentID), XLIVE_CONTENT_ID_SIZE) == 0;
	return result ? TRUE : FALSE;
}

// #5363
HRESULT WINAPI XLiveContentGetLicensePath(uint32_t user_index, XLIVE_CONTENT_INFO* content_info, wchar_t* result_license_pathname, size_t* result_license_pathname_size)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return E_INVALIDARG;
	}
	if (!content_info) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (content_info->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s content_info->dwContentAPIVersion (%u) != XLIVE_CONTENT_API_VERSION (%u).", __func__, content_info->dwContentAPIVersion, XLIVE_CONTENT_API_VERSION);
		return E_INVALIDARG;
	}
	if (!result_license_pathname_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_license_pathname_size is NULL.", __func__);
		return E_INVALIDARG;
	}
	if (!*result_license_pathname_size && result_license_pathname) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_license_pathname is not NULL when *result_license_pathname_size is 0.", __func__);
		return E_INVALIDARG;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	XLIVE_CONTENT_RETRIEVAL_INFO contentRetrievalInfo;
	contentRetrievalInfo.dwContentAPIVersion = XLIVE_CONTENT_API_VERSION;
	contentRetrievalInfo.dwRetrievalMask = XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_TITLES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_CONTENT_TYPES | XLIVE_CONTENT_FLAG_RETRIEVE_FOR_ALL_USERS;
	contentRetrievalInfo.dwUserIndex = 0;
	contentRetrievalInfo.xuidUser = INVALID_XUID;
	contentRetrievalInfo.dwTitleID = 0;
	contentRetrievalInfo.dwContentType = 0;
	memset(contentRetrievalInfo.abContentID, 0, sizeof(contentRetrievalInfo.abContentID));
	size_t bufferSizeRequired = 0;
	HANDLE enumeratorHandle = INVALID_HANDLE_VALUE;
	uint32_t resultCreateEnumerator = XLiveContentCreateEnumerator(1, &contentRetrievalInfo, &bufferSizeRequired, &enumeratorHandle);
	if (resultCreateEnumerator != ERROR_SUCCESS) {
		XLLN_DEBUG_LOG_ECODE(resultCreateEnumerator, XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s Failed to create XContent enumerator:", __func__);
		return HRESULT_FROM_WIN32(resultCreateEnumerator);
	}

	HRESULT result = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);

	EnterCriticalSection(&xlive_critsec_xcontent);
	auto const &itrXContentEnumerator = xlive_xcontent_enumerators.find(enumeratorHandle);
	if (itrXContentEnumerator != xlive_xcontent_enumerators.end()) {
		for (auto itrContentDetail : itrXContentEnumerator->second->contentDetails) {
			CONTENT_DETAILS* contentDetails = itrContentDetail.second;
			if (contentDetails->titleId != content_info->dwTitleID
				|| contentDetails->contentPackageType != content_info->dwContentType
				|| memcmp(contentDetails->contentId, content_info->abContentID, sizeof(contentDetails->contentId)) != 0
			) {
				continue;
			}
			if (contentDetails->licensePath) {
				size_t licensePathLength = wcslen(contentDetails->licensePath) + 1;
				if (*result_license_pathname_size < licensePathLength) {
					result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
				}
				else {
					memcpy(result_license_pathname, contentDetails->licensePath, licensePathLength * sizeof(wchar_t));
					for (wchar_t* separator = result_license_pathname; *separator; separator++) {
						if (*separator == L'/') {
							*separator = L'\\';
						}
					}
					result = S_OK;
				}
				*result_license_pathname_size = licensePathLength;
			}
			break;
		}
	}
	LeaveCriticalSection(&xlive_critsec_xcontent);

	if (!XCloseHandle(enumeratorHandle)) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s Failed to close XContent enumerator handle.", __func__);
	}

	return result;
}

// #5367
uint32_t WINAPI XContentGetMarketplaceCounts(uint32_t user_index, uint32_t content_categories, size_t result_content_offers_size, XOFFERING_CONTENTAVAILABLE_RESULT* result_content_offers, XOVERLAPPED* xoverlapped)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return ERROR_INVALID_PARAMETER;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return ERROR_INVALID_PARAMETER;
	}
	if (!result_content_offers_size) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_content_offers_size is 0.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (result_content_offers_size < sizeof(XOFFERING_CONTENTAVAILABLE_RESULT)) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_content_offers_size (0x%zx) < sizeof(XOFFERING_CONTENTAVAILABLE_RESULT) (0x%zx).", __func__, result_content_offers_size, sizeof(XOFFERING_CONTENTAVAILABLE_RESULT));
		return ERROR_INSUFFICIENT_BUFFER;
	}
	if (!result_content_offers) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_content_offers is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	
	std::vector<XLLN_OFFER_DETAILS> offers;
	XllnContentCollectOffers(offers);
	result_content_offers->dwNewOffers = 0;
	result_content_offers->dwTotalOffers = offers.size();
	uint32_t result = ERROR_SUCCESS;
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s categories 0x%08x -> total %u new %u.", __func__, content_categories, result_content_offers->dwTotalOffers, result_content_offers->dwNewOffers);

	if (xoverlapped) {
		//asynchronous
		
		xoverlapped->InternalLow = result;
		xoverlapped->InternalHigh = result;
		xoverlapped->dwExtendedError = result;
		
		Check_Overlapped(xoverlapped);
		
		return ERROR_IO_PENDING;
	}
	
	//synchronous
	return result;
}
