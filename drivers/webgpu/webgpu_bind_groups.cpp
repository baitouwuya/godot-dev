#include "webgpu_shader_resources.h"

#include <algorithm>
#include <atomic>

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

const LogicalBinding *logical_binding(const LogicalUniformSets &p_sets, const BindingKey &p_key) {
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
			r_shader.bind_groups.resize(binding.group + 1);
		}
		auto &group = r_shader.bind_groups[binding.group];
		group.bindings.push_back({ &binding, nullptr, dynamic_binding(r_shader, binding) });
		group.uses_push |= binding.key.role == Role::PUSH_CONSTANT;
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
				entry.buffer.hasDynamicOffset = group.bindings.back().dynamic;
				dynamic_uniforms += entry.buffer.hasDynamicOffset;
				break;
			case Role::PUSH_CONSTANT:
				entry.buffer.minBindingSize = plan.push_constant.size;
				[[fallthrough]];
			case Role::STORAGE_BUFFER:
				entry.buffer.type = binding.read_only ? WGPUBufferBindingType_ReadOnlyStorage : WGPUBufferBindingType_Storage;
				entry.buffer.hasDynamicOffset = group.bindings.back().dynamic;
				dynamic_storage += entry.buffer.hasDynamicOffset;
				break;
		}
		entries[binding.group].push_back(entry);
	}
	ERR_FAIL_COND_V_MSG(dynamic_uniforms > limits.maxDynamicUniformBuffersPerPipelineLayout || dynamic_storage > limits.maxDynamicStorageBuffersPerPipelineLayout, false,
			"Shader dynamic offsets exceed enabled WebGPU pipeline layout limits.");
	std::vector<WGPUBindGroupLayout> groups;
	for (uint32_t i = 0; i < entries.size(); ++i) {
		auto &group = r_shader.bind_groups[i];
		auto &layout_entries = entries[i];
		std::sort(layout_entries.begin(), layout_entries.end(), [](const auto &p_a, const auto &p_b) { return p_a.binding < p_b.binding; });
		std::sort(group.bindings.begin(), group.bindings.end(), [](const auto &p_a, const auto &p_b) { return p_a.binding->binding < p_b.binding->binding; });
		if (!group.uses_push) {
			for (size_t j = 0; j < layout_entries.size(); ++j) {
				const auto &entry = layout_entries[j];
				const auto &planned = group.bindings[j];
				const auto &binding = *planned.binding;
				const auto &key = binding.key;
				// Compare descriptor fields, not C-struct padding or frontend handles.
				// Layout equivalence alone does not establish the logical-to-physical mapping.
				group.recipe_key.insert(group.recipe_key.end(), { entry.binding, entry.visibility,
						entry.buffer.type, entry.buffer.hasDynamicOffset, entry.buffer.minBindingSize, entry.sampler.type,
						entry.texture.sampleType, entry.texture.viewDimension, entry.texture.multisampled,
						entry.storageTexture.access, entry.storageTexture.format, entry.storageTexture.viewDimension,
						key.set, key.binding, key.element, uint64_t(key.role),
						binding.format.atomic_buffer, binding.format.atomic_buffer ? binding.format.logical_format : 0,
						binding.format.atomic_buffer ? shader_view_dimension(binding.format) : WGPUTextureViewDimension_Undefined });
			}
		}
		WGPUBindGroupLayoutDescriptor desc = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		desc.entryCount = layout_entries.size();
		desc.entries = layout_entries.data();
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
	return true;
}

} // namespace godot::webgpu

RDD::UniformSetID RenderingDeviceDriverWebGPU::uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID, uint32_t p_set_index, int) {
	static std::atomic<uint64_t> next_uid{ 1 };
	auto set = std::make_shared<LogicalUniformSet>();
	set->uid = next_uid.fetch_add(1, std::memory_order_relaxed);
	set->set = p_set_index;
	std::map<uint32_t, LogicalBinding *> dynamic;
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
				auto &binding = bind(0, role);
				binding.buffer = buffer;
				if (uniform.is_dynamic()) {
					dynamic.emplace(uniform.binding, &binding);
				}
			} break;
			default:
				ERR_FAIL_V_MSG({}, "This logical uniform type cannot be represented in WebGPU.");
		}
	}
	for (const auto &entry : dynamic) {
		auto *binding = entry.second;
		binding->dynamic_frame_shift = set->dynamic.size() * DYNAMIC_FRAME_BITS;
		set->dynamic.push_back(binding->buffer);
	}
	for (const auto &[key, binding] : set->bindings) {
		if (!binding.texture) {
			continue;
		}
		const auto &format = binding.texture->format;
		const auto storage_format = static_cast<uint32_t>(format.storage_format);
		if (key.role == Role::TEXTURE && format.depth) {
			set->specialization.push_back({ key, BindingSpecialization::RAW_DEPTH });
		} else if (key.role == Role::TEXTURE && format.sample_type == SampleType::UNFILTERABLE_FLOAT) {
			set->specialization.push_back({ key, BindingSpecialization::UNFILTERABLE_FLOAT });
		} else if (key.role == Role::STORAGE_TEXTURE) {
			set->specialization.push_back({ key, BindingSpecialization::STORAGE_FORMAT, storage_format });
		} else if (key.role == Role::STORAGE_BUFFER) {
			// The compiler keys its atomic override by the original SPIR-V image.
			set->specialization.push_back({ { key.set, key.binding, key.element, Role::STORAGE_TEXTURE }, BindingSpecialization::ATOMIC_IMAGE, storage_format });
		}
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
			ERR_FAIL_COND_V(shift + DYNAMIC_FRAME_BITS > sizeof(packed) * CHAR_BIT, 0);
			packed |= buffer->frame << shift;
			shift += DYNAMIC_FRAME_BITS;
		}
	}
	return packed;
}

Error RenderingDeviceDriverWebGPU::get_bind_group(ShaderVariant &r_shader, uint32_t p_group, const LogicalUniformSets &p_sets,
		const PushConstantSlice *p_push, BindGroupScratch &r_scratch, std::shared_ptr<BindGroupResource> &r_group) {
	state->assert_owner();
	DEV_ASSERT(p_group < r_shader.groups.size());
	auto &plan = r_shader.bind_groups[p_group];
	if (!plan.bindings_resolved) {
		// Resolve immutable samplers against the fully realized variant, not during layout creation.
		for (auto &entry : plan.bindings) {
			const auto &key = entry.binding->key;
			if (key.role == Role::SAMPLER) {
				const auto sampler = r_shader.immutable.find(key);
				if (sampler != r_shader.immutable.end()) {
					entry.immutable = &sampler->second;
				}
			}
			if (!entry.immutable && key.role != Role::PUSH_CONSTANT) {
				plan.logical_sets.push_back(key.set);
			}
		}
		std::sort(plan.logical_sets.begin(), plan.logical_sets.end());
		plan.logical_sets.erase(std::unique(plan.logical_sets.begin(), plan.logical_sets.end()), plan.logical_sets.end());
		if (!plan.uses_push && plan.logical_sets.size() <= 1) {
			for (const auto &entry : plan.bindings) {
				plan.recipe_key.push_back(entry.immutable ? reinterpret_cast<uintptr_t>(entry.immutable->get()) : 0);
			}
		} else {
			plan.recipe_key = std::vector<uint64_t>();
		}
		plan.bindings_resolved = true;
	}
	auto &key = r_scratch.key;
	key.clear();
	const bool share_recipe = !plan.uses_push && plan.logical_sets.size() <= 1;
	LogicalUniformSet *set = nullptr;
	bool cacheable = true;
	for (uint32_t index : plan.logical_sets) {
		const auto it = p_sets.find(index);
		ERR_FAIL_COND_V_MSG(it == p_sets.end(), ERR_INVALID_DATA, vformat("Missing logical set %d for WebGPU group %d.", index, p_group));
		set = it->second.get();
		cacheable &= set->uid != 0; // Direct callers may supply temporary sets not created by uniform_set_create().
		if (!share_recipe) {
			key.push_back(set->uid);
		}
	}
	if (plan.uses_push) {
		ERR_FAIL_NULL_V_MSG(p_push, ERR_INVALID_DATA, vformat("Missing push constants for WebGPU group %d.", p_group));
		ERR_FAIL_COND_V_MSG(p_push->size < r_shader.prepared->plan.push_constant.size, ERR_INVALID_DATA,
				vformat("Insufficient push constant data for WebGPU group %d.", p_group));
		DEV_ASSERT(p_push->buffer && p_push->page);
		DEV_ASSERT(p_push->offset % state->limits.minStorageBufferOffsetAlignment == 0);
		DEV_ASSERT(uint64_t(p_push->offset) + p_push->size <= p_push->buffer->allocation_size);
	}

	std::shared_ptr<BindGroupResource> *slot = nullptr;
	if (cacheable && share_recipe) {
		if (!plan.recipe) {
			const SubmitSerial completed = state->completed();
			if (bind_group_recipes_cleanup != completed) {
				for (auto it = bind_group_recipes.begin(); it != bind_group_recipes.end();) {
					it = it->second.expired() ? bind_group_recipes.erase(it) : std::next(it);
				}
				bind_group_recipes_cleanup = completed;
			}
			auto recipe_key = std::move(plan.recipe_key);
			auto &recipe = bind_group_recipes[std::move(recipe_key)];
			plan.recipe = recipe.lock();
			if (!plan.recipe) {
				plan.recipe = std::make_shared<BindGroupRecipe>();
				for (const auto &entry : plan.bindings) {
					if (entry.immutable) {
						plan.recipe->immutable.push_back(*entry.immutable);
					}
				}
				recipe = plan.recipe;
			}
		}
		slot = set ? &set->bind_groups[plan.recipe] : &plan.recipe->static_group;
		if (*slot) {
			r_group = *slot;
			return OK;
		}
	} else if (cacheable) {
		if (plan.uses_push) {
			// A live group owns this buffer. Its byte offset is dynamic, its size is not.
			key.push_back(reinterpret_cast<uintptr_t>(p_push->buffer.get()));
			key.push_back(p_push->size);
		}
		const auto found = plan.groups.find(key);
		if (found != plan.groups.end()) {
			r_group = found->second.lock();
			if (r_group) {
				return OK;
			}
		}
		const SubmitSerial completed = state->completed();
		if (plan.last_cleanup != completed) {
			for (auto it = plan.groups.begin(); it != plan.groups.end();) {
				it = it->second.expired() ? plan.groups.erase(it) : std::next(it);
			}
			plan.last_cleanup = completed;
		}
	}

	auto group = std::make_shared<BindGroupResource>();
	auto &entries = r_scratch.entries;
	entries.clear();
	entries.reserve(plan.bindings.size());
	for (const auto &planned : plan.bindings) {
		const auto &binding = *planned.binding;
		const auto &logical = binding.key;
		const auto describe_binding = [&]() {
			return vformat("set %d, binding %d, role %s, element %d (WebGPU group %d, binding %d)",
					logical.set, logical.binding, binding_role_name(logical.role), logical.element, binding.group, binding.binding);
		};
		WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
		entry.binding = binding.binding;
		const void *buffer_resource = nullptr;
		const uint32_t dynamic_index = planned.dynamic ? uint32_t(group->dynamic.size()) : UINT32_MAX;
		if (logical.role == Role::PUSH_CONSTANT) {
			entry.buffer = p_push->buffer->buffer.get();
			entry.size = p_push->size;
			buffer_resource = p_push->buffer.get();
			group->dynamic.push_back({ nullptr, 0, 0 });
			group->resources.push_back(p_push->buffer);
		} else if (logical.role == Role::SAMPLER) {
			const LogicalBinding *bound = planned.immutable ? nullptr : logical_binding(p_sets, logical);
			const auto sampler = planned.immutable ? *planned.immutable : bound ? bound->sampler : nullptr;
			ERR_FAIL_COND_V_MSG(!sampler, ERR_INVALID_DATA, "Missing sampler for " + describe_binding());
			DEV_ASSERT(binding.sampler_type != SamplerType::NONE && sampler->type != SamplerType::NONE);
			const bool comparison = binding.sampler_type == SamplerType::COMPARISON;
			ERR_FAIL_COND_V_MSG(comparison != (sampler->type == SamplerType::COMPARISON), ERR_INVALID_DATA,
					"WebGPU sampler is incompatible with the compiled binding type at " + describe_binding());
			const bool nearest = binding.sampler_type == SamplerType::NON_FILTERING && sampler->type == SamplerType::FILTERING;
			entry.sampler = nearest ? sampler->non_filtering.get() : sampler->sampler.get();
			group->resources.push_back(sampler);
		} else {
			const LogicalBinding *bound = logical_binding(p_sets, logical);
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
					buffer_resource = texture->backing.get();
				} else if (logical.role == Role::STORAGE_TEXTURE) {
					const auto physical = storage_texture_format(static_cast<spv::ImageFormat>(binding.format.physical_format));
					ERR_FAIL_COND_V_MSG(!texture->storage || texture->format.format != physical, ERR_INVALID_DATA,
							vformat("Storage texture view does not match ShaderPlan at %s (expected format %d, got %d).",
									describe_binding(), static_cast<uint32_t>(physical), static_cast<uint32_t>(texture->format.format)));
					entry.textureView = texture->storage.get();
					const ScopeUse use = binding.format.access == ImageAccess::READ ? ScopeUse::STORAGE_READ : ScopeUse::STORAGE_WRITE;
					const auto range = texture_scope_use(*texture, use, 1, texture->layer_count, WGPUTextureAspect_All);
					group->usage.add_texture(range);
					if (use == ScopeUse::STORAGE_WRITE) {
						group->writable_textures.push_back({ range, binding.stages });
					}
				} else {
					const bool depth = binding.format.sample_type == SampleType::DEPTH;
					entry.textureView = depth ? texture->depth_sampling.get() : texture->sampling.get();
					const bool compatible = texture->format.sample_type == binding.format.sample_type ||
							(binding.format.sample_type == SampleType::UNFILTERABLE_FLOAT && texture->format.sample_type == SampleType::FLOAT);
					ERR_FAIL_COND_V_MSG(!entry.textureView || !compatible || texture->dimension != shader_view_dimension(binding.format) ||
									binding.format.multisampled != (texture->backing->desc.samples != TEXTURE_SAMPLES_1),
							ERR_INVALID_DATA,
							"Sampled texture view does not match ShaderPlan at " + describe_binding());
					group->usage.add_texture(texture_scope_use(*texture, ScopeUse::READ, texture->mip_count, texture->layer_count,
							depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All));
				}
				group->resources.push_back(texture);
				group->resources.push_back(texture->backing);
			} else {
				const auto &buffer = bound->buffer;
				DEV_ASSERT(buffer);
				entry.buffer = buffer->buffer.get();
				entry.size = align_up(buffer->size, BUFFER_COPY_ALIGNMENT);
				buffer_resource = buffer.get();
				if (planned.dynamic) {
					DEV_ASSERT(buffer->access == BufferAccess::DYNAMIC);
					group->dynamic.push_back({ buffer.get(), logical.set, bound->dynamic_frame_shift });
				}
				group->resources.push_back(buffer);
			}
		}
		if (buffer_resource) {
			const ScopeUse use = logical.role == Role::UNIFORM_BUFFER ? ScopeUse::READ : binding.read_only ? ScopeUse::STORAGE_READ : ScopeUse::STORAGE_WRITE;
			group->usage.add_buffer(buffer_resource, use);
			if (use == ScopeUse::STORAGE_WRITE) {
				group->writable_buffers.push_back({ buffer_resource, entry.size, dynamic_index, binding.stages });
			}
		}
		entries.push_back(entry);
	}
	group->internal_conflict = !group->usage.compatible(group->usage);
	WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	if (set) {
		desc.label = wg_string(set->label);
	}
	desc.layout = r_shader.groups[p_group].get();
	desc.entryCount = entries.size();
	desc.entries = entries.data();
	group->handle.reset(wgpuDeviceCreateBindGroup(state->device.get(), &desc));
	ERR_FAIL_COND_V(!group->handle, ERR_CANT_CREATE);
	if (slot) {
		*slot = group;
	} else if (cacheable) {
		plan.groups.insert_or_assign(key, group);
	}
	r_group = std::move(group);
	return OK;
}

Error RenderingDeviceDriverWebGPU::build_bind_group(ShaderVariant &p_shader, uint32_t p_group, const LogicalUniformSets &p_sets,
		const std::map<const BufferResource *, uint32_t> &p_frames, const PushConstantSlice *p_push, PhysicalBindGroup &r_group) {
	BindGroupScratch scratch;
	const Error error = get_bind_group(p_shader, p_group, p_sets, p_push, scratch, r_group.group);
	if (error != OK) {
		return error;
	}
	// Direct callers can submit just the returned use list. Preserve that contract.
	r_group.uses = r_group.group->resources;
	r_group.uses.push_back(r_group.group);
	r_group.dynamic_offsets.clear();
	for (const auto &binding : r_group.group->dynamic) {
		if (!binding.buffer) {
			r_group.dynamic_offsets.push_back(p_push->offset);
			r_group.uses.push_back(p_push->page);
		} else {
			const auto frame_it = p_frames.find(binding.buffer);
			const uint32_t frame = frame_it == p_frames.end() ? binding.buffer->frame : frame_it->second;
			DEV_ASSERT(frame < frame_count && binding.buffer->offset(frame) <= UINT32_MAX);
			r_group.dynamic_offsets.push_back(binding.buffer->offset(frame));
			r_group.uses.push_back(binding.buffer->pages[frame]);
		}
	}
	return OK;
}
