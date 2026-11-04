// Derived from Dear ImGui's imgui_impl_dx11.cpp (MIT licensed, (c) Ocornut and
// contributors), restructured for XLLN: precompiled shaders instead of D3DCompile,
// the title's own render target taken and checked every frame, and an sRGB shader
// variant for a render target that the hardware encodes on write.
#include <winsock2.h>
#include <Windows.h>
#include "overlay-dx11.hpp"
#include "overlay-shaders-dxgi.hpp"
#include "../xlln/debug-log.hpp"
#include <imgui.h>
#include <stddef.h>
#include <stdint.h>

static ID3D11Device* overlay_dx11_device = 0;
static ID3D11DeviceContext* overlay_dx11_context = 0;
static ID3D11VertexShader* overlay_dx11_vertex_shader = 0;
static ID3D11InputLayout* overlay_dx11_input_layout = 0;
static ID3D11Buffer* overlay_dx11_vertex_constant_buffer = 0;
static ID3D11PixelShader* overlay_dx11_pixel_shader = 0;
static ID3D11PixelShader* overlay_dx11_pixel_shader_srgb = 0;
static ID3D11SamplerState* overlay_dx11_sampler = 0;
static ID3D11BlendState* overlay_dx11_blend_state = 0;
static ID3D11RasterizerState* overlay_dx11_rasterizer_state = 0;
static ID3D11DepthStencilState* overlay_dx11_depth_stencil_state = 0;
static ID3D11ShaderResourceView* overlay_dx11_font_texture_view = 0;
static ID3D11Buffer* overlay_dx11_vertex_buffer = 0;
static ID3D11Buffer* overlay_dx11_index_buffer = 0;
static int overlay_dx11_vertex_buffer_size = 5000;
static int overlay_dx11_index_buffer_size = 10000;

// The shape of what the title presents. A render target that does not match it is one of the
// title's own intermediate buffers.
static uint32_t overlay_dx11_expected_width = 0;
static uint32_t overlay_dx11_expected_height = 0;
static uint32_t overlay_dx11_expected_sample_count = 0;
static DXGI_FORMAT overlay_dx11_expected_format = DXGI_FORMAT_UNKNOWN;
// Lives only for the length of one draw. A reference to a back buffer that outlived the draw
// would make the swap chain resize that the title asks for fail.
static ID3D11RenderTargetView* overlay_dx11_frame_render_target = 0;
static bool overlay_dx11_frame_target_is_srgb = false;

// Nothing recovers from a device that cannot build the shaders, so it is asked once.
static bool overlay_dx11_device_objects_failed = false;
// Which render target the title binds is the one assumption this backend makes, so the first
// decision is logged, and every later change of decision as well.
static bool overlay_dx11_target_decision_logged = false;
static bool overlay_dx11_target_decision_accepted = false;

typedef struct _OVERLAY_DX11_STATE {
	UINT scissorRectCount;
	UINT viewportCount;
	D3D11_RECT scissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	ID3D11RasterizerState* rasterizerState;
	ID3D11BlendState* blendState;
	FLOAT blendFactor[4];
	UINT sampleMask;
	UINT stencilReference;
	ID3D11DepthStencilState* depthStencilState;
	ID3D11ShaderResourceView* pixelShaderResource;
	ID3D11SamplerState* pixelShaderSampler;
	ID3D11PixelShader* pixelShader;
	ID3D11VertexShader* vertexShader;
	ID3D11GeometryShader* geometryShader;
	ID3D11HullShader* hullShader;
	ID3D11DomainShader* domainShader;
	UINT pixelShaderInstanceCount;
	UINT vertexShaderInstanceCount;
	UINT geometryShaderInstanceCount;
	UINT hullShaderInstanceCount;
	UINT domainShaderInstanceCount;
	ID3D11ClassInstance* pixelShaderInstances[256];
	ID3D11ClassInstance* vertexShaderInstances[256];
	ID3D11ClassInstance* geometryShaderInstances[256];
	ID3D11ClassInstance* hullShaderInstances[256];
	ID3D11ClassInstance* domainShaderInstances[256];
	D3D11_PRIMITIVE_TOPOLOGY primitiveTopology;
	ID3D11Buffer* indexBuffer;
	ID3D11Buffer* vertexBuffer;
	ID3D11Buffer* vertexShaderConstantBuffer;
	UINT indexBufferOffset;
	UINT vertexBufferStride;
	UINT vertexBufferOffset;
	DXGI_FORMAT indexBufferFormat;
	ID3D11InputLayout* inputLayout;
	ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView* depthStencilView;
} OVERLAY_DX11_STATE;

typedef struct _OVERLAY_DXGI_VERTEX_CONSTANTS {
	float projectionMatrix[4][4];
} OVERLAY_DXGI_VERTEX_CONSTANTS;

// The hardware encodes the shader output for these formats, so the shader has to give it linear.
static bool OverlayDX11FormatIsSrgb(DXGI_FORMAT format)
{
	return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
		|| format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
		|| format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

// A render target view may add the sRGB encoding over the format the swap chain was made with, so
// the two count as the same surface format.
static DXGI_FORMAT OverlayDX11FormatWithoutSrgb(DXGI_FORMAT format)
{
	switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
	}
	return format;
}

static void OverlayDX11StoreExpectedTarget(const DXGI_SWAP_CHAIN_DESC* swap_chain_description)
{
	overlay_dx11_expected_width = 0;
	overlay_dx11_expected_height = 0;
	overlay_dx11_expected_sample_count = 0;
	overlay_dx11_expected_format = DXGI_FORMAT_UNKNOWN;
	if (!swap_chain_description) {
		return;
	}
	overlay_dx11_expected_width = swap_chain_description->BufferDesc.Width;
	overlay_dx11_expected_height = swap_chain_description->BufferDesc.Height;
	overlay_dx11_expected_sample_count = swap_chain_description->SampleDesc.Count;
	overlay_dx11_expected_format = swap_chain_description->BufferDesc.Format;
	if ((!overlay_dx11_expected_width || !overlay_dx11_expected_height) && swap_chain_description->OutputWindow) {
		RECT clientRect;
		if (GetClientRect(swap_chain_description->OutputWindow, &clientRect)) {
			overlay_dx11_expected_width = clientRect.right - clientRect.left;
			overlay_dx11_expected_height = clientRect.bottom - clientRect.top;
		}
	}
}

bool OverlayDX11Init(ID3D11Device* d3d_device, const DXGI_SWAP_CHAIN_DESC* swap_chain_description)
{
	if (!d3d_device) {
		return false;
	}
	// The overlay drives the immediate context from the Title's render thread while the device
	// callbacks arrive on the thread that made the device. The runtime guards the device's own state
	// across threads unless this flag turns that off, and the flag is also the Title stating that it
	// drives Direct3D from one thread. Do not attach a second thread to it.
	if (d3d_device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
			, "%s the Title made a single threaded Direct3D 11 device, which cannot take calls from the render thread, so the built-in overlay does not attach to it."
			, __func__
		);
		return false;
	}
	OverlayDX11Shutdown();

	overlay_dx11_device = d3d_device;
	overlay_dx11_device->AddRef();
	overlay_dx11_device->GetImmediateContext(&overlay_dx11_context);
	if (!overlay_dx11_context) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s the D3D11 device has no immediate context."
			, __func__
		);
		OverlayDX11Shutdown();
		return false;
	}

	OverlayDX11StoreExpectedTarget(swap_chain_description);

	XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_INFO
		, "%s overlay renderer attached to a D3D11 device (feature level 0x%04x) presenting (%ux%u) format (%d) samples (%u)."
		, __func__
		, (uint32_t)overlay_dx11_device->GetFeatureLevel()
		, overlay_dx11_expected_width
		, overlay_dx11_expected_height
		, (int)overlay_dx11_expected_format
		, overlay_dx11_expected_sample_count
	);

	return true;
}

void OverlayDX11InvalidateDeviceObjects()
{
	if (overlay_dx11_vertex_buffer) {
		overlay_dx11_vertex_buffer->Release();
		overlay_dx11_vertex_buffer = 0;
	}
	if (overlay_dx11_index_buffer) {
		overlay_dx11_index_buffer->Release();
		overlay_dx11_index_buffer = 0;
	}
	OverlayDX11MarkFontsDirty();
}

static void OverlayDX11ReleasePipelineObjects()
{
	if (overlay_dx11_blend_state) {
		overlay_dx11_blend_state->Release();
		overlay_dx11_blend_state = 0;
	}
	if (overlay_dx11_depth_stencil_state) {
		overlay_dx11_depth_stencil_state->Release();
		overlay_dx11_depth_stencil_state = 0;
	}
	if (overlay_dx11_rasterizer_state) {
		overlay_dx11_rasterizer_state->Release();
		overlay_dx11_rasterizer_state = 0;
	}
	if (overlay_dx11_sampler) {
		overlay_dx11_sampler->Release();
		overlay_dx11_sampler = 0;
	}
	if (overlay_dx11_pixel_shader) {
		overlay_dx11_pixel_shader->Release();
		overlay_dx11_pixel_shader = 0;
	}
	if (overlay_dx11_pixel_shader_srgb) {
		overlay_dx11_pixel_shader_srgb->Release();
		overlay_dx11_pixel_shader_srgb = 0;
	}
	if (overlay_dx11_vertex_constant_buffer) {
		overlay_dx11_vertex_constant_buffer->Release();
		overlay_dx11_vertex_constant_buffer = 0;
	}
	if (overlay_dx11_input_layout) {
		overlay_dx11_input_layout->Release();
		overlay_dx11_input_layout = 0;
	}
	if (overlay_dx11_vertex_shader) {
		overlay_dx11_vertex_shader->Release();
		overlay_dx11_vertex_shader = 0;
	}
}

static void OverlayDX11ReleaseFrameRenderTarget()
{
	if (overlay_dx11_frame_render_target) {
		overlay_dx11_frame_render_target->Release();
		overlay_dx11_frame_render_target = 0;
	}
	overlay_dx11_frame_target_is_srgb = false;
}

void OverlayDX11Shutdown()
{
	OverlayDX11ReleaseFrameRenderTarget();
	OverlayDX11InvalidateDeviceObjects();
	OverlayDX11ReleasePipelineObjects();
	if (overlay_dx11_context) {
		overlay_dx11_context->Release();
		overlay_dx11_context = 0;
	}
	if (overlay_dx11_device) {
		overlay_dx11_device->Release();
		overlay_dx11_device = 0;
	}
	overlay_dx11_expected_width = 0;
	overlay_dx11_expected_height = 0;
	overlay_dx11_device_objects_failed = false;
	overlay_dx11_target_decision_logged = false;
}

void OverlayDX11OnDeviceReset(const DXGI_SWAP_CHAIN_DESC* swap_chain_description)
{
	if (!overlay_dx11_device) {
		return;
	}
	// A DXGI resize keeps every resource, so only the size that the render target is checked
	// against has to follow the swap chain.
	OverlayDX11StoreExpectedTarget(swap_chain_description);
	overlay_dx11_target_decision_logged = false;
}

void OverlayDX11MarkFontsDirty()
{
	if (overlay_dx11_font_texture_view) {
		overlay_dx11_font_texture_view->Release();
		overlay_dx11_font_texture_view = 0;
		if (ImGui::GetCurrentContext()) {
			ImGui::GetIO().Fonts->SetTexID(0);
		}
	}
}

void* OverlayDX11CreateTextureRgba(uint32_t width, uint32_t height, const uint8_t* rgba)
{
	if (!overlay_dx11_device || !rgba || !width || !height) {
		return 0;
	}

	D3D11_TEXTURE2D_DESC textureDescription;
	ZeroMemory(&textureDescription, sizeof(textureDescription));
	textureDescription.Width = width;
	textureDescription.Height = height;
	textureDescription.MipLevels = 1;
	textureDescription.ArraySize = 1;
	textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	textureDescription.SampleDesc.Count = 1;
	textureDescription.Usage = D3D11_USAGE_DEFAULT;
	textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA textureData;
	ZeroMemory(&textureData, sizeof(textureData));
	textureData.pSysMem = rgba;
	textureData.SysMemPitch = width * 4;

	ID3D11Texture2D* texture = 0;
	HRESULT resultCreateTexture = overlay_dx11_device->CreateTexture2D(&textureDescription, &textureData, &texture);
	if (FAILED(resultCreateTexture) || !texture) {
		XLLN_DEBUG_LOG_ECODE(resultCreateTexture, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s CreateTexture2D(%ux%u) failed."
			, __func__
			, width
			, height
		);
		return 0;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription;
	ZeroMemory(&viewDescription, sizeof(viewDescription));
	viewDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	viewDescription.Texture2D.MipLevels = 1;

	ID3D11ShaderResourceView* textureView = 0;
	HRESULT resultCreateView = overlay_dx11_device->CreateShaderResourceView(texture, &viewDescription, &textureView);
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

void OverlayDX11DestroyTexture(void* texture)
{
	if (texture) {
		((ID3D11ShaderResourceView*)texture)->Release();
	}
}

static bool OverlayDX11CreateFontsTexture()
{
	ImGuiIO &imguiIo = ImGui::GetIO();
	unsigned char* atlasPixels = 0;
	int atlasWidth = 0;
	int atlasHeight = 0;
	int bytesPerPixel = 0;
	imguiIo.Fonts->GetTexDataAsRGBA32(&atlasPixels, &atlasWidth, &atlasHeight, &bytesPerPixel);

	overlay_dx11_font_texture_view = (ID3D11ShaderResourceView*)OverlayDX11CreateTextureRgba((uint32_t)atlasWidth, (uint32_t)atlasHeight, atlasPixels);
	if (!overlay_dx11_font_texture_view) {
		return false;
	}

	imguiIo.Fonts->SetTexID((ImTextureID)overlay_dx11_font_texture_view);
	return true;
}

static bool OverlayDX11CreateDeviceObjects()
{
	if (!overlay_dx11_device) {
		return false;
	}
	OverlayDX11ReleasePipelineObjects();

	HRESULT resultCreate = overlay_dx11_device->CreateVertexShader(overlay_dxgi_vertex_shader, sizeof(overlay_dxgi_vertex_shader), 0, &overlay_dx11_vertex_shader);
	if (FAILED(resultCreate)) {
		// Shader model 4.0 needs feature level 10_0. There is no lower profile that this backend
		// and the Direct3D 10 one can share, so a device below that keeps the desktop window.
		XLLN_DEBUG_LOG_ECODE(resultCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR
			, "%s CreateVertexShader failed; the device reports feature level 0x%04x."
			, __func__
			, (uint32_t)overlay_dx11_device->GetFeatureLevel()
		);
		OverlayDX11ReleasePipelineObjects();
		return false;
	}

	const D3D11_INPUT_ELEMENT_DESC inputElements[3] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, uv), D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)offsetof(ImDrawVert, col), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	resultCreate = overlay_dx11_device->CreateInputLayout(inputElements, 3, overlay_dxgi_vertex_shader, sizeof(overlay_dxgi_vertex_shader), &overlay_dx11_input_layout);
	if (FAILED(resultCreate)) {
		XLLN_DEBUG_LOG_ECODE(resultCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR, "%s CreateInputLayout failed.", __func__);
		OverlayDX11ReleasePipelineObjects();
		return false;
	}

	D3D11_BUFFER_DESC constantBufferDescription;
	ZeroMemory(&constantBufferDescription, sizeof(constantBufferDescription));
	constantBufferDescription.ByteWidth = sizeof(OVERLAY_DXGI_VERTEX_CONSTANTS);
	constantBufferDescription.Usage = D3D11_USAGE_DYNAMIC;
	constantBufferDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	constantBufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	resultCreate = overlay_dx11_device->CreateBuffer(&constantBufferDescription, 0, &overlay_dx11_vertex_constant_buffer);
	if (FAILED(resultCreate)) {
		XLLN_DEBUG_LOG_ECODE(resultCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR, "%s CreateBuffer(constants) failed.", __func__);
		OverlayDX11ReleasePipelineObjects();
		return false;
	}

	resultCreate = overlay_dx11_device->CreatePixelShader(overlay_dxgi_pixel_shader, sizeof(overlay_dxgi_pixel_shader), 0, &overlay_dx11_pixel_shader);
	if (SUCCEEDED(resultCreate)) {
		resultCreate = overlay_dx11_device->CreatePixelShader(overlay_dxgi_pixel_shader_srgb, sizeof(overlay_dxgi_pixel_shader_srgb), 0, &overlay_dx11_pixel_shader_srgb);
	}
	if (FAILED(resultCreate)) {
		XLLN_DEBUG_LOG_ECODE(resultCreate, XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR, "%s CreatePixelShader failed.", __func__);
		OverlayDX11ReleasePipelineObjects();
		return false;
	}

	D3D11_BLEND_DESC blendDescription;
	ZeroMemory(&blendDescription, sizeof(blendDescription));
	blendDescription.AlphaToCoverageEnable = FALSE;
	blendDescription.RenderTarget[0].BlendEnable = TRUE;
	blendDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blendDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blendDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blendDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blendDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blendDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blendDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	overlay_dx11_device->CreateBlendState(&blendDescription, &overlay_dx11_blend_state);

	D3D11_RASTERIZER_DESC rasterizerDescription;
	ZeroMemory(&rasterizerDescription, sizeof(rasterizerDescription));
	rasterizerDescription.FillMode = D3D11_FILL_SOLID;
	rasterizerDescription.CullMode = D3D11_CULL_NONE;
	rasterizerDescription.ScissorEnable = TRUE;
	rasterizerDescription.DepthClipEnable = TRUE;
	overlay_dx11_device->CreateRasterizerState(&rasterizerDescription, &overlay_dx11_rasterizer_state);

	D3D11_DEPTH_STENCIL_DESC depthStencilDescription;
	ZeroMemory(&depthStencilDescription, sizeof(depthStencilDescription));
	depthStencilDescription.DepthEnable = FALSE;
	depthStencilDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthStencilDescription.DepthFunc = D3D11_COMPARISON_ALWAYS;
	depthStencilDescription.StencilEnable = FALSE;
	depthStencilDescription.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDescription.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDescription.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDescription.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	depthStencilDescription.BackFace = depthStencilDescription.FrontFace;
	overlay_dx11_device->CreateDepthStencilState(&depthStencilDescription, &overlay_dx11_depth_stencil_state);

	D3D11_SAMPLER_DESC samplerDescription;
	ZeroMemory(&samplerDescription, sizeof(samplerDescription));
	samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDescription.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	overlay_dx11_device->CreateSamplerState(&samplerDescription, &overlay_dx11_sampler);

	if (!overlay_dx11_blend_state || !overlay_dx11_rasterizer_state || !overlay_dx11_depth_stencil_state || !overlay_dx11_sampler) {
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_ERROR, "%s a pipeline state object could not be created.", __func__);
		OverlayDX11ReleasePipelineObjects();
		return false;
	}

	return true;
}

// Takes the render target that the title has bound and decides whether the overlay can draw on it.
// A DXGI title gives XLLN a description of its swap chain and never the swap chain, so there is no
// back buffer to ask for. A target of a size other than the one the title presents at is an
// intermediate buffer, and drawing on one would put the overlay somewhere the player never sees,
// or inside an image the title still has passes to run over.
static bool OverlayDX11CaptureRenderTarget()
{
	OverlayDX11ReleaseFrameRenderTarget();

	ID3D11RenderTargetView* renderTargetView = 0;
	overlay_dx11_context->OMGetRenderTargets(1, &renderTargetView, 0);
	if (!renderTargetView) {
		if (!overlay_dx11_target_decision_logged || overlay_dx11_target_decision_accepted) {
			overlay_dx11_target_decision_logged = true;
			overlay_dx11_target_decision_accepted = false;
			XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | XLLN_LOG_LEVEL_WARN
				, "%s the Title has no render target bound, so the overlay skips the frame."
				, __func__
			);
		}
		return false;
	}

	ID3D11Resource* renderTargetResource = 0;
	renderTargetView->GetResource(&renderTargetResource);
	ID3D11Texture2D* renderTargetTexture = 0;
	if (renderTargetResource) {
		renderTargetResource->QueryInterface(IID_ID3D11Texture2D, (void**)&renderTargetTexture);
		renderTargetResource->Release();
	}
	if (!renderTargetTexture) {
		renderTargetView->Release();
		return false;
	}

	D3D11_TEXTURE2D_DESC textureDescription;
	renderTargetTexture->GetDesc(&textureDescription);
	renderTargetTexture->Release();

	D3D11_RENDER_TARGET_VIEW_DESC viewDescription;
	renderTargetView->GetDesc(&viewDescription);

	// Size alone cannot tell the back buffer from an intermediate buffer, because a title renders
	// its scene at the size it presents at. Sample count and surface format are what separate
	// them: a multisampled scene buffer in front of a single sampled swap chain matches on size
	// and on nothing else.
	const char* decision = "the overlay draws on it";
	bool accepted = true;
	if ((overlay_dx11_expected_width && overlay_dx11_expected_height)
		&& (textureDescription.Width != overlay_dx11_expected_width || textureDescription.Height != overlay_dx11_expected_height)) {
		accepted = false;
		decision = "the size is not the one it presents, so the overlay skips the frame";
	}
	else if (overlay_dx11_expected_sample_count && textureDescription.SampleDesc.Count != overlay_dx11_expected_sample_count) {
		accepted = false;
		decision = "the sample count is not the one it presents, so the overlay skips the frame";
	}
	else if (overlay_dx11_expected_format != DXGI_FORMAT_UNKNOWN
		&& OverlayDX11FormatWithoutSrgb(viewDescription.Format) != OverlayDX11FormatWithoutSrgb(overlay_dx11_expected_format)) {
		accepted = false;
		decision = "the format is not the one it presents, so the overlay skips the frame";
	}

	if (!overlay_dx11_target_decision_logged || overlay_dx11_target_decision_accepted != accepted) {
		overlay_dx11_target_decision_logged = true;
		overlay_dx11_target_decision_accepted = accepted;
		XLLN_DEBUG_LOG(XLLN_LOG_CONTEXT_XLIVELESSNESS | (accepted ? XLLN_LOG_LEVEL_INFO : XLLN_LOG_LEVEL_WARN)
			, "%s the Title has a (%ux%u) format (%d) samples (%u) target bound, and presents (%ux%u) format (%d) samples (%u): %s."
			, __func__
			, textureDescription.Width
			, textureDescription.Height
			, (int)viewDescription.Format
			, textureDescription.SampleDesc.Count
			, overlay_dx11_expected_width
			, overlay_dx11_expected_height
			, (int)overlay_dx11_expected_format
			, overlay_dx11_expected_sample_count
			, decision
		);
	}

	if (!accepted) {
		renderTargetView->Release();
		return false;
	}

	overlay_dx11_frame_render_target = renderTargetView;
	overlay_dx11_frame_target_is_srgb = OverlayDX11FormatIsSrgb(viewDescription.Format);
	return true;
}

bool OverlayDX11TargetAccepted()
{
	// A target the backend has not judged yet counts as good, so nothing stands down before the
	// first decision.
	return !overlay_dx11_target_decision_logged || overlay_dx11_target_decision_accepted;
}

bool OverlayDX11BeginFrame()
{
	if (!overlay_dx11_device || !overlay_dx11_context) {
		return false;
	}
	if (overlay_dx11_device_objects_failed) {
		return false;
	}
	if (!overlay_dx11_vertex_shader && !OverlayDX11CreateDeviceObjects()) {
		overlay_dx11_device_objects_failed = true;
		return false;
	}
	if (!overlay_dx11_font_texture_view && !OverlayDX11CreateFontsTexture()) {
		return false;
	}
	return true;
}

static void OverlayDX11SetupRenderState(ImDrawData* draw_data)
{
	ID3D11DeviceContext* context = overlay_dx11_context;

	D3D11_VIEWPORT viewport;
	ZeroMemory(&viewport, sizeof(viewport));
	viewport.Width = draw_data->DisplaySize.x;
	viewport.Height = draw_data->DisplaySize.y;
	viewport.MinDepth = 0.f;
	viewport.MaxDepth = 1.f;
	viewport.TopLeftX = 0.f;
	viewport.TopLeftY = 0.f;
	context->RSSetViewports(1, &viewport);

	const unsigned int vertexStride = sizeof(ImDrawVert);
	const unsigned int vertexOffset = 0;
	context->IASetInputLayout(overlay_dx11_input_layout);
	context->IASetVertexBuffers(0, 1, &overlay_dx11_vertex_buffer, &vertexStride, &vertexOffset);
	context->IASetIndexBuffer(overlay_dx11_index_buffer, sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(overlay_dx11_vertex_shader, 0, 0);
	context->VSSetConstantBuffers(0, 1, &overlay_dx11_vertex_constant_buffer);
	context->PSSetShader(overlay_dx11_frame_target_is_srgb ? overlay_dx11_pixel_shader_srgb : overlay_dx11_pixel_shader, 0, 0);
	context->PSSetSamplers(0, 1, &overlay_dx11_sampler);
	// Clear the geometry, hull and domain stages for the overlay draw. The geometry shader runs on the
	// triangle list; a bound hull shader makes a non-patch draw invalid, so the title's tessellation
	// shaders have to be cleared here and put back below. The compute stage cannot affect a Draw and is
	// left untouched.
	context->GSSetShader(0, 0, 0);
	context->HSSetShader(0, 0, 0);
	context->DSSetShader(0, 0, 0);

	const float blendFactor[4] = { 0.f, 0.f, 0.f, 0.f };
	context->OMSetBlendState(overlay_dx11_blend_state, blendFactor, 0xffffffff);
	context->OMSetDepthStencilState(overlay_dx11_depth_stencil_state, 0);
	context->RSSetState(overlay_dx11_rasterizer_state);

	// Only the render target that was checked, and no depth buffer. A title that binds several
	// targets at once must not get the overlay written into the others.
	context->OMSetRenderTargets(1, &overlay_dx11_frame_render_target, 0);
}

void OverlayDX11RenderDrawData(ImDrawData* draw_data)
{
	if (!draw_data || !overlay_dx11_context) {
		return;
	}
	if (draw_data->DisplaySize.x <= 0.f || draw_data->DisplaySize.y <= 0.f) {
		return;
	}
	if (!OverlayDX11CaptureRenderTarget()) {
		return;
	}

	ID3D11DeviceContext* context = overlay_dx11_context;

	if (!overlay_dx11_vertex_buffer || overlay_dx11_vertex_buffer_size < draw_data->TotalVtxCount) {
		if (overlay_dx11_vertex_buffer) {
			overlay_dx11_vertex_buffer->Release();
			overlay_dx11_vertex_buffer = 0;
		}
		overlay_dx11_vertex_buffer_size = draw_data->TotalVtxCount + 5000;
		D3D11_BUFFER_DESC bufferDescription;
		ZeroMemory(&bufferDescription, sizeof(bufferDescription));
		bufferDescription.Usage = D3D11_USAGE_DYNAMIC;
		bufferDescription.ByteWidth = overlay_dx11_vertex_buffer_size * sizeof(ImDrawVert);
		bufferDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(overlay_dx11_device->CreateBuffer(&bufferDescription, 0, &overlay_dx11_vertex_buffer))) {
			OverlayDX11ReleaseFrameRenderTarget();
			return;
		}
	}
	if (!overlay_dx11_index_buffer || overlay_dx11_index_buffer_size < draw_data->TotalIdxCount) {
		if (overlay_dx11_index_buffer) {
			overlay_dx11_index_buffer->Release();
			overlay_dx11_index_buffer = 0;
		}
		overlay_dx11_index_buffer_size = draw_data->TotalIdxCount + 10000;
		D3D11_BUFFER_DESC bufferDescription;
		ZeroMemory(&bufferDescription, sizeof(bufferDescription));
		bufferDescription.Usage = D3D11_USAGE_DYNAMIC;
		bufferDescription.ByteWidth = overlay_dx11_index_buffer_size * sizeof(ImDrawIdx);
		bufferDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
		bufferDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(overlay_dx11_device->CreateBuffer(&bufferDescription, 0, &overlay_dx11_index_buffer))) {
			OverlayDX11ReleaseFrameRenderTarget();
			return;
		}
	}

	D3D11_MAPPED_SUBRESOURCE mappedVertices;
	D3D11_MAPPED_SUBRESOURCE mappedIndices;
	if (FAILED(context->Map(overlay_dx11_vertex_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedVertices))) {
		OverlayDX11ReleaseFrameRenderTarget();
		return;
	}
	if (FAILED(context->Map(overlay_dx11_index_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedIndices))) {
		context->Unmap(overlay_dx11_vertex_buffer, 0);
		OverlayDX11ReleaseFrameRenderTarget();
		return;
	}
	ImDrawVert* vertexDestination = (ImDrawVert*)mappedVertices.pData;
	ImDrawIdx* indexDestination = (ImDrawIdx*)mappedIndices.pData;
	for (int iCmdList = 0; iCmdList < draw_data->CmdListsCount; iCmdList++) {
		const ImDrawList* cmdList = draw_data->CmdLists[iCmdList];
		memcpy(vertexDestination, cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
		memcpy(indexDestination, cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));
		vertexDestination += cmdList->VtxBuffer.Size;
		indexDestination += cmdList->IdxBuffer.Size;
	}
	context->Unmap(overlay_dx11_vertex_buffer, 0);
	context->Unmap(overlay_dx11_index_buffer, 0);

	{
		D3D11_MAPPED_SUBRESOURCE mappedConstants;
		if (FAILED(context->Map(overlay_dx11_vertex_constant_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedConstants))) {
			OverlayDX11ReleaseFrameRenderTarget();
			return;
		}
		OVERLAY_DXGI_VERTEX_CONSTANTS* constants = (OVERLAY_DXGI_VERTEX_CONSTANTS*)mappedConstants.pData;
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
		context->Unmap(overlay_dx11_vertex_constant_buffer, 0);
	}

	OVERLAY_DX11_STATE state;
	state.scissorRectCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	state.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetScissorRects(&state.scissorRectCount, state.scissorRects);
	context->RSGetViewports(&state.viewportCount, state.viewports);
	context->RSGetState(&state.rasterizerState);
	context->OMGetBlendState(&state.blendState, state.blendFactor, &state.sampleMask);
	context->OMGetDepthStencilState(&state.depthStencilState, &state.stencilReference);
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.renderTargets, &state.depthStencilView);
	context->PSGetShaderResources(0, 1, &state.pixelShaderResource);
	context->PSGetSamplers(0, 1, &state.pixelShaderSampler);
	state.pixelShaderInstanceCount = 256;
	state.vertexShaderInstanceCount = 256;
	state.geometryShaderInstanceCount = 256;
	context->PSGetShader(&state.pixelShader, state.pixelShaderInstances, &state.pixelShaderInstanceCount);
	context->VSGetShader(&state.vertexShader, state.vertexShaderInstances, &state.vertexShaderInstanceCount);
	context->VSGetConstantBuffers(0, 1, &state.vertexShaderConstantBuffer);
	context->GSGetShader(&state.geometryShader, state.geometryShaderInstances, &state.geometryShaderInstanceCount);
	state.hullShaderInstanceCount = 256;
	state.domainShaderInstanceCount = 256;
	context->HSGetShader(&state.hullShader, state.hullShaderInstances, &state.hullShaderInstanceCount);
	context->DSGetShader(&state.domainShader, state.domainShaderInstances, &state.domainShaderInstanceCount);
	context->IAGetPrimitiveTopology(&state.primitiveTopology);
	context->IAGetIndexBuffer(&state.indexBuffer, &state.indexBufferFormat, &state.indexBufferOffset);
	context->IAGetVertexBuffers(0, 1, &state.vertexBuffer, &state.vertexBufferStride, &state.vertexBufferOffset);
	context->IAGetInputLayout(&state.inputLayout);

	OverlayDX11SetupRenderState(draw_data);

	int globalVertexOffset = 0;
	int globalIndexOffset = 0;
	const ImVec2 clipOffset = draw_data->DisplayPos;
	for (int iCmdList = 0; iCmdList < draw_data->CmdListsCount; iCmdList++) {
		const ImDrawList* cmdList = draw_data->CmdLists[iCmdList];
		for (int iCmd = 0; iCmd < cmdList->CmdBuffer.Size; iCmd++) {
			const ImDrawCmd* drawCommand = &cmdList->CmdBuffer[iCmd];
			if (drawCommand->UserCallback) {
				if (drawCommand->UserCallback == ImDrawCallback_ResetRenderState) {
					OverlayDX11SetupRenderState(draw_data);
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

			const D3D11_RECT scissorRect = { (LONG)clipMinX, (LONG)clipMinY, (LONG)clipMaxX, (LONG)clipMaxY };
			context->RSSetScissorRects(1, &scissorRect);

			ID3D11ShaderResourceView* textureView = (ID3D11ShaderResourceView*)drawCommand->GetTexID();
			context->PSSetShaderResources(0, 1, &textureView);
			context->DrawIndexed(drawCommand->ElemCount, drawCommand->IdxOffset + globalIndexOffset, drawCommand->VtxOffset + globalVertexOffset);
		}
		globalIndexOffset += cmdList->IdxBuffer.Size;
		globalVertexOffset += cmdList->VtxBuffer.Size;
	}

	context->RSSetScissorRects(state.scissorRectCount, state.scissorRects);
	context->RSSetViewports(state.viewportCount, state.viewports);
	context->RSSetState(state.rasterizerState);
	if (state.rasterizerState) {
		state.rasterizerState->Release();
	}
	context->OMSetBlendState(state.blendState, state.blendFactor, state.sampleMask);
	if (state.blendState) {
		state.blendState->Release();
	}
	context->OMSetDepthStencilState(state.depthStencilState, state.stencilReference);
	if (state.depthStencilState) {
		state.depthStencilState->Release();
	}
	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.renderTargets, state.depthStencilView);
	for (int iRenderTarget = 0; iRenderTarget < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; iRenderTarget++) {
		if (state.renderTargets[iRenderTarget]) {
			state.renderTargets[iRenderTarget]->Release();
		}
	}
	if (state.depthStencilView) {
		state.depthStencilView->Release();
	}
	context->PSSetShaderResources(0, 1, &state.pixelShaderResource);
	if (state.pixelShaderResource) {
		state.pixelShaderResource->Release();
	}
	context->PSSetSamplers(0, 1, &state.pixelShaderSampler);
	if (state.pixelShaderSampler) {
		state.pixelShaderSampler->Release();
	}
	context->PSSetShader(state.pixelShader, state.pixelShaderInstances, state.pixelShaderInstanceCount);
	if (state.pixelShader) {
		state.pixelShader->Release();
	}
	for (UINT iInstance = 0; iInstance < state.pixelShaderInstanceCount; iInstance++) {
		if (state.pixelShaderInstances[iInstance]) {
			state.pixelShaderInstances[iInstance]->Release();
		}
	}
	context->VSSetShader(state.vertexShader, state.vertexShaderInstances, state.vertexShaderInstanceCount);
	if (state.vertexShader) {
		state.vertexShader->Release();
	}
	for (UINT iInstance = 0; iInstance < state.vertexShaderInstanceCount; iInstance++) {
		if (state.vertexShaderInstances[iInstance]) {
			state.vertexShaderInstances[iInstance]->Release();
		}
	}
	context->VSSetConstantBuffers(0, 1, &state.vertexShaderConstantBuffer);
	if (state.vertexShaderConstantBuffer) {
		state.vertexShaderConstantBuffer->Release();
	}
	context->GSSetShader(state.geometryShader, state.geometryShaderInstances, state.geometryShaderInstanceCount);
	if (state.geometryShader) {
		state.geometryShader->Release();
	}
	for (UINT iInstance = 0; iInstance < state.geometryShaderInstanceCount; iInstance++) {
		if (state.geometryShaderInstances[iInstance]) {
			state.geometryShaderInstances[iInstance]->Release();
		}
	}
	context->HSSetShader(state.hullShader, state.hullShaderInstances, state.hullShaderInstanceCount);
	if (state.hullShader) {
		state.hullShader->Release();
	}
	for (UINT iInstance = 0; iInstance < state.hullShaderInstanceCount; iInstance++) {
		if (state.hullShaderInstances[iInstance]) {
			state.hullShaderInstances[iInstance]->Release();
		}
	}
	context->DSSetShader(state.domainShader, state.domainShaderInstances, state.domainShaderInstanceCount);
	if (state.domainShader) {
		state.domainShader->Release();
	}
	for (UINT iInstance = 0; iInstance < state.domainShaderInstanceCount; iInstance++) {
		if (state.domainShaderInstances[iInstance]) {
			state.domainShaderInstances[iInstance]->Release();
		}
	}
	context->IASetPrimitiveTopology(state.primitiveTopology);
	context->IASetIndexBuffer(state.indexBuffer, state.indexBufferFormat, state.indexBufferOffset);
	if (state.indexBuffer) {
		state.indexBuffer->Release();
	}
	context->IASetVertexBuffers(0, 1, &state.vertexBuffer, &state.vertexBufferStride, &state.vertexBufferOffset);
	if (state.vertexBuffer) {
		state.vertexBuffer->Release();
	}
	context->IASetInputLayout(state.inputLayout);
	if (state.inputLayout) {
		state.inputLayout->Release();
	}

	OverlayDX11ReleaseFrameRenderTarget();
}
