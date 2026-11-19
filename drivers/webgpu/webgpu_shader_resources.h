#pragma once

#include "rendering_device_driver_webgpu.h"
#include "rendering_shader_container_webgpu.h"

#include <functional>
#include <tuple>
#include <unordered_map>

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

// These ranges describe WebGPU usage scopes, not individual shader accesses.
enum class ScopeUse : uint8_t { READ, STORAGE_READ, STORAGE_WRITE, ATTACHMENT };

struct BufferScopeUse {
	const void *resource;
	ScopeUse use;
};

struct TextureScopeUse {
	const TextureBacking *backing;
	uint32_t mip_begin, mip_end, layer_begin, layer_end;
	uint8_t aspect_mask;
	ScopeUse use;
	bool overlaps(const TextureScopeUse &p_other) const;
};

struct UsageScope {
	std::vector<BufferScopeUse> read_buffers, write_buffers;
	std::vector<TextureScopeUse> read_textures, write_textures;
	void add_buffer(const void *p_resource, ScopeUse p_use);
	void add_texture(TextureScopeUse p_range);
	bool can_read_buffer(const void *p_resource) const;
	bool compatible(const UsageScope &p_other) const;
	void merge(const UsageScope &p_other);
	void clear();
};

TextureScopeUse texture_scope_use(const TextureResource &p_texture, ScopeUse p_use, uint32_t p_mips, uint32_t p_layers, WGPUTextureAspect p_aspect);

struct BindGroupResource : GpuUse {
	struct DynamicBinding {
		const BufferResource *buffer; // Null only for the push-constant slice.
		uint32_t set;
		uint32_t frame_shift;
	};
	struct WritableBuffer {
		const void *resource;
		uint64_t size;
		uint32_t dynamic_index;
		WGPUShaderStage stages;
	};
	struct WritableTexture {
		TextureScopeUse range;
		WGPUShaderStage stages;
	};

	BindGroupHandle handle;
	UseList resources;
	std::vector<DynamicBinding> dynamic;
	UsageScope usage;
	std::vector<WritableBuffer> writable_buffers;
	std::vector<WritableTexture> writable_textures;
	bool internal_conflict = false;
};

struct BindGroupRecipe {
	// Pin any immutable sampler identities used by the interner's key.
	UseList immutable;
	std::shared_ptr<BindGroupResource> static_group;
};

struct BindGroupPlan {
	struct Entry {
		const BindingPlan *binding;
		const std::shared_ptr<SamplerResource> *immutable;
		bool dynamic;
	};
	std::vector<Entry> bindings;
	std::vector<uint32_t> logical_sets;
	bool uses_push = false;
	bool bindings_resolved = false;
	std::vector<uint64_t> recipe_key;
	std::shared_ptr<BindGroupRecipe> recipe;
	SubmitSerial last_cleanup = 0;
	std::map<std::vector<uint64_t>, std::weak_ptr<BindGroupResource>> groups;
};

struct BindGroupScratch {
	std::vector<WGPUBindGroupEntry> entries;
	std::vector<uint64_t> key;
};

struct ShaderVariant : GpuUse {
	std::shared_ptr<const PreparedShader> prepared;
	std::array<ShaderModuleHandle, SHADER_STAGE_COUNT> modules;
	std::vector<BindGroupLayoutHandle> groups;
	PipelineLayoutHandle layout;
	std::map<BindingKey, std::shared_ptr<SamplerResource>> immutable;
	std::set<BindingKey> dynamic_buffers;
	std::vector<BindGroupPlan> bind_groups;
	const StageOutput &stage(Stage p_stage) const;
};

struct LogicalBinding {
	std::shared_ptr<BufferResource> buffer;
	std::shared_ptr<TextureResource> texture;
	std::shared_ptr<SamplerResource> sampler;
	uint32_t dynamic_frame_shift = UINT32_MAX;
};

struct BindingSpecialization {
	enum Kind : uint8_t { RAW_DEPTH, UNFILTERABLE_FLOAT, STORAGE_FORMAT, ATOMIC_IMAGE };
	BindingKey key;
	Kind kind;
	uint32_t format = 0;
	bool operator<(const BindingSpecialization &p_other) const {
		return std::tie(key, kind, format) < std::tie(p_other.key, p_other.kind, p_other.format);
	}
};

struct LogicalUniformSet {
	uint32_t set = 0;
	std::map<BindingKey, LogicalBinding> bindings;
	std::vector<std::shared_ptr<BufferResource>> dynamic;
	std::string label;
	uint64_t uid = 0;
	std::vector<BindingSpecialization> specialization;
	// This side of the (recipe, uid) cache owns the groups: freeing a set drops
	// its entries without a sweep over all shader variants.
	std::unordered_map<std::shared_ptr<BindGroupRecipe>, std::shared_ptr<BindGroupResource>> bind_groups;
};

struct PipelineResource : GpuUse {
	std::shared_ptr<ShaderResource> source;
	std::vector<RDD::PipelineSpecializationConstant> constants;
	std::function<bool(PipelineResource &, const ShaderVariant &)> create;
	std::map<CompileKey, std::shared_ptr<PipelineResource>> variants;
	std::shared_ptr<PipelineResource> default_variant;
	std::map<std::vector<BindingSpecialization>, std::shared_ptr<PipelineResource>> specialized_variants;
	std::vector<BindingSpecialization> specialization_key;
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
