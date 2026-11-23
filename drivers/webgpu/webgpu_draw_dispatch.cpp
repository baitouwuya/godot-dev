#include "webgpu_commands.h"
#include "webgpu_perf_stats.h"

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

constexpr uint32_t DYNAMIC_FRAME_MASK = (1U << DYNAMIC_FRAME_BITS) - 1;
static_assert(MAX_DRIVER_FRAMES >= 3 && MAX_DRIVER_FRAMES - 1 <= DYNAMIC_FRAME_MASK);
static_assert(MAX_DRIVER_FRAMES - 1 < (1U << VERTEX_FRAME_BITS));

void count_draws(uint32_t p_count) {
	if (webgpu_perf_stats.in_raf && webgpu_perf_is_enabled()) {
		webgpu_perf_stats.frame.work.draws += p_count;
	}
}

void bind_sets(CommandRecording &r_cmd, VectorView<RDD::UniformSetID> p_sets, uint32_t p_first, uint32_t p_count, uint32_t p_dynamic_offsets) {
	uint32_t shift = 0;
	for (uint32_t i = 0; i < p_count; ++i) {
		const auto &set = resource<LogicalUniformSet>(p_sets[i]);
		DEV_ASSERT(set->set == p_first + i);
		const uint32_t bits = set->dynamic.size() * DYNAMIC_FRAME_BITS;
		ERR_FAIL_COND(shift + bits > sizeof(p_dynamic_offsets) * CHAR_BIT);
		r_cmd.sets[set->set] = set;
		if (bits > 0) {
			r_cmd.frames[set->set] = p_dynamic_offsets >> shift;
		}
		shift += bits;
	}
}

bool writable_alias(const PhysicalBindGroup &p_a, const PhysicalBindGroup &p_b, bool p_same_slot) {
	const auto &a_buffers = p_a.group->writable_buffers;
	const auto &b_buffers = p_b.group->writable_buffers;
	for (size_t i = 0; i < a_buffers.size(); ++i) {
		const auto &a = a_buffers[i];
		const uint64_t a_offset = a.dynamic_index == UINT32_MAX ? 0 : p_a.dynamic_offsets[a.dynamic_index];
		for (size_t j = p_same_slot ? i + 1 : 0; j < b_buffers.size(); ++j) {
			const auto &b = b_buffers[j];
			if (a.resource != b.resource || !(a.stages & b.stages)) {
				continue;
			}
			const uint64_t b_offset = b.dynamic_index == UINT32_MAX ? 0 : p_b.dynamic_offsets[b.dynamic_index];
			if (a_offset < b_offset + b.size && b_offset < a_offset + a.size) {
				return true;
			}
		}
	}
	const auto &a_textures = p_a.group->writable_textures;
	const auto &b_textures = p_b.group->writable_textures;
	for (size_t i = 0; i < a_textures.size(); ++i) {
		for (size_t j = p_same_slot ? i + 1 : 0; j < b_textures.size(); ++j) {
			if ((a_textures[i].stages & b_textures[j].stages) && a_textures[i].range.overlaps(b_textures[j].range)) {
				return true;
			}
		}
	}
	return false;
}

bool has_writes(const BindGroupResource &p_group) {
	return !p_group.writable_buffers.empty() || !p_group.writable_textures.empty();
}

bool can_read_inputs(const CommandRecording &p_cmd, const UsageScope &p_scope, bool p_compute, bool p_indexed, const BufferResource *p_indirect) {
	if (p_scope.write_buffers.empty()) {
		return true;
	}
	if (p_indirect && !p_scope.can_read_buffer(p_indirect)) {
		return false;
	}
	if (!p_compute) {
		for (uint32_t slot : p_cmd.pipeline->vertex_bindings) {
			if (!p_scope.can_read_buffer(p_cmd.vertex[slot].buffer.get())) {
				return false;
			}
		}
		if (p_indexed && !p_scope.can_read_buffer(p_cmd.index.buffer.get())) {
			return false;
		}
	}
	return true;
}

void apply_vertex_state(CommandRecording &r_cmd, bool p_indexed) {
	const auto pass = r_cmd.render.get();
	const auto &rect = r_cmd.viewport;
	if (r_cmd.active_viewport != rect) {
		wgpuRenderPassEncoderSetViewport(pass, rect.position.x, rect.position.y, rect.size.x, rect.size.y, 0.f, 1.f);
		r_cmd.active_viewport = rect;
	}
	const Rect2i scissor = r_cmd.scissor.intersection(Rect2i(0, 0, r_cmd.framebuffer->width, r_cmd.framebuffer->height));
	if (r_cmd.active_scissor != scissor) {
		wgpuRenderPassEncoderSetScissorRect(pass, scissor.position.x, scissor.position.y, scissor.size.x, scissor.size.y);
		r_cmd.active_scissor = scissor;
	}
	const auto blend = r_cmd.blend.value_or(r_cmd.pipeline->blend_constant);
	if (r_cmd.active_blend != blend) {
		const auto color = wg_color(blend);
		wgpuRenderPassEncoderSetBlendConstant(pass, &color);
		r_cmd.active_blend = blend;
	}
	const uint32_t stencil = r_cmd.pipeline->stencil_reference;
	if (r_cmd.active_stencil_reference != stencil) {
		wgpuRenderPassEncoderSetStencilReference(pass, stencil);
		r_cmd.active_stencil_reference = stencil;
	}
	if (r_cmd.active_vertex_buffers.size() < r_cmd.pipeline->vertex_bindings.size()) {
		r_cmd.active_vertex_buffers.resize(r_cmd.pipeline->vertex_bindings.size());
	}
	for (uint32_t slot = 0; slot < r_cmd.pipeline->vertex_bindings.size(); ++slot) {
		const auto &v = r_cmd.vertex[r_cmd.pipeline->vertex_bindings[slot]];
		const auto &buffer = v.buffer;
		DEV_ASSERT(buffer && v.offset < buffer->size);
		const ActiveBufferBinding target{ buffer->buffer.get(), buffer->offset(v.frame) + v.offset, buffer->size - v.offset };
		if (!(r_cmd.active_vertex_buffers[slot] == target)) {
			wgpuRenderPassEncoderSetVertexBuffer(pass, slot, target.buffer, target.offset, target.size);
			r_cmd.active_vertex_buffers[slot] = target;
			r_cmd.keep_render_read(buffer);
			if (buffer->access == BufferAccess::DYNAMIC) {
				r_cmd.keep(buffer->pages[v.frame]);
			}
		}
	}
	if (p_indexed) {
		const auto &i = r_cmd.index;
		const ActiveIndexBinding target{ { i.buffer->buffer.get(), i.offset, i.buffer->size - i.offset }, r_cmd.index_format };
		if (!(r_cmd.active_index_buffer == target)) {
			wgpuRenderPassEncoderSetIndexBuffer(pass, target.buffer.buffer, target.format, target.buffer.offset, target.buffer.size);
			r_cmd.active_index_buffer = target;
			r_cmd.keep_render_read(i.buffer);
		}
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
	const auto &recipe = *r_cmd.pipeline;
	if (!p_compute) {
		for (uint32_t slot : recipe.vertex_bindings) {
			ERR_FAIL_COND_V(slot >= r_cmd.vertex.size() || !r_cmd.vertex[slot].buffer, false);
		}
		ERR_FAIL_COND_V(p_indexed && !r_cmd.index.buffer, false);
	}
	const uint32_t group_count = variant->groups.size();
	auto &groups = r_cmd.draw_groups;
	if (groups.size() < group_count) {
		groups.resize(group_count);
	}
	if (r_cmd.active_bind_groups.size() < group_count) {
		r_cmd.active_bind_groups.resize(group_count);
	}
	const auto *push = r_cmd.push.page ? &r_cmd.push : nullptr;
	for (uint32_t i = 0; i < group_count; ++i) {
		auto &group = groups[i];
		const Error error = get_bind_group(*variant, i, r_cmd.sets, push, r_cmd.bind_group_scratch, group.group);
		ERR_FAIL_COND_V_MSG(error != OK, false,
				vformat("WebGPU %s failed to bind group %d for shader '%s' (pass '%s').", p_compute ? "dispatch" : "draw", i,
						String::utf8(recipe.label.empty() ? recipe.source->source->shader_name.get_data() : recipe.label.c_str()),
						r_cmd.labels.empty() ? String("unlabeled") : String::utf8(r_cmd.labels.back().c_str())));
		group.dynamic_offsets.clear();
		group.uses.clear();
		for (const auto &binding : group.group->dynamic) {
			if (!binding.buffer) {
				group.dynamic_offsets.push_back(push->offset);
				group.uses.push_back(push->page);
			} else {
				const auto it = r_cmd.frames.find(binding.set);
				DEV_ASSERT(it != r_cmd.frames.end() && binding.frame_shift <= 32 - DYNAMIC_FRAME_BITS);
				const uint32_t frame = (it->second >> binding.frame_shift) & DYNAMIC_FRAME_MASK;
				DEV_ASSERT(frame < frame_count && binding.buffer->offset(frame) <= UINT32_MAX);
				group.dynamic_offsets.push_back(binding.buffer->offset(frame));
				group.uses.push_back(binding.buffer->pages[frame]);
			}
		}
	}
	// Scope compatibility permits repeated storage writes in a pass. A draw or
	// dispatch still cannot alias writable binding ranges visible to the same stage.
	for (uint32_t i = 0; i < group_count; ++i) {
		const auto &a = groups[i];
		if (!has_writes(*a.group)) {
			continue;
		}
		ERR_FAIL_COND_V_MSG(a.group->internal_conflict || writable_alias(a, a, true), false, "Conflicting usages inside a WebGPU bind group.");
		ERR_FAIL_COND_V_MSG(!can_read_inputs(r_cmd, a.group->usage, p_compute, p_indexed, p_indirect.get()), false,
				"A vertex, index, or indirect buffer is a writable binding of the same WebGPU draw or dispatch.");
		for (uint32_t j = 0; j < group_count; ++j) {
			const auto &b = groups[j];
			if (j == i || (j < i && has_writes(*b.group))) {
				continue;
			}
			ERR_FAIL_COND_V_MSG(!a.group->usage.compatible(b.group->usage) || writable_alias(a, b, false), false,
					"Conflicting WebGPU bind groups in a single draw or dispatch.");
		}
	}
	if (p_compute) {
		r_cmd.open_compute(); // WebGPU validates the accumulated usages per dispatch, not per compute pass.
		ERR_FAIL_COND_V(!r_cmd.compute, false);
		const auto handle = pipeline->compute.get();
		if (r_cmd.active_compute_pipeline != handle) {
			wgpuComputePassEncoderSetPipeline(r_cmd.compute.get(), handle);
			r_cmd.active_compute_pipeline = handle;
			r_cmd.keep(pipeline);
		}
		if (p_indirect) {
			r_cmd.keep(p_indirect);
		}
	} else {
		r_cmd.open_render();
		ERR_FAIL_COND_V(!r_cmd.render, false);
		bool compatible = can_read_inputs(r_cmd, r_cmd.scope, false, p_indexed, p_indirect.get());
		for (uint32_t i = 0; i < group_count && compatible; ++i) {
			const auto &group = *groups[i].group;
			compatible = r_cmd.active_bind_groups[i].handle == group.handle.get() || r_cmd.scoped(&group) || r_cmd.scope.compatible(group.usage);
		}
		if (!compatible) {
			for (uint32_t i = 0; i < group_count; ++i) {
				ERR_FAIL_COND_V_MSG(!r_cmd.attachments.compatible(groups[i].group->usage), false,
						"A WebGPU draw has a binding incompatible with its own attachment.");
			}
			r_cmd.end_render();
			r_cmd.open_render();
			ERR_FAIL_COND_V(!r_cmd.render, false);
			if (webgpu_perf_stats.in_raf && webgpu_perf_is_enabled()) {
				webgpu_perf_stats.frame.work.render_pass_splits_alias++;
			}
		}
		r_cmd.trim_render_slots(group_count, recipe.vertex_bindings.size(), state->limits.maxBindGroupsPlusVertexBuffers);
		const auto handle = pipeline->render_variant(p_indexed ? r_cmd.index_format : WGPUIndexFormat_Undefined);
		if (r_cmd.active_render_pipeline != handle) {
			wgpuRenderPassEncoderSetPipeline(r_cmd.render.get(), handle);
			r_cmd.active_render_pipeline = handle;
			r_cmd.keep(pipeline);
		}
		// The logical pipeline owns the same fixed-function and vertex state as the materialized variant.
		apply_vertex_state(r_cmd, p_indexed);
		if (p_indirect) {
			r_cmd.keep_render_read(p_indirect);
		}
	}
	// Pipeline changes do not clear WebGPU's per-slot bind group state.
	for (uint32_t i = 0; i < group_count; ++i) {
		auto &group = groups[i];
		auto &active = r_cmd.active_bind_groups[i];
		const auto handle = group.group->handle.get();
		if (active.handle != handle || active.offsets != group.dynamic_offsets) {
			if (p_compute) {
				wgpuComputePassEncoderSetBindGroup(r_cmd.compute.get(), i, handle, group.dynamic_offsets.size(), group.dynamic_offsets.data());
			} else {
				wgpuRenderPassEncoderSetBindGroup(r_cmd.render.get(), i, handle, group.dynamic_offsets.size(), group.dynamic_offsets.data());
			}
			active.handle = handle;
			active.offsets = group.dynamic_offsets;
			r_cmd.keep_bind_group(group.group, !p_compute);
		}
		for (const auto &use : group.uses) {
			r_cmd.keep(use);
		}
		group.uses.clear();
		group.group.reset();
	}
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
		count_draws(1);
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed(CommandBufferID p_buffer, uint32_t p_indices, uint32_t p_instances, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) {
	auto &cmd = recording(*state, p_buffer);
	if (prepare_dispatch(cmd, false, true)) {
		wgpuRenderPassEncoderDrawIndexed(cmd.render.get(), p_indices, p_instances, p_first_index, p_vertex_offset, p_first_instance);
		count_draws(1);
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw_indexed_indirect(CommandBufferID p_buffer, BufferID p_indirect, uint64_t p_offset, uint32_t p_count, uint32_t p_stride) {
	auto &cmd = recording(*state, p_buffer);
	const auto &indirect = resource<BufferResource>(p_indirect);
	if (prepare_dispatch(cmd, false, true, indirect)) {
		for (uint32_t i = 0; i < p_count; ++i) {
			wgpuRenderPassEncoderDrawIndexedIndirect(cmd.render.get(), indirect->buffer.get(), p_offset + uint64_t(i) * p_stride);
		}
		count_draws(p_count);
	}
}

void RenderingDeviceDriverWebGPU::command_render_draw_indirect(CommandBufferID p_buffer, BufferID p_indirect, uint64_t p_offset, uint32_t p_count, uint32_t p_stride) {
	auto &cmd = recording(*state, p_buffer);
	const auto &indirect = resource<BufferResource>(p_indirect);
	if (prepare_dispatch(cmd, false, false, indirect)) {
		for (uint32_t i = 0; i < p_count; ++i) {
			wgpuRenderPassEncoderDrawIndirect(cmd.render.get(), indirect->buffer.get(), p_offset + uint64_t(i) * p_stride);
		}
		count_draws(p_count);
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
		if (webgpu_perf_stats.in_raf && webgpu_perf_is_enabled()) {
			webgpu_perf_stats.frame.work.dispatches++;
		}
	}
}

void RenderingDeviceDriverWebGPU::command_compute_dispatch_indirect(CommandBufferID p_buffer, BufferID p_indirect, uint64_t p_offset) {
	auto &cmd = recording(*state, p_buffer);
	const auto &indirect = resource<BufferResource>(p_indirect);
	if (prepare_dispatch(cmd, true, false, indirect)) {
		wgpuComputePassEncoderDispatchWorkgroupsIndirect(cmd.compute.get(), indirect->buffer.get(), p_offset);
		if (webgpu_perf_stats.in_raf && webgpu_perf_is_enabled()) {
			webgpu_perf_stats.frame.work.dispatches++;
		}
	}
}
