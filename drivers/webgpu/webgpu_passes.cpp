#include "webgpu_commands.h"

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

bool overlaps(const BufferScopeUse &p_a, const BufferScopeUse &p_b) {
	return p_a.resource == p_b.resource;
}

bool overlaps(const TextureScopeUse &p_a, const TextureScopeUse &p_b) {
	return p_a.overlaps(p_b);
}

template <typename T>
bool has_conflict(const std::vector<T> &p_left, const std::vector<T> &p_right) {
	for (const auto &a : p_left) {
		for (const auto &b : p_right) {
			if (a.use != b.use && overlaps(a, b)) {
				return true;
			}
		}
	}
	return false;
}

WGPULoadOp attachment_load(bool p_started, RDD::AttachmentLoadOp p_load_op) {
	return p_started || p_load_op == RDD::ATTACHMENT_LOAD_OP_LOAD ? WGPULoadOp_Load : WGPULoadOp_Clear;
}

} // namespace

bool TextureScopeUse::overlaps(const TextureScopeUse &p_other) const {
	return backing == p_other.backing && (aspect_mask & p_other.aspect_mask) != 0 &&
			mip_begin < p_other.mip_end && p_other.mip_begin < mip_end &&
			layer_begin < p_other.layer_end && p_other.layer_begin < layer_end;
}

TextureScopeUse texture_scope_use(const TextureResource &p_texture, ScopeUse p_use, uint32_t p_mips, uint32_t p_layers, WGPUTextureAspect p_aspect) {
	constexpr uint8_t COLOR = 1, DEPTH = 2, STENCIL = 4;
	uint8_t mask = 0;
	switch (p_aspect) {
		case WGPUTextureAspect_DepthOnly: mask = DEPTH; break;
		case WGPUTextureAspect_StencilOnly: mask = STENCIL; break;
		default:
			mask = (p_texture.format.depth ? DEPTH : 0) | (p_texture.format.stencil ? STENCIL : 0);
			if (!mask) {
				mask = COLOR;
			}
			break;
	}
	// WebGPU gives non-2D textures one subresource per mip and aspect, not one per depth slice.
	const bool array = p_texture.backing->dimension == WGPUTextureDimension_2D;
	return { p_texture.backing.get(), p_texture.base_mip, p_texture.base_mip + p_mips,
		array ? p_texture.base_layer : 0, array ? p_texture.base_layer + p_layers : 1, mask, p_use };
}

void UsageScope::add_buffer(const void *p_resource, ScopeUse p_use) {
	(p_use <= ScopeUse::STORAGE_READ ? read_buffers : write_buffers).push_back({ p_resource, p_use });
}

void UsageScope::add_texture(TextureScopeUse p_range) {
	(p_range.use <= ScopeUse::STORAGE_READ ? read_textures : write_textures).push_back(p_range);
}

bool UsageScope::can_read_buffer(const void *p_resource) const {
	for (const auto &binding : write_buffers) {
		if (binding.resource == p_resource) {
			return false;
		}
	}
	return true;
}

bool UsageScope::compatible(const UsageScope &p_other) const {
	// Buffer writes all use STORAGE_WRITE; only textures can mix two write usages.
	return !has_conflict(write_buffers, p_other.read_buffers) && !has_conflict(p_other.write_buffers, read_buffers) &&
			!has_conflict(write_textures, p_other.read_textures) && !has_conflict(p_other.write_textures, read_textures) &&
			!has_conflict(write_textures, p_other.write_textures);
}

void UsageScope::merge(const UsageScope &p_other) {
	read_buffers.insert(read_buffers.end(), p_other.read_buffers.begin(), p_other.read_buffers.end());
	write_buffers.insert(write_buffers.end(), p_other.write_buffers.begin(), p_other.write_buffers.end());
	read_textures.insert(read_textures.end(), p_other.read_textures.begin(), p_other.read_textures.end());
	write_textures.insert(write_textures.end(), p_other.write_textures.begin(), p_other.write_textures.end());
}

void UsageScope::clear() {
	read_buffers.clear();
	write_buffers.clear();
	read_textures.clear();
	write_textures.clear();
}

void CommandRecording::keep_render_read(const std::shared_ptr<BufferResource> &p_buffer) {
	const auto [it, first] = referenced.try_emplace(p_buffer.get(), 0);
	if (it->second != render_scope_epoch) {
		scope.add_buffer(p_buffer.get(), ScopeUse::READ);
		it->second = render_scope_epoch;
	}
	if (first) {
		uses.push_back(p_buffer);
	}
}

void CommandRecording::keep_bind_group(const std::shared_ptr<BindGroupResource> &p_group, bool p_render) {
	const auto [it, first] = referenced.try_emplace(p_group.get(), 0);
	if (p_render && it->second != render_scope_epoch) {
		scope.merge(p_group->usage);
		it->second = render_scope_epoch;
	}
	if (first) {
		uses.push_back(p_group);
		// Preserve submission-use tracking for children without copying their refs per draw.
		for (const auto &use : p_group->resources) {
			keep(use);
		}
	}
}

void CommandRecording::reset_encoder_state() {
	active_render_pipeline = nullptr;
	active_compute_pipeline = nullptr;
	for (auto &group : active_bind_groups) {
		group.handle = nullptr;
	}
	active_viewport.reset();
	active_scissor.reset();
	active_blend.reset();
	active_stencil_reference.reset();
	for (auto &binding : active_vertex_buffers) {
		binding = {};
	}
	active_index_buffer = {};
}

void CommandRecording::trim_render_slots(uint32_t p_groups, uint32_t p_vertices, uint32_t p_limit) {
	DEV_ASSERT(p_groups + p_vertices <= p_limit);
	// The combined draw limit includes still-bound slots that the new pipeline does not use.
	while (MAX(bound_render_groups, p_groups) + MAX(bound_render_vertices, p_vertices) > p_limit) {
		if (bound_render_groups > p_groups) {
			const uint32_t slot = --bound_render_groups;
			wgpuRenderPassEncoderSetBindGroup(render.get(), slot, nullptr, 0, nullptr);
			active_bind_groups[slot].handle = nullptr;
		} else {
			const uint32_t slot = --bound_render_vertices;
			wgpuRenderPassEncoderSetVertexBuffer(render.get(), slot, nullptr, 0, 0);
			active_vertex_buffers[slot] = {};
		}
	}
	bound_render_groups = MAX(bound_render_groups, p_groups);
	bound_render_vertices = MAX(bound_render_vertices, p_vertices);
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
			attachments.add_texture(texture_scope_use(*tex, ScopeUse::ATTACHMENT, 1, 1, WGPUTextureAspect_All));
		}
		colors.push_back(desc);
	};
	attachments.clear();
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
		attachments.add_texture(texture_scope_use(*target, ScopeUse::ATTACHMENT, 1, 1, WGPUTextureAspect_All));
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
		attachments.add_texture(texture_scope_use(*tex, read_only ? ScopeUse::READ : ScopeUse::ATTACHMENT, 1, 1, WGPUTextureAspect_All));
	}
	WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	desc.colorAttachmentCount = colors.size();
	desc.colorAttachments = colors.data();
	desc.depthStencilAttachment = depth.view ? &depth : nullptr;
	render.reset(wgpuCommandEncoderBeginRenderPass(encoder.get(), &desc));
	ERR_FAIL_COND(!render);
	reset_encoder_state();
	++render_scope_epoch;
	DEV_ASSERT(render_scope_epoch != 0);
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
		bound_render_groups = bound_render_vertices = 0;
		reset_encoder_state();
		scope.clear();
	}
}

void CommandRecording::open_compute() {
	if (!compute) {
		DEV_ASSERT(!layout);
		compute.reset(wgpuCommandEncoderBeginComputePass(encoder.get(), nullptr));
		ERR_FAIL_COND(!compute);
		reset_encoder_state();
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
		reset_encoder_state();
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
