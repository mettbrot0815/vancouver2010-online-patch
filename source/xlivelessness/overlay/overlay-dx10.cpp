// Derived from Dear ImGui's imgui_impl_dx10.cpp (MIT licensed, (c) Ocornut and
// contributors), and kept line for line beside overlay-dx11.cpp so the pair can be read
// together. Direct3D 10 has no separate device context, so the device takes every call
// that the Direct3D 11 backend makes on its context, and a buffer is mapped through the
// buffer itself.
#include <winsock2.h>
#include <Windows.h>
#include "overlay-dx10.hpp"
#include "overlay-shaders-dxgi.hpp"
#include "../xlln/debug-log.hpp"
#include <imgui.h>
#include <stddef.h>
#include <stdint.h>

static ID3D10Device* overlay_dx10_device = 0;
static ID3D10VertexShader* overlay_dx10_vertex_shader = 0;
static ID3D10InputLayout* overlay_dx10_input_layout = 0;
static ID3D10Buffer* overlay_dx10_vertex_constant_buffer = 0;
static ID3D10PixelShader* overlay_dx10_pixel_shader = 0;
static ID3D10PixelShader* overlay_dx10_pixel_shader_srgb = 0;
static ID3D10SamplerState* overlay_dx10_sampler = 0;
static ID3D10BlendState* overlay_dx10_blend_state = 0;
static ID3D10RasterizerState* overlay_dx10_rasterizer_state = 0;
static ID3D10DepthStencilState* overlay_dx10_depth_stencil_state = 0;
static ID3D10ShaderResourceView* overlay_dx10_font_texture_view = 0;
static ID3D10Buffer* overlay_dx10_vertex_buffer = 0;
static ID3D10Buffer* overlay_dx10_index_buffer = 0;
static int overlay_dx10_vertex_buffer_size = 5000;
static int overlay_dx10_index_buffer_size = 10000;

// The shape of what the title presents. A render target that does not match it is one of the
// title's own intermediate buffers.
static uint32_t overlay_dx10_expected_width = 0;
static uint32_t overlay_dx10_expected_height = 0;
static uint32_t overlay_dx10_expected_sample_count = 0;
static DXGI_FORMAT overlay_dx10_expected_format = DXGI_FORMAT_UNKNOWN;
// Lives only for the length of one draw. A reference to a back buffer that outlived the draw
// would make the swap chain resize that the title asks for fail.
static ID3D10RenderTargetView* overlay_dx10_frame_render_target = 0;
static bool overlay_dx10_frame_target_is_srgb = false;

// Nothing recovers from a device that cannot build the shaders, so it is asked once.
static bool overlay_dx10_device_objects_failed = false;
// Which render target the title binds is the one assumption this backend makes, so the first
// decision is logged, and every later change of decision as well.
static bool overlay_dx10_target_decision_logged = false;
static bool overlay_dx10_target_decision_accepted = false;

typedef struct _OVERLAY_DX10_STATE {
	UINT scissorRectCount;
	UINT viewportCount;
	D3D10_RECT scissorRects[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	D3D10_VIEWPORT viewports[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	ID3D10RasterizerState* rasterizerState;
	ID3D10BlendState* blendState;
	FLOAT blendFactor[4];
	UINT sampleMask;
	UINT stencilReference;
	ID3D10DepthStencilState* depthStencilState;
	ID3D10ShaderResourceView* pixelShaderResource;
	ID3D10SamplerState* pixelShaderSampler;
	ID3D10PixelShader* pixelShader;
	ID3D10VertexShader* vertexShader;
	ID3D10GeometryShader* geometryShader;
	D3D10_PRIMITIVE_TOPOLOGY primitiveTopology;
	ID3D10Buffer* indexBuffer;
	ID3D10Buffer* vertexBuffer;
	ID3D10Buffer* vertexShaderConstantBuffer;
	UINT indexBufferOffset;
	UINT vertexBufferStride;
	UINT vertexBufferOffset;
	DXGI_FORMAT indexBufferFormat;
	ID3D10InputLayout* inputLayout;
	ID3D10RenderTargetView* renderTargets[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D10DepthStencilView* depthStencilView;
} OVERLAY_DX10_STATE;

typedef struct _OVERLAY_DX10_VERTEX_CONSTANTS {
	float projectionMatrix[4][4];
} OVERLAY_DX10_VERTEX_CONSTANTS;

// The hardware encodes the shader output for these formats, so the shader has to give it linear.
static bool OverlayDX10FormatIsSrgb(DXGI_FORMAT format)
{
	return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
		|| format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
		|| format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

// A render target view may add the sRGB encoding over the format the swap chain was made with, so
// the two count as the same surface format.
static DXGI_FORMAT OverlayDX10FormatWithoutSrgb(DXGI_FORMAT format)
{
	switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
	}
	return format;
}

static void OverlayDX10StoreExpectedTarget(const DXGI_SWAP_CHAIN_DESC* swap_chain_description)
{
	overlay_dx10_expected_width = 0;
	overlay_dx10_expected_height = 0;
	overlay_dx10_expected_sample_count = 0;
	overlay_dx10_expected_format = DXGI_FORMAT_UNKNOWN;
	if (!swap_chain_description) {
		return;
	}
	overlay_dx10_expected_width = swap_chain_description->BufferDesc.Width;
	overlay_dx10_expected_height = swap_chain_description->BufferDesc.Height;
	overlay_dx10_expected_sample_count = swap_chain_description->SampleDesc.Count;
	overlay_dx10_expected_format = swap_chain_description->BufferDesc.Format;
	if ((!overlay_dx10_expected_width || !overlay_dx10_expected_height) && swap_chain_description->OutputWindow) {
		RECT clientRect;
		if (GetClientRect(swap_chain_description->OutputWindow, &clientRect)) {
			overlay_dx10_expected_width = clientRect.right - clientRect.left;
			overlay_dx10_expected_height = clientRect.bottom - clientRect.top;
		}
	}
}

bool OverlayDX10Init(ID3D10Device* d3d_device, const DXGI_SWAP_CHAIN_DESC* swap_chain_description)
{
	if (!d3d_device) {
		return false;
	}
	// The overlay takes the device from the Title's render thread while the device callbacks arrive
	// on the thread that made it. Direct3D 10 serialises that itself, but not for a device made with
	// this flag. Stand down and let the XLLN window answer instead.
	if (d3d_device->GetCreationFlags() & D3D10_CREATE_DEVICE_SINGLETHREADED) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
			, "%s the Title made a single threaded Direct3D 10 device, which cannot take calls from the render thread, so the built-in overlay does not attach to it."
			, __func__
		);
		return false;
	}
	OverlayDX10Shutdown();

	overlay_dx10_device = d3d_device;
	overlay_dx10_device->AddRef();

	OverlayDX10StoreExpectedTarget(swap_chain_description);

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s overlay renderer attached to a D3D10 device presenting (%ux%u) format (%d) samples (%u)."
		, __func__
		, overlay_dx10_expected_width
		, overlay_dx10_expected_height
		, (int)overlay_dx10_expected_format
		, overlay_dx10_expected_sample_count
	);

	return true;
}

void OverlayDX10InvalidateDeviceObjects()
{
	if (overlay_dx10_vertex_buffer) {
		overlay_dx10_vertex_buffer->Release();
		overlay_dx10_vertex_buffer = 0;
	}
	if (overlay_dx10_index_buffer) {
		overlay_dx10_index_buffer->Release();
		overlay_dx10_index_buffer = 0;
	}
	OverlayDX10MarkFontsDirty();
}

static void OverlayDX10ReleasePipelineObjects()
{
	if (overlay_dx10_blend_state) {
		overlay_dx10_blend_state->Release();
		overlay_dx10_blend_state = 0;
	}
	if (overlay_dx10_depth_stencil_state) {
		overlay_dx10_depth_stencil_state->Release();
		overlay_dx10_depth_stencil_state = 0;
	}
	if (overlay_dx10_rasterizer_state) {
		overlay_dx10_rasterizer_state->Release();
		overlay_dx10_rasterizer_state = 0;
	}
	if (overlay_dx10_sampler) {
		overlay_dx10_sampler->Release();
		overlay_dx10_sampler = 0;
	}
	if (overlay_dx10_pixel_shader) {
		overlay_dx10_pixel_shader->Release();
		overlay_dx10_pixel_shader = 0;
	}
	if (overlay_dx10_pixel_shader_srgb) {
		overlay_dx10_pixel_shader_srgb->Release();
		overlay_dx10_pixel_shader_srgb = 0;
	}
	if (overlay_dx10_vertex_constant_buffer) {
		overlay_dx10_vertex_constant_buffer->Release();
		overlay_dx10_vertex_constant_buffer = 0;
	}
	if (overlay_dx10_input_layout) {
		overlay_dx10_input_layout->Release();
		overlay_dx10_input_layout = 0;
	}
	if (overlay_dx10_vertex_shader) {
		overlay_dx10_vertex_shader->Release();
		overlay_dx10_vertex_shader = 0;
	}
}

static void OverlayDX10ReleaseFrameRenderTarget()
{
	if (overlay_dx10_frame_render_target) {
		overlay_dx10_frame_render_target->Release();
		overlay_dx10_frame_render_target = 0;
	}
	overlay_dx10_frame_target_is_srgb = false;
}

void OverlayDX10Shutdown()
{
	OverlayDX10ReleaseFrameRenderTarget();
	OverlayDX10InvalidateDeviceObjects();
	OverlayDX10ReleasePipelineObjects();
	if (overlay_dx10_device) {
		overlay_dx10_device->Release();
		overlay_dx10_device = 0;
	}
	overlay_dx10_expected_width = 0;
	overlay_dx10_expected_height = 0;
	overlay_dx10_device_objects_failed = false;
	overlay_dx10_target_decision_logged = false;
}

void OverlayDX10OnDeviceReset(const DXGI_SWAP_CHAIN_DESC* swap_chain_description)
{
	if (!overlay_dx10_device) {
		return;
	}
	// A DXGI resize keeps every resource, so only the size that the render target is checked
	// against has to follow the swap chain.
	OverlayDX10StoreExpectedTarget(swap_chain_description);
	overlay_dx10_target_decision_logged = false;
}

void OverlayDX10MarkFontsDirty()
{
	if (overlay_dx10_font_texture_view) {
		overlay_dx10_font_texture_view->Release();
		overlay_dx10_font_texture_view = 0;
		if (ImGui::GetCurrentContext()) {
			ImGui::GetIO().Fonts->SetTexID(0);
		}
	}
}

void* OverlayDX10CreateTextureRgba(uint32_t width, uint32_t height, const uint8_t* rgba)
{
	if (!overlay_dx10_device || !rgba || !width || !height) {
		return 0;
	}

	D3D10_TEXTURE2D_DESC textureDescription;
	ZeroMemory(&textureDescription, sizeof(textureDescription));
	textureDescription.Width = width;
	textureDescription.Height = height;
	textureDescription.MipLevels = 1;
	textureDescription.ArraySize = 1;
	textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	textureDescription.SampleDesc.Count = 1;
	textureDescription.Usage = D3D10_USAGE_DEFAULT;
	textureDescription.BindFlags = D3D10_BIND_SHADER_RESOURCE;

	D3D10_SUBRESOURCE_DATA textureData;
	ZeroMemory(&textureData, sizeof(textureData));
	textureData.pSysMem = rgba;
	textureData.SysMemPitch = width * 4;

	ID3D10Texture2D* texture = 0;
	HRESULT resultCreateTexture = overlay_dx10_device->CreateTexture2D(&textureDescription, &textureData, &texture);
	if (FAILED(resultCreateTexture) || !texture) {
		XLLN_DEBUG_LOG_ECODE(resultCreateTexture, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s CreateTexture2D(%ux%u) failed."
			, __func__
			, width
			, height
		);
		return 0;
	}

	D3D10_SHADER_RESOURCE_VIEW_DESC viewDescription;
	ZeroMemory(&viewDescription, sizeof(viewDescription));
	viewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	viewDescription.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
	viewDescription.Texture2D.MipLevels = 1;

	ID3D10ShaderResourceView* textureView = 0;
	HRESULT resultCreateView = overlay_dx10_device->CreateShaderResourceView(texture, &viewDescription, &textureView);
	// The view holds a reference of its own, so the texture stays alive without this one.
	texture->Release();
	if (FAILED(resultCreateView)) {
		XLLN_DEBUG_LOG_ECODE(resultCreateView, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s CreateShaderResourceView(%ux%u) failed."
			, __func__
			, width
			, height
		);
		return 0;
	}

	return textureView;
}

void OverlayDX10DestroyTexture(void* texture)
{
	if (texture) {
		((ID3D10ShaderResourceView*)texture)->Release();
	}
}

static bool OverlayDX10CreateFontsTexture()
{
	ImGuiIO &imguiIo = ImGui::GetIO();
	unsigned char* atlasPixels = 0;
	int atlasWidth = 0;
	int atlasHeight = 0;
	int bytesPerPixel = 0;
	imguiIo.Fonts->GetTexDataAsRGBA32(&atlasPixels, &atlasWidth, &atlasHeight, &bytesPerPixel);

	overlay_dx10_font_texture_view = (ID3D10ShaderResourceView*)OverlayDX10CreateTextureRgba((uint32_t)atlasWidth, (uint32_t)atlasHeight, atlasPixels);
	if (!overlay_dx10_font_texture_view) {
		return false;
	}

	imguiIo.Fonts->SetTexID((ImTextureID)overlay_dx10_font_texture_view);
	return true;
}

static bool OverlayDX10CreateDeviceObjects()
{
	if (!overlay_dx10_device) {
		return false;
	}
	OverlayDX10ReleasePipelineObjects();

	HRESULT resultCreate = overlay_dx10_device->CreateVertexShader(overlay_dxgi_vertex_shader, sizeof(overlay_dxgi_vertex_shader), &overlay_dx10_vertex_shader);
	if (FAILED(resultCreate)) {
		XLLN_DEBUG_LOG_ECODE(resultCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR, "%s CreateVertexShader failed.", __func__);
		OverlayDX10ReleasePipelineObjects();
		return false;
	}

	const D3D10_INPUT_ELEMENT_DESC inputElements[3] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, pos), D3D10_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, uv), D3D10_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)offsetof(ImDrawVert, col), D3D10_INPUT_PER_VERTEX_DATA, 0 },
	};
	resultCreate = overlay_dx10_device->CreateInputLayout(inputElements, 3, overlay_dxgi_vertex_shader, sizeof(overlay_dxgi_vertex_shader), &overlay_dx10_input_layout);
	if (FAILED(resultCreate)) {
		XLLN_DEBUG_LOG_ECODE(resultCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR, "%s CreateInputLayout failed.", __func__);
		OverlayDX10ReleasePipelineObjects();
		return false;
	}

	D3D10_BUFFER_DESC constantBufferDescription;
	ZeroMemory(&constantBufferDescription, sizeof(constantBufferDescription));
	constantBufferDescription.ByteWidth = sizeof(OVERLAY_DX10_VERTEX_CONSTANTS);
	constantBufferDescription.Usage = D3D10_USAGE_DYNAMIC;
	constantBufferDescription.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
	constantBufferDescription.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
	resultCreate = overlay_dx10_device->CreateBuffer(&constantBufferDescription, 0, &overlay_dx10_vertex_constant_buffer);
	if (FAILED(resultCreate)) {
		XLLN_DEBUG_LOG_ECODE(resultCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR, "%s CreateBuffer(constants) failed.", __func__);
		OverlayDX10ReleasePipelineObjects();
		return false;
	}

	resultCreate = overlay_dx10_device->CreatePixelShader(overlay_dxgi_pixel_shader, sizeof(overlay_dxgi_pixel_shader), &overlay_dx10_pixel_shader);
	if (SUCCEEDED(resultCreate)) {
		resultCreate = overlay_dx10_device->CreatePixelShader(overlay_dxgi_pixel_shader_srgb, sizeof(overlay_dxgi_pixel_shader_srgb), &overlay_dx10_pixel_shader_srgb);
	}
	if (FAILED(resultCreate)) {
		XLLN_DEBUG_LOG_ECODE(resultCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR, "%s CreatePixelShader failed.", __func__);
		OverlayDX10ReleasePipelineObjects();
		return false;
	}

	D3D10_BLEND_DESC blendDescription;
	ZeroMemory(&blendDescription, sizeof(blendDescription));
	blendDescription.AlphaToCoverageEnable = FALSE;
	blendDescription.BlendEnable[0] = TRUE;
	blendDescription.SrcBlend = D3D10_BLEND_SRC_ALPHA;
	blendDescription.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
	blendDescription.BlendOp = D3D10_BLEND_OP_ADD;
	blendDescription.SrcBlendAlpha = D3D10_BLEND_ONE;
	blendDescription.DestBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
	blendDescription.BlendOpAlpha = D3D10_BLEND_OP_ADD;
	blendDescription.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
	overlay_dx10_device->CreateBlendState(&blendDescription, &overlay_dx10_blend_state);

	D3D10_RASTERIZER_DESC rasterizerDescription;
	ZeroMemory(&rasterizerDescription, sizeof(rasterizerDescription));
	rasterizerDescription.FillMode = D3D10_FILL_SOLID;
	rasterizerDescription.CullMode = D3D10_CULL_NONE;
	rasterizerDescription.ScissorEnable = TRUE;
	rasterizerDescription.DepthClipEnable = TRUE;
	overlay_dx10_device->CreateRasterizerState(&rasterizerDescription, &overlay_dx10_rasterizer_state);

	D3D10_DEPTH_STENCIL_DESC depthStencilDescription;
	ZeroMemory(&depthStencilDescription, sizeof(depthStencilDescription));
	depthStencilDescription.DepthEnable = FALSE;
	depthStencilDescription.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
	depthStencilDescription.DepthFunc = D3D10_COMPARISON_ALWAYS;
	depthStencilDescription.StencilEnable = FALSE;
	depthStencilDescription.FrontFace.StencilFailOp = D3D10_STENCIL_OP_KEEP;
	depthStencilDescription.FrontFace.StencilDepthFailOp = D3D10_STENCIL_OP_KEEP;
	depthStencilDescription.FrontFace.StencilPassOp = D3D10_STENCIL_OP_KEEP;
	depthStencilDescription.FrontFace.StencilFunc = D3D10_COMPARISON_ALWAYS;
	depthStencilDescription.BackFace = depthStencilDescription.FrontFace;
	overlay_dx10_device->CreateDepthStencilState(&depthStencilDescription, &overlay_dx10_depth_stencil_state);

	D3D10_SAMPLER_DESC samplerDescription;
	ZeroMemory(&samplerDescription, sizeof(samplerDescription));
	samplerDescription.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDescription.AddressU = D3D10_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressV = D3D10_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressW = D3D10_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.ComparisonFunc = D3D10_COMPARISON_ALWAYS;
	overlay_dx10_device->CreateSamplerState(&samplerDescription, &overlay_dx10_sampler);

	if (!overlay_dx10_blend_state || !overlay_dx10_rasterizer_state || !overlay_dx10_depth_stencil_state || !overlay_dx10_sampler) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR, "%s a pipeline state object could not be created.", __func__);
		OverlayDX10ReleasePipelineObjects();
		return false;
	}

	return true;
}

// Takes the render target that the title has bound and decides whether the overlay can draw on it.
// A DXGI title gives XLLN a description of its swap chain and never the swap chain, so there is no
// back buffer to ask for. A target of a size other than the one the title presents at is an
// intermediate buffer, and drawing on one would put the overlay somewhere the player never sees,
// or inside an image the title still has passes to run over.
static bool OverlayDX10CaptureRenderTarget()
{
	OverlayDX10ReleaseFrameRenderTarget();

	ID3D10RenderTargetView* renderTargetView = 0;
	overlay_dx10_device->OMGetRenderTargets(1, &renderTargetView, 0);
	if (!renderTargetView) {
		if (!overlay_dx10_target_decision_logged || overlay_dx10_target_decision_accepted) {
			overlay_dx10_target_decision_logged = true;
			overlay_dx10_target_decision_accepted = false;
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
				, "%s the Title has no render target bound, so the overlay skips the frame."
				, __func__
			);
		}
		return false;
	}

	ID3D10Resource* renderTargetResource = 0;
	renderTargetView->GetResource(&renderTargetResource);
	ID3D10Texture2D* renderTargetTexture = 0;
	if (renderTargetResource) {
		renderTargetResource->QueryInterface(IID_ID3D10Texture2D, (void**)&renderTargetTexture);
		renderTargetResource->Release();
	}
	if (!renderTargetTexture) {
		renderTargetView->Release();
		return false;
	}

	D3D10_TEXTURE2D_DESC textureDescription;
	renderTargetTexture->GetDesc(&textureDescription);
	renderTargetTexture->Release();

	D3D10_RENDER_TARGET_VIEW_DESC viewDescription;
	renderTargetView->GetDesc(&viewDescription);

	// Size alone cannot tell the back buffer from an intermediate buffer, because a title renders
	// its scene at the size it presents at. Sample count and surface format are what separate
	// them: a multisampled scene buffer in front of a single sampled swap chain matches on size
	// and on nothing else.
	const char* decision = "the overlay draws on it";
	bool accepted = true;
	if ((overlay_dx10_expected_width && overlay_dx10_expected_height)
		&& (textureDescription.Width != overlay_dx10_expected_width || textureDescription.Height != overlay_dx10_expected_height)) {
		accepted = false;
		decision = "the size is not the one it presents, so the overlay skips the frame";
	}
	else if (overlay_dx10_expected_sample_count && textureDescription.SampleDesc.Count != overlay_dx10_expected_sample_count) {
		accepted = false;
		decision = "the sample count is not the one it presents, so the overlay skips the frame";
	}
	else if (overlay_dx10_expected_format != DXGI_FORMAT_UNKNOWN
		&& OverlayDX10FormatWithoutSrgb(viewDescription.Format) != OverlayDX10FormatWithoutSrgb(overlay_dx10_expected_format)) {
		accepted = false;
		decision = "the format is not the one it presents, so the overlay skips the frame";
	}

	if (!overlay_dx10_target_decision_logged || overlay_dx10_target_decision_accepted != accepted) {
		overlay_dx10_target_decision_logged = true;
		overlay_dx10_target_decision_accepted = accepted;
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | (accepted ? XLLN_LOG_LEVEL_INFO : XLLN_LOG_LEVEL_WARN)
			, "%s the Title has a (%ux%u) format (%d) samples (%u) target bound, and presents (%ux%u) format (%d) samples (%u): %s."
			, __func__
			, textureDescription.Width
			, textureDescription.Height
			, (int)viewDescription.Format
			, textureDescription.SampleDesc.Count
			, overlay_dx10_expected_width
			, overlay_dx10_expected_height
			, (int)overlay_dx10_expected_format
			, overlay_dx10_expected_sample_count
			, decision
		);
	}

	if (!accepted) {
		renderTargetView->Release();
		return false;
	}

	overlay_dx10_frame_render_target = renderTargetView;
	overlay_dx10_frame_target_is_srgb = OverlayDX10FormatIsSrgb(viewDescription.Format);
	return true;
}

bool OverlayDX10TargetAccepted()
{
	// A target the backend has not judged yet counts as good, so nothing stands down before the
	// first decision.
	return !overlay_dx10_target_decision_logged || overlay_dx10_target_decision_accepted;
}

bool OverlayDX10BeginFrame()
{
	if (!overlay_dx10_device) {
		return false;
	}
	if (overlay_dx10_device_objects_failed) {
		return false;
	}
	if (!overlay_dx10_vertex_shader && !OverlayDX10CreateDeviceObjects()) {
		overlay_dx10_device_objects_failed = true;
		return false;
	}
	if (!overlay_dx10_font_texture_view && !OverlayDX10CreateFontsTexture()) {
		return false;
	}
	return true;
}

static void OverlayDX10SetupRenderState(ImDrawData* draw_data)
{
	ID3D10Device* device = overlay_dx10_device;

	D3D10_VIEWPORT viewport;
	ZeroMemory(&viewport, sizeof(viewport));
	viewport.Width = (UINT)draw_data->DisplaySize.x;
	viewport.Height = (UINT)draw_data->DisplaySize.y;
	viewport.MinDepth = 0.f;
	viewport.MaxDepth = 1.f;
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	device->RSSetViewports(1, &viewport);

	const unsigned int vertexStride = sizeof(ImDrawVert);
	const unsigned int vertexOffset = 0;
	device->IASetInputLayout(overlay_dx10_input_layout);
	device->IASetVertexBuffers(0, 1, &overlay_dx10_vertex_buffer, &vertexStride, &vertexOffset);
	device->IASetIndexBuffer(overlay_dx10_index_buffer, sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
	device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	device->VSSetShader(overlay_dx10_vertex_shader);
	device->VSSetConstantBuffers(0, 1, &overlay_dx10_vertex_constant_buffer);
	device->PSSetShader(overlay_dx10_frame_target_is_srgb ? overlay_dx10_pixel_shader_srgb : overlay_dx10_pixel_shader);
	device->PSSetSamplers(0, 1, &overlay_dx10_sampler);
	device->GSSetShader(0);

	const float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
	device->OMSetBlendState(overlay_dx10_blend_state, blendFactor, 0xffffffff);
	device->OMSetDepthStencilState(overlay_dx10_depth_stencil_state, 0);
	device->RSSetState(overlay_dx10_rasterizer_state);

	// Only the render target that was checked, and no depth buffer. A title that binds several
	// targets at once must not get the overlay written into the others.
	device->OMSetRenderTargets(1, &overlay_dx10_frame_render_target, 0);
}

void OverlayDX10RenderDrawData(ImDrawData* draw_data)
{
	if (!draw_data || !overlay_dx10_device) {
		return;
	}
	if (draw_data->DisplaySize.x <= 0.f || draw_data->DisplaySize.y <= 0.f) {
		return;
	}
	if (!OverlayDX10CaptureRenderTarget()) {
		return;
	}

	ID3D10Device* device = overlay_dx10_device;

	if (!overlay_dx10_vertex_buffer || overlay_dx10_vertex_buffer_size < draw_data->TotalVtxCount) {
		if (overlay_dx10_vertex_buffer) {
			overlay_dx10_vertex_buffer->Release();
			overlay_dx10_vertex_buffer = 0;
		}
		overlay_dx10_vertex_buffer_size = draw_data->TotalVtxCount + 5000;
		D3D10_BUFFER_DESC bufferDescription;
		ZeroMemory(&bufferDescription, sizeof(bufferDescription));
		bufferDescription.Usage = D3D10_USAGE_DYNAMIC;
		bufferDescription.ByteWidth = overlay_dx10_vertex_buffer_size * sizeof(ImDrawVert);
		bufferDescription.BindFlags = D3D10_BIND_VERTEX_BUFFER;
		bufferDescription.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&bufferDescription, 0, &overlay_dx10_vertex_buffer))) {
			OverlayDX10ReleaseFrameRenderTarget();
			return;
		}
	}
	if (!overlay_dx10_index_buffer || overlay_dx10_index_buffer_size < draw_data->TotalIdxCount) {
		if (overlay_dx10_index_buffer) {
			overlay_dx10_index_buffer->Release();
			overlay_dx10_index_buffer = 0;
		}
		overlay_dx10_index_buffer_size = draw_data->TotalIdxCount + 10000;
		D3D10_BUFFER_DESC bufferDescription;
		ZeroMemory(&bufferDescription, sizeof(bufferDescription));
		bufferDescription.Usage = D3D10_USAGE_DYNAMIC;
		bufferDescription.ByteWidth = overlay_dx10_index_buffer_size * sizeof(ImDrawIdx);
		bufferDescription.BindFlags = D3D10_BIND_INDEX_BUFFER;
		bufferDescription.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&bufferDescription, 0, &overlay_dx10_index_buffer))) {
			OverlayDX10ReleaseFrameRenderTarget();
			return;
		}
	}

	void* mappedVertices = 0;
	void* mappedIndices = 0;
	if (FAILED(overlay_dx10_vertex_buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mappedVertices))) {
		OverlayDX10ReleaseFrameRenderTarget();
		return;
	}
	if (FAILED(overlay_dx10_index_buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mappedIndices))) {
		overlay_dx10_vertex_buffer->Unmap();
		OverlayDX10ReleaseFrameRenderTarget();
		return;
	}
	ImDrawVert* vertexDestination = (ImDrawVert*)mappedVertices;
	ImDrawIdx* indexDestination = (ImDrawIdx*)mappedIndices;
	for (int iCmdList = 0; iCmdList < draw_data->CmdListsCount; iCmdList++) {
		const ImDrawList* cmdList = draw_data->CmdLists[iCmdList];
		memcpy(vertexDestination, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
		memcpy(indexDestination, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));
		vertexDestination += cmdList->VtxBuffer.Size;
		indexDestination += cmdList->IdxBuffer.Size;
	}
	overlay_dx10_vertex_buffer->Unmap();
	overlay_dx10_index_buffer->Unmap();

	{
		void* mappedConstants = 0;
		if (FAILED(overlay_dx10_vertex_constant_buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &mappedConstants))) {
			OverlayDX10ReleaseFrameRenderTarget();
			return;
		}
		OVERLAY_DX10_VERTEX_CONSTANTS* constants = (OVERLAY_DX10_VERTEX_CONSTANTS*)mappedConstants;
		const float left = draw_data->DisplayPos.x;
		const float right = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
		const float top = draw_data->DisplayPos.y;
		const float bottom = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
		const float projection[4][4] = {
			{ 2.f / (right - left), 0.f, 0.f, 0.f },
			{ 0.f, 2.f / (top - bottom), 0.f, 0.f },
			{ 0.f, 0.f, 0.5f, 0.f },
			{ (right + left) / (left - right), (top + bottom) / (bottom - top), 0.5f, 1.f },
		};
		memcpy(&constants->projectionMatrix, projection, sizeof(projection));
		overlay_dx10_vertex_constant_buffer->Unmap();
	}

	OVERLAY_DX10_STATE state;
	state.scissorRectCount = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	state.viewportCount = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	device->RSGetScissorRects(&state.scissorRectCount, state.scissorRects);
	device->RSGetViewports(&state.viewportCount, state.viewports);
	device->RSGetState(&state.rasterizerState);
	device->OMGetBlendState(&state.blendState, state.blendFactor, &state.sampleMask);
	device->OMGetDepthStencilState(&state.depthStencilState, &state.stencilReference);
	device->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, state.renderTargets, &state.depthStencilView);
	device->PSGetShaderResources(0, 1, &state.pixelShaderResource);
	device->PSGetSamplers(0, 1, &state.pixelShaderSampler);
	device->PSGetShader(&state.pixelShader);
	device->VSGetShader(&state.vertexShader);
	device->VSGetConstantBuffers(0, 1, &state.vertexShaderConstantBuffer);
	device->GSGetShader(&state.geometryShader);
	device->IAGetPrimitiveTopology(&state.primitiveTopology);
	device->IAGetIndexBuffer(&state.indexBuffer, &state.indexBufferFormat, &state.indexBufferOffset);
	device->IAGetVertexBuffers(0, 1, &state.vertexBuffer, &state.vertexBufferStride, &state.vertexBufferOffset);
	device->IAGetInputLayout(&state.inputLayout);

	OverlayDX10SetupRenderState(draw_data);

	int globalVertexOffset = 0;
	int globalIndexOffset = 0;
	const ImVec2 clipOffset = draw_data->DisplayPos;
	for (int iCmdList = 0; iCmdList < draw_data->CmdListsCount; iCmdList++) {
		const ImDrawList* cmdList = draw_data->CmdLists[iCmdList];
		for (int iCmd = 0; iCmd < cmdList->CmdBuffer.Size; iCmd++) {
			const ImDrawCmd* drawCommand = &cmdList->CmdBuffer[iCmd];
			if (drawCommand->UserCallback) {
				if (drawCommand->UserCallback == ImDrawCallback_ResetRenderState) {
					OverlayDX10SetupRenderState(draw_data);
				}
				else {
					drawCommand->UserCallback(cmdList, drawCommand);
				}
				continue;
			}

			const float clipMinX = drawCommand->ClipRect.x - clipOffset.x;
			const float clipMinY = drawCommand->ClipRect.y - clipOffset.y;
			const float clipMaxX = drawCommand->ClipRect.z - clipOffset.x;
			const float clipMaxY = drawCommand->ClipRect.w - clipOffset.y;
			if (clipMaxX <= clipMinX || clipMaxY <= clipMinY) {
				continue;
			}

			const D3D10_RECT scissorRect = { (LONG)clipMinX, (LONG)clipMinY, (LONG)clipMaxX, (LONG)clipMaxY };
			device->RSSetScissorRects(1, &scissorRect);

			ID3D10ShaderResourceView* textureView = (ID3D10ShaderResourceView*)drawCommand->GetTexID();
			device->PSSetShaderResources(0, 1, &textureView);
			device->DrawIndexed(drawCommand->ElemCount, drawCommand->IdxOffset + globalIndexOffset, drawCommand->VtxOffset + globalVertexOffset);
		}
		globalIndexOffset += cmdList->IdxBuffer.Size;
		globalVertexOffset += cmdList->VtxBuffer.Size;
	}

	device->RSSetScissorRects(state.scissorRectCount, state.scissorRects);
	device->RSSetViewports(state.viewportCount, state.viewports);
	device->RSSetState(state.rasterizerState);
	if (state.rasterizerState) {
		state.rasterizerState->Release();
	}
	device->OMSetBlendState(state.blendState, state.blendFactor, state.sampleMask);
	if (state.blendState) {
		state.blendState->Release();
	}
	device->OMSetDepthStencilState(state.depthStencilState, state.stencilReference);
	if (state.depthStencilState) {
		state.depthStencilState->Release();
	}
	device->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, state.renderTargets, state.depthStencilView);
	for (int iRenderTarget = 0; iRenderTarget < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; iRenderTarget++) {
		if (state.renderTargets[iRenderTarget]) {
			state.renderTargets[iRenderTarget]->Release();
		}
	}
	if (state.depthStencilView) {
		state.depthStencilView->Release();
	}
	device->PSSetShaderResources(0, 1, &state.pixelShaderResource);
	if (state.pixelShaderResource) {
		state.pixelShaderResource->Release();
	}
	device->PSSetSamplers(0, 1, &state.pixelShaderSampler);
	if (state.pixelShaderSampler) {
		state.pixelShaderSampler->Release();
	}
	device->PSSetShader(state.pixelShader);
	if (state.pixelShader) {
		state.pixelShader->Release();
	}
	device->VSSetShader(state.vertexShader);
	if (state.vertexShader) {
		state.vertexShader->Release();
	}
	device->VSSetConstantBuffers(0, 1, &state.vertexShaderConstantBuffer);
	if (state.vertexShaderConstantBuffer) {
		state.vertexShaderConstantBuffer->Release();
	}
	device->GSSetShader(state.geometryShader);
	if (state.geometryShader) {
		state.geometryShader->Release();
	}
	device->IASetPrimitiveTopology(state.primitiveTopology);
	device->IASetIndexBuffer(state.indexBuffer, state.indexBufferFormat, state.indexBufferOffset);
	if (state.indexBuffer) {
		state.indexBuffer->Release();
	}
	device->IASetVertexBuffers(0, 1, &state.vertexBuffer, &state.vertexBufferStride, &state.vertexBufferOffset);
	if (state.vertexBuffer) {
		state.vertexBuffer->Release();
	}
	device->IASetInputLayout(state.inputLayout);
	if (state.inputLayout) {
		state.inputLayout->Release();
	}

	OverlayDX10ReleaseFrameRenderTarget();
}
