#pragma once

#include "webgpu_shader_resources.h"

#include <cstring>
#include <unordered_set>

namespace godot::webgpu {

struct PushPage : GpuUse {
	std::shared_ptr<BufferResource> gpu;
	std::vector<uint8_t> bytes;
	uint32_t used = 0;
};

// WebGPU usage scope: compatible read usages may be combined, and one
// writable usage may be repeated, but may not be combined with other usages.
enum class ScopeUse : uint8_t { READ, STORAGE_READ, STORAGE_WRITE, ATTACHMENT };
struct ScopeKey {
	const void *resource;
	WGPUTextureAspect aspect = WGPUTextureAspect_Undefined;
	uint32_t mip = 0;
	uint32_t layer = 0;
	bool operator<(const ScopeKey &p_other) const {
		if (resource != p_other.resource) {
			return std::less<const void *>{}(resource, p_other.resource);
		}
		return std::tie(aspect, mip, layer) < std::tie(p_other.aspect, p_other.mip, p_other.layer);
	}
};
struct UsageScope {
	std::map<ScopeKey, ScopeUse> usages;
	bool add(ScopeKey p_key, ScopeUse p_use);
	bool add_texture(const TextureResource &p_texture, ScopeUse p_use, uint32_t p_mips, uint32_t p_layers, WGPUTextureAspect p_aspect);
	bool compatible(const UsageScope &p_other) const;
	void merge(const UsageScope &p_other);
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
	std::shared_ptr<PipelineResource> pipeline;
	std::map<uint32_t, std::shared_ptr<LogicalUniformSet>> sets;
	std::map<const BufferResource *, uint32_t> frames;
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
	std::unordered_set<const GpuUse *> referenced;

	void open_render(VectorView<RDD::AttachmentClear> p_clears = {});
	void end_render();
	void open_compute();
	void end_compute();
	void outside_pass();
	void keep(const std::shared_ptr<GpuUse> &p_use) {
		if (referenced.insert(p_use.get()).second) {
			uses.push_back(p_use);
		}
	}
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
