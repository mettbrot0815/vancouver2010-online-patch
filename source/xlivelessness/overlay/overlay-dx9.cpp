// Derived from Dear ImGui's imgui_impl_dx9.cpp (MIT licensed, (c) Ocornut and
// contributors), restructured for XLLN: managed-pool resources on plain D3D9 so
// the overlay never blocks the title's device Reset, no GetTransform on pure
// devices, and frame gating through TestCooperativeLevel.
#include <winsock2.h>
#include <Windows.h>
#include "overlay-dx9.hpp"
#include "../xlln/debug-log.hpp"
#include <imgui.h>
#include <stdint.h>

typedef struct _OVERLAY_DX9_VERTEX {
	float position[3];
	D3DCOLOR color;
	float uv[2];
} OVERLAY_DX9_VERTEX;
#define OVERLAY_DX9_VERTEX_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

// ImGui packs colors as ABGR (RGBA little-endian); D3D9 fixed function wants ARGB.
#define OVERLAY_DX9_COLOR_TO_ARGB(imgui_color) \
	(((imgui_color) & 0xFF00FF00) | (((imgui_color) & 0x00FF0000) >> 16) | (((imgui_color) & 0x000000FF) << 16))

static IDirect3DDevice9* overlay_d3d9_device = 0;
static bool overlay_d3d9_is_ex = false;
static bool overlay_d3d9_is_pure_device = false;
// Reported in the attach log only. It does not decide whether the overlay draws, because it makes
// single calls safe and says nothing about a sequence of them.
static bool overlay_d3d9_is_multithreaded = false;
static D3DPOOL overlay_d3d9_resource_pool = D3DPOOL_MANAGED;
static IDirect3DVertexBuffer9* overlay_d3d9_vertex_buffer = 0;
static IDirect3DIndexBuffer9* overlay_d3d9_index_buffer = 0;
static IDirect3DTexture9* overlay_d3d9_font_texture = 0;
static uint32_t overlay_d3d9_vertex_buffer_size = 5000;
static uint32_t overlay_d3d9_index_buffer_size = 10000;

bool OverlayDX9Init(IDirect3DDevice9* d3d_device, bool is_d3d9ex)
{
	if (!d3d_device) {
		return false;
	}
	OverlayDX9Shutdown();

	overlay_d3d9_device = d3d_device;
	overlay_d3d9_device->AddRef();
	overlay_d3d9_is_ex = is_d3d9ex;
	// Managed pool does not exist on D3D9Ex devices, but those also do not lose
	// default-pool resources across Reset, so each mode gets the pool that keeps
	// the title's Reset call safe.
	overlay_d3d9_resource_pool = is_d3d9ex ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED;

	DWORD behaviourFlags = 0;
	D3DDEVICE_CREATION_PARAMETERS creationParameters;
	if (SUCCEEDED(overlay_d3d9_device->GetCreationParameters(&creationParameters))) {
		behaviourFlags = creationParameters.BehaviorFlags;
	}
	overlay_d3d9_is_pure_device = (behaviourFlags & D3DCREATE_PUREDEVICE) != 0;
	overlay_d3d9_is_multithreaded = (behaviourFlags & D3DCREATE_MULTITHREADED) != 0;

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s overlay renderer attached to D3D9%s device (%s pool%s%s) behaviour (0x%08x)."
		, __func__
		, is_d3d9ex ? "Ex" : ""
		, is_d3d9ex ? "default" : "managed"
		, overlay_d3d9_is_pure_device ? ", pure device" : ""
		, overlay_d3d9_is_multithreaded ? ", multithreaded" : ""
		, (uint32_t)behaviourFlags
	);

	return true;
}

void OverlayDX9Shutdown()
{
	OverlayDX9InvalidateDeviceObjects();
	if (overlay_d3d9_device) {
		overlay_d3d9_device->Release();
		overlay_d3d9_device = 0;
	}
}

void OverlayDX9InvalidateDeviceObjects()
{
	if (overlay_d3d9_vertex_buffer) {
		overlay_d3d9_vertex_buffer->Release();
		overlay_d3d9_vertex_buffer = 0;
	}
	if (overlay_d3d9_index_buffer) {
		overlay_d3d9_index_buffer->Release();
		overlay_d3d9_index_buffer = 0;
	}
	OverlayDX9MarkFontsDirty();
}

void* OverlayDX9CreateTextureRgba(uint32_t width, uint32_t height, const uint8_t* rgba)
{
	if (!overlay_d3d9_device || !rgba || !width || !height) {
		return 0;
	}

	IDirect3DTexture9* texture = 0;
	HRESULT resultCreateTexture = overlay_d3d9_device->CreateTexture(
		width
		, height
		, 1
		, overlay_d3d9_is_ex ? D3DUSAGE_DYNAMIC : 0
		, D3DFMT_A8R8G8B8
		, overlay_d3d9_resource_pool
		, &texture
		, 0
	);
	if (FAILED(resultCreateTexture)) {
		XLLN_DEBUG_LOG_ECODE(resultCreateTexture, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s CreateTexture(%ux%u) failed."
			, __func__
			, width
			, height
		);
		return 0;
	}

	D3DLOCKED_RECT lockedRect;
	if (FAILED(texture->LockRect(0, &lockedRect, 0, 0))) {
		texture->Release();
		return 0;
	}
	for (uint32_t iRow = 0; iRow < height; iRow++) {
		uint32_t* destination = (uint32_t*)((unsigned char*)lockedRect.pBits + ((size_t)lockedRect.Pitch * iRow));
		const uint32_t* source = (const uint32_t*)(rgba + ((size_t)width * 4 * iRow));
		for (uint32_t iColumn = 0; iColumn < width; iColumn++) {
			destination[iColumn] = OVERLAY_DX9_COLOR_TO_ARGB(source[iColumn]);
		}
	}
	texture->UnlockRect(0);

	return texture;
}

void OverlayDX9DestroyTexture(void* texture)
{
	if (texture) {
		((IDirect3DTexture9*)texture)->Release();
	}
}

void OverlayDX9MarkFontsDirty()
{
	if (overlay_d3d9_font_texture) {
		overlay_d3d9_font_texture->Release();
		overlay_d3d9_font_texture = 0;
		if (ImGui::GetCurrentContext()) {
			ImGui::GetIO().Fonts->SetTexID(0);
		}
	}
}

static bool OverlayDX9CreateFontsTexture()
{
	ImGuiIO &imguiIo = ImGui::GetIO();
	unsigned char* atlasPixels = 0;
	int atlasWidth = 0;
	int atlasHeight = 0;
	int bytesPerPixel = 0;
	imguiIo.Fonts->GetTexDataAsRGBA32(&atlasPixels, &atlasWidth, &atlasHeight, &bytesPerPixel);

	overlay_d3d9_font_texture = (IDirect3DTexture9*)OverlayDX9CreateTextureRgba((uint32_t)atlasWidth, (uint32_t)atlasHeight, atlasPixels);
	if (!overlay_d3d9_font_texture) {
		return false;
	}

	imguiIo.Fonts->SetTexID((ImTextureID)(intptr_t)overlay_d3d9_font_texture);
	return true;
}

bool OverlayDX9BeginFrame()
{
	if (!overlay_d3d9_device) {
		return false;
	}
	// On plain D3D9 a lost device cannot draw, and drawing must also stay out of the
	// way while the title is expected to Reset. The driver keeps managed-pool resources
	// through that whole cycle. D3D9Ex reports D3D_OK here by design.
	HRESULT resultCooperativeLevel = overlay_d3d9_device->TestCooperativeLevel();
	if (resultCooperativeLevel != D3D_OK) {
		return false;
	}
	if (!overlay_d3d9_font_texture && !OverlayDX9CreateFontsTexture()) {
		return false;
	}
	return true;
}

static void OverlayDX9SetupRenderState(ImDrawData* draw_data)
{
	IDirect3DDevice9* device = overlay_d3d9_device;

	D3DVIEWPORT9 viewport;
	viewport.X = 0;
	viewport.Y = 0;
	viewport.Width = (DWORD)draw_data->DisplaySize.x;
	viewport.Height = (DWORD)draw_data->DisplaySize.y;
	viewport.MinZ = 0.0f;
	viewport.MaxZ = 1.0f;
	device->SetViewport(&viewport);

	device->SetPixelShader(0);
	device->SetVertexShader(0);
	device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	device->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
	device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	device->SetRenderState(D3DRS_ZENABLE, FALSE);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
	device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
	device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
	device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
	device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	device->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
	device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
	device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	device->SetRenderState(D3DRS_CLIPPING, TRUE);
	device->SetRenderState(D3DRS_LIGHTING, FALSE);
	device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
	device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	device->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
	device->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

	// Orthographic projection with a half-texel offset, built by hand to avoid a d3dx dependency.
	{
		float left = draw_data->DisplayPos.x + 0.5f;
		float right = draw_data->DisplayPos.x + draw_data->DisplaySize.x + 0.5f;
		float top = draw_data->DisplayPos.y + 0.5f;
		float bottom = draw_data->DisplayPos.y + draw_data->DisplaySize.y + 0.5f;
		D3DMATRIX matrixIdentity = {{{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		}}};
		D3DMATRIX matrixProjection = {{{
			2.0f / (right - left), 0.0f, 0.0f, 0.0f,
			0.0f, 2.0f / (top - bottom), 0.0f, 0.0f,
			0.0f, 0.0f, 0.5f, 0.0f,
			(left + right) / (left - right), (top + bottom) / (bottom - top), 0.5f, 1.0f
		}}};
		device->SetTransform(D3DTS_WORLD, &matrixIdentity);
		device->SetTransform(D3DTS_VIEW, &matrixIdentity);
		device->SetTransform(D3DTS_PROJECTION, &matrixProjection);
	}
}

void OverlayDX9RenderDrawData(ImDrawData* draw_data)
{
	if (!overlay_d3d9_device || !draw_data || draw_data->CmdListsCount <= 0) {
		return;
	}
	if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f) {
		return;
	}
	IDirect3DDevice9* device = overlay_d3d9_device;

	// Grow the buffers when needed. Managed-pool buffers cannot be dynamic, so the
	// plain D3D9 path locks with no flags instead of DISCARD.
	DWORD bufferUsage = D3DUSAGE_WRITEONLY | (overlay_d3d9_resource_pool == D3DPOOL_DEFAULT ? D3DUSAGE_DYNAMIC : 0);
	DWORD lockFlags = (overlay_d3d9_resource_pool == D3DPOOL_DEFAULT) ? D3DLOCK_DISCARD : 0;
	if (!overlay_d3d9_vertex_buffer || overlay_d3d9_vertex_buffer_size < (uint32_t)draw_data->TotalVtxCount) {
		if (overlay_d3d9_vertex_buffer) {
			overlay_d3d9_vertex_buffer->Release();
			overlay_d3d9_vertex_buffer = 0;
		}
		overlay_d3d9_vertex_buffer_size = draw_data->TotalVtxCount + 5000;
		if (FAILED(device->CreateVertexBuffer(
			overlay_d3d9_vertex_buffer_size * sizeof(OVERLAY_DX9_VERTEX)
			, bufferUsage
			, OVERLAY_DX9_VERTEX_FVF
			, overlay_d3d9_resource_pool
			, &overlay_d3d9_vertex_buffer
			, 0
		))) {
			return;
		}
	}
	if (!overlay_d3d9_index_buffer || overlay_d3d9_index_buffer_size < (uint32_t)draw_data->TotalIdxCount) {
		if (overlay_d3d9_index_buffer) {
			overlay_d3d9_index_buffer->Release();
			overlay_d3d9_index_buffer = 0;
		}
		overlay_d3d9_index_buffer_size = draw_data->TotalIdxCount + 10000;
		if (FAILED(device->CreateIndexBuffer(
			overlay_d3d9_index_buffer_size * sizeof(ImDrawIdx)
			, bufferUsage
			, sizeof(ImDrawIdx) == 2 ? D3DFMT_INDEX16 : D3DFMT_INDEX32
			, overlay_d3d9_resource_pool
			, &overlay_d3d9_index_buffer
			, 0
		))) {
			return;
		}
	}

	OVERLAY_DX9_VERTEX* vertexDestination = 0;
	ImDrawIdx* indexDestination = 0;
	if (FAILED(overlay_d3d9_vertex_buffer->Lock(0, (UINT)(draw_data->TotalVtxCount * sizeof(OVERLAY_DX9_VERTEX)), (void**)&vertexDestination, lockFlags))) {
		return;
	}
	if (FAILED(overlay_d3d9_index_buffer->Lock(0, (UINT)(draw_data->TotalIdxCount * sizeof(ImDrawIdx)), (void**)&indexDestination, lockFlags))) {
		overlay_d3d9_vertex_buffer->Unlock();
		return;
	}
	for (int iCmdList = 0; iCmdList < draw_data->CmdListsCount; iCmdList++) {
		const ImDrawList* commandList = draw_data->CmdLists[iCmdList];
		const ImDrawVert* vertexSource = commandList->VtxBuffer.Data;
		for (int iVertex = 0; iVertex < commandList->VtxBuffer.Size; iVertex++) {
			vertexDestination->position[0] = vertexSource->pos.x;
			vertexDestination->position[1] = vertexSource->pos.y;
			vertexDestination->position[2] = 0.0f;
			vertexDestination->color = OVERLAY_DX9_COLOR_TO_ARGB(vertexSource->col);
			vertexDestination->uv[0] = vertexSource->uv.x;
			vertexDestination->uv[1] = vertexSource->uv.y;
			vertexDestination++;
			vertexSource++;
		}
		memcpy(indexDestination, commandList->IdxBuffer.Data, commandList->IdxBuffer.Size * sizeof(ImDrawIdx));
		indexDestination += commandList->IdxBuffer.Size;
	}
	overlay_d3d9_vertex_buffer->Unlock();
	overlay_d3d9_index_buffer->Unlock();

	// Back up the title's pipeline state. Pure devices reject state reads, so
	// transforms are only backed up manually on non-pure devices (the state block
	// restore covers the rest either way).
	IDirect3DStateBlock9* stateBlock = 0;
	if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock))) {
		return;
	}
	if (FAILED(stateBlock->Capture())) {
		stateBlock->Release();
		return;
	}
	D3DMATRIX lastWorld;
	D3DMATRIX lastView;
	D3DMATRIX lastProjection;
	if (!overlay_d3d9_is_pure_device) {
		device->GetTransform(D3DTS_WORLD, &lastWorld);
		device->GetTransform(D3DTS_VIEW, &lastView);
		device->GetTransform(D3DTS_PROJECTION, &lastProjection);
	}

	// A title may call XLiveRender inside or outside a scene of its own, so an already open scene
	// makes BeginScene fail and only the successful call has an EndScene to match it.
	HRESULT resultBeginScene = device->BeginScene();

	device->SetStreamSource(0, overlay_d3d9_vertex_buffer, 0, sizeof(OVERLAY_DX9_VERTEX));
	device->SetIndices(overlay_d3d9_index_buffer);
	device->SetFVF(OVERLAY_DX9_VERTEX_FVF);
	OverlayDX9SetupRenderState(draw_data);

	int globalVertexOffset = 0;
	int globalIndexOffset = 0;
	ImVec2 clipOffset = draw_data->DisplayPos;
	for (int iCmdList = 0; iCmdList < draw_data->CmdListsCount; iCmdList++) {
		const ImDrawList* commandList = draw_data->CmdLists[iCmdList];
		for (int iCommand = 0; iCommand < commandList->CmdBuffer.Size; iCommand++) {
			const ImDrawCmd* drawCommand = &commandList->CmdBuffer[iCommand];
			if (drawCommand->UserCallback) {
				if (drawCommand->UserCallback == ImDrawCallback_ResetRenderState) {
					OverlayDX9SetupRenderState(draw_data);
				}
				else {
					drawCommand->UserCallback(commandList, drawCommand);
				}
				continue;
			}
			ImVec2 clipMin(drawCommand->ClipRect.x - clipOffset.x, drawCommand->ClipRect.y - clipOffset.y);
			ImVec2 clipMax(drawCommand->ClipRect.z - clipOffset.x, drawCommand->ClipRect.w - clipOffset.y);
			if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) {
				continue;
			}
			RECT scissorRect = { (LONG)clipMin.x, (LONG)clipMin.y, (LONG)clipMax.x, (LONG)clipMax.y };
			device->SetScissorRect(&scissorRect);
			device->SetTexture(0, (IDirect3DTexture9*)(intptr_t)drawCommand->GetTexID());
			device->DrawIndexedPrimitive(
				D3DPT_TRIANGLELIST
				, globalVertexOffset + drawCommand->VtxOffset
				, 0
				, (UINT)(commandList->VtxBuffer.Size - drawCommand->VtxOffset)
				, globalIndexOffset + drawCommand->IdxOffset
				, drawCommand->ElemCount / 3
			);
		}
		globalVertexOffset += commandList->VtxBuffer.Size;
		globalIndexOffset += commandList->IdxBuffer.Size;
	}

	if (!overlay_d3d9_is_pure_device) {
		device->SetTransform(D3DTS_WORLD, &lastWorld);
		device->SetTransform(D3DTS_VIEW, &lastView);
		device->SetTransform(D3DTS_PROJECTION, &lastProjection);
	}
	stateBlock->Apply();
	stateBlock->Release();

	if (SUCCEEDED(resultBeginScene)) {
		device->EndScene();
	}
}
