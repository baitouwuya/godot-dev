#include "rendering_device_driver_webgpu.h"
#include "webgpu_perf_stats.h"

#include <algorithm>
#include <cstring>

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

void count_buffer_write(uint64_t p_bytes) {
	if (webgpu_perf_stats.in_raf && webgpu_perf_is_enabled()) {
		auto &work = webgpu_perf_stats.frame.work;
		work.write_buffer_calls++;
		work.write_buffer_bytes += p_bytes;
	}
}

WGPUBufferUsage resolve_usage(BitField<RDD::BufferUsageBits> p_usage) {
	WGPUBufferUsage usage = WGPUBufferUsage_None;
	if (p_usage.has_flag(RDD::BUFFER_USAGE_TRANSFER_FROM_BIT)) {
		usage |= WGPUBufferUsage_CopySrc;
	}
	if (p_usage.has_flag(RDD::BUFFER_USAGE_TRANSFER_TO_BIT)) {
		usage |= WGPUBufferUsage_CopyDst;
	}
	if (p_usage.has_flag(RDD::BUFFER_USAGE_UNIFORM_BIT)) {
		usage |= WGPUBufferUsage_Uniform;
	}
	if (p_usage.has_flag(RDD::BUFFER_USAGE_STORAGE_BIT)) {
		usage |= WGPUBufferUsage_Storage;
	}
	if (p_usage.has_flag(RDD::BUFFER_USAGE_INDEX_BIT)) {
		usage |= WGPUBufferUsage_Index;
	}
	if (p_usage.has_flag(RDD::BUFFER_USAGE_VERTEX_BIT)) {
		usage |= WGPUBufferUsage_Vertex;
	}
	if (p_usage.has_flag(RDD::BUFFER_USAGE_INDIRECT_BIT)) {
		usage |= WGPUBufferUsage_Indirect | WGPUBufferUsage_Storage;
	}
	return usage;
}

} // namespace
} // namespace godot::webgpu

RenderingDeviceDriverWebGPU::RenderingDeviceDriverWebGPU(DeviceLease p_state) : state(std::move(p_state)) {
	DEV_ASSERT(state);
}

RenderingDeviceDriverWebGPU::~RenderingDeviceDriverWebGPU() {
	state->assert_owner();
}

Error RenderingDeviceDriverWebGPU::initialize(uint32_t p_device_index, uint32_t p_frame_count) {
	state->assert_owner();
	DEV_ASSERT(p_device_index == 0);
	ERR_FAIL_COND_V_MSG(!p_frame_count || p_frame_count > MAX_DRIVER_FRAMES, ERR_INVALID_PARAMETER, "Invalid WebGPU frame count.");
	if (state->failed()) {
		return ERR_UNAVAILABLE;
	}
	frame_count = p_frame_count;
	return OK;
}

RDD::BufferID RenderingDeviceDriverWebGPU::buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, uint64_t) {
	return create_buffer(p_size, p_usage, p_allocation_type, {});
}

RDD::BufferID RenderingDeviceDriverWebGPU::buffer_create_initialized(uint64_t p_size, BitField<BufferUsageBits> p_usage, Span<uint8_t> p_initial_data) {
	return create_buffer(p_size, p_usage, MEMORY_ALLOCATION_TYPE_GPU, p_initial_data);
}

RDD::BufferID RenderingDeviceDriverWebGPU::create_buffer(uint64_t p_size, BitField<BufferUsageBits> p_usage, MemoryAllocationType p_allocation_type, Span<uint8_t> p_initial_data) {
	state->assert_owner();
	DEV_ASSERT(p_size && p_initial_data.size() <= p_size);
	ERR_FAIL_COND_V_MSG(p_usage.has_flag(BUFFER_USAGE_DEVICE_ADDRESS_BIT) || p_usage.has_flag(BUFFER_USAGE_TEXEL_BIT) ||
					p_usage.has_flag(BUFFER_USAGE_SHADER_BINDING_TABLE_BIT) || p_usage.has_flag(BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT) ||
					p_usage.has_flag(BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT),
			{}, "This Godot buffer usage has no native WebGPU binding.");
	ERR_FAIL_COND_V(p_size > state->limits.maxBufferSize, {});
	auto buf = std::make_shared<BufferResource>();
	buf->size = p_size;
	buf->usage = resolve_usage(p_usage);
	buf->frame_stride = align_up(p_size, BUFFER_COPY_ALIGNMENT);
	buf->allocation_size = buf->frame_stride;
	if (p_usage.has_flag(BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT)) {
		DEV_ASSERT(frame_count);
		buf->access = BufferAccess::DYNAMIC;
		const uint32_t alignment = MAX(state->limits.minUniformBufferOffsetAlignment, state->limits.minStorageBufferOffsetAlignment);
		buf->frame_stride = align_up(p_size, alignment);
		buf->allocation_size = buf->frame_stride * frame_count;
		for (uint32_t i = 0; i < frame_count; ++i) {
			buf->pages[i] = std::make_shared<GpuUse>();
		}
	} else if (p_allocation_type == MEMORY_ALLOCATION_TYPE_CPU) {
		if (buf->usage == WGPUBufferUsage_CopyDst) {
			buf->access = BufferAccess::DOWNLOAD;
			buf->usage |= WGPUBufferUsage_MapRead;
		} else {
			DEV_ASSERT(buf->usage == WGPUBufferUsage_CopySrc);
			buf->access = BufferAccess::UPLOAD;
			buf->usage |= WGPUBufferUsage_CopyDst;
		}
	}
	ERR_FAIL_COND_V(buf->allocation_size > state->limits.maxBufferSize || buf->allocation_size > SIZE_MAX, {});
	if (buf->access == BufferAccess::UPLOAD || buf->access == BufferAccess::DYNAMIC) {
		buf->usage |= WGPUBufferUsage_CopyDst;
		buf->upload.resize(buf->allocation_size);
		if (!p_initial_data.is_empty()) {
			std::memcpy(buf->upload.data(), p_initial_data.ptr(), p_initial_data.size());
		}
	}
	WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	desc.size = buf->allocation_size;
	desc.usage = buf->usage;
	desc.mappedAtCreation = !p_initial_data.is_empty();
	buf->buffer.reset(wgpuDeviceCreateBuffer(state->device.get(), &desc));
	ERR_FAIL_COND_V(!buf->buffer, {});
	if (webgpu_perf_stats.in_raf && webgpu_perf_is_enabled()) {
		webgpu_perf_stats.frame.work.buffer_creates++;
	}
	if (desc.mappedAtCreation) {
		void *mapped = wgpuBufferGetMappedRange(buf->buffer.get(), 0, desc.size);
		ERR_FAIL_NULL_V(mapped, {});
		std::memcpy(mapped, p_initial_data.ptr(), p_initial_data.size());
		wgpuBufferUnmap(buf->buffer.get());
	}
	return resource_id<BufferID>(std::move(buf));
}

bool RenderingDeviceDriverWebGPU::buffer_set_texel_format(BufferID, DataFormat) {
	ERR_FAIL_V_MSG(false, "WebGPU does not expose native texel buffers; the shader and resource need a matched lowering.");
}

void RenderingDeviceDriverWebGPU::buffer_free(BufferID p_buffer) {
	state->assert_owner();
	free_id<BufferResource>(p_buffer);
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_allocation_size(BufferID p_buffer) {
	return resource<BufferResource>(p_buffer)->allocation_size;
}

uint8_t *RenderingDeviceDriverWebGPU::buffer_map(BufferID p_buffer) {
	state->assert_owner();
	auto &buf = *resource<BufferResource>(p_buffer);
	DEV_ASSERT(!buf.cpu_mapped);
	if (buf.access == BufferAccess::DOWNLOAD) {
		if (!buf.download_regions.empty()) {
			buf.download_bytes.resize(buf.allocation_size);
			for (const auto &region : buf.download_regions) {
				const auto &src = *region.staging;
				DEV_ASSERT(src.download_state == DownloadState::MAPPED);
				const auto *data = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(src.buffer.get(), 0, src.allocation_size));
				ERR_FAIL_NULL_V(data, nullptr);
				if (region.byte_count) {
					std::memcpy(buf.download_bytes.data() + region.offset, data + region.source_offset, region.byte_count);
				} else {
					unpack_texture_rows(region.format, region.extent, data, region.src_pitch, buf.download_bytes.data() + region.offset, region.dst_pitch);
				}
				wgpuBufferUnmap(src.buffer.get());
			}
			buf.download_regions.clear();
		}
		if (!buf.download_bytes.empty()) {
			buf.cpu_mapped = true;
			return buf.download_bytes.data();
		}
		ERR_FAIL_COND_V_MSG(buf.download_state != DownloadState::MAPPED, nullptr, "WebGPU buffer data is not mapped; wait for the asynchronous map completion.");
		const void *data = wgpuBufferGetConstMappedRange(buf.buffer.get(), 0, buf.allocation_size);
		ERR_FAIL_NULL_V(data, nullptr);
		buf.download_bytes.resize(buf.allocation_size);
		std::memcpy(buf.download_bytes.data(), data, buf.allocation_size);
		wgpuBufferUnmap(buf.buffer.get());
		buf.download_state = DownloadState::IDLE;
		buf.cpu_mapped = true;
		return buf.download_bytes.data();
	}
	ERR_FAIL_COND_V_MSG(buf.access != BufferAccess::UPLOAD, nullptr, "GPU-only buffers require an asynchronous readback ticket.");
	buf.cpu_mapped = true;
	return buf.upload.data();
}

void RenderingDeviceDriverWebGPU::buffer_unmap(BufferID p_buffer) {
	state->assert_owner();
	auto &buf = *resource<BufferResource>(p_buffer);
	DEV_ASSERT(buf.cpu_mapped);
	buf.cpu_mapped = false;
}

uint8_t *RenderingDeviceDriverWebGPU::buffer_persistent_map_advance(BufferID p_buffer, uint64_t p_frames_drawn) {
	state->assert_owner();
	auto &buf = *resource<BufferResource>(p_buffer);
	DEV_ASSERT(buf.access == BufferAccess::DYNAMIC && frame_count);
	const uint32_t next = p_frames_drawn % frame_count;
	// CPU shadow bytes are uploaded via wgpuQueueWriteBuffer on the single ordered queue,
	// which executes strictly after all previously submitted command buffers finish reading.
	buf.frame = next;
	return buf.upload.data() + buf.offset(next);
}

uint64_t RenderingDeviceDriverWebGPU::buffer_get_dynamic_offsets(Span<BufferID> p_buffers) {
	uint64_t bits = 0;
	uint32_t shift = 0;
	for (const BufferID &id : p_buffers) {
		if (!id) {
			continue; // Sparse vertex bindings do not consume a dynamic slot.
		}
		const auto &buf = *resource<BufferResource>(id);
		if (buf.access == BufferAccess::DYNAMIC) {
			ERR_FAIL_COND_V(shift >= sizeof(bits) * CHAR_BIT, 0);
			bits |= uint64_t(buf.frame) << shift;
			shift += VERTEX_FRAME_BITS;
		}
	}
	return bits;
}

void RenderingDeviceDriverWebGPU::buffer_flush(BufferID p_buffer) {
	state->assert_owner();
	const auto &buf = *resource<BufferResource>(p_buffer);
	DEV_ASSERT(buf.access == BufferAccess::DYNAMIC);
	const uint64_t slice_offset = buf.offset(buf.frame);
	const RenderingDeviceDriver::BufferFlushRange &range = RenderingDeviceDriver::get_active_buffer_flush_range();
	if (range.size == 0) {
		return;
	}
	const uint64_t first = range.size == BUFFER_WHOLE_SIZE ? 0 : range.offset & ~(uint64_t(BUFFER_COPY_ALIGNMENT) - 1);
	const uint64_t end = range.size == BUFFER_WHOLE_SIZE ? align_up(buf.size, BUFFER_COPY_ALIGNMENT) : align_up(range.offset + range.size, BUFFER_COPY_ALIGNMENT);
	DEV_ASSERT(first <= end && end <= buf.allocation_size - slice_offset);
	const uint64_t bytes = end - first;
	wgpuQueueWriteBuffer(state->queue.get(), buf.buffer.get(), slice_offset + first, buf.upload.data() + slice_offset + first, bytes);
	count_buffer_write(bytes);
}

Error RenderingDeviceDriverWebGPU::prepare_upload_for_copy(BufferID p_buffer, uint64_t p_offset, uint64_t p_size) {
	state->assert_owner();
	const auto &buf = *resource<BufferResource>(p_buffer);
	if (buf.access != BufferAccess::UPLOAD) {
		return OK;
	}
	DEV_ASSERT(p_offset <= buf.size && p_size <= buf.size - p_offset);
	const uint64_t first = p_offset & ~(uint64_t(BUFFER_COPY_ALIGNMENT) - 1);
	const uint64_t end = align_up(p_offset + p_size, BUFFER_COPY_ALIGNMENT);
	const uint64_t bytes = end - first;
	wgpuQueueWriteBuffer(state->queue.get(), buf.buffer.get(), first, buf.upload.data() + first, bytes);
	count_buffer_write(bytes);
	return OK;
}
