#pragma once

#include "webgpu_shader_resources.h"

#include <cstring>

namespace godot::webgpu {

struct PushPage : GpuUse {
	std::shared_ptr<BufferResource> gpu;
	std::vector<uint8_t> bytes;
	uint32_t used = 0;
};

struct Fence {
	SubmitSerial serial = 0;
	std::vector<std::shared_ptr<BufferResource>> downloads;
};

struct TimestampCapture : GpuUse {
	QuerySetHandle queries;
	BufferHandle resolve;
	std::shared_ptr<BufferResource> download;
	uint32_t count = 0;
};
struct TimestampPool {
	uint32_t capacity = 0;
	std::shared_ptr<TimestampCapture> latest;
};

struct VertexBufferBinding {
	std::shared_ptr<BufferResource> buffer;
	uint64_t offset = 0;
	uint32_t frame = 0;
};

struct ActiveBufferBinding {
	WGPUBuffer buffer = nullptr;
	uint64_t offset = 0;
	uint64_t size = 0;
	bool operator==(const ActiveBufferBinding &p_other) const {
		return buffer == p_other.buffer && offset == p_other.offset && size == p_other.size;
	}
};

struct ActiveIndexBinding {
	ActiveBufferBinding buffer;
	WGPUIndexFormat format = WGPUIndexFormat_Undefined;
	bool operator==(const ActiveIndexBinding &p_other) const {
		return buffer == p_other.buffer && format == p_other.format;
	}
};

struct ActiveBindGroup {
	WGPUBindGroup handle = nullptr;
	std::vector<uint32_t> offsets;
};

struct CommandRecording {
	CommandEncoderHandle encoder;
	CommandBufferHandle finished;
	RenderPassEncoderHandle render;
	ComputePassEncoderHandle compute;
	std::shared_ptr<RenderPassLayout> layout;
	std::shared_ptr<FramebufferResource> framebuffer;
	std::vector<RDD::RenderPassClearValue> clear_values;
	std::vector<bool> attachment_started;
	uint32_t subpass = 0;
	uint32_t render_samples = 1;
	Rect2i area, viewport, scissor;
	UsageScope scope, attachments;
	uint64_t render_scope_epoch = 0;
	std::shared_ptr<PipelineResource> pipeline;
	LogicalUniformSets sets;
	std::map<uint32_t, uint32_t> frames; // Packed frame indices captured per logical set.
	std::vector<VertexBufferBinding> vertex;
	VertexBufferBinding index;
	WGPUIndexFormat index_format = WGPUIndexFormat_Undefined;
	std::optional<Color> blend;
	std::array<uint8_t, MAX_PUSH_CONSTANT_BYTES> push_data{};
	PushConstantSlice push;
	std::vector<std::shared_ptr<PushPage>> push_pages;
	std::vector<std::shared_ptr<BufferResource>> downloads;
	std::vector<ReadbackTicket> tickets;
	std::map<TimestampPool *, std::shared_ptr<TimestampCapture>> timestamps;
	std::vector<std::string> labels;
	UseList uses;
	std::unordered_map<const GpuUse *, uint64_t> referenced; // Zero or the last render usage scope epoch.
	BindGroupScratch bind_group_scratch;
	std::vector<PhysicalBindGroup> draw_groups;
	WGPURenderPipeline active_render_pipeline = nullptr;
	WGPUComputePipeline active_compute_pipeline = nullptr;
	// Actual slot extent, even when a utility draw invalidates the cached state below.
	uint32_t bound_render_groups = 0, bound_render_vertices = 0;
	std::vector<ActiveBindGroup> active_bind_groups;
	std::optional<Rect2i> active_viewport, active_scissor;
	std::optional<Color> active_blend;
	std::optional<uint32_t> active_stencil_reference;
	std::vector<ActiveBufferBinding> active_vertex_buffers;
	ActiveIndexBinding active_index_buffer;

	void open_render(VectorView<RDD::AttachmentClear> p_clears = {});
	void end_render();
	void open_compute();
	void end_compute();
	void outside_pass();
	void reset_encoder_state();
	void trim_render_slots(uint32_t p_groups, uint32_t p_vertices, uint32_t p_limit);
	template <typename T>
	void keep(const std::shared_ptr<T> &p_use) {
		if (referenced.try_emplace(p_use.get(), 0).second) {
			uses.push_back(p_use);
		}
	}
	bool scoped(const GpuUse *p_use) const {
		const auto it = referenced.find(p_use);
		return it != referenced.end() && it->second == render_scope_epoch;
	}
	void keep_render_read(const std::shared_ptr<BufferResource> &p_buffer);
	void keep_bind_group(const std::shared_ptr<BindGroupResource> &p_group, bool p_render);
	void download(const std::shared_ptr<BufferResource> &p_buffer);
};

struct CommandPool {
	std::vector<std::unique_ptr<CommandRecording>> buffers;
};

inline CommandRecording &recording(const DeviceState &p_state, RDD::CommandBufferID p_id) {
	p_state.assert_owner();
	DEV_ASSERT(p_id);
	return *reinterpret_cast<CommandRecording *>(p_id.id);
}

inline WGPUColor wg_color(const Color &p_color) {
	return { p_color.r, p_color.g, p_color.b, p_color.a };
}

TextureViewHandle single_attachment(const TextureResource &p_texture, uint32_t p_mip, uint32_t p_layer, WGPUTextureAspect p_aspect = WGPUTextureAspect_All);
std::shared_ptr<BufferResource> make_transfer_buffer(DeviceState &p_state, uint64_t p_size, WGPUBufferUsage p_usage, bool p_mapped = false);

} // namespace godot::webgpu
