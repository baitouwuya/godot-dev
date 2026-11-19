#include "webgpu_shader_resources.h"

#include <algorithm>

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

bool check_stage_limits(const WGPULimits &p_limits, const ShaderPlan &p_plan) {
	for (uint32_t stage = 0; stage < SHADER_STAGE_COUNT; ++stage) {
		const auto &s = p_plan.limits[stage];
		uint32_t buffers = p_limits.maxStorageBuffersPerShaderStage;
		uint32_t images = p_limits.maxStorageTexturesPerShaderStage;
		if (stage == static_cast<uint32_t>(Stage::VERTEX)) {
			buffers = MIN(buffers, p_limits.maxStorageBuffersInVertexStage);
			images = MIN(images, p_limits.maxStorageTexturesInVertexStage);
		} else if (stage == static_cast<uint32_t>(Stage::FRAGMENT)) {
			buffers = MIN(buffers, p_limits.maxStorageBuffersInFragmentStage);
			images = MIN(images, p_limits.maxStorageTexturesInFragmentStage);
		}
		if (s.sampled_textures > p_limits.maxSampledTexturesPerShaderStage || s.samplers > p_limits.maxSamplersPerShaderStage ||
				s.storage_buffers > buffers || s.storage_textures > images || s.uniform_buffers > p_limits.maxUniformBuffersPerShaderStage) {
			return false;
		}
	}
	return true;
}

WGPUTextureSampleType sample_type(SampleType p_type) {
	switch (p_type) {
		case SampleType::FLOAT:
			return WGPUTextureSampleType_Float;
		case SampleType::UNFILTERABLE_FLOAT:
			return WGPUTextureSampleType_UnfilterableFloat;
		case SampleType::DEPTH:
			return WGPUTextureSampleType_Depth;
		case SampleType::SINT:
			return WGPUTextureSampleType_Sint;
		case SampleType::UINT:
			return WGPUTextureSampleType_Uint;
		default:
			return WGPUTextureSampleType_Undefined;
	}
}

WGPUStorageTextureAccess storage_access(ImageAccess p_access) {
	switch (p_access) {
		case ImageAccess::READ:
			return WGPUStorageTextureAccess_ReadOnly;
		case ImageAccess::WRITE:
			return WGPUStorageTextureAccess_WriteOnly;
		case ImageAccess::READ_WRITE:
			return WGPUStorageTextureAccess_ReadWrite;
		default:
			return WGPUStorageTextureAccess_Undefined;
	}
}

bool dynamic_binding(const ShaderVariant &p_shader, const BindingPlan &p_binding) {
	return p_binding.key.role == Role::PUSH_CONSTANT || p_shader.dynamic_buffers.count(p_binding.key);
}

const char *binding_role_name(Role p_role) {
	switch (p_role) {
		case Role::TEXTURE: return "sampled texture";
		case Role::SAMPLER: return "sampler";
		case Role::PUSH_CONSTANT: return "push constants";
		case Role::STORAGE_BUFFER: return "storage buffer";
		case Role::STORAGE_TEXTURE: return "storage texture";
		case Role::UNIFORM_BUFFER: return "uniform buffer";
	}
	return "unknown";
}

const LogicalBinding *logical_binding(const std::map<uint32_t, std::shared_ptr<LogicalUniformSet>> &p_sets, const BindingKey &p_key) {
	auto set = p_sets.find(p_key.set);
	if (set == p_sets.end()) {
		return nullptr; // A folded group may not yet have all of its logical sets bound.
	}
	auto binding = set->second->bindings.find(p_key);
	return binding == set->second->bindings.end() ? nullptr : &binding->second;
}

} // namespace

bool create_pipeline_layout(DeviceState &p_state, ShaderVariant &r_shader) {
	static_assert(stage_mask(Stage::VERTEX) == WGPUShaderStage_Vertex && stage_mask(Stage::FRAGMENT) == WGPUShaderStage_Fragment && stage_mask(Stage::COMPUTE) == WGPUShaderStage_Compute);
	const ShaderPlan &plan = r_shader.prepared->plan;
	const auto &limits = p_state.limits;
	ERR_FAIL_COND_V_MSG(plan.uses_texture_barrier && !p_state.has_read_write_storage_textures, false,
			"This shader requires WGSL textureBarrier support.");
	ERR_FAIL_COND_V_MSG(!check_stage_limits(limits, plan), false, "Shader binding counts exceed enabled WebGPU device limits.");
	std::vector<std::vector<WGPUBindGroupLayoutEntry>> entries;
	uint32_t dynamic_uniforms = 0, dynamic_storage = 0;
	for (const auto &binding : plan.bindings) {
		ERR_FAIL_COND_V(binding.group >= limits.maxBindGroups || binding.binding >= limits.maxBindingsPerBindGroup, false);
		if (entries.size() <= binding.group) {
			entries.resize(binding.group + 1);
		}
		WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
		entry.binding = binding.binding;
		entry.visibility = binding.stages;
		switch (binding.key.role) {
			case Role::TEXTURE:
				entry.texture.sampleType = sample_type(binding.format.sample_type);
				entry.texture.viewDimension = shader_view_dimension(binding.format);
				entry.texture.multisampled = binding.format.multisampled;
				ERR_FAIL_COND_V(entry.texture.viewDimension == WGPUTextureViewDimension_Undefined || entry.texture.sampleType == WGPUTextureSampleType_Undefined, false);
				break;
			case Role::STORAGE_TEXTURE:
				ERR_FAIL_COND_V_MSG(binding.format.access != ImageAccess::WRITE && !p_state.has_read_write_storage_textures, false,
						"This shader requires the readonly_and_readwrite_storage_textures WGSL language feature.");
				entry.storageTexture.access = storage_access(binding.format.access);
				entry.storageTexture.format = storage_texture_format(static_cast<spv::ImageFormat>(binding.format.physical_format));
				entry.storageTexture.viewDimension = shader_view_dimension(binding.format);
				ERR_FAIL_COND_V(entry.storageTexture.format == WGPUTextureFormat_Undefined || entry.storageTexture.viewDimension == WGPUTextureViewDimension_Undefined, false);
				break;
			case Role::SAMPLER:
				entry.sampler.type = binding.sampler_type == SamplerType::COMPARISON ? WGPUSamplerBindingType_Comparison : binding.sampler_type == SamplerType::NON_FILTERING ? WGPUSamplerBindingType_NonFiltering
																																											  : WGPUSamplerBindingType_Filtering;
				break;
			case Role::UNIFORM_BUFFER:
				entry.buffer.type = WGPUBufferBindingType_Uniform;
				entry.buffer.hasDynamicOffset = dynamic_binding(r_shader, binding);
				dynamic_uniforms += entry.buffer.hasDynamicOffset;
				break;
			case Role::PUSH_CONSTANT:
				entry.buffer.minBindingSize = plan.push_constant.size;
				[[fallthrough]];
			case Role::STORAGE_BUFFER:
				entry.buffer.type = binding.read_only ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage;
				entry.buffer.hasDynamicOffset = dynamic_binding(r_shader, binding);
				dynamic_storage += entry.buffer.hasDynamicOffset;
				break;
		}
		entries[binding.group].push_back(entry);
	}
	ERR_FAIL_COND_V_MSG(dynamic_uniforms > limits.maxDynamicUniformBuffersPerPipelineLayout || dynamic_storage > limits.maxDynamicStorageBuffersPerPipelineLayout, false,
			"Shader dynamic offsets exceed enabled WebGPU pipeline layout limits.");
	std::vector<WGPUBindGroupLayout> groups;
	for (const auto &group : entries) {
		WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		desc.entryCount = group.size();
		desc.entries = group.data();
		BindGroupLayoutHandle handle(wgpuDeviceCreateBindGroupLayout(p_state.device.get(), &desc));
		ERR_FAIL_COND_V(!handle, false);
		groups.push_back(handle.get());
		r_shader.groups.push_back(std::move(handle));
	}
	WGPUPipelineLayoutDescriptor desc = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
	desc.bindGroupLayoutCount = groups.size();
	desc.bindGroupLayouts = groups.data();
	r_shader.layout.reset(wgpuDeviceCreatePipelineLayout(p_state.device.get(), &desc));
	ERR_FAIL_COND_V(!r_shader.layout, false);
	r_shader.bind_groups.resize(groups.size());
	return true;
}

} // namespace godot::webgpu

RDD::UniformSetID RenderingDeviceDriverWebGPU::uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID, uint32_t p_set_index, int) {
	auto set = std::make_shared<LogicalUniformSet>();
	set->set = p_set_index;
	std::map<uint32_t, std::shared_ptr<BufferResource>> dynamic;
	for (uint32_t index = 0; index < p_uniforms.size(); ++index) {
		const auto &uniform = p_uniforms[index];
		const auto &ids = uniform.ids;
		auto bind = [&](uint32_t p_element, Role p_role) -> LogicalBinding & { return set->bindings[{ p_set_index, uniform.binding, p_element, p_role }]; };
		switch (uniform.type) {
			case UNIFORM_TYPE_SAMPLER:
				if (!uniform.immutable_sampler) {
					for (uint32_t i = 0; i < ids.size(); ++i) {
						bind(i, Role::SAMPLER).sampler = resource<SamplerResource>(ids[i]);
					}
				}
				break;
			case UNIFORM_TYPE_SAMPLER_WITH_TEXTURE:
				DEV_ASSERT(ids.size() % 2 == 0);
				for (uint32_t i = 0; i < ids.size(); i += 2) {
					if (!uniform.immutable_sampler) {
						bind(i / 2, Role::SAMPLER).sampler = resource<SamplerResource>(ids[i]);
					}
					bind(i / 2, Role::TEXTURE).texture = resource<TextureResource>(ids[i + 1]);
				}
				break;
			case UNIFORM_TYPE_TEXTURE:
			case UNIFORM_TYPE_INPUT_ATTACHMENT:
				for (uint32_t i = 0; i < ids.size(); ++i) {
					bind(i, Role::TEXTURE).texture = resource<TextureResource>(ids[i]);
				}
				break;
			case UNIFORM_TYPE_IMAGE:
				for (uint32_t i = 0; i < ids.size(); ++i) {
					const auto &texture = resource<TextureResource>(ids[i]);
					bind(i, texture->backing->atomic_buffer ? Role::STORAGE_BUFFER : Role::STORAGE_TEXTURE).texture = texture;
				}
				break;
			case UNIFORM_TYPE_UNIFORM_BUFFER:
			case UNIFORM_TYPE_STORAGE_BUFFER:
			case UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC:
			case UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC: {
				DEV_ASSERT(ids.size() == 1);
				const Role role = uniform.type == UNIFORM_TYPE_UNIFORM_BUFFER || uniform.type == UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC ? Role::UNIFORM_BUFFER : Role::STORAGE_BUFFER;
				const auto &buffer = resource<BufferResource>(ids[0]);
				bind(0, role).buffer = buffer;
				if (uniform.is_dynamic()) {
					dynamic.emplace(uniform.binding, buffer);
				}
			} break;
			default:
				ERR_FAIL_V_MSG({}, "This logical uniform type cannot be represented in WebGPU.");
		}
	}
	for (const auto &it : dynamic) {
		set->dynamic.push_back(it.second);
	}
	return resource_id<UniformSetID>(std::move(set));
}

void RenderingDeviceDriverWebGPU::uniform_set_free(UniformSetID p_uniform_set) {
	state->assert_owner();
	free_id<LogicalUniformSet>(p_uniform_set);
}

uint32_t RenderingDeviceDriverWebGPU::uniform_sets_get_dynamic_offsets(VectorView<UniformSetID> p_uniform_sets, ShaderID, uint32_t, uint32_t p_set_count) const {
	uint32_t packed = 0, shift = 0;
	for (uint32_t i = 0; i < p_set_count; ++i) {
		for (const auto &buffer : resource<LogicalUniformSet>(p_uniform_sets[i])->dynamic) {
			ERR_FAIL_COND_V(shift >= sizeof(packed) * CHAR_BIT, 0);
			packed |= buffer->frame << shift;
			shift += DYNAMIC_FRAME_BITS;
		}
	}
	return packed;
}

Error RenderingDeviceDriverWebGPU::build_bind_group(ShaderVariant &p_shader, uint32_t p_group,
		const std::map<uint32_t, std::shared_ptr<LogicalUniformSet>> &p_sets, const std::map<const BufferResource *, uint32_t> &p_frames,
		const PushConstantSlice *p_push, PhysicalBindGroup &r_group) {
	state->assert_owner();
	DEV_ASSERT(p_group < p_shader.groups.size());
	const auto &plan = p_shader.prepared->plan;
	std::vector<WGPUBindGroupEntry> entries;
	std::vector<uintptr_t> key;
	std::map<uint32_t, uint32_t> offsets; // WebGPU consumes dynamic offsets in binding-index order.
	UseList uses, pages;
	for (const BindingPlan &binding : plan.bindings) {
		if (binding.group != p_group) {
			continue;
		}
		const auto &logical = binding.key;
		const LogicalBinding *bound = logical_binding(p_sets, logical);
		const auto describe_binding = [&]() {
			return vformat("set %d, binding %d, role %s, element %d (WebGPU group %d, binding %d)",
					logical.set, logical.binding, binding_role_name(logical.role), logical.element, binding.group, binding.binding);
		};
		WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
		entry.binding = binding.binding;
		if (logical.role == Role::PUSH_CONSTANT) {
			ERR_FAIL_NULL_V_MSG(p_push, ERR_INVALID_DATA, "Missing push constants for " + describe_binding());
			ERR_FAIL_COND_V_MSG(p_push->size < plan.push_constant.size, ERR_INVALID_DATA,
					vformat("Push constants for %s require %d bytes, but %d bytes were supplied.",
							describe_binding(), plan.push_constant.size, p_push->size));
			DEV_ASSERT(p_push->buffer && p_push->page);
			DEV_ASSERT(p_push->offset % state->limits.minStorageBufferOffsetAlignment == 0);
			DEV_ASSERT(uint64_t(p_push->offset) + p_push->size <= p_push->buffer->allocation_size);
			entry.buffer = p_push->buffer->buffer.get();
			entry.size = p_push->size;
			offsets.emplace(entry.binding, p_push->offset);
			uses.push_back(p_push->buffer);
			pages.push_back(p_push->page);
		} else if (logical.role == Role::SAMPLER) {
			const auto immutable = p_shader.immutable.find(logical);
			const auto sampler = immutable != p_shader.immutable.end() ? immutable->second : bound ? bound->sampler
																								   : nullptr;
			ERR_FAIL_COND_V_MSG(!sampler, ERR_INVALID_DATA, "Missing sampler for " + describe_binding());
			DEV_ASSERT(binding.sampler_type != SamplerType::NONE && sampler->type != SamplerType::NONE);
			const bool comparison = binding.sampler_type == SamplerType::COMPARISON;
			ERR_FAIL_COND_V_MSG(comparison != (sampler->type == SamplerType::COMPARISON), ERR_INVALID_DATA,
					"WebGPU sampler is incompatible with the compiled binding type at " + describe_binding());
			const bool nearest = binding.sampler_type == SamplerType::NON_FILTERING && sampler->type == SamplerType::FILTERING;
			entry.sampler = nearest ? sampler->non_filtering.get() : sampler->sampler.get();
			uses.push_back(sampler);
		} else {
			ERR_FAIL_NULL_V_MSG(bound, ERR_INVALID_DATA, "Missing resource for " + describe_binding());
			if (logical.role == Role::TEXTURE || logical.role == Role::STORAGE_TEXTURE || binding.format.atomic_buffer) {
				const auto &texture = bound->texture;
				DEV_ASSERT(texture);
				if (binding.format.atomic_buffer) {
					ERR_FAIL_COND_V_MSG(!texture->backing->atomic_buffer, ERR_INVALID_DATA,
							"Missing atomic image backing for " + describe_binding());
					ERR_FAIL_COND_V_MSG(static_cast<uint32_t>(texture->format.storage_format) != binding.format.logical_format ||
									texture->dimension != shader_view_dimension(binding.format),
							ERR_INVALID_DATA, "Atomic image format or dimension does not match ShaderPlan at " + describe_binding());
					entry.buffer = texture->backing->atomic_buffer.get();
					entry.size = texture->backing->atomic_size;
				} else if (logical.role == Role::STORAGE_TEXTURE) {
					const auto physical = storage_texture_format(static_cast<spv::ImageFormat>(binding.format.physical_format));
					ERR_FAIL_COND_V_MSG(!texture->storage || texture->format.format != physical, ERR_INVALID_DATA,
							vformat("Storage texture view does not match ShaderPlan at %s (expected format %d, got %d).",
									describe_binding(), static_cast<uint32_t>(physical), static_cast<uint32_t>(texture->format.format)));
					entry.textureView = texture->storage.get();
				} else {
					const bool depth = binding.format.sample_type == SampleType::DEPTH;
					entry.textureView = depth ? texture->depth_sampling.get() : texture->sampling.get();
					const bool compatible = texture->format.sample_type == binding.format.sample_type ||
							(binding.format.sample_type == SampleType::UNFILTERABLE_FLOAT && texture->format.sample_type == SampleType::FLOAT);
					ERR_FAIL_COND_V_MSG(!entry.textureView || !compatible || texture->dimension != shader_view_dimension(binding.format) ||
									binding.format.multisampled != (texture->backing->desc.samples != TEXTURE_SAMPLES_1),
							ERR_INVALID_DATA,
							"Sampled texture view does not match ShaderPlan at " + describe_binding());
				}
				uses.push_back(texture);
				uses.push_back(texture->backing);
			} else {
				const auto &buffer = bound->buffer;
				DEV_ASSERT(buffer);
				entry.buffer = buffer->buffer.get();
				entry.size = align_up(buffer->size, BUFFER_COPY_ALIGNMENT);
				if (dynamic_binding(p_shader, binding)) {
					DEV_ASSERT(buffer->access == BufferAccess::DYNAMIC);
					const auto it = p_frames.find(buffer.get());
					const uint32_t frame = it == p_frames.end() ? buffer->frame : it->second;
					DEV_ASSERT(frame < frame_count && buffer->offset(frame) <= UINT32_MAX);
					offsets.emplace(entry.binding, buffer->offset(frame));
					pages.push_back(buffer->pages[frame]);
				}
				uses.push_back(buffer);
			}
		}
		key.insert(key.end(), { reinterpret_cast<uintptr_t>(entry.buffer), reinterpret_cast<uintptr_t>(entry.sampler), reinterpret_cast<uintptr_t>(entry.textureView), static_cast<uintptr_t>(entry.size) });
		entries.push_back(entry);
	}
	auto &cache = p_shader.bind_groups[p_group];
	if (cache.last_cleanup != state->completed()) {
		for (auto it = cache.entries.begin(); it != cache.entries.end();) {
			it = it->second.expired() ? cache.entries.erase(it) : std::next(it);
		}
		cache.last_cleanup = state->completed();
	}
	std::shared_ptr<BindGroupResource> group;
	if (auto it = cache.entries.find(key); it != cache.entries.end()) {
		group = it->second.lock();
	}
	if (!group) {
		group = std::make_shared<BindGroupResource>();
		group->resources = std::move(uses);
		WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		const auto logical_set = p_sets.find(p_group);
		if (logical_set != p_sets.end()) {
			desc.label = wg_string(logical_set->second->label);
		}
		desc.layout = p_shader.groups[p_group].get();
		desc.entryCount = entries.size();
		desc.entries = entries.data();
		group->handle.reset(wgpuDeviceCreateBindGroup(state->device.get(), &desc));
		ERR_FAIL_COND_V(!group->handle, ERR_CANT_CREATE);
		cache.entries.insert_or_assign(std::move(key), group);
	}
	r_group.group = group;
	r_group.uses = std::move(pages);
	r_group.uses.insert(r_group.uses.end(), group->resources.begin(), group->resources.end());
	r_group.uses.push_back(std::move(group));
	r_group.dynamic_offsets.clear();
	for (const auto &offset : offsets) {
		r_group.dynamic_offsets.push_back(offset.second);
	}
	return OK;
}
