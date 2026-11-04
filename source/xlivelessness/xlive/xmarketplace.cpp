#include <winsock2.h>
#include "xdefs.hpp"
#include "xmarketplace.hpp"
#include "xlive.hpp"
#include "../xlln/debug-log.hpp"
#include "../xlln/xlln.hpp"

CRITICAL_SECTION xlive_critsec_xmarketplace;
std::map<HANDLE, MARKETPLACE_ENUMERATION_DETAILS*> xlive_xmarketplace_enumerators;

// #5370
uint32_t WINAPI XMarketplaceConsumeAssets(uint32_t user_index, size_t consumable_asset_count, const XMARKETPLACE_ASSET* consumable_assets, XOVERLAPPED* xoverlapped)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return ERROR_NO_SUCH_USER;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return ERROR_NOT_LOGGED_ON;
	}
	if (!consumable_asset_count) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s consumable_asset_count is 0.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (!consumable_assets) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s consumable_assets is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	
	if (xoverlapped) {
		//asynchronous
		
		xoverlapped->InternalLow = ERROR_SUCCESS;
		xoverlapped->InternalHigh = ERROR_SUCCESS;
		xoverlapped->dwExtendedError = ERROR_SUCCESS;
		
		Check_Overlapped(xoverlapped);
		
		return ERROR_IO_PENDING;
	}
	
	//synchronous
	return ERROR_SUCCESS;
}

// #5371
uint32_t WINAPI XMarketplaceCreateAssetEnumerator(uint32_t user_index, size_t asset_count, size_t* result_buffer_size, HANDLE* enumerator_handle)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return ERROR_NO_SUCH_USER;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return ERROR_NOT_LOGGED_ON;
	}
	if (!asset_count) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s asset_count is 0.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (asset_count > 0x64) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s asset_count (%zu) is greater than 0x64.", __func__, asset_count);
		return ERROR_INVALID_PARAMETER;
	}
	if (!enumerator_handle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s enumerator_handle is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	
	if (result_buffer_size) {
		// i think 2524 is that including the maximum size of each of the pointers in it.
		//*result_buffer_size = 2524 * asset_count;
		*result_buffer_size = (sizeof(XMARKETPLACE_CONTENTOFFER_INFO*) + sizeof(XMARKETPLACE_CONTENTOFFER_INFO) + 2416) * asset_count;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s user %u asset_count %zu; no assets are served yet.", __func__, user_index, asset_count);
	*enumerator_handle = CreateMutex(NULL, NULL, NULL);
	XllnHandleReused(*enumerator_handle);
	EnterCriticalSection(&xlive_critsec_xmarketplace);
	xlive_xmarketplace_enumerators[*enumerator_handle];
	LeaveCriticalSection(&xlive_critsec_xmarketplace);
	
	return ERROR_SUCCESS;
}

// #5372
uint32_t WINAPI XMarketplaceCreateOfferEnumerator(uint32_t user_index, uint32_t offer_type, uint32_t content_categories, size_t offer_count, size_t* result_buffer_size, HANDLE* enumerator_handle)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return ERROR_NO_SUCH_USER;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return ERROR_NOT_LOGGED_ON;
	}
	if (!offer_type) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s offer_type is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (!offer_count) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s offer_count is 0.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (offer_count > 0x64) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s offer_count (%zu) is greater than 0x64.", __func__, offer_count);
		return ERROR_INVALID_PARAMETER;
	}
	if (!enumerator_handle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s enumerator_handle is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	
	if (result_buffer_size) {
		// i think 2524 is that including the maximum size of each of the pointers in it.
		//*result_buffer_size = 2524 * offer_count;
		*result_buffer_size = (sizeof(XMARKETPLACE_CONTENTOFFER_INFO*) + sizeof(XMARKETPLACE_CONTENTOFFER_INFO) + 2416) * offer_count;
	}
	
	MARKETPLACE_ENUMERATION_DETAILS* marketplaceEnumerationDetails = new MARKETPLACE_ENUMERATION_DETAILS;
	marketplaceEnumerationDetails->userIndex = user_index;
	marketplaceEnumerationDetails->batchSize = offer_count;
	// A pack carries no category, so the category filter selects nothing out.
	if (offer_type & XMARKETPLACE_OFFERING_TYPE_CONTENT) {
		XllnContentCollectOffers(marketplaceEnumerationDetails->offers);
	}
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s user %u offer_type 0x%08x categories 0x%08x batch %zu -> %zu offer(s).", __func__, user_index, offer_type, content_categories, offer_count, marketplaceEnumerationDetails->offers.size());
	*enumerator_handle = CreateMutex(NULL, NULL, NULL);
	XllnHandleReused(*enumerator_handle);
	EnterCriticalSection(&xlive_critsec_xmarketplace);
	xlive_xmarketplace_enumerators[*enumerator_handle] = marketplaceEnumerationDetails;
	LeaveCriticalSection(&xlive_critsec_xmarketplace);
	
	return ERROR_SUCCESS;
}

// #5374
uint32_t WINAPI XMarketplaceGetDownloadStatus(uint32_t user_index, uint64_t offer_id, uint32_t* result_download_status)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return ERROR_NO_SUCH_USER;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return ERROR_NOT_LOGGED_ON;
	}
	if (!offer_id) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s offer_id is 0.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s TODO.", __func__);
	
	return ERROR_NOT_FOUND;
	return ERROR_IO_PENDING;
	return ERROR_DISK_FULL;
}

// #5375
void WINAPI XMarketplaceGetImageUrl(uint32_t title_id, uint64_t offer_id, size_t result_image_url_size, wchar_t* result_image_url)
{
	TRACE_FX();
	if (result_image_url_size < XMARKETPLACE_IMAGE_URL_MINIMUM_WCHARCOUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (result_image_url_size < XMARKETPLACE_IMAGE_URL_MINIMUM_WCHARCOUNT) (0x%zx < 0x%x).", __func__, result_image_url_size, XMARKETPLACE_IMAGE_URL_MINIMUM_WCHARCOUNT);
		return;
	}
	if (!result_image_url) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s result_image_url is NULL.", __func__);
		return;
	}
	swprintf(result_image_url, result_image_url_size, L"//global/t:%08x/marketplace/0/%016I64x", title_id, offer_id);
}

// #5376
uint32_t WINAPI XMarketplaceCreateOfferEnumeratorByOffering(uint32_t user_index, size_t offer_count, const uint64_t* offer_ids, uint16_t offer_id_count, size_t* result_buffer_size, HANDLE* enumerator_handle)
{
	TRACE_FX();
	if (user_index >= XLIVE_LOCAL_USER_COUNT) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User 0x%08x does not exist.", __func__, user_index);
		return ERROR_NO_SUCH_USER;
	}
	if (xlive_local_users[user_index].signin_state == eXUserSigninState_NotSignedIn) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s User %u is not signed in.", __func__, user_index);
		return ERROR_NOT_LOGGED_ON;
	}
	if (!offer_count) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s offer_count is 0.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (offer_count > 0x64) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s offer_count (%zu) is greater than 0x64.", __func__, offer_count);
		return ERROR_INVALID_PARAMETER;
	}
	if (!offer_ids) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s offer_ids is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (!offer_id_count) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s offer_id_count is 0.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	if (offer_id_count > XMARKETPLACE_MAX_OFFERIDS) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s (offer_id_count > XMARKETPLACE_MAX_OFFERIDS) (%zu > %u).", __func__, offer_id_count, XMARKETPLACE_MAX_OFFERIDS);
		return ERROR_INVALID_PARAMETER;
	}
	if (!enumerator_handle) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s enumerator_handle is NULL.", __func__);
		return ERROR_INVALID_PARAMETER;
	}
	
	if (result_buffer_size) {
		// i think 2524 is that including the maximum size of each of the pointers in it.
		//*result_buffer_size = 2524 * offer_count;
		*result_buffer_size = (sizeof(XMARKETPLACE_CONTENTOFFER_INFO*) + sizeof(XMARKETPLACE_CONTENTOFFER_INFO) + 2416) * offer_count;
	}
	
	for (uint16_t iOfferId = 0; iOfferId < offer_id_count; iOfferId++) {
		for (uint16_t iOther = 0; iOther < iOfferId; iOther++) {
			if (offer_ids[iOther] == offer_ids[iOfferId]) {
				XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_ERROR, "%s offer_id 0x%016I64x is listed twice.", __func__, offer_ids[iOfferId]);
				return ERROR_FUNCTION_FAILED;
			}
		}
	}
	std::vector<XLLN_OFFER_DETAILS> offers;
	XllnContentCollectOffers(offers);
	MARKETPLACE_ENUMERATION_DETAILS* marketplaceEnumerationDetails = new MARKETPLACE_ENUMERATION_DETAILS;
	marketplaceEnumerationDetails->userIndex = user_index;
	marketplaceEnumerationDetails->batchSize = offer_count;
	for (uint16_t iOfferId = 0; iOfferId < offer_id_count; iOfferId++) {
		bool served = false;
		for (auto const &offer : offers) {
			if (offer.offeringId == offer_ids[iOfferId]) {
				marketplaceEnumerationDetails->offers.push_back(offer);
				served = true;
				break;
			}
		}
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s offer_id[%hu] 0x%016I64x %s.", __func__, iOfferId, offer_ids[iOfferId], served ? "served" : "unknown");
	}
	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVE | XLLN_LOG_LEVEL_DEBUG, "%s user %u batch %zu -> %zu of %hu offer(s).", __func__, user_index, offer_count, marketplaceEnumerationDetails->offers.size(), offer_id_count);
	*enumerator_handle = CreateMutex(NULL, NULL, NULL);
	XllnHandleReused(*enumerator_handle);
	EnterCriticalSection(&xlive_critsec_xmarketplace);
	xlive_xmarketplace_enumerators[*enumerator_handle] = marketplaceEnumerationDetails;
	LeaveCriticalSection(&xlive_critsec_xmarketplace);
	
	return ERROR_SUCCESS;
}
