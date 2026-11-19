#include "webgpu_commands.h"

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

bool compatible_usage(ScopeUse p_a, ScopeUse p_b) {
	return p_a == p_b || (p_a <= ScopeUse::STORAGE_READ && p_b <= ScopeUse::STORAGE_READ);
}

WGPULoadOp attachment_load(bool p_started, RDD::AttachmentLoadOp p_load_op) {
	return p_started || p_load_op == RDD::ATTACHMENT_LOAD_OP_LOAD ? WGPULoadOp_Load : WGPULoadOp_Clear;
}

} // namespace

bool UsageScope::add(ScopeKey p_key, ScopeUse p_use) {
	const auto [it, inserted] = usages.emplace(p_key, p_use);
	return inserted || compatible_usage(it->second, p_use);
}

bool UsageScope::add_texture(const TextureResource &p_texture, ScopeUse p_use, uint32_t p_mips, uint32_t p_layers, WGPUTextureAspect p_aspect) {
	const auto &tex = *p_texture.backing;
	if (tex.atomic_buffer) {
		return add({ &tex }, p_use);
	}
	if (p_aspect == WGPUTextureAspect_All && (p_texture.format.depth || p_texture.format.stencil)) {
		return (!p_texture.format.depth || add_texture(p_texture, p_use, p_mips, p_layers, WGPUTextureAspect_DepthOnly)) &&
				(!p_texture.format.stencil || add_texture(p_texture, p_use, p_mips, p_layers, WGPUTextureAspect_StencilOnly));
	}
	for (uint32_t m = 0; m < p_mips; ++m) {
		for (uint32_t l = 0; l < p_layers; ++l) {
			if (!add({ &tex, p_aspect, p_texture.base_mip + m, p_texture.base_layer + l }, p_use)) {
				return false;
			}
		}
	}
	return true;
}

bool UsageScope::compatible(const UsageScope &p_other) const {
	for (const auto &[key, use] : p_other.usages) {
		const auto it = usages.find(key);
		if (it != usages.end() && !compatible_usage(it->second, use)) {
			return false;
		}
	}
	return true;
}

void UsageScope::merge(const UsageScope &p_other) {
	usages.insert(p_other.usages.begin(), p_other.usages.end());
}

void CommandRecording::open_render(VectorView<RDD::AttachmentClear> p_clears) {
	DEV_ASSERT(layout && framebuffer);
	const bool synthetic = layout->requires_synthetic_target(subpass);
	const uint32_t samples = synthetic && pipeline ? pipeline->sample_count : 1;
	if (render && render_samples == samples) {
		return;
	}
	end_render();
	render_samples = samples;
	end_compute();
	const auto &pass = layout->subpasses[subpass];
	std::vector<WGPURenderPassColorAttachment> colors;
	auto color = [&](uint32_t p_index, uint32_t p_slot) {
		WGPURenderPassColorAttachment desc = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		if (p_index != RDD::AttachmentReference::UNUSED) {
			const auto &tex = framebuffer->attachments[p_index];
			desc.view = tex->attachment.get();
			desc.loadOp = attachment_load(attachment_started[p_index], layout->attachments[p_index].load_op);
			// Contents must survive a potential usage-scope split. Resolve is
			// recorded once, after the last physical pass of this subpass.
			desc.storeOp = WGPUStoreOp_Store;
			if (desc.loadOp == WGPULoadOp_Clear && layout->attachments[p_index].load_op == RDD::ATTACHMENT_LOAD_OP_CLEAR) {
				desc.clearValue = wg_color(clear_values[p_index].color);
			}
			for (uint32_t i = 0; i < p_clears.size(); ++i) {
				if (p_clears[i].aspect.has_flag(RDD::TEXTURE_ASPECT_COLOR_BIT) && p_clears[i].color_attachment == p_slot) {
					desc.loadOp = WGPULoadOp_Clear;
					desc.clearValue = wg_color(p_clears[i].value.color);
				}
			}
			attachment_started[p_index] = true;
			attachments.add_texture(*tex, ScopeUse::ATTACHMENT, 1, 1, WGPUTextureAspect_All);
		}
		colors.push_back(desc);
	};
	attachments = {};
	for (uint32_t i = 0; i < pass.color_references.size(); ++i) {
		color(pass.color_references[i].attachment, i);
	}
	if (synthetic) {
		WGPURenderPassColorAttachment desc = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
		const auto &target = samples == 1 ? framebuffer->synthetic_target : framebuffer->synthetic_target_msaa;
		DEV_ASSERT(target);
		desc.view = target->attachment.get();
		desc.loadOp = WGPULoadOp_Clear;
		desc.storeOp = WGPUStoreOp_Discard;
		colors.assign(1, desc);
	}
	WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	const uint32_t d = pass.depth_stencil_reference.attachment;
	if (d != RDD::AttachmentReference::UNUSED) {
		const auto &tex = framebuffer->attachments[d];
		const auto &a = layout->attachments[d];
		depth.view = tex->attachment.get();
		const bool read_only = pass.depth_stencil_reference.layout == RDD::TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		if (tex->format.depth) {
			depth.depthReadOnly = read_only;
			if (!read_only) {
				depth.depthLoadOp = attachment_load(attachment_started[d], a.load_op);
				depth.depthStoreOp = WGPUStoreOp_Store;
				depth.depthClearValue = a.load_op == RDD::ATTACHMENT_LOAD_OP_CLEAR ? clear_values[d].depth : 0.f;
			}
		}
		if (tex->format.stencil) {
			depth.stencilReadOnly = read_only;
			if (!read_only) {
				depth.stencilLoadOp = attachment_load(attachment_started[d], a.stencil_load_op);
				depth.stencilStoreOp = WGPUStoreOp_Store;
				depth.stencilClearValue = a.stencil_load_op == RDD::ATTACHMENT_LOAD_OP_CLEAR ? clear_values[d].stencil : 0;
			}
		}
		for (uint32_t i = 0; i < p_clears.size(); ++i) {
			if (tex->format.depth && !depth.depthReadOnly && p_clears[i].aspect.has_flag(RDD::TEXTURE_ASPECT_DEPTH_BIT)) {
				depth.depthLoadOp = WGPULoadOp_Clear;
				depth.depthClearValue = p_clears[i].value.depth;
			}
			if (tex->format.stencil && !depth.stencilReadOnly && p_clears[i].aspect.has_flag(RDD::TEXTURE_ASPECT_STENCIL_BIT)) {
				depth.stencilLoadOp = WGPULoadOp_Clear;
				depth.stencilClearValue = p_clears[i].value.stencil;
			}
		}
		attachment_started[d] = true;
		attachments.add_texture(*tex, read_only ? ScopeUse::READ : ScopeUse::ATTACHMENT, 1, 1, WGPUTextureAspect_All);
	}
	WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	desc.colorAttachmentCount = colors.size();
	desc.colorAttachments = colors.data();
	desc.depthStencilAttachment = depth.view ? &depth : nullptr;
	render.reset(wgpuCommandEncoderBeginRenderPass(encoder.get(), &desc));
	ERR_FAIL_COND(!render);
	for (const auto &label : labels) {
		wgpuRenderPassEncoderPushDebugGroup(render.get(), wg_string(label));
	}
	scope = attachments;
}

void CommandRecording::end_render() {
	if (render) {
		for (size_t i = 0; i < labels.size(); ++i) {
			wgpuRenderPassEncoderPopDebugGroup(render.get());
		}
		wgpuRenderPassEncoderEnd(render.get());
		render.reset();
	}
}

void CommandRecording::open_compute() {
	if (!compute) {
		DEV_ASSERT(!layout);
		compute.reset(wgpuCommandEncoderBeginComputePass(encoder.get(), nullptr));
		ERR_FAIL_COND(!compute);
		for (const auto &label : labels) {
			wgpuComputePassEncoderPushDebugGroup(compute.get(), wg_string(label));
		}
	}
}

void CommandRecording::end_compute() {
	if (compute) {
		for (size_t i = 0; i < labels.size(); ++i) {
			wgpuComputePassEncoderPopDebugGroup(compute.get());
		}
		wgpuComputePassEncoderEnd(compute.get());
		compute.reset();
	}
}

void CommandRecording::outside_pass() {
	end_compute();
	end_render();
}

} // namespace godot::webgpu

void RenderingDeviceDriverWebGPU::command_begin_render_pass(CommandBufferID p_buffer, RenderPassID p_pass, FramebufferID p_framebuffer,
		CommandBufferType p_type, const Rect2i &p_area, VectorView<RenderPassClearValue> p_values) {
	auto &cmd = recording(*state, p_buffer);
	DEV_ASSERT(!cmd.layout && p_type == COMMAND_BUFFER_TYPE_PRIMARY);
	cmd.end_compute();
	cmd.layout = resource<RenderPassLayout>(p_pass);
	cmd.framebuffer = resource<FramebufferResource>(p_framebuffer);
	cmd.keep(cmd.framebuffer);
	cmd.keep(cmd.layout);
	cmd.subpass = 0;
	cmd.pipeline.reset();
	cmd.scissor = cmd.viewport = cmd.area = p_area;
	cmd.attachment_started.assign(cmd.layout->attachments.size(), false);
	cmd.clear_values.assign(cmd.layout->attachments.size(), RenderPassClearValue{});
	const uint32_t count = MIN(uint32_t(cmd.clear_values.size()), p_values.size());
	for (uint32_t i = 0; i < count; ++i) {
		cmd.clear_values[i] = p_values[i];
	}
}

void RenderingDeviceDriverWebGPU::finish_subpass(CommandBufferID p_buffer) {
	auto &cmd = recording(*state, p_buffer);
	cmd.open_render();
	cmd.end_render();
	const auto &pass = cmd.layout->subpasses[cmd.subpass];
	auto resolve = [&](const AttachmentReference &p_src, const AttachmentReference &p_dst) {
		if (p_dst.attachment != AttachmentReference::UNUSED) {
			DEV_ASSERT(p_src.attachment < cmd.framebuffer->attachments.size() && p_dst.attachment < cmd.framebuffer->attachments.size());
			DEV_ASSERT(cmd.framebuffer->attachments[p_src.attachment] && cmd.framebuffer->attachments[p_dst.attachment]);
			// Use the actual framebuffer views rather than reconstructing their owner IDs.
			if (resolve_attachment(cmd, *cmd.framebuffer->attachments[p_src.attachment], *cmd.framebuffer->attachments[p_dst.attachment], 0, 0, 0, 0)) {
				cmd.attachment_started[p_dst.attachment] = true;
			}
		}
	};
	for (uint32_t i = 0; i < pass.resolve_references.size(); ++i) {
		const auto &dst = pass.resolve_references[i];
		if (dst.attachment != AttachmentReference::UNUSED) {
			DEV_ASSERT(i < pass.color_references.size());
			resolve(pass.color_references[i], dst);
		}
	}
	resolve(pass.depth_stencil_reference, pass.depth_resolve_reference);
}

void RenderingDeviceDriverWebGPU::command_end_render_pass(CommandBufferID p_buffer) {
	finish_subpass(p_buffer);
	auto &cmd = recording(*state, p_buffer);
	cmd.layout.reset();
	cmd.framebuffer.reset();
}

void RenderingDeviceDriverWebGPU::command_next_render_subpass(CommandBufferID p_buffer, CommandBufferType p_type) {
	DEV_ASSERT(p_type == COMMAND_BUFFER_TYPE_PRIMARY);
	finish_subpass(p_buffer);
	auto &cmd = recording(*state, p_buffer);
	++cmd.subpass;
	cmd.pipeline.reset();
}

void RenderingDeviceDriverWebGPU::command_pipeline_barrier(CommandBufferID p_buffer, BitField<PipelineStageBits>, BitField<PipelineStageBits>,
		VectorView<MemoryAccessBarrier>, VectorView<BufferBarrier>, VectorView<TextureBarrier>, VectorView<AccelerationStructureBarrier>) {
	// WebGPU performs transitions at usage boundaries. Close an active pass
	// here when Godot requests an explicit dependency between its commands.
	recording(*state, p_buffer).outside_pass();
}
