#include "webgpu_commands.h"

using namespace godot::webgpu;

RDD::AccelerationStructureID RenderingDeviceDriverWebGPU::blas_create(VectorView<AccelerationStructureGeometry> , BitField<AccelerationStructureFlagBits> ) {
	ERR_FAIL_V_MSG({}, "WebGPU does not expose hardware ray tracing.");
}

RDD::AccelerationStructureID RenderingDeviceDriverWebGPU::tlas_create(uint32_t , BitField<AccelerationStructureFlagBits> ) {
	ERR_FAIL_V_MSG({}, "WebGPU does not expose hardware ray tracing.");
}

void RenderingDeviceDriverWebGPU::acceleration_structure_instance_write(uint8_t *, const AccelerationStructureInstance &) {
	ERR_FAIL_MSG("WebGPU does not expose hardware ray tracing.");
}

void RenderingDeviceDriverWebGPU::acceleration_structure_free(AccelerationStructureID ) {
	ERR_FAIL_MSG("WebGPU does not expose hardware ray tracing.");
}

uint32_t RenderingDeviceDriverWebGPU::acceleration_structure_get_scratch_size_bytes(AccelerationStructureID ) {
	ERR_FAIL_V_MSG(0, "WebGPU does not expose hardware ray tracing.");
}

RDD::RaytracingPipelineID RenderingDeviceDriverWebGPU::raytracing_pipeline_create(VectorView<PipelineShader> , VectorView<uint32_t> , VectorView<uint32_t> , VectorView<HitGroup> , uint32_t , ShaderID ) {
	ERR_FAIL_V_MSG({}, "WebGPU does not expose hardware ray tracing.");
}

void RenderingDeviceDriverWebGPU::raytracing_pipeline_free(RaytracingPipelineID ) {
	ERR_FAIL_MSG("WebGPU does not expose hardware ray tracing.");
}

bool RenderingDeviceDriverWebGPU::raytracing_pipeline_get_shader_group_handles(RaytracingPipelineID , uint32_t , VectorView<uint32_t> , uint8_t *, uint32_t ) {
	ERR_FAIL_V_MSG(false, "WebGPU does not expose hardware ray tracing.");
}

void RenderingDeviceDriverWebGPU::command_build_blas(CommandBufferID , AccelerationStructureID , BufferID ) {
	ERR_FAIL_MSG("WebGPU does not expose hardware ray tracing.");
}

void RenderingDeviceDriverWebGPU::command_build_tlas(CommandBufferID , AccelerationStructureID , BufferID , BufferID , uint32_t , uint32_t ) {
	ERR_FAIL_MSG("WebGPU does not expose hardware ray tracing.");
}

void RenderingDeviceDriverWebGPU::command_bind_raytracing_pipeline(CommandBufferID , RaytracingPipelineID ) {
	ERR_FAIL_MSG("WebGPU does not expose hardware ray tracing.");
}

void RenderingDeviceDriverWebGPU::command_bind_raytracing_uniform_set(CommandBufferID , UniformSetID , ShaderID , uint32_t ) {
	ERR_FAIL_MSG("WebGPU does not expose hardware ray tracing.");
}

void RenderingDeviceDriverWebGPU::command_trace_rays(CommandBufferID , const ShaderBindingTable &, const ShaderBindingTable &, const ShaderBindingTable &, uint32_t , uint32_t , uint32_t ) {
	ERR_FAIL_MSG("WebGPU does not expose hardware ray tracing.");
}


uint64_t RenderingDeviceDriverWebGPU::api_trait_get(ApiTrait p_trait) {
	switch (p_trait) {
		case API_TRAIT_ASYNC_GPU_COMPLETION:
		case API_TRAIT_OWNER_THREAD_GPU_OBJECTS:
			return true;
		case API_TRAIT_CLEARS_WITH_COPY_ENGINE:
		case API_TRAIT_SECONDARY_VIEWPORT_SCISSOR:
		case API_TRAIT_SUPPORTS_INPUT_ATTACHMENTS:
		case API_TRAIT_SUPPORTS_TEXTURE_VIEW_SWIZZLE:
		case API_TRAIT_SUPPORTS_TEXEL_BUFFERS:
		case API_TRAIT_SUPPORTS_CLAMP_TO_BORDER:
			return false;
		case API_TRAIT_SUPPORTS_STORAGE_TEXTURE_BARRIERS:
			return state->has_read_write_storage_textures;
		case API_TRAIT_SUPPORTS_EXTENDED_STORAGE_FORMATS_READ_WRITE:
			return state->has_read_write_storage_textures && state->has_texture_formats_tier2;
		case API_TRAIT_TEXTURE_TRANSFER_ALIGNMENT:
			return BUFFER_COPY_ALIGNMENT;
		case API_TRAIT_TEXTURE_DATA_ROW_PITCH_STEP:
			return COPY_ROW_ALIGNMENT;
		default:
			return RenderingDeviceDriver::api_trait_get(p_trait);
	}
}

uint64_t RenderingDeviceDriverWebGPU::limit_get(Limit p_limit) {
	const auto &l = state->limits;
	switch (p_limit) {
		case LIMIT_MAX_BOUND_UNIFORM_SETS: return l.maxBindGroups;
		case LIMIT_MAX_FRAMEBUFFER_COLOR_ATTACHMENTS: return l.maxColorAttachments;
		case LIMIT_MAX_TEXTURES_PER_UNIFORM_SET:
		case LIMIT_MAX_TEXTURES_PER_SHADER_STAGE: return l.maxSampledTexturesPerShaderStage;
		case LIMIT_MAX_SAMPLERS_PER_UNIFORM_SET:
		case LIMIT_MAX_SAMPLERS_PER_SHADER_STAGE: return l.maxSamplersPerShaderStage;
		case LIMIT_MAX_STORAGE_BUFFERS_PER_UNIFORM_SET:
		case LIMIT_MAX_STORAGE_BUFFERS_PER_SHADER_STAGE: return l.maxStorageBuffersPerShaderStage;
		case LIMIT_MAX_STORAGE_IMAGES_PER_UNIFORM_SET:
		case LIMIT_MAX_STORAGE_IMAGES_PER_SHADER_STAGE: return l.maxStorageTexturesPerShaderStage;
		case LIMIT_MAX_UNIFORM_BUFFERS_PER_UNIFORM_SET:
		case LIMIT_MAX_UNIFORM_BUFFERS_PER_SHADER_STAGE: return l.maxUniformBuffersPerShaderStage;
		case LIMIT_MAX_DRAW_INDEXED_INDEX: return UINT32_MAX;
		case LIMIT_MAX_FRAMEBUFFER_HEIGHT:
		case LIMIT_MAX_FRAMEBUFFER_WIDTH:
		case LIMIT_MAX_VIEWPORT_DIMENSIONS_X:
		case LIMIT_MAX_VIEWPORT_DIMENSIONS_Y:
		case LIMIT_MAX_TEXTURE_SIZE_2D:
		case LIMIT_MAX_TEXTURE_SIZE_CUBE: return l.maxTextureDimension2D;
		case LIMIT_MAX_TEXTURE_ARRAY_LAYERS: return l.maxTextureArrayLayers;
		case LIMIT_MAX_TEXTURE_SIZE_1D: return l.maxTextureDimension1D;
		case LIMIT_MAX_TEXTURE_SIZE_3D: return l.maxTextureDimension3D;
		case LIMIT_MAX_PUSH_CONSTANT_SIZE: return MAX_PUSH_CONSTANT_BYTES;
		case LIMIT_MAX_UNIFORM_BUFFER_SIZE: return l.maxUniformBufferBindingSize;
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTE_OFFSET: return l.maxVertexBufferArrayStride - BUFFER_COPY_ALIGNMENT;
		case LIMIT_MAX_VERTEX_INPUT_ATTRIBUTES: return l.maxVertexAttributes;
		case LIMIT_MAX_VERTEX_INPUT_BINDINGS: return l.maxVertexBuffers;
		case LIMIT_MAX_VERTEX_INPUT_BINDING_STRIDE: return l.maxVertexBufferArrayStride;
		case LIMIT_MIN_UNIFORM_BUFFER_OFFSET_ALIGNMENT: return l.minUniformBufferOffsetAlignment;
		case LIMIT_MAX_COMPUTE_SHARED_MEMORY_SIZE: return l.maxComputeWorkgroupStorageSize;
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X:
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y:
		case LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Z: return l.maxComputeWorkgroupsPerDimension;
		case LIMIT_MAX_COMPUTE_WORKGROUP_INVOCATIONS: return l.maxComputeInvocationsPerWorkgroup;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_X: return l.maxComputeWorkgroupSizeX;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Y: return l.maxComputeWorkgroupSizeY;
		case LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Z: return l.maxComputeWorkgroupSizeZ;
		case LIMIT_MAX_SHADER_VARYINGS: return l.maxInterStageShaderVariables;
		default: return 0;
	}
}

bool RenderingDeviceDriverWebGPU::has_feature(Features p_feature) {
	switch (p_feature) {
		case SUPPORTS_HALF_FLOAT: return state->has(WGPUFeatureName_ShaderF16);
		case SUPPORTS_FRAGMENT_SHADER_WITH_ONLY_SIDE_EFFECTS:
		case SUPPORTS_FRAMEBUFFER_DEPTH_RESOLVE:
		case SUPPORTS_IMAGE_ATOMIC_32_BIT: return true;
		default: return false;
	}
}

const RDD::MultiviewCapabilities &RenderingDeviceDriverWebGPU::get_multiview_capabilities() {
	static const MultiviewCapabilities value;
	return value;
}

const RDD::FragmentShadingRateCapabilities &RenderingDeviceDriverWebGPU::get_fragment_shading_rate_capabilities() {
	static const FragmentShadingRateCapabilities value;
	return value;
}

const RDD::FragmentDensityMapCapabilities &RenderingDeviceDriverWebGPU::get_fragment_density_map_capabilities() {
	static const FragmentDensityMapCapabilities value;
	return value;
}

const RDD::Capabilities &RenderingDeviceDriverWebGPU::get_capabilities() const {
	static const Capabilities value{ DEVICE_WEBGPU, 1, 0 };
	return value;
}

String RenderingDeviceDriverWebGPU::get_api_name() const { return "WebGPU"; }
String RenderingDeviceDriverWebGPU::get_api_version() const { return "1.0"; }
String RenderingDeviceDriverWebGPU::get_pipeline_cache_uuid() const { return {}; }
bool RenderingDeviceDriverWebGPU::pipeline_cache_create(const Vector<uint8_t> &) { return false; }
void RenderingDeviceDriverWebGPU::pipeline_cache_free() {}
size_t RenderingDeviceDriverWebGPU::pipeline_cache_query_size() { return 0; }
Vector<uint8_t> RenderingDeviceDriverWebGPU::pipeline_cache_serialize() { return {}; }
uint64_t RenderingDeviceDriverWebGPU::get_total_memory_used() { return 0; } // WebGPU does not expose heap residency.
uint64_t RenderingDeviceDriverWebGPU::get_lazily_memory_used() { return 0; }

bool RenderingDeviceDriverWebGPU::is_composite_alpha_supported(CommandQueueID) const {
	// The surface selects premultiplied alpha when requested during configuration.
	return true;
}

void RenderingDeviceDriverWebGPU::set_object_name(ObjectType p_type, ID p_id, const String &p_name) {
	state->assert_owner();
	const CharString utf8 = p_name.utf8();
	const WGPUStringView label{ utf8.get_data(), size_t(utf8.length()) };
	switch (p_type) {
		case OBJECT_TYPE_TEXTURE: {
			const auto &backing = *resource<TextureResource>(p_id)->backing;
			if (backing.texture) {
				wgpuTextureSetLabel(backing.texture.get(), label);
			} else {
				wgpuBufferSetLabel(backing.atomic_buffer.get(), label);
			}
		} break;
		case OBJECT_TYPE_BUFFER: wgpuBufferSetLabel(resource<BufferResource>(p_id)->buffer.get(), label); break;
		case OBJECT_TYPE_SAMPLER: {
			const auto &sampler = *resource<SamplerResource>(p_id);
			wgpuSamplerSetLabel(sampler.sampler.get(), label);
			if (sampler.non_filtering) {
				wgpuSamplerSetLabel(sampler.non_filtering.get(), label);
			}
		} break;
		case OBJECT_TYPE_SHADER: {
			auto &shader = *resource<ShaderResource>(p_id);
			shader.label.assign(label.data, label.length);
			for (const auto &[key, variant] : shader.variants) {
				for (const auto &module : variant->modules) {
					if (module) {
						wgpuShaderModuleSetLabel(module.get(), label);
					}
				}
			}
		} break;
		case OBJECT_TYPE_PIPELINE: {
			auto &pipeline = *resource<PipelineResource>(p_id);
			pipeline.label.assign(label.data, label.length);
			for (const auto &[key, variant] : pipeline.variants) {
				if (variant->compute) {
					wgpuComputePipelineSetLabel(variant->compute.get(), label);
				}
				for (const auto &render : variant->raster) {
					if (render) {
						wgpuRenderPipelineSetLabel(render.get(), label);
					}
				}
			}
		} break;
		case OBJECT_TYPE_UNIFORM_SET: resource<LogicalUniformSet>(p_id)->label.assign(label.data, label.length); break;
		default: break;
	}
}

uint64_t RenderingDeviceDriverWebGPU::get_resource_native_handle(DriverResource p_type, ID p_id) {
	switch (p_type) {
		case DRIVER_RESOURCE_LOGICAL_DEVICE: return reinterpret_cast<uint64_t>(state->device.get());
		case DRIVER_RESOURCE_TOPMOST_OBJECT: return reinterpret_cast<uint64_t>(state->instance.get());
		case DRIVER_RESOURCE_COMMAND_QUEUE: return reinterpret_cast<uint64_t>(state->queue.get());
		case DRIVER_RESOURCE_TEXTURE: return reinterpret_cast<uint64_t>(resource<TextureResource>(p_id)->backing->texture.get());
		case DRIVER_RESOURCE_TEXTURE_VIEW: {
			const auto &tex = *resource<TextureResource>(p_id);
			return reinterpret_cast<uint64_t>(tex.format.depth ? tex.depth_sampling.get() : tex.sampling.get());
		}
		case DRIVER_RESOURCE_TEXTURE_DATA_FORMAT: return resource<TextureResource>(p_id)->format.format;
		case DRIVER_RESOURCE_SAMPLER: return reinterpret_cast<uint64_t>(resource<SamplerResource>(p_id)->sampler.get());
		case DRIVER_RESOURCE_BUFFER: return reinterpret_cast<uint64_t>(resource<BufferResource>(p_id)->buffer.get());
		default: return 0; // No single native handle for logical variant pipelines and folded uniform sets.
	}
}
