#pragma once

#include "rendering_context_driver_webgpu.h"
#include "webgpu_formats.h"

#include <array>
#include <optional>

class RenderingShaderContainerWebGPU;

namespace godot::webgpu {

template <class T>
const std::shared_ptr<T> &resource(RDD::ID p_id) {
	DEV_ASSERT(p_id.id);
	return *reinterpret_cast<const std::shared_ptr<T> *>(p_id.id);
}

template <class Id, class T>
Id resource_id(std::shared_ptr<T> p_resource) {
	return Id(memnew(std::shared_ptr<T>(std::move(p_resource))));
}

template <class T>
void free_id(RDD::ID p_id) {
	DEV_ASSERT(p_id.id);
	memdelete(reinterpret_cast<std::shared_ptr<T> *>(p_id.id));
}

enum class BufferAccess : uint8_t { GPU,
	UPLOAD,
	DOWNLOAD,
	DYNAMIC };
enum class DownloadState : uint8_t { IDLE,
	PENDING,
	MAPPED,
	FAILED };

struct BufferResource : GpuUse {
	struct DownloadRegion {
		std::shared_ptr<BufferResource> staging;
		TextureFormatInfo format;
		WGPUExtent3D extent;
		uint64_t offset;
		uint32_t src_pitch;
		uint32_t dst_pitch;
		uint64_t source_offset = 0;
		uint64_t byte_count = 0;
	};
	BufferHandle buffer;
	WGPUBufferUsage usage = WGPUBufferUsage_None;
	BufferAccess access = BufferAccess::GPU;
	DownloadState download_state = DownloadState::IDLE;
	uint64_t size = 0;
	uint64_t allocation_size = 0;
	uint64_t frame_stride = 0;
	uint32_t frame = 0;
	bool cpu_mapped = false;
	std::vector<uint8_t> upload;
	std::vector<DownloadRegion> download_regions;
	std::vector<uint8_t> download_bytes;
	std::array<std::shared_ptr<GpuUse>, MAX_DRIVER_FRAMES> pages;
	uint64_t offset(uint32_t p_frame) const { return uint64_t(p_frame) * frame_stride; }
};

struct TextureBacking : GpuUse {
	TextureHandle texture;
	BufferHandle atomic_buffer;
	uint64_t atomic_size = 0;
	WGPUTextureUsage usage = WGPUTextureUsage_None;
	TextureFormatInfo storage;
	Rdc::TextureFormat desc;
	WGPUTextureDimension dimension = WGPUTextureDimension_Undefined;
	std::vector<WGPUTextureFormat> view_formats;
};

struct TextureResource : GpuUse {
	std::shared_ptr<TextureBacking> backing;
	TextureFormatInfo format;
	WGPUTextureViewDimension dimension = WGPUTextureViewDimension_Undefined;
	uint32_t base_mip = 0;
	uint32_t mip_count = 0;
	uint32_t base_layer = 0;
	uint32_t layer_count = 0;
	TextureViewHandle sampling;
	TextureViewHandle depth_sampling;
	TextureViewHandle storage;
	TextureViewHandle attachment;

	WGPUExtent3D extent(uint32_t p_mip, uint32_t p_layers) const;
	Error initialize_views();
};

TextureViewHandle create_texture_view(WGPUTexture p_texture, const WGPUTextureViewDescriptor &p_desc);

struct SamplerResource : GpuUse {
	SamplerHandle sampler;
	SamplerHandle non_filtering;
	SamplerType type = SamplerType::NONE;
};

struct RenderPassLayout : GpuUse {
	std::vector<RDD::Attachment> attachments;
	std::vector<RDD::Subpass> subpasses;
	std::vector<RDD::SubpassDependency> dependencies;
	uint32_t view_count = 1;
	bool requires_synthetic_target(uint32_t p_subpass) const;
};

struct FramebufferResource : GpuUse {
	std::shared_ptr<RenderPassLayout> pass;
	std::vector<std::shared_ptr<TextureResource>> attachments;
	std::shared_ptr<TextureResource> synthetic_target;
	std::shared_ptr<TextureResource> synthetic_target_msaa;
	uint32_t width = 0;
	uint32_t height = 0;
};

struct SwapChainResource : GpuUse {
	std::shared_ptr<SurfaceState> surface;
	RDD::RenderPassID render_pass;
	RDD::FramebufferID current;
};

struct ReadbackTicket {
	uint32_t slot = 0;
	uint64_t generation = 0;
	SubmitSerial submit_serial = 0;
	explicit operator bool() const { return generation != 0; }
};

struct ReadbackRequest;
struct CommandRecording;
struct PushPage;
struct ShaderResource;
struct ShaderVariant;
struct PreparedShader;
struct LogicalUniformSet;
struct BindGroupResource;
struct PipelineResource;

struct PushConstantSlice {
	std::shared_ptr<BufferResource> buffer;
	std::shared_ptr<GpuUse> page;
	uint32_t offset = 0;
	uint32_t size = 0;
};

struct PhysicalBindGroup {
	std::shared_ptr<BindGroupResource> group;
	std::vector<uint32_t> dynamic_offsets;
	UseList uses;
};

struct VertexBinding {
	uint32_t index = 0; // Godot buffer binding; WebGPU layouts use dense slots.
	Rdc::VertexAttributeBinding layout;
	std::vector<WGPUVertexAttribute> attributes;
};

struct VertexFormatResource {
	std::vector<VertexBinding> bindings;
};

} // namespace godot::webgpu

class RenderingDeviceDriverWebGPU : public RenderingDeviceDriver {
	GDSOFTCLASS(RenderingDeviceDriverWebGPU, RenderingDeviceDriver);

public:
	explicit RenderingDeviceDriverWebGPU(godot::webgpu::DeviceLease p_state);
	~RenderingDeviceDriverWebGPU() override;
	Error initialize(uint32_t p_device_index, uint32_t p_frame_count) override;

	BufferID buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) override;
	BufferID buffer_create_initialized(uint64_t p_size, BitField<BufferUsageBits> p_usage, Span<uint8_t> p_initial_data);
	bool buffer_set_texel_format(BufferID p_buffer, DataFormat p_format) override;
	void buffer_free(BufferID p_buffer) override;
	uint64_t buffer_get_allocation_size(BufferID p_buffer) override;
	uint8_t *buffer_map(BufferID p_buffer) override;
	void buffer_unmap(BufferID p_buffer) override;
	uint8_t *buffer_persistent_map_advance(BufferID p_buffer, uint64_t p_frames_drawn) override;
	uint64_t buffer_get_dynamic_offsets(Span<BufferID> p_buffers) override;
	void buffer_flush(BufferID p_buffer) override;
	uint64_t buffer_get_device_address(BufferID) override { return 0; }

	TextureID texture_create(const TextureFormat &p_format, const TextureView &p_view) override;
	TextureID texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool p_depth_stencil, uint32_t p_mipmaps) override;
	TextureID texture_create_shared(TextureID p_original_texture, const TextureView &p_view) override;
	TextureID texture_create_shared_from_slice(TextureID p_original_texture, const TextureView &p_view, TextureSliceType p_slice_type, uint32_t p_layer, uint32_t p_layers, uint32_t p_mipmap, uint32_t p_mipmaps) override;
	void texture_free(TextureID p_texture) override;
	uint64_t texture_get_allocation_size(TextureID p_texture) override;
	void texture_get_copyable_layout(TextureID p_texture, const TextureSubresource &p_subresource, TextureCopyableLayout *r_layout) override;
	Vector<uint8_t> texture_get_data(TextureID p_texture, uint32_t p_layer) override;
	BitField<TextureUsageBits> texture_get_usages_supported_by_format(DataFormat p_format, bool p_cpu_readable) override;
	bool texture_can_make_shared_with_format(TextureID p_texture, DataFormat p_format, bool &r_raw_reinterpretation) override;
	SamplerID sampler_create(const SamplerState &p_state) override;
	void sampler_free(SamplerID p_sampler) override;
	bool sampler_is_format_supported_for_filter(DataFormat p_format, SamplerFilter p_filter) override;

	SwapChainID swap_chain_create(RenderingContextDriver::SurfaceID p_surface) override;
	Error swap_chain_resize(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, uint32_t p_desired_framebuffer_count) override;
	FramebufferID swap_chain_acquire_framebuffer(CommandQueueID p_cmd_queue, SwapChainID p_swap_chain, bool &r_resize_required) override;
	RenderPassID swap_chain_get_render_pass(SwapChainID p_swap_chain) override;
	DataFormat swap_chain_get_format(SwapChainID p_swap_chain) override;
	ColorSpace swap_chain_get_color_space(SwapChainID) override { return COLOR_SPACE_REC709_NONLINEAR_SRGB; }
	bool swap_chain_get_hdr_output_supported(SwapChainID) override { return false; }
	void swap_chain_free(SwapChainID p_swap_chain) override;
	FramebufferID framebuffer_create(RenderPassID p_render_pass, VectorView<TextureID> p_attachments, uint32_t p_width, uint32_t p_height) override;
	void framebuffer_free(FramebufferID p_framebuffer) override;
	RenderPassID render_pass_create(VectorView<Attachment> p_attachments, VectorView<Subpass> p_subpasses, VectorView<SubpassDependency> p_subpass_dependencies, uint32_t p_view_count, AttachmentReference p_fragment_density_map_attachment) override;
	void render_pass_free(RenderPassID p_render_pass) override;

	ShaderID shader_create_from_container(const Ref<RenderingShaderContainer> &p_shader_container, const Vector<ImmutableSampler> &p_immutable_samplers) override;
	void shader_free(ShaderID p_shader) override;
	void shader_destroy_modules(ShaderID p_shader) override;
	const RenderingShaderContainerFormat &get_shader_container_format() const override;
	Error set_shader_compile_options(ShaderID p_shader, const godot::webgpu::CompileOptions &p_options);
	int64_t schedule_shader_prepare(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_constants);
	UniformSetID uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader, uint32_t p_set_index, int p_linear_pool_index) override;
	void uniform_set_free(UniformSetID p_uniform_set) override;
	uint32_t uniform_sets_get_dynamic_offsets(VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count) const override;
	VertexFormatID vertex_format_create(Span<VertexAttribute> p_vertex_attribs, const VertexAttributeBindingsMap &p_vertex_bindings) override;
	void vertex_format_free(VertexFormatID p_vertex_format) override;
	PipelineID render_pipeline_create(ShaderID p_shader, VertexFormatID p_vertex_format, RenderPrimitive p_render_primitive,
			PipelineRasterizationState p_rasterization_state, PipelineMultisampleState p_multisample_state,
			PipelineDepthStencilState p_depth_stencil_state, PipelineColorBlendState p_blend_state,
			VectorView<int32_t> p_color_attachments, BitField<PipelineDynamicStateFlags> p_dynamic_state, RenderPassID p_render_pass,
			uint32_t p_render_subpass, VectorView<PipelineSpecializationConstant> p_specialization_constants) override;
	PipelineID compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_specialization_constants) override;
	void pipeline_free(PipelineID p_pipeline) override;

	// 2B-2 calls these on the owner. The encoder must submit the recorded copies
	// before arm_readback() or arm_download_buffer().
	godot::webgpu::ReadbackTicket encode_buffer_readback(WGPUCommandEncoder p_encoder, BufferID p_buffer, uint64_t p_offset, uint64_t p_size, godot::webgpu::UseList &r_uses);
	godot::webgpu::ReadbackTicket encode_texture_readback(WGPUCommandEncoder p_encoder, TextureID p_texture, const TextureSubresource &p_subresource, godot::webgpu::UseList &r_uses);
	Error arm_readback(godot::webgpu::ReadbackTicket &r_ticket, godot::webgpu::SubmitSerial p_serial);
	Error take_readback(const godot::webgpu::ReadbackTicket &p_ticket, Vector<uint8_t> &r_data);
	void cancel_readback(const godot::webgpu::ReadbackTicket &p_ticket);
	Error arm_download_buffer(BufferID p_buffer, godot::webgpu::SubmitSerial p_serial);
	Error prepare_upload_for_copy(BufferID p_buffer, uint64_t p_offset, uint64_t p_size);
	void swap_chain_drop_frame(SwapChainID p_swap_chain);
	uint64_t api_trait_get(ApiTrait p_trait) override;
	bool is_composite_alpha_supported(CommandQueueID p_queue) const override;
	Error build_bind_group(godot::webgpu::ShaderVariant &p_shader, uint32_t p_group, const std::map<uint32_t, std::shared_ptr<godot::webgpu::LogicalUniformSet>> &p_sets,
			const std::map<const godot::webgpu::BufferResource *, uint32_t> &p_frames, const godot::webgpu::PushConstantSlice *p_push, godot::webgpu::PhysicalBindGroup &r_group);

	void command_pipeline_barrier(
			CommandBufferID p_cmd_buffer,
			BitField<PipelineStageBits> p_src_stages,
			BitField<PipelineStageBits> p_dst_stages,
			VectorView<MemoryAccessBarrier> p_memory_barriers,
			VectorView<BufferBarrier> p_buffer_barriers,
			VectorView<TextureBarrier> p_texture_barriers,
			VectorView<AccelerationStructureBarrier> p_acceleration_structure_barriers) override;
	FenceID fence_create() override;
	Error fence_wait(FenceID p_fence) override;
	void fence_free(FenceID p_fence) override;
	SemaphoreID semaphore_create() override;
	void semaphore_free(SemaphoreID p_semaphore) override;
	CommandQueueFamilyID command_queue_family_get(BitField<CommandQueueFamilyBits> p_cmd_queue_family_bits, RenderingContextDriver::SurfaceID p_surface = 0) override;
	CommandQueueID command_queue_create(CommandQueueFamilyID p_cmd_queue_family, bool p_identify_as_main_queue = false) override;
	Error command_queue_execute_and_present(CommandQueueID p_cmd_queue, VectorView<SemaphoreID> p_wait_semaphores, VectorView<CommandBufferID> p_cmd_buffers, VectorView<SemaphoreID> p_cmd_semaphores, FenceID p_cmd_fence, VectorView<SwapChainID> p_swap_chains) override;
	void command_queue_free(CommandQueueID p_cmd_queue) override;
	CommandPoolID command_pool_create(CommandQueueFamilyID p_cmd_queue_family, CommandBufferType p_cmd_buffer_type) override;
	bool command_pool_reset(CommandPoolID p_cmd_pool) override;
	void command_pool_free(CommandPoolID p_cmd_pool) override;
	CommandBufferID command_buffer_create(CommandPoolID p_cmd_pool) override;
	bool command_buffer_begin(CommandBufferID p_cmd_buffer) override;
	bool command_buffer_begin_secondary(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, uint32_t p_subpass, FramebufferID p_framebuffer) override;
	void command_buffer_end(CommandBufferID p_cmd_buffer) override;
	void command_buffer_execute_secondary(CommandBufferID p_cmd_buffer, VectorView<CommandBufferID> p_secondary_cmd_buffers) override;
	void command_uniform_set_prepare_for_use(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) override;
	void command_clear_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, uint64_t p_offset, uint64_t p_size) override;
	void command_copy_buffer(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, BufferID p_dst_buffer, VectorView<BufferCopyRegion> p_regions) override;
	void command_copy_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<TextureCopyRegion> p_regions) override;
	void command_resolve_texture(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, uint32_t p_src_layer, uint32_t p_src_mipmap, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, uint32_t p_dst_layer, uint32_t p_dst_mipmap) override;
	void command_clear_color_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, const Color &p_color, const TextureSubresourceRange &p_subresources) override;
	void command_clear_depth_stencil_texture(CommandBufferID p_cmd_buffer, TextureID p_texture, TextureLayout p_texture_layout, float p_depth, uint8_t p_stencil, const TextureSubresourceRange &p_subresources) override;
	void command_copy_buffer_to_texture(CommandBufferID p_cmd_buffer, BufferID p_src_buffer, TextureID p_dst_texture, TextureLayout p_dst_texture_layout, VectorView<BufferTextureCopyRegion> p_regions) override;
	void command_copy_texture_to_buffer(CommandBufferID p_cmd_buffer, TextureID p_src_texture, TextureLayout p_src_texture_layout, BufferID p_dst_buffer, VectorView<BufferTextureCopyRegion> p_regions) override;
	void command_bind_push_constants(CommandBufferID p_cmd_buffer, ShaderID p_shader, uint32_t p_first_index, VectorView<uint32_t> p_data) override;
	bool pipeline_cache_create(const Vector<uint8_t> &p_data) override;
	void pipeline_cache_free() override;
	size_t pipeline_cache_query_size() override;
	Vector<uint8_t> pipeline_cache_serialize() override;
	void command_begin_render_pass(CommandBufferID p_cmd_buffer, RenderPassID p_render_pass, FramebufferID p_framebuffer, CommandBufferType p_cmd_buffer_type, const Rect2i &p_rect, VectorView<RenderPassClearValue> p_clear_values) override;
	void command_end_render_pass(CommandBufferID p_cmd_buffer) override;
	void command_next_render_subpass(CommandBufferID p_cmd_buffer, CommandBufferType p_cmd_buffer_type) override;
	void command_render_set_viewport(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_viewports) override;
	void command_render_set_scissor(CommandBufferID p_cmd_buffer, VectorView<Rect2i> p_scissors) override;
	void command_render_clear_attachments(CommandBufferID p_cmd_buffer, VectorView<AttachmentClear> p_attachment_clears, VectorView<Rect2i> p_rects) override;
	void command_bind_render_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) override;
	void command_bind_render_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) override;
	void command_render_draw(CommandBufferID p_cmd_buffer, uint32_t p_vertex_count, uint32_t p_instance_count, uint32_t p_base_vertex, uint32_t p_first_instance) override;
	void command_render_draw_indexed(CommandBufferID p_cmd_buffer, uint32_t p_index_count, uint32_t p_instance_count, uint32_t p_first_index, int32_t p_vertex_offset, uint32_t p_first_instance) override;
	void command_render_draw_indexed_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) override;
	void command_render_draw_indexed_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) override;
	void command_render_draw_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, uint32_t p_draw_count, uint32_t p_stride) override;
	void command_render_draw_indirect_count(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_buffer_offset, uint32_t p_max_draw_count, uint32_t p_stride) override;
	void command_render_bind_vertex_buffers(CommandBufferID p_cmd_buffer, uint32_t p_binding_count, const BufferID *p_buffers, const uint64_t *p_offsets, uint64_t p_dynamic_offsets) override;
	void command_render_bind_index_buffer(CommandBufferID p_cmd_buffer, BufferID p_buffer, IndexBufferFormat p_format, uint64_t p_offset) override;
	void command_render_set_blend_constants(CommandBufferID p_cmd_buffer, const Color &p_constants) override;
	void command_render_set_line_width(CommandBufferID p_cmd_buffer, float p_width) override;
	void command_bind_compute_pipeline(CommandBufferID p_cmd_buffer, PipelineID p_pipeline) override;
	void command_bind_compute_uniform_sets(CommandBufferID p_cmd_buffer, VectorView<UniformSetID> p_uniform_sets, ShaderID p_shader, uint32_t p_first_set_index, uint32_t p_set_count, uint32_t p_dynamic_offsets) override;
	void command_compute_dispatch(CommandBufferID p_cmd_buffer, uint32_t p_x_groups, uint32_t p_y_groups, uint32_t p_z_groups) override;
	void command_compute_dispatch_indirect(CommandBufferID p_cmd_buffer, BufferID p_indirect_buffer, uint64_t p_offset) override;
	AccelerationStructureID blas_create(VectorView<AccelerationStructureGeometry> p_geometries, BitField<AccelerationStructureFlagBits> p_flags) override;
	AccelerationStructureID tlas_create(uint32_t p_max_instance_count, BitField<AccelerationStructureFlagBits> p_flags) override;
	void acceleration_structure_instance_write(uint8_t *r_driver_instance, const AccelerationStructureInstance &p_instance) override;
	void acceleration_structure_free(AccelerationStructureID p_acceleration_structure) override;
	uint32_t acceleration_structure_get_scratch_size_bytes(AccelerationStructureID p_acceleration_structure) override;
	RaytracingPipelineID raytracing_pipeline_create(VectorView<PipelineShader> p_shaders, VectorView<uint32_t> p_raygen_shader_indices, VectorView<uint32_t> p_miss_shader_indices, VectorView<HitGroup> p_hit_groups, uint32_t p_max_trace_recursion_depth, ShaderID p_layout_defining_shader) override;
	void raytracing_pipeline_free(RaytracingPipelineID p_pipeline) override;
	bool raytracing_pipeline_get_shader_group_handles(RaytracingPipelineID p_pipeline, uint32_t p_group_index_offset, VectorView<uint32_t> p_group_indices, uint8_t *r_data, uint32_t p_data_stride_bytes) override;
	void command_build_blas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer) override;
	void command_build_tlas(CommandBufferID p_cmd_buffer, AccelerationStructureID p_acceleration_structure, BufferID p_scratch_buffer, BufferID p_instance_buffer, uint32_t p_instance_offset, uint32_t p_instance_count) override;
	void command_bind_raytracing_pipeline(CommandBufferID p_cmd_buffer, RaytracingPipelineID p_pipeline) override;
	void command_bind_raytracing_uniform_set(CommandBufferID p_cmd_buffer, UniformSetID p_uniform_set, ShaderID p_shader, uint32_t p_set_index) override;
	void command_trace_rays(CommandBufferID p_cmd_buffer, const ShaderBindingTable &p_raygen_sbt, const ShaderBindingTable &p_miss_sbt, const ShaderBindingTable &p_hit_sbt, uint32_t p_width, uint32_t p_height, uint32_t p_depth) override;
	QueryPoolID timestamp_query_pool_create(uint32_t p_query_count) override;
	void timestamp_query_pool_free(QueryPoolID p_pool_id) override;
	void timestamp_query_pool_get_results(QueryPoolID p_pool_id, uint32_t p_query_count, uint64_t *r_results) override;
	uint64_t timestamp_query_result_to_time(uint64_t p_result) override;
	void command_timestamp_query_pool_reset(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_query_count) override;
	void command_timestamp_write(CommandBufferID p_cmd_buffer, QueryPoolID p_pool_id, uint32_t p_index) override;
	void command_begin_label(CommandBufferID p_cmd_buffer, const char *p_label_name, const Color &p_color) override;
	void command_end_label(CommandBufferID p_cmd_buffer) override;
	void command_insert_breadcrumb(CommandBufferID p_cmd_buffer, uint32_t p_data) override;
	void begin_segment(uint32_t p_frame_index, uint32_t p_frames_drawn) override;
	void end_segment() override;
	void set_object_name(ObjectType p_type, ID p_driver_id, const String &p_name) override;
	uint64_t get_resource_native_handle(DriverResource p_type, ID p_driver_id) override;
	uint64_t get_total_memory_used() override;
	uint64_t get_lazily_memory_used() override;
	uint64_t limit_get(Limit p_limit) override;
	bool has_feature(Features p_feature) override;
	const MultiviewCapabilities &get_multiview_capabilities() override;
	const FragmentShadingRateCapabilities &get_fragment_shading_rate_capabilities() override;
	const FragmentDensityMapCapabilities &get_fragment_density_map_capabilities() override;
	String get_api_name() const override;
	String get_api_version() const override;
	String get_pipeline_cache_uuid() const override;
	const Capabilities &get_capabilities() const override;

protected:
	const godot::webgpu::DeviceLease state;
	uint32_t frame_count = 0;
	std::shared_ptr<godot::webgpu::ShaderVariant> realize_shader(const std::shared_ptr<godot::webgpu::ShaderResource> &p_shader, VectorView<PipelineSpecializationConstant> p_constants, const godot::webgpu::CompileOptions &p_options);

private:
	std::vector<std::shared_ptr<godot::webgpu::PushPage>> push_pool;
	std::shared_ptr<godot::webgpu::PushPage> acquire_push_page(uint32_t p_size);
	std::shared_ptr<godot::webgpu::PipelineResource> materialize_pipeline(godot::webgpu::PipelineResource &r_pipeline, const std::map<uint32_t, std::shared_ptr<godot::webgpu::LogicalUniformSet>> &p_sets);
	bool prepare_dispatch(godot::webgpu::CommandRecording &r_cmd, bool p_compute, bool p_indexed, const std::shared_ptr<godot::webgpu::BufferResource> &p_indirect = {});
	void draw_indirect_count(CommandBufferID p_cmd, BufferID p_arguments, uint64_t p_offset, BufferID p_count_buffer, uint64_t p_count_offset,
			uint32_t p_max_draw_count, uint32_t p_stride, bool p_indexed);
	godot::webgpu::ComputePipelineHandle indirect_count_pipeline;
	void encode_queries(godot::webgpu::CommandRecording &r_cmd);
	void finish_subpass(CommandBufferID p_buffer);
	bool resolve_attachment(godot::webgpu::CommandRecording &r_cmd, const godot::webgpu::TextureResource &p_src, const godot::webgpu::TextureResource &p_dst,
			uint32_t p_src_mip, uint32_t p_src_layer, uint32_t p_dst_mip, uint32_t p_dst_layer);
	static void arm_download(const godot::webgpu::DeviceLease &p_state, const std::shared_ptr<godot::webgpu::BufferResource> &p_buffer);
	bool resolve_depth(godot::webgpu::CommandRecording &r_cmd, const godot::webgpu::TextureResource &p_src, WGPUTextureView p_src_view, const godot::webgpu::TextureResource &p_dst, WGPUTextureView p_dst_view);
	std::map<std::vector<uint32_t>, godot::webgpu::RenderPipelineHandle> utility_pipelines;
	BufferID create_buffer(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, Span<uint8_t> p_initial_data);
	godot::webgpu::ReadbackTicket add_readback(std::shared_ptr<godot::webgpu::ReadbackRequest> p_request);
	uint64_t next_readback_generation = 0;
	std::vector<std::shared_ptr<godot::webgpu::ReadbackRequest>> readbacks;
	std::vector<uint32_t> free_readback_slots;
};
