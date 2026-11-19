#include "webgpu_commands.h"

using namespace godot::webgpu;

namespace {
	constexpr uint32_t workgroup_size = 64;
	constexpr uint32_t draw_words = 4;
	constexpr uint32_t draw_indexed_words = 5;
	constexpr char indirect_mask_shader[] = R"(
struct Params { source: u32, stride: u32, count_offset: u32, max_draws: u32, words: u32, }
@group(0) @binding(0) var<storage, read> source: array<u32>;
@group(0) @binding(1) var<storage, read> counts: array<u32>;
@group(0) @binding(2) var<storage, read_write> masked_args: array<u32>;
@group(0) @binding(3) var<uniform> params: Params;
override workgroup_size: u32;
@compute @workgroup_size(workgroup_size)
fn mask_draws(@builtin(global_invocation_id) id: vec3u, @builtin(num_workgroups) groups: vec3u) {
    let count = min(params.max_draws, counts[params.count_offset]);
    for (var draw = id.x; draw < params.max_draws; draw += groups.x * workgroup_size) {
        for (var word = 0u; word < params.words; word++) {
            var value = 0u;
            if (draw < count) {
                value = source[params.source + draw * params.stride + word];
            }
            masked_args[draw * params.words + word] = value;
        }
    }
})";
} // namespace

void RenderingDeviceDriverWebGPU::draw_indirect_count(CommandBufferID p_buffer, BufferID p_arguments, uint64_t p_offset, BufferID p_count,
		uint64_t p_count_offset, uint32_t p_max, uint32_t p_stride, bool p_indexed) {
	if (p_max == 0) {
		return;
	}
	auto &cmd = recording(*state, p_buffer);
	const auto &args = resource<BufferResource>(p_arguments);
	const auto &count = resource<BufferResource>(p_count);
	DEV_ASSERT((p_offset | p_stride | p_count_offset) % sizeof(uint32_t) == 0);
	const uint32_t words = p_indexed ? draw_indexed_words : draw_words;
	const uint64_t output_size = uint64_t(p_max) * words * sizeof(uint32_t);
	ERR_FAIL_COND_MSG(MAX(output_size, MAX(args->allocation_size, count->allocation_size)) > state->limits.maxStorageBufferBindingSize,
			"Counted indirect draw exceeds the WebGPU storage binding limit.");
	if (!indirect_count_pipeline) {
		WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
		wgsl.code = { indirect_mask_shader, sizeof(indirect_mask_shader) - 1 };
		WGPUShaderModuleDescriptor shader = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shader.nextInChain = &wgsl.chain;
		ShaderModuleHandle module(wgpuDeviceCreateShaderModule(state->device.get(), &shader));
		ERR_FAIL_COND(!module);
		WGPUConstantEntry constant = WGPU_CONSTANT_ENTRY_INIT;
		constant.key = wg_string("workgroup_size");
		constant.value = workgroup_size;
		WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		desc.compute.module = module.get();
		desc.compute.constantCount = 1;
		desc.compute.constants = &constant;
		indirect_count_pipeline.reset(wgpuDeviceCreateComputePipeline(state->device.get(), &desc));
		ERR_FAIL_COND(!indirect_count_pipeline);
	}
	const uint32_t params[] = { uint32_t(p_offset / sizeof(uint32_t)), p_stride / uint32_t(sizeof(uint32_t)),
		uint32_t(p_count_offset / sizeof(uint32_t)), p_max, words };
	const auto uniform = make_transfer_buffer(*state, sizeof(params), WGPUBufferUsage_Uniform, true);
	const auto masked = make_transfer_buffer(*state, output_size, WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect);
	ERR_FAIL_COND(!uniform || !masked);
	void *data = wgpuBufferGetMappedRange(uniform->buffer.get(), 0, sizeof(params));
	ERR_FAIL_NULL(data);
	memcpy(data, params, sizeof(params));
	wgpuBufferUnmap(uniform->buffer.get());
	BindGroupLayoutHandle layout(wgpuComputePipelineGetBindGroupLayout(indirect_count_pipeline.get(), 0));
	WGPUBindGroupEntry entries[4];
	const BufferResource *buffers[] = { args.get(), count.get(), masked.get(), uniform.get() };
	for (uint32_t i = 0; i < std::size(entries); ++i) {
		entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
		entries[i].binding = i;
		entries[i].buffer = buffers[i]->buffer.get();
		entries[i].size = buffers[i]->allocation_size;
	}
	WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	group_desc.layout = layout.get();
	group_desc.entryCount = std::size(entries);
	group_desc.entries = entries;
	BindGroupHandle group(wgpuDeviceCreateBindGroup(state->device.get(), &group_desc));
	ERR_FAIL_COND(!group);
	cmd.outside_pass();
	ComputePassEncoderHandle mask(wgpuCommandEncoderBeginComputePass(cmd.encoder.get(), nullptr));
	ERR_FAIL_COND(!mask);
	wgpuComputePassEncoderSetPipeline(mask.get(), indirect_count_pipeline.get());
	wgpuComputePassEncoderSetBindGroup(mask.get(), 0, group.get(), 0, nullptr);
	wgpuComputePassEncoderDispatchWorkgroups(mask.get(), MIN(uint32_t((uint64_t(p_max) + workgroup_size - 1) / workgroup_size), state->limits.maxComputeWorkgroupsPerDimension), 1, 1);
	wgpuComputePassEncoderEnd(mask.get());
	cmd.keep(args);
	cmd.keep(count);
	cmd.keep(uniform);
	cmd.keep(masked);
	if (!prepare_dispatch(cmd, false, p_indexed, masked)) {
		return;
	}
	for (uint32_t i = 0; i < p_max; ++i) {
		const uint64_t offset = uint64_t(i) * words * sizeof(uint32_t);
		if (p_indexed) {
			wgpuRenderPassEncoderDrawIndexedIndirect(cmd.render.get(), masked->buffer.get(), offset);
		} else {
			wgpuRenderPassEncoderDrawIndirect(cmd.render.get(), masked->buffer.get(), offset);
		}
	}
}
