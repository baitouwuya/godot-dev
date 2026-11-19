#include "rendering_device_driver_webgpu.h"

#include <cstring>

using namespace godot::webgpu;

namespace godot::webgpu {

struct ReadbackRequest : GpuUse {
	BufferHandle staging;
	ReadbackTicket ticket;
	DownloadState status = DownloadState::IDLE;
	uint64_t staging_bytes = 0;
	uint64_t first_byte = 0;
	uint64_t result_bytes = 0;
	std::optional<TextureFormatInfo> format;
	WGPUExtent3D extent{};
	CopyLayout physical;
	CopyLayout logical;
};

namespace {

bool create_staging(DeviceState &p_device, ReadbackRequest &r_request) {
	ERR_FAIL_COND_V(r_request.staging_bytes > p_device.limits.maxBufferSize || r_request.staging_bytes > SIZE_MAX, false);
	WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	desc.size = r_request.staging_bytes;
	desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
	r_request.staging.reset(wgpuDeviceCreateBuffer(p_device.device.get(), &desc));
	ERR_FAIL_COND_V(!r_request.staging, false);
	return true;
}

template <class T>
struct MapCompletion {
	DeviceLease state;
	std::shared_ptr<T> resource;
	DownloadState T::*status;

	static void callback(WGPUMapAsyncStatus p_status, WGPUStringView, void *p_userdata, void *) {
		std::unique_ptr<MapCompletion> completion(static_cast<MapCompletion *>(p_userdata));
		completion->state->assert_owner();
		completion->resource.get()->*completion->status = p_status == WGPUMapAsyncStatus_Success ? DownloadState::MAPPED : DownloadState::FAILED;
	}

	static void start(const DeviceLease &p_state, const std::shared_ptr<T> &p_resource, DownloadState T::*p_status, WGPUBuffer p_buffer, size_t p_size) {
		p_resource.get()->*p_status = DownloadState::PENDING;
		WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
		info.mode = WGPUCallbackMode_AllowSpontaneous;
		info.callback = callback;
		info.userdata1 = new MapCompletion{ p_state, p_resource, p_status };
		wgpuBufferMapAsync(p_buffer, WGPUMapMode_Read, 0, p_size, info);
	}
};

} // namespace
} // namespace godot::webgpu

ReadbackTicket RenderingDeviceDriverWebGPU::add_readback(std::shared_ptr<ReadbackRequest> p_request) {
	uint32_t slot;
	if (free_readback_slots.empty()) {
		slot = readbacks.size();
		readbacks.emplace_back();
	} else {
		slot = free_readback_slots.back();
		free_readback_slots.pop_back();
	}
	p_request->ticket = { slot, ++next_readback_generation, 0 };
	readbacks[slot] = std::move(p_request);
	return readbacks[slot]->ticket;
}

ReadbackTicket RenderingDeviceDriverWebGPU::encode_buffer_readback(WGPUCommandEncoder p_encoder, BufferID p_buffer, uint64_t p_offset, uint64_t p_size, UseList &r_uses) {
	state->assert_owner();
	const auto &source = resource<BufferResource>(p_buffer);
	DEV_ASSERT(p_size && p_offset <= source->size && p_size <= source->size - p_offset);
	ERR_FAIL_COND_V_MSG(!(source->usage & WGPUBufferUsage_CopySrc), {}, "WebGPU readback requires CopySrc usage.");
	const Error upload_status = prepare_upload_for_copy(p_buffer, p_offset, p_size);
	ERR_FAIL_COND_V(upload_status != OK, {});
	const uint64_t first = p_offset & ~(uint64_t(BUFFER_COPY_ALIGNMENT) - 1);
	auto request = std::make_shared<ReadbackRequest>();
	request->first_byte = p_offset - first;
	request->result_bytes = p_size;
	request->staging_bytes = align_up(request->first_byte + p_size, BUFFER_COPY_ALIGNMENT);
	if (!create_staging(*state, *request)) {
		return {};
	}
	uint64_t base = 0;
	if (source->access == BufferAccess::DYNAMIC) {
		base = source->offset(source->frame);
		r_uses.push_back(source->pages[source->frame]);
	}
	wgpuCommandEncoderCopyBufferToBuffer(p_encoder, source->buffer.get(), base + first, request->staging.get(), 0, request->staging_bytes);
	r_uses.push_back(source);
	r_uses.push_back(request);
	return add_readback(std::move(request));
}

ReadbackTicket RenderingDeviceDriverWebGPU::encode_texture_readback(WGPUCommandEncoder p_encoder, TextureID p_texture, const TextureSubresource &p_subresource, UseList &r_uses) {
	state->assert_owner();
	const auto &source = resource<TextureResource>(p_texture);
	const auto &backing = *source->backing;
	DEV_ASSERT(p_subresource.mipmap < source->mip_count && p_subresource.layer < source->layer_count);
	ERR_FAIL_COND_V_MSG(!(backing.usage & WGPUTextureUsage_CopySrc) || !backing.texture || backing.desc.samples != TEXTURE_SAMPLES_1, {}, "WebGPU texture readback requires single-sampled CopySrc texture backing.");
	auto request = std::make_shared<ReadbackRequest>();
	request->format = source->format;
	TextureFormatInfo &format = *request->format;
	WGPUTextureAspect aspect = WGPUTextureAspect_All;
	switch (p_subresource.aspect) {
		case TEXTURE_ASPECT_DEPTH:
			DEV_ASSERT(format.depth);
			aspect = WGPUTextureAspect_DepthOnly;
			break;
		case TEXTURE_ASPECT_STENCIL:
			DEV_ASSERT(format.stencil);
			format.logical_bytes = format.physical_bytes = 1;
			aspect = WGPUTextureAspect_StencilOnly;
			break;
		default:
			DEV_ASSERT(!format.depth && !format.stencil);
			break;
	}
	ERR_FAIL_COND_V_MSG(!format.logical_bytes, {}, "The requested opaque depth aspect has no copyable WebGPU representation.");
	request->extent = source->extent(p_subresource.mipmap, 1);
	request->physical = copy_layout(format, request->extent, true, true);
	request->logical = copy_layout(format, request->extent, false, false);
	request->staging_bytes = request->physical.size();
	request->result_bytes = request->logical.size();
	if (!create_staging(*state, *request)) {
		return {};
	}
	WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	src.texture = backing.texture.get();
	src.mipLevel = source->base_mip + p_subresource.mipmap;
	src.origin.z = source->base_layer + p_subresource.layer;
	src.aspect = aspect;
	WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	dst.buffer = request->staging.get();
	dst.layout.bytesPerRow = request->physical.row_pitch;
	dst.layout.rowsPerImage = request->physical.rows;
	WGPUExtent3D copy_extent = request->extent;
	copy_extent.width = ((copy_extent.width + format.block_width - 1) / format.block_width) * format.block_width;
	copy_extent.height = ((copy_extent.height + format.block_height - 1) / format.block_height) * format.block_height;
	wgpuCommandEncoderCopyTextureToBuffer(p_encoder, &src, &dst, &copy_extent);
	r_uses.push_back(source);
	r_uses.push_back(source->backing);
	r_uses.push_back(request);
	return add_readback(std::move(request));
}

Error RenderingDeviceDriverWebGPU::arm_readback(ReadbackTicket &r_ticket, SubmitSerial p_serial) {
	state->assert_owner();
	if (r_ticket.slot >= readbacks.size() || !readbacks[r_ticket.slot] || readbacks[r_ticket.slot]->ticket.generation != r_ticket.generation) {
		return ERR_INVALID_PARAMETER;
	}
	const auto &request = readbacks[r_ticket.slot];
	DEV_ASSERT(request->status == DownloadState::IDLE && p_serial && request->last_use_serial == p_serial);
	request->ticket.submit_serial = r_ticket.submit_serial = p_serial;
	MapCompletion<ReadbackRequest>::start(state, request, &ReadbackRequest::status, request->staging.get(), request->staging_bytes);
	return OK;
}

Error RenderingDeviceDriverWebGPU::take_readback(const ReadbackTicket &p_ticket, Vector<uint8_t> &r_data) {
	state->assert_owner();
	if (p_ticket.slot >= readbacks.size() || !readbacks[p_ticket.slot] || readbacks[p_ticket.slot]->ticket.generation != p_ticket.generation ||
			readbacks[p_ticket.slot]->ticket.submit_serial != p_ticket.submit_serial) {
		return ERR_INVALID_PARAMETER;
	}
	const auto &request = readbacks[p_ticket.slot];
	if (request->status == DownloadState::FAILED) {
		cancel_readback(p_ticket);
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	if (request->status != DownloadState::MAPPED) {
		return ERR_BUSY;
	}
	const auto *data = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(request->staging.get(), 0, request->staging_bytes));
	if (!data || r_data.resize(request->result_bytes) != OK) {
		cancel_readback(p_ticket);
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	if (request->format) {
		unpack_texture_rows(*request->format, request->extent, data, request->physical.row_pitch, r_data.ptrw(), request->logical.row_pitch);
	} else {
		std::memcpy(r_data.ptrw(), data + request->first_byte, request->result_bytes);
	}
	cancel_readback(p_ticket);
	return OK;
}

void RenderingDeviceDriverWebGPU::cancel_readback(const ReadbackTicket &p_ticket) {
	state->assert_owner();
	if (p_ticket.slot >= readbacks.size() || !readbacks[p_ticket.slot] || readbacks[p_ticket.slot]->ticket.generation != p_ticket.generation) {
		return;
	}
	auto request = std::move(readbacks[p_ticket.slot]);
	free_readback_slots.push_back(p_ticket.slot);
	if (request->status == DownloadState::PENDING || request->status == DownloadState::MAPPED) {
		wgpuBufferUnmap(request->staging.get());
	}
}

Error RenderingDeviceDriverWebGPU::arm_download_buffer(BufferID p_buffer, SubmitSerial p_serial) {
	state->assert_owner();
	const auto &buffer = resource<BufferResource>(p_buffer);
	DEV_ASSERT(buffer->access == BufferAccess::DOWNLOAD && buffer->download_state == DownloadState::IDLE && p_serial && buffer->last_use_serial == p_serial);
	arm_download(state, buffer);
	return OK;
}

void RenderingDeviceDriverWebGPU::arm_download(const DeviceLease &p_state, const std::shared_ptr<BufferResource> &p_buffer) {
	MapCompletion<BufferResource>::start(p_state, p_buffer, &BufferResource::download_state, p_buffer->buffer.get(), p_buffer->allocation_size);
}
