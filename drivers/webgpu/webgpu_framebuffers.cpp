#include "rendering_device_driver_webgpu.h"

#include <algorithm>

using namespace godot::webgpu;

bool RenderPassLayout::requires_synthetic_target(uint32_t p_subpass) const {
	const auto &subpass = subpasses[p_subpass];
	if (subpass.depth_stencil_reference.attachment != RDD::AttachmentReference::UNUSED) {
		return false;
	}
	return std::none_of(subpass.color_references.begin(), subpass.color_references.end(), [](const RDD::AttachmentReference &p_ref) {
		return p_ref.attachment != RDD::AttachmentReference::UNUSED;
	});
}

RDD::RenderPassID RenderingDeviceDriverWebGPU::render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses,
		VectorView<SubpassDependency> p_dependencies, uint32_t p_view_count, AttachmentReference p_density_map) {
	ERR_FAIL_COND_V_MSG(p_view_count != 1 || p_density_map.attachment != AttachmentReference::UNUSED, {}, "WebGPU does not support this multiview or fragment density pass.");
	auto pass = std::make_shared<RenderPassLayout>();
	pass->view_count = p_view_count;
	for (uint32_t i = 0; i < p_attachments.size(); ++i) {
		pass->attachments.push_back(p_attachments[i]);
	}
	for (uint32_t i = 0; i < p_subpasses.size(); ++i) {
		pass->subpasses.push_back(p_subpasses[i]);
	}
	for (uint32_t i = 0; i < p_dependencies.size(); ++i) {
		pass->dependencies.push_back(p_dependencies[i]);
	}
	return resource_id<RenderPassID>(std::move(pass));
}

void RenderingDeviceDriverWebGPU::render_pass_free(RenderPassID p_render_pass) {
	free_id<RenderPassLayout>(p_render_pass);
}

RDD::FramebufferID RenderingDeviceDriverWebGPU::framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) {
	state->assert_owner();
	auto fb = std::make_shared<FramebufferResource>();
	fb->pass = resource<RenderPassLayout>(p_render_pass);
	fb->width = p_width;
	fb->height = p_height;
	for (uint32_t i = 0; i < p_attachments.size(); ++i) {
		const TextureID texture = p_attachments[i];
		fb->attachments.push_back(texture ? resource<TextureResource>(texture) : nullptr);
	}
	for (uint32_t i = 0; i < fb->pass->subpasses.size(); ++i) {
		if (fb->pass->requires_synthetic_target(i)) {
			TextureFormat info;
			info.format = DATA_FORMAT_R8_UNORM;
			info.width = p_width;
			info.height = p_height;
			info.usage_bits = TEXTURE_USAGE_COLOR_ATTACHMENT_BIT;
			TextureID synthetic = texture_create(info, {});
			ERR_FAIL_COND_V(!synthetic, {});
			fb->synthetic_target = resource<TextureResource>(synthetic);
			texture_free(synthetic);
			info.samples = TEXTURE_SAMPLES_4;
			TextureID synthetic_msaa = texture_create(info, {});
			ERR_FAIL_COND_V(!synthetic_msaa, {});
			fb->synthetic_target_msaa = resource<TextureResource>(synthetic_msaa);
			texture_free(synthetic_msaa);
			break;
		}
	}
	return resource_id<FramebufferID>(std::move(fb));
}

void RenderingDeviceDriverWebGPU::framebuffer_free(FramebufferID p_framebuffer) {
	state->assert_owner();
	free_id<FramebufferResource>(p_framebuffer);
}

RDD::SwapChainID RenderingDeviceDriverWebGPU::swap_chain_create(RenderingContextDriver::SurfaceID p_surface) {
	state->assert_owner();
	auto chain = std::make_shared<SwapChainResource>();
	chain->surface = RenderingContextDriverWebGPU::surface_state(p_surface);
	DEV_ASSERT(chain->surface->state == state);
	return resource_id<SwapChainID>(std::move(chain));
}

Error RenderingDeviceDriverWebGPU::swap_chain_resize(CommandQueueID, SwapChainID p_swap_chain, uint32_t) {
	state->assert_owner();
	auto &chain = *resource<SwapChainResource>(p_swap_chain);
	swap_chain_drop_frame(p_swap_chain);
	const Error status = chain.surface->configure();
	if (status != OK) {
		return status;
	}
	const DataFormat format = swap_chain_get_format(p_swap_chain);
	if (chain.render_pass && resource<RenderPassLayout>(chain.render_pass)->attachments[0].format == format) {
		return OK;
	}
	if (chain.render_pass) {
		render_pass_free(chain.render_pass);
	}
	auto pass = std::make_shared<RenderPassLayout>();
	Attachment color;
	color.format = format;
	color.samples = TEXTURE_SAMPLES_1;
	color.load_op = ATTACHMENT_LOAD_OP_CLEAR;
	color.store_op = ATTACHMENT_STORE_OP_STORE;
	pass->attachments.push_back(color);
	Subpass subpass;
	AttachmentReference ref;
	ref.attachment = 0;
	ref.layout = TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ref.aspect = TEXTURE_ASPECT_COLOR_BIT;
	subpass.color_references.push_back(ref);
	pass->subpasses.push_back(std::move(subpass));
	chain.render_pass = resource_id<RenderPassID>(std::move(pass));
	return OK;
}

RDD::FramebufferID RenderingDeviceDriverWebGPU::swap_chain_acquire_framebuffer(CommandQueueID, SwapChainID p_swap_chain, bool &r_resize_required) {
	state->assert_owner();
	auto &chain = *resource<SwapChainResource>(p_swap_chain);
	DEV_ASSERT(!chain.current);
	TextureHandle texture;
	const auto status = chain.surface->acquire(texture);
	r_resize_required = chain.surface->needs_resize;
	if (status == WGPUSurfaceGetCurrentTextureStatus_Error) {
		ERR_FAIL_V_MSG({}, "WebGPU canvas failed to acquire a current texture.");
	}
	if (!texture) {
		return {};
	}
	DEV_ASSERT(chain.render_pass);
	TextureID tex_id = texture_create_from_extension(reinterpret_cast<uint64_t>(texture.get()), TEXTURE_TYPE_2D, swap_chain_get_format(p_swap_chain), 1, false, 1);
	ERR_FAIL_COND_V(!tex_id, {});
	auto fb = std::make_shared<FramebufferResource>();
	fb->pass = resource<RenderPassLayout>(chain.render_pass);
	fb->attachments.push_back(resource<TextureResource>(tex_id));
	fb->width = chain.surface->width;
	fb->height = chain.surface->height;
	texture_free(tex_id);
	chain.current = resource_id<FramebufferID>(std::move(fb));
	return chain.current;
}

RDD::RenderPassID RenderingDeviceDriverWebGPU::swap_chain_get_render_pass(SwapChainID p_swap_chain) {
	return resource<SwapChainResource>(p_swap_chain)->render_pass;
}

RDD::DataFormat RenderingDeviceDriverWebGPU::swap_chain_get_format(SwapChainID p_swap_chain) {
	switch (resource<SwapChainResource>(p_swap_chain)->surface->format) {
		case WGPUTextureFormat_RGBA8Unorm:
			return DATA_FORMAT_R8G8B8A8_UNORM;
		case WGPUTextureFormat_BGRA8Unorm:
			return DATA_FORMAT_B8G8R8A8_UNORM;
		case WGPUTextureFormat_RGBA16Float:
			return DATA_FORMAT_R16G16B16A16_SFLOAT;
		default:
			CRASH_NOW_MSG("Unexpected negotiated WebGPU canvas format.");
	}
}

void RenderingDeviceDriverWebGPU::swap_chain_drop_frame(SwapChainID p_swap_chain) {
	state->assert_owner();
	auto &chain = *resource<SwapChainResource>(p_swap_chain);
	if (chain.current) {
		framebuffer_free(chain.current);
		chain.current = FramebufferID{};
	}
}

void RenderingDeviceDriverWebGPU::swap_chain_free(SwapChainID p_swap_chain) {
	state->assert_owner();
	swap_chain_drop_frame(p_swap_chain);
	const auto &chain = *resource<SwapChainResource>(p_swap_chain);
	if (chain.render_pass) {
		render_pass_free(chain.render_pass);
	}
	free_id<SwapChainResource>(p_swap_chain);
}
