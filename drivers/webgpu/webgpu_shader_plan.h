#pragma once

#include "webgpu_constants.h"

#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace godot::webgpu {

enum class Stage : uint8_t { VERTEX, FRAGMENT, COMPUTE };
enum class Role : uint8_t { TEXTURE, SAMPLER, STORAGE_BUFFER, STORAGE_TEXTURE, UNIFORM_BUFFER, PUSH_CONSTANT };
enum class SampleType : uint8_t { NONE, FLOAT, UNFILTERABLE_FLOAT, DEPTH, SINT, UINT };
enum class SamplerType : uint8_t { NONE, FILTERING, NON_FILTERING, COMPARISON };
enum class ImageAccess : uint8_t { NONE, READ, WRITE, READ_WRITE };
enum class FormatConversion : uint8_t { NONE, UNORM, SNORM, NARROW_UINT, NARROW_SINT, FLOAT16 };
enum class SubgroupLowering : uint8_t { NONE, CLUSTERED_SCENE, CLUSTER_RASTER };

struct BindingKey {
	uint32_t set = 0;
	uint32_t binding = 0;
	uint32_t element = 0;
	Role role = Role::TEXTURE;

	auto order() const { return std::tie(set, binding, element, role); }
	bool operator<(const BindingKey &p_other) const { return order() < p_other.order(); }
	bool operator==(const BindingKey &p_other) const { return order() == p_other.order(); }
};

struct FormatPlan {
	// Values correspond to the SPIR-V Image Format and Dim enumerants.
	uint32_t logical_format = 0;
	uint32_t physical_format = 0;
	uint32_t dimension = 0;
	bool arrayed = false;
	bool multisampled = false;
	bool depth_only_view = false;
	ImageAccess access = ImageAccess::NONE;
	SampleType sample_type = SampleType::NONE;
	FormatConversion conversion = FormatConversion::NONE;
	uint8_t component_bits = 0;
	uint8_t channels = 0;
	// An atomic image uses a storage buffer with an extent header, followed by
	// a tightly packed array of atomic texels in x-major order.
	bool atomic_buffer = false;
};

struct BindingPlan {
	BindingKey key;
	uint32_t group = 0;
	uint32_t binding = 0;
	uint8_t stages = 0;
	bool read_only = false;
	SamplerType sampler_type = SamplerType::NONE;
	FormatPlan format;
	// Populated only for samplers with an explicitly supplied equivalence contract.
	std::vector<BindingKey> aliases;
};

struct StageLimits {
	uint32_t sampled_textures = 0;
	uint32_t samplers = 0;
	uint32_t storage_buffers = 0;
	uint32_t storage_textures = 0;
	uint32_t uniform_buffers = 0;
};

struct PushConstantPlan {
	uint32_t size = 0;
	uint8_t stages = 0;
	uint32_t group = FIRST_FOLDED_GROUP;
	uint32_t binding = 0;
};

struct StageInput {
	Stage stage;
	std::vector<uint32_t> spirv;
};

struct StageOutput {
	Stage stage;
	std::string entry_point;
	std::string wgsl;
};

struct CompileOptions {
	// Exact 32-bit specialization payloads, indexed by SPIR-V SpecId. All supplied
	// values apply before freezing/layout reflection; absent values use the module defaults.
	std::map<uint32_t, uint32_t> specialization;
	// Bindings declared as ordinary f32 in GLSL but backed by a depth-only view.
	std::vector<BindingKey> raw_depth;
	std::vector<BindingKey> unfilterable_float;
	std::map<BindingKey, uint32_t> storage_formats; // Required for formatless storage images.
	// Equal IDs certify equal runtime sampler descriptors. Never infer this from shader use.
	std::map<BindingKey, uint32_t> immutable_samplers;
	// A memory-only fence can become a convergent workgroup barrier only when the
	// shader variant explicitly guarantees uniform participation at its fences.
	bool uniform_memory_fences = false;
	bool texture_barrier = false;
	bool promote_narrow_storage = true;
	bool non_uniform_derivatives = false;
	bool flip_vertex_y = true;
	// These Godot algorithms use subgroups only to batch otherwise independent
	// lane-local work. Opt in per known shader, never for arbitrary SPIR-V.
	SubgroupLowering subgroup_lowering = SubgroupLowering::NONE;
};

struct ShaderPlan {
	std::vector<BindingPlan> bindings;
	std::array<StageLimits, SHADER_STAGE_COUNT> limits{};
	PushConstantPlan push_constant;
	bool uses_texture_barrier = false;
	std::vector<StageOutput> stages;
	std::string diagnostic;
	bool ok() const { return diagnostic.empty(); }
};

constexpr uint8_t stage_mask(Stage p_stage) {
	return uint8_t(1u << static_cast<unsigned>(p_stage));
}

} // namespace godot::webgpu
