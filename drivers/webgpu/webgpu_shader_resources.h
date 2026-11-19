#pragma once

#include "rendering_device_driver_webgpu.h"
#include "rendering_shader_container_webgpu.h"

#include <functional>

namespace godot::webgpu {

struct ShaderResource {
	Ref<RenderingShaderContainerWebGPU> source;
	Rdc::ShaderReflection reflection;
	CompileOptions options;
	std::map<BindingKey, std::shared_ptr<SamplerResource>> immutable;
	std::map<CompileKey, std::shared_ptr<ShaderVariant>> variants;
	std::string label;
	int64_t prepare_task = -1;
};

struct BindGroupResource : GpuUse {
	BindGroupHandle handle;
	UseList resources;
};

struct BindGroupCache {
	SubmitSerial last_cleanup = 0;
	std::map<std::vector<uintptr_t>, std::weak_ptr<BindGroupResource>> entries;
};

struct ShaderVariant : GpuUse {
	std::shared_ptr<const PreparedShader> prepared;
	std::array<ShaderModuleHandle, SHADER_STAGE_COUNT> modules;
	std::vector<BindGroupLayoutHandle> groups;
	PipelineLayoutHandle layout;
	std::map<BindingKey, std::shared_ptr<SamplerResource>> immutable;
	std::set<BindingKey> dynamic_buffers;
	std::vector<BindGroupCache> bind_groups;
	const StageOutput &stage(Stage p_stage) const;
};

struct LogicalBinding {
	std::shared_ptr<BufferResource> buffer;
	std::shared_ptr<TextureResource> texture;
	std::shared_ptr<SamplerResource> sampler;
};

struct LogicalUniformSet {
	uint32_t set = 0;
	std::map<BindingKey, LogicalBinding> bindings;
	std::vector<std::shared_ptr<BufferResource>> dynamic;
	std::string label;
};

struct PipelineResource : GpuUse {
	std::shared_ptr<ShaderResource> source;
	std::vector<RDD::PipelineSpecializationConstant> constants;
	std::function<bool(PipelineResource &, const ShaderVariant &)> create;
	std::map<CompileKey, std::shared_ptr<PipelineResource>> variants;
	std::string label;
	std::shared_ptr<ShaderVariant> shader;
	ComputePipelineHandle compute;
	// None, Uint16, Uint32; non-strip pipelines use slot zero only.
	std::array<RenderPipelineHandle, 3> raster;
	std::vector<uint32_t> vertex_bindings;
	Color blend_constant;
	uint32_t stencil_reference = 0;
	bool dynamic_stencil_reference = false;
	bool is_strip = false;
	bool synthetic_attachment = false;
	uint32_t sample_count = 1;
	WGPURenderPipeline render_variant(WGPUIndexFormat p_index) const;
};

bool create_pipeline_layout(DeviceState &p_state, ShaderVariant &r_shader);

} // namespace godot::webgpu
