#include "webgpu_commands.h"

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

bool collect_shader_usage(const CommandRecording &p_cmd, const ShaderPlan &p_plan, UsageScope &r_scope) {
	for (const auto &binding : p_plan.bindings) {
		const BindingKey &key = binding.key;
		const auto set = p_cmd.sets.find(key.set);
		if (key.role == Role::SAMPLER || key.role == Role::PUSH_CONSTANT || set == p_cmd.sets.end()) {
			continue;
		}
		const auto uniform = set->second->bindings.find(key);
		if (uniform == set->second->bindings.end()) {
			continue; // build_bind_group diagnoses a missing required binding.
		}
		const auto &value = uniform->second;
		if (value.buffer) {
			const ScopeUse use = key.role == Role::STORAGE_BUFFER ? (binding.read_only ? ScopeUse::STORAGE_READ : ScopeUse::STORAGE_WRITE) : ScopeUse::READ;
			if (!r_scope.add({ value.buffer.get() }, use)) {
				return false;
			}
		} else if (value.texture) {
			const auto &texture = *value.texture;
			const bool storage = key.role == Role::STORAGE_TEXTURE || binding.format.atomic_buffer;
			const ScopeUse use = storage ? (binding.format.access == ImageAccess::READ ? ScopeUse::STORAGE_READ : ScopeUse::STORAGE_WRITE) : ScopeUse::READ;
			if (!r_scope.add_texture(texture, use, storage ? 1 : texture.mip_count, texture.layer_count,
					texture.format.depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All)) {
				return false;
			}
		}
	}
	return true;
}

void bind_sets(CommandRecording &r_cmd, VectorView<RDD::UniformSetID> p_sets, uint32_t p_first, uint32_t p_count, uint32_t p_dynamic_offsets) {
	uint32_t shift = 0;
	for (uint32_t i = 0; i < p_count; ++i) {
		const auto &set = resource<LogicalUniformSet>(p_sets[i]);
		DEV_ASSERT(set->set == p_first + i);
		r_cmd.sets[set->set] = set;
		for (const auto &buffer : set->dynamic) {
			DEV_ASSERT(shift < sizeof(p_dynamic_offsets) * CHAR_BIT);
			r_cmd.frames[buffer.get()] = (p_dynamic_offsets >> shift) & ((1u << DYNAMIC_FRAME_BITS) - 1);
			shift += DYNAMIC_FRAME_BITS;
		}
	}
}

void apply_vertex_state(CommandRecording &r_cmd, bool p_indexed) {
	const auto pass = r_cmd.render.get();
	const auto &rect = r_cmd.viewport;
	wgpuRenderPassEncoderSetViewport(pass, rect.position.x, rect.position.y, rect.size.x, rect.size.y, 0., 1.);
	const Rect2i scissor = r_cmd.scissor.intersection(Rect2i(0, 0, r_cmd.framebuffer->width, r_cmd.framebuffer->height));
	wgpuRenderPassEncoderSetScissorRect(pass, scissor.position.x, scissor.position.y, scissor.size.x, scissor.size.y);
	const auto blend = wg_color(r_cmd.blend.value_or(r_cmd.pipeline->blend_constant));
	wgpuRenderPassEncoderSetBlendConstant(pass, &blend);
	wgpuRenderPassEncoderSetStencilReference(pass, r_cmd.pipeline->stencil_reference);
	for (uint32_t slot = 0; slot < r_cmd.pipeline->vertex_bindings.size(); ++slot) {
		const auto &v = r_cmd.vertex[r_cmd.pipeline->vertex_bindings[slot]];
		const auto &buffer = v.buffer;
		DEV_ASSERT(buffer && v.offset < buffer->size);
		wgpuRenderPassEncoderSetVertexBuffer(pass, slot, buffer->buffer.get(), buffer->offset(v.frame) + v.offset, buffer->size - v.offset);
		r_cmd.keep(buffer);
		if (buffer->access == BufferAccess::DYNAMIC) {
			r_cmd.keep(buffer->pages[v.frame]);
		}
	}
	if (p_indexed) {
		const auto &i = r_cmd.index;
		wgpuRenderPassEncoderSetIndexBuffer(pass, i.buffer->buffer.get(), r_cmd.index_format, i.offset, i.buffer->size - i.offset);
		r_cmd.keep(i.buffer);
	}
}

} // namespace
} // namespace godot::webgpu

bool RenderingDeviceDriverWebGPU::prepare_dispatch(CommandRecording &r_cmd, bool p_compute, bool p_indexed, const std::shared_ptr<BufferResource> &p_indirect) {
	DEV_ASSERT(r_cmd.pipeline && (p_compute == !r_cmd.layout));
	const auto pipeline = materialize_pipeline(*r_cmd.pipeline, r_cmd.sets);
	if (!pipeline) {
		return false; // Pipeline realization has already reported the reason.
	}
	const auto &variant = pipeline->shader;
	UsageScope usage;
	ERR_FAIL_COND_V_MSG(!collect_shader_usage(r_cmd, variant->prepared->plan, usage), false, "Conflicting WebGPU bindings in a single draw or dispatch.");
	if (p_indirect) {
		ERR_FAIL_COND_V_MSG(!usage.add({ p_indirect.get() }, ScopeUse::READ), false, "Indirect arguments are writable bindings of the same dispatch or draw.");
		r_cmd.keep(p_indirect);
	}
	if (!p_compute) {
		for (uint32_t slot : r_cmd.pipeline->vertex_bindings) {
			ERR_FAIL_COND_V(slot >= r_cmd.vertex.size() || !r_cmd.vertex[slot].buffer, false);
			ERR_FAIL_COND_V_MSG(!usage.add({ r_cmd.vertex[slot].buffer.get() }, ScopeUse::READ), false, "The vertex buffer is writable in the same draw.");
		}
		if (p_indexed) {
			ERR_FAIL_COND_V(!r_cmd.index.buffer, false);
			ERR_FAIL_COND_V_MSG(!usage.add({ r_cmd.index.buffer.get() }, ScopeUse::READ), false, "The index buffer is writable in the same draw.");
		}
	}
	std::vector<PhysicalBindGroup> groups(variant->groups.size());
	for (uint32_t group = 0; group < groups.size(); ++group) {
		const Error error = build_bind_group(*variant, group, r_cmd.sets, r_cmd.frames, r_cmd.push.page ? &r_cmd.push : nullptr, groups[group]);
		ERR_FAIL_COND_V_MSG(error != OK, false, "Missing or incompatible WebGPU descriptor for the current draw or dispatch.");
	}
	if (p_compute) {
		r_cmd.open_compute(); // WebGPU validates the accumulated usages per dispatch, not per compute pass.
		ERR_FAIL_COND_V(!r_cmd.compute, false);
		wgpuComputePassEncoderSetPipeline(r_cmd.compute.get(), pipeline->compute.get());
	} else {
		r_cmd.open_render();
		ERR_FAIL_COND_V(!r_cmd.render, false);
		ERR_FAIL_COND_V_MSG(!r_cmd.attachments.compatible(usage), false, "A draw cannot sample or write its own writable attachment.");
		if (!r_cmd.scope.compatible(usage)) {
			r_cmd.end_render();
			r_cmd.open_render();
			ERR_FAIL_COND_V(!r_cmd.render, false);
		}
		r_cmd.scope.merge(usage);
		wgpuRenderPassEncoderSetPipeline(r_cmd.render.get(), pipeline->render_variant(p_indexed ? r_cmd.index_format : WGPUIndexFormat_Undefined));
		// The logical pipeline owns the same fixed-function and vertex state as the materialized variant.
		apply_vertex_state(r_cmd, p_indexed);
	}
	for (uint32_t i = 0; i < groups.size(); ++i) {
		const auto &group = groups[i];
		if (p_compute) {
			wgpuComputePassEncoderSetBindGroup(r_cmd.compute.get(), i, group.group->handle.get(), group.dynamic_offsets.size(), group.dynamic_offsets.data());
		} else {
			wgpuRenderPassEncoderSetBindGroup(r_cmd.render.get(), i, group.group->handle.get(), group.dynamic_offsets.size(), group.dynamic_offsets.data());
		}
		for (const auto &use : group.uses) {
			r_cmd.keep(use);
		}
	}
	r_cmd.keep(pipeline);
	return true;
}

void RenderingDeviceDriverWebGPU::command_bind_render_pipeline(CommandBufferID p_buffer, PipelineID p_pipeline) {
	auto &cmd = recording(*state, p_buffer);
	cmd.pipeline = resource<PipelineResource>(p_pipeline);
	cmd.blend.reset();
}

void RenderingDeviceDriverWebGPU::command_bind_compute_pipeline(CommandBufferID p_buffer, PipelineID p_pipeline) {
	recording(*state, p_buffer).pipeline = resource<PipelineResource>(p_pipeline);
}

void RenderingDeviceDriverWebGPU::command_bind_render_uniform_sets(CommandBufferID p_buffer, VectorView<UniformSetID> p_sets, ShaderID, uint32_t p_first, uint32_t p_count, uint32_t p_offsets) {
	bind_sets(recording(*state, p_buffer), p_sets, p_first, p_count, p_offsets);
}

void RenderingDeviceDriverWebGPU::command_bind_compute_uniform_sets(CommandBufferID p_buffer, VectorView<UniformSetID> p_sets, ShaderID, uint32_t p_first, uint32_t p_count, uint32_t p_offsets) {
	bind_sets(recording(*state, p_buffer), p_sets, p_first, p_count, p_offsets);
}

void RenderingDeviceDriverWebGPU::command_uniform_set_prepare_for_use(CommandBufferID, UniformSetID, ShaderID, uint32_t) {
	// Bind groups are materialized against the actual pipeline at the draw/dispatch.
}

void RenderingDeviceDriverWebGPU::command_render_bind_vertex_buffers(CommandBufferID p_buffer, uint32_t p_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic) {
	auto &cmd = recording(*state, p_buffer);
	cmd.vertex.resize(p_count);
	uint32_t shift = 0;
	for (uint32_t i = 0; i < p_count; ++i) {
		if (!p_buffers[i]) {
			cmd.vertex[i] = {};
			continue; // Sparse vertex binding numbers are valid in RDD.
		}
		const auto &buffer = resource<BufferResource>(p_buffers[i]);
		uint32_t frame = 0;
		if (buffer->access == BufferAccess::DYNAMIC) {
			DEV_ASSERT(shift + VERTEX_FRAME_BITS <= sizeof(p_dynamic) * CHAR_BIT);
			frame = (p_dynamic >> shift) & ((1u << VERTEX_FRAME_BITS) - 1);
			shift += VERTEX_FRAME_BITS;
		}
		cmd.vertex[i] = { buffer, p_offsets[i], frame };
	}
}

void RenderingDeviceDriverWebGPU::command_render_bind_index_buffer(CommandBufferID p_buffer, BufferID p_index, IndexBufferFormat p_format, uint64_t p_offset) {
	auto &cmd = recording(*state, p_buffer);
	cmd.index = { resource<BufferResource>(p_index), p_offset, 0 };
	cmd.index_format = p_format == INDEX_BUFFER_FORMAT_UINT16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32;
}

void RenderingDeviceDriverWebGPU::command_render_set_viewport(CommandBufferID p_buffer, VectorView<Rect2i> p_viewports) {
	DEV_ASSERT(p_viewports.size() == 1);
	recording(*state, p_buffer).viewport = p_viewports[0];
}

void RenderingDeviceDriverWebGPU::command_render_set_scissor(CommandBufferID p_buffer, VectorView<Rect2i> p_scissors) {
	DEV_ASSERT(p_scissors.size() == 1);
	recording(*state, p_buffer).scissor = p_scissors[0];
}

void RenderingDeviceDriverWebGPU::command_render_set_blend_constants(CommandBufferID p_buffer, const Color &p_constants) {
	recording(*state, p_buffer).blend = p_constants;
}

void RenderingDeviceDriverWebGPU::command_render_set_line_width(CommandBufferID, float p_width) {
	ERR_FAIL_COND_MSG(p_width != 1.f, "WebGPU supports only unit-width lines.");
}

void RenderingDeviceDriverWebGPU::command_render_draw(CommandBufferID p_buffer, uint32_t p_vertices, uint32_t p_instances, uint32_t p_first_vertex, uint32_t p_first_instance) {
	auto &cmd = recording(*state, p_buffer);
	if (prepare_dispatch(cmd, false, false)) {
		wgpuRenderPassEncoderDraw(cmd.render.get(), p_vertices, p_instances, p_first_vertex, p_first_instance);
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed(CommandBufferID p_buffer, uint32_t p_indices, uint32_t p_instances, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) {
	auto &cmd = recording(*state, p_buffer);
	if (prepare_dispatch(cmd, false, true)) {
		wgpuRenderPassEncoderDrawIndexed(cmd.render.get(), p_indices, p_instances, p_first_index, p_vertex_offset, p_first_instance);
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed_indirect(CommandBufferID p_buffer, BufferID p_indirect, uint64_t p_offset, uint32_t p_count, uint32_t p_stride) {
	auto &cmd = recording(*state, p_buffer);
	const auto &indirect = resource<BufferResource>(p_indirect);
	if (prepare_dispatch(cmd, false, true, indirect)) {
		for (uint32_t i = 0; i < p_count; ++i) {
			wgpuRenderPassEncoderDrawIndexedIndirect(cmd.render.get(), indirect->buffer.get(), p_offset + uint64_t(i) * p_stride);
		}
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw_indirect(CommandBufferID p_buffer, BufferID p_indirect, uint64_t p_offset, uint32_t p_count, uint32_t p_stride) {
	auto &cmd = recording(*state, p_buffer);
	const auto &indirect = resource<BufferResource>(p_indirect);
	if (prepare_dispatch(cmd, false, false, indirect)) {
		for (uint32_t i = 0; i < p_count; ++i) {
			wgpuRenderPassEncoderDrawIndirect(cmd.render.get(), indirect->buffer.get(), p_offset + uint64_t(i) * p_stride);
		}
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed_indirect_count(CommandBufferID p_cmd, BufferID p_indirect, uint64_t p_offset, BufferID p_count, uint64_t p_count_offset, uint32_t p_max, uint32_t p_stride) {
	draw_indirect_count(p_cmd, p_indirect, p_offset, p_count, p_count_offset, p_max, p_stride, true);
}

void RenderingDeviceDriverWebGPU::command_render_draw_indirect_count(CommandBufferID p_cmd, BufferID p_indirect, uint64_t p_offset, BufferID p_count, uint64_t p_count_offset, uint32_t p_max, uint32_t p_stride) {
	draw_indirect_count(p_cmd, p_indirect, p_offset, p_count, p_count_offset, p_max, p_stride, false);
}

void RenderingDeviceDriverWebGPU::command_compute_dispatch(CommandBufferID p_buffer, uint32_t p_x, uint32_t p_y, uint32_t p_z) {
	auto &cmd = recording(*state, p_buffer);
	if (prepare_dispatch(cmd, true, false)) {
		wgpuComputePassEncoderDispatchWorkgroups(cmd.compute.get(), p_x, p_y, p_z);
	}
}

void RenderingDeviceDriverWebGPU::command_compute_dispatch_indirect(CommandBufferID p_buffer, BufferID p_indirect, uint64_t p_offset) {
	auto &cmd = recording(*state, p_buffer);
	const auto &indirect = resource<BufferResource>(p_indirect);
	if (prepare_dispatch(cmd, true, false, indirect)) {
		wgpuComputePassEncoderDispatchWorkgroupsIndirect(cmd.compute.get(), indirect->buffer.get(), p_offset);
	}
}
