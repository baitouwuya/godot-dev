#include "rendering_context_driver_webgpu.h"

#include <algorithm>

namespace godot::webgpu {

SurfaceState::~SurfaceState() {
	state->assert_owner();
	if (configured) {
		wgpuSurfaceUnconfigure(surface.get());
	}
}

Error SurfaceState::create() {
	state->assert_owner();
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
	source.selector = wg_string(canvas_selector);
	WGPUSurfaceDescriptor desc = WGPU_SURFACE_DESCRIPTOR_INIT;
	desc.nextInChain = &source.chain;
	SurfaceHandle candidate(wgpuInstanceCreateSurface(state->instance.get(), &desc));
	ERR_FAIL_COND_V_MSG(!candidate, ERR_CANT_CREATE, "Cannot create the requested WebGPU canvas surface.");
	WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
	// emdawn's browser implementation ignores the adapter parameter and orders
	// formats by navigator.gpu.getPreferredCanvasFormat().
	ERR_FAIL_COND_V(wgpuSurfaceGetCapabilities(candidate.get(), nullptr, &caps) != WGPUStatus_Success, ERR_UNAVAILABLE);
	format = caps.formatCount ? caps.formats[0] : WGPUTextureFormat_Undefined;
	present_mode = caps.presentModeCount ? caps.presentModes[0] : WGPUPresentMode_Undefined;
	const auto preferred = wants_transparency ? WGPUCompositeAlphaMode_Premultiplied : WGPUCompositeAlphaMode_Opaque;
	alpha = caps.alphaModeCount ? caps.alphaModes[0] : WGPUCompositeAlphaMode_Auto;
	for (size_t i = 0; i < caps.alphaModeCount; ++i) {
		if (caps.alphaModes[i] == preferred) {
			alpha = preferred;
			break;
		}
	}
	wgpuSurfaceCapabilitiesFreeMembers(caps);
	ERR_FAIL_COND_V(format == WGPUTextureFormat_Undefined || present_mode == WGPUPresentMode_Undefined, ERR_UNAVAILABLE);
	if (configured) {
		wgpuSurfaceUnconfigure(surface.get());
	}
	surface = std::move(candidate);
	configured = false;
	needs_resize = true;
	return OK;
}

Error SurfaceState::configure() {
	state->assert_owner();
	if (!width || !height) {
		if (configured) {
			wgpuSurfaceUnconfigure(surface.get());
			configured = false;
		}
		return ERR_SKIP;
	}
	ERR_FAIL_COND_V_MSG(width > state->limits.maxTextureDimension2D || height > state->limits.maxTextureDimension2D, ERR_INVALID_PARAMETER,
			"WebGPU canvas size exceeds maxTextureDimension2D.");
	WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
	config.device = state->device.get();
	config.format = format;
	config.width = width;
	config.height = height;
	config.alphaMode = alpha;
	config.presentMode = present_mode;
	wgpuSurfaceConfigure(surface.get(), &config);
	configured = true;
	needs_resize = false;
	return OK;
}

WGPUSurfaceGetCurrentTextureStatus SurfaceState::acquire(TextureHandle &r_texture) {
	state->assert_owner();
	DEV_ASSERT(!r_texture);
	if (!configured || needs_resize) {
		return WGPUSurfaceGetCurrentTextureStatus_Outdated;
	}
	WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
	wgpuSurfaceGetCurrentTexture(surface.get(), &current);
	TextureHandle acquired(current.texture);
	switch (current.status) {
		case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
			needs_resize = true;
			[[fallthrough]];
		case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
			ERR_FAIL_COND_V(!acquired, WGPUSurfaceGetCurrentTextureStatus_Error);
			r_texture = std::move(acquired);
			break;
		case WGPUSurfaceGetCurrentTextureStatus_Lost: {
			needs_resize = true;
			const Error error = create();
			ERR_FAIL_COND_V(error != OK, WGPUSurfaceGetCurrentTextureStatus_Error);
		} break;
		case WGPUSurfaceGetCurrentTextureStatus_Outdated:
			needs_resize = true;
			break;
		default:
			break;
	}
	return current.status;
}

} // namespace godot::webgpu

using godot::webgpu::SurfaceState;

const std::shared_ptr<SurfaceState> &RenderingContextDriverWebGPU::surface_state(SurfaceID p_surface) {
	DEV_ASSERT(p_surface);
	const auto &surface = *reinterpret_cast<std::shared_ptr<SurfaceState> *>(p_surface);
	surface->state->assert_owner();
	return surface;
}

RenderingContextDriver::SurfaceID RenderingContextDriverWebGPU::surface_create(const void *p_platform_data) {
	state->assert_owner();
	const auto *platform = static_cast<const godot::webgpu::WebGPUSurfacePlatformData *>(p_platform_data);
	auto surface = std::make_shared<SurfaceState>();
	surface->state = state;
	surface->canvas_selector = platform && platform->canvas_selector && *platform->canvas_selector ? platform->canvas_selector : "#canvas";
	surface->wants_transparency = platform && platform->premultiplied_alpha;
	if (surface->create() != OK) {
		return 0;
	}
	return reinterpret_cast<SurfaceID>(memnew(std::shared_ptr<SurfaceState>(std::move(surface))));
}

void RenderingContextDriverWebGPU::surface_destroy(SurfaceID p_surface) {
	state->assert_owner();
	DEV_ASSERT(p_surface);
	memdelete(reinterpret_cast<std::shared_ptr<SurfaceState> *>(p_surface));
}

void RenderingContextDriverWebGPU::surface_set_size(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) {
	auto &surface = *surface_state(p_surface);
	surface.needs_resize |= surface.width != p_width || surface.height != p_height;
	surface.width = p_width;
	surface.height = p_height;
}

void RenderingContextDriverWebGPU::surface_set_vsync_mode(SurfaceID p_surface, DisplayServerEnums::VSyncMode p_vsync_mode) {
	surface_state(p_surface);
	if (p_vsync_mode != DisplayServerEnums::VSYNC_ENABLED) {
		WARN_PRINT_ONCE("WebGPU canvas presentation is browser-paced and supports only FIFO.");
	}
}

DisplayServerEnums::VSyncMode RenderingContextDriverWebGPU::surface_get_vsync_mode(SurfaceID p_surface) const {
	surface_state(p_surface);
	return DisplayServerEnums::VSYNC_ENABLED;
}

void RenderingContextDriverWebGPU::surface_set_hdr_output_enabled(SurfaceID p_surface, bool p_enabled) {
	surface_state(p_surface);
	if (p_enabled) {
		WARN_PRINT_ONCE("HDR canvas output is not implemented by this WebGPU backend.");
	}
}

uint32_t RenderingContextDriverWebGPU::surface_get_width(SurfaceID p_surface) const {
	return surface_state(p_surface)->width;
}
uint32_t RenderingContextDriverWebGPU::surface_get_height(SurfaceID p_surface) const {
	return surface_state(p_surface)->height;
}
bool RenderingContextDriverWebGPU::surface_get_needs_resize(SurfaceID p_surface) const {
	return surface_state(p_surface)->needs_resize;
}
void RenderingContextDriverWebGPU::surface_set_needs_resize(SurfaceID p_surface, bool p_needs_resize) {
	surface_state(p_surface)->needs_resize = p_needs_resize;
}
