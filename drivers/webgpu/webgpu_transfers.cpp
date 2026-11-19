#include "webgpu_commands.h"

#include <algorithm>

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

WGPUTextureAspect copy_aspect(RDD::TextureAspect p_aspect) {
	return p_aspect == RDD::TEXTURE_ASPECT_DEPTH ? WGPUTextureAspect_DepthOnly : p_aspect == RDD::TEXTURE_ASPECT_STENCIL ? WGPUTextureAspect_StencilOnly : WGPUTextureAspect_All;
}

WGPUTexelCopyTextureInfo texture_copy(const TextureResource &p_texture, uint32_t p_mip, uint32_t p_layer, const Vector3i &p_offset, WGPUTextureAspect p_aspect) {
	WGPUTexelCopyTextureInfo copy = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	copy.texture = p_texture.backing->texture.get();
	copy.mipLevel = p_texture.base_mip + p_mip;
	copy.origin = { uint32_t(p_offset.x), uint32_t(p_offset.y), uint32_t(p_offset.z) + p_layer + p_texture.base_layer };
	copy.aspect = p_aspect;
	return copy;
}

uint32_t round_to_block(uint32_t p_pixels, uint32_t p_block) {
	return ((p_pixels + p_block - 1) / p_block) * p_block;
}

WGPUExtent3D region_extent(const TextureFormatInfo &p_format, const Vector3i &p_size) {
	return { round_to_block(p_size.x, p_format.block_width), round_to_block(p_size.y, p_format.block_height), uint32_t(p_size.z) };
}

WGPUTexelCopyBufferInfo buffer_copy(WGPUBuffer p_buffer, uint64_t p_offset, const CopyLayout &p_layout) {
	WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	dst.buffer = p_buffer;
	dst.layout.offset = p_offset;
	dst.layout.bytesPerRow = p_layout.row_pitch;
	dst.layout.rowsPerImage = p_layout.rows;
	return dst;
}

} // namespace

std::shared_ptr<BufferResource> make_transfer_buffer(DeviceState &p_state, uint64_t p_size, WGPUBufferUsage p_usage, bool p_mapped) {
	WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	desc.usage = p_usage;
	desc.size = align_up(p_size, BUFFER_COPY_ALIGNMENT);
	desc.mappedAtCreation = p_mapped;
	ERR_FAIL_COND_V(desc.size > p_state.limits.maxBufferSize || desc.size > SIZE_MAX, {});
	auto buffer = std::make_shared<BufferResource>();
	buffer->size = p_size;
	buffer->allocation_size = desc.size;
	buffer->usage = p_usage;
	buffer->buffer.reset(wgpuDeviceCreateBuffer(p_state.device.get(), &desc));
	ERR_FAIL_COND_V(!buffer->buffer, {});
	return buffer;
}

TextureViewHandle single_attachment(const TextureResource &p_texture, uint32_t p_mip, uint32_t p_layer, WGPUTextureAspect p_aspect) {
	WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	desc.usage = WGPUTextureUsage_RenderAttachment;
	desc.dimension = WGPUTextureViewDimension_2D;
	desc.format = p_aspect == WGPUTextureAspect_All ? p_texture.format.format : WGPUTextureFormat_Undefined;
	desc.aspect = p_aspect;
	desc.baseMipLevel = p_texture.base_mip + p_mip;
	desc.mipLevelCount = 1;
	desc.baseArrayLayer = p_texture.base_layer + p_layer;
	desc.arrayLayerCount = 1;
	return create_texture_view(p_texture.backing->texture.get(), desc);
}

void CommandRecording::download(const std::shared_ptr<BufferResource> &p_buffer) {
	if (std::find(downloads.begin(), downloads.end(), p_buffer) != downloads.end()) {
		return;
	}
	DEV_ASSERT(!p_buffer->cpu_mapped && p_buffer->download_state != DownloadState::PENDING);
	if (p_buffer->download_state == DownloadState::MAPPED) {
		wgpuBufferUnmap(p_buffer->buffer.get());
	}
	p_buffer->download_state = DownloadState::IDLE;
	p_buffer->download_bytes.clear();
	downloads.push_back(p_buffer);
	keep(p_buffer);
}

} // namespace godot::webgpu

void RenderingDeviceDriverWebGPU::command_clear_buffer(CommandBufferID p_buffer, BufferID p_destination, uint64_t p_offset, uint64_t p_size) {
	auto &cmd = recording(*state, p_buffer);
	cmd.outside_pass();
	const auto &dst = resource<BufferResource>(p_destination);
	const uint64_t size = p_offset + p_size == dst->size ? align_up(p_size, BUFFER_COPY_ALIGNMENT) : p_size;
	ERR_FAIL_COND_MSG((p_offset | size) % BUFFER_COPY_ALIGNMENT, "WebGPU buffer clear requires four-byte alignment.");
	wgpuCommandEncoderClearBuffer(cmd.encoder.get(), dst->buffer.get(), p_offset, size);
	cmd.keep(dst);
}

void RenderingDeviceDriverWebGPU::command_copy_buffer(CommandBufferID p_buffer, BufferID p_source, BufferID p_destination, VectorView<BufferCopyRegion> p_regions) {
	auto &cmd = recording(*state, p_buffer);
	cmd.outside_pass();
	const auto &src = resource<BufferResource>(p_source);
	const auto &dst = resource<BufferResource>(p_destination);
	for (uint32_t i = 0; i < p_regions.size(); ++i) {
		const auto &region = p_regions[i];
		if (dst->access == BufferAccess::DOWNLOAD) {
			DEV_ASSERT(src->access != BufferAccess::UPLOAD);
			const uint64_t start = src->offset(src->frame) + region.src_offset;
			const uint64_t first = start - start % BUFFER_COPY_ALIGNMENT;
			const uint64_t size = align_up(start + region.size, BUFFER_COPY_ALIGNMENT) - first;
			auto staging = make_transfer_buffer(*state, size, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
			ERR_FAIL_COND(!staging);
			wgpuCommandEncoderCopyBufferToBuffer(cmd.encoder.get(), src->buffer.get(), first, staging->buffer.get(), 0, size);
			dst->download_bytes.clear();
			dst->download_regions.push_back({ staging, {}, {}, region.dst_offset, 0, 0, start - first, region.size });
			cmd.download(staging);
			cmd.keep(src);
			if (src->access == BufferAccess::DYNAMIC) {
				cmd.keep(src->pages[src->frame]);
			}
			continue;
		}
		const uint64_t size = region.dst_offset + region.size == dst->size ? align_up(region.size, BUFFER_COPY_ALIGNMENT) : region.size;
		ERR_FAIL_COND_MSG((region.dst_offset | size) % BUFFER_COPY_ALIGNMENT, "WebGPU buffer copy destination and length require four-byte alignment.");
		if (src->access == BufferAccess::UPLOAD) {
			auto staging = make_transfer_buffer(*state, size, WGPUBufferUsage_CopySrc, true);
			ERR_FAIL_COND(!staging);
			void *data = wgpuBufferGetMappedRange(staging->buffer.get(), 0, size);
			ERR_FAIL_NULL(data);
			std::memcpy(data, src->upload.data() + region.src_offset, region.size);
			wgpuBufferUnmap(staging->buffer.get());
			wgpuCommandEncoderCopyBufferToBuffer(cmd.encoder.get(), staging->buffer.get(), 0, dst->buffer.get(), region.dst_offset, size);
			cmd.keep(staging);
		} else {
			ERR_FAIL_COND_MSG(region.src_offset % BUFFER_COPY_ALIGNMENT || region.src_offset + size > src->allocation_size, "WebGPU buffer copy source requires a four-byte-aligned range.");
			wgpuCommandEncoderCopyBufferToBuffer(cmd.encoder.get(), src->buffer.get(), src->offset(src->frame) + region.src_offset, dst->buffer.get(), region.dst_offset, size);
			cmd.keep(src);
		}
	}
	cmd.keep(dst);
}

void RenderingDeviceDriverWebGPU::command_copy_texture(CommandBufferID p_buffer, TextureID p_source, TextureLayout, TextureID p_destination, TextureLayout, VectorView<TextureCopyRegion> p_regions) {
	auto &cmd = recording(*state, p_buffer);
	cmd.outside_pass();
	const auto &src = resource<TextureResource>(p_source);
	const auto &dst = resource<TextureResource>(p_destination);
	ERR_FAIL_COND_MSG(!src->backing->texture || !dst->backing->texture || src->format.encoding != dst->format.encoding, "These WebGPU textures do not have copy-compatible physical backing.");
	for (uint32_t i = 0; i < p_regions.size(); ++i) {
		const auto &region = p_regions[i];
		const auto from = texture_copy(*src, region.src_subresources.mipmap, region.src_subresources.base_layer, region.src_offset, WGPUTextureAspect_All);
		const auto to = texture_copy(*dst, region.dst_subresources.mipmap, region.dst_subresources.base_layer, region.dst_offset, WGPUTextureAspect_All);
		auto extent = region_extent(src->format, region.size);
		if (src->backing->dimension != WGPUTextureDimension_3D) {
			extent.depthOrArrayLayers = region.src_subresources.layer_count;
		}
		wgpuCommandEncoderCopyTextureToTexture(cmd.encoder.get(), &from, &to, &extent);
	}
	cmd.keep(src);
	cmd.keep(dst);
}

void RenderingDeviceDriverWebGPU::command_copy_buffer_to_texture(CommandBufferID p_buffer, BufferID p_source, TextureID p_destination, TextureLayout, VectorView<BufferTextureCopyRegion> p_regions) {
	auto &cmd = recording(*state, p_buffer);
	cmd.outside_pass();
	const auto &src = resource<BufferResource>(p_source);
	const auto &dst = resource<TextureResource>(p_destination);
	ERR_FAIL_COND_MSG(!dst->backing->texture, "Copy into an atomic image requires the storage-buffer image transfer path.");
	for (uint32_t i = 0; i < p_regions.size(); ++i) {
		const auto &r = p_regions[i];
		const auto to = texture_copy(*dst, r.texture_subresource.mipmap, r.texture_subresource.layer, r.texture_offset, copy_aspect(r.texture_subresource.aspect));
		const auto extent = region_extent(dst->format, r.texture_region_size);
		const auto physical = copy_layout(dst->format, extent, true, true);
		if (src->access == BufferAccess::UPLOAD) {
			auto staging = make_transfer_buffer(*state, physical.size(), WGPUBufferUsage_CopySrc, true);
			ERR_FAIL_COND(!staging);
			auto *bytes = static_cast<uint8_t *>(wgpuBufferGetMappedRange(staging->buffer.get(), 0, staging->allocation_size));
			ERR_FAIL_NULL(bytes);
			pack_texture_rows(dst->format, extent, src->upload.data() + r.buffer_offset, r.row_pitch, bytes, physical.row_pitch);
			wgpuBufferUnmap(staging->buffer.get());
			const auto from = buffer_copy(staging->buffer.get(), 0, physical);
			wgpuCommandEncoderCopyBufferToTexture(cmd.encoder.get(), &from, &to, &extent);
			cmd.keep(staging);
		} else {
			ERR_FAIL_COND_MSG(dst->format.encoding != TexelEncoding::COPY || r.row_pitch != physical.row_pitch, "GPU texture upload requires matching physical texel packing.");
			const auto from = buffer_copy(src->buffer.get(), r.buffer_offset, physical);
			wgpuCommandEncoderCopyBufferToTexture(cmd.encoder.get(), &from, &to, &extent);
			cmd.keep(src);
		}
	}
	cmd.keep(dst);
}

void RenderingDeviceDriverWebGPU::command_copy_texture_to_buffer(CommandBufferID p_buffer, TextureID p_source, TextureLayout, BufferID p_destination, VectorView<BufferTextureCopyRegion> p_regions) {
	auto &cmd = recording(*state, p_buffer);
	cmd.outside_pass();
	const auto &src = resource<TextureResource>(p_source);
	const auto &dst = resource<BufferResource>(p_destination);
	ERR_FAIL_COND_MSG(!src->backing->texture, "Copy from an atomic image requires the storage-buffer image transfer path.");
	for (uint32_t i = 0; i < p_regions.size(); ++i) {
		const auto &r = p_regions[i];
		auto format = src->format;
		if (r.texture_subresource.aspect == TEXTURE_ASPECT_STENCIL) {
			format.logical_bytes = format.physical_bytes = 1;
		}
		ERR_FAIL_COND_MSG(!format.physical_bytes, "This WebGPU depth format does not expose a portable buffer copy layout.");
		const auto from = texture_copy(*src, r.texture_subresource.mipmap, r.texture_subresource.layer, r.texture_offset, copy_aspect(r.texture_subresource.aspect));
		const auto extent = region_extent(format, r.texture_region_size);
		const auto physical = copy_layout(format, extent, true, true);
		if (dst->access == BufferAccess::DOWNLOAD) {
			auto staging = make_transfer_buffer(*state, physical.size(), WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
			ERR_FAIL_COND(!staging);
			const auto to = buffer_copy(staging->buffer.get(), 0, physical);
			wgpuCommandEncoderCopyTextureToBuffer(cmd.encoder.get(), &from, &to, &extent);
			dst->download_bytes.clear();
			dst->download_regions.push_back({ staging, format, extent, r.buffer_offset, physical.row_pitch, uint32_t(r.row_pitch) });
			cmd.download(staging);
		} else {
			ERR_FAIL_COND_MSG(format.encoding != TexelEncoding::COPY || r.row_pitch != physical.row_pitch, "GPU texture download requires matching physical texel packing.");
			const auto to = buffer_copy(dst->buffer.get(), r.buffer_offset, physical);
			wgpuCommandEncoderCopyTextureToBuffer(cmd.encoder.get(), &from, &to, &extent);
		}
	}
	cmd.keep(src);
	cmd.keep(dst);
}

bool RenderingDeviceDriverWebGPU::resolve_attachment(CommandRecording &r_cmd, const TextureResource &p_src, const TextureResource &p_dst,
		uint32_t p_src_mip, uint32_t p_src_layer, uint32_t p_dst_mip, uint32_t p_dst_layer) {
	r_cmd.outside_pass();
	const auto dst = single_attachment(p_dst, p_dst_mip, p_dst_layer);
	ERR_FAIL_COND_V(!dst, false);
	if (p_src.format.depth) {
		const auto src = single_attachment(p_src, p_src_mip, p_src_layer, WGPUTextureAspect_DepthOnly);
		ERR_FAIL_COND_V(!src, false);
		return resolve_depth(r_cmd, p_src, src.get(), p_dst, dst.get());
	}
	const auto src = single_attachment(p_src, p_src_mip, p_src_layer);
	ERR_FAIL_COND_V(!src, false);
	WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	color.view = src.get();
	color.resolveTarget = dst.get();
	color.loadOp = WGPULoadOp_Load;
	color.storeOp = WGPUStoreOp_Store;
	WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	desc.colorAttachmentCount = 1;
	desc.colorAttachments = &color;
	RenderPassEncoderHandle pass(wgpuCommandEncoderBeginRenderPass(r_cmd.encoder.get(), &desc));
	ERR_FAIL_COND_V(!pass, false);
	wgpuRenderPassEncoderEnd(pass.get());
	return true;
}

void RenderingDeviceDriverWebGPU::command_resolve_texture(CommandBufferID p_buffer, TextureID p_source, TextureLayout, uint32_t p_src_layer, uint32_t p_src_mip,
		TextureID p_destination, TextureLayout, uint32_t p_dst_layer, uint32_t p_dst_mip) {
	auto &cmd = recording(*state, p_buffer);
	const auto &src = resource<TextureResource>(p_source);
	const auto &dst = resource<TextureResource>(p_destination);
	resolve_attachment(cmd, *src, *dst, p_src_mip, p_src_layer, p_dst_mip, p_dst_layer);
	cmd.keep(src);
	cmd.keep(dst);
}

void RenderingDeviceDriverWebGPU::command_clear_color_texture(CommandBufferID p_buffer, TextureID p_texture, TextureLayout, const Color &p_color, const TextureSubresourceRange &p_range) {
	auto &cmd = recording(*state, p_buffer);
	cmd.outside_pass();
	const auto &texture = resource<TextureResource>(p_texture);
	const auto &backing = *texture->backing;
	if (backing.atomic_buffer) {
		ERR_FAIL_COND_MSG(p_color.r != 0.f, "Nonzero storage-buffer image clear is not supported.");
		wgpuCommandEncoderClearBuffer(cmd.encoder.get(), backing.atomic_buffer.get(), ATOMIC_IMAGE_HEADER_BYTES, backing.atomic_size - ATOMIC_IMAGE_HEADER_BYTES);
		cmd.keep(texture);
		return;
	}
	for (uint32_t m = 0; m < p_range.mipmap_count; ++m) {
		// RD's clear ranges refer to the backing texture, not the slice view.
		const uint32_t mip = p_range.base_mipmap - texture->base_mip + m;
		const uint32_t base_layer = p_range.base_layer - texture->base_layer;
		if (!(backing.usage & WGPUTextureUsage_RenderAttachment)) {
			ERR_FAIL_COND_MSG(p_color.r != 0.f || p_color.g != 0.f || p_color.b != 0.f || p_color.a != 0.f || !(backing.usage & WGPUTextureUsage_CopyDst),
					"Non-attachment WebGPU texture clear requires a copyable zero fill.");
			auto extent = texture->extent(mip, p_range.layer_count);
			extent.width = round_to_block(extent.width, texture->format.block_width);
			extent.height = round_to_block(extent.height, texture->format.block_height);
			const auto layout = copy_layout(texture->format, extent, true, true);
			auto zero = make_transfer_buffer(*state, layout.size(), WGPUBufferUsage_CopySrc);
			ERR_FAIL_COND(!zero);
			const auto from = buffer_copy(zero->buffer.get(), 0, layout);
			const auto to = texture_copy(*texture, mip, base_layer, {}, WGPUTextureAspect_All);
			wgpuCommandEncoderCopyBufferToTexture(cmd.encoder.get(), &from, &to, &extent);
			cmd.keep(zero);
			continue;
		}
		for (uint32_t l = 0; l < p_range.layer_count; ++l) {
			const auto view = single_attachment(*texture, mip, base_layer + l);
			ERR_FAIL_COND(!view);
			WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			color.view = view.get();
			color.loadOp = WGPULoadOp_Clear;
			color.storeOp = WGPUStoreOp_Store;
			color.clearValue = wg_color(p_color);
			WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
			desc.colorAttachmentCount = 1;
			desc.colorAttachments = &color;
			RenderPassEncoderHandle pass(wgpuCommandEncoderBeginRenderPass(cmd.encoder.get(), &desc));
			ERR_FAIL_COND(!pass);
			wgpuRenderPassEncoderEnd(pass.get());
		}
	}
	cmd.keep(texture);
}

void RenderingDeviceDriverWebGPU::command_clear_depth_stencil_texture(CommandBufferID p_buffer, TextureID p_texture, TextureLayout, float p_depth, uint8_t p_stencil, const TextureSubresourceRange &p_range) {
	auto &cmd = recording(*state, p_buffer);
	cmd.outside_pass();
	const auto &texture = resource<TextureResource>(p_texture);
	for (uint32_t m = 0; m < p_range.mipmap_count; ++m) {
		for (uint32_t l = 0; l < p_range.layer_count; ++l) {
			const auto view = single_attachment(*texture, p_range.base_mipmap - texture->base_mip + m, p_range.base_layer - texture->base_layer + l);
			ERR_FAIL_COND(!view);
			WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
			depth.view = view.get();
			if (texture->format.depth) {
				depth.depthReadOnly = !p_range.aspect.has_flag(TEXTURE_ASPECT_DEPTH_BIT);
				if (!depth.depthReadOnly) {
					depth.depthLoadOp = WGPULoadOp_Clear;
					depth.depthStoreOp = WGPUStoreOp_Store;
					depth.depthClearValue = p_depth;
				}
			}
			if (texture->format.stencil) {
				depth.stencilReadOnly = !p_range.aspect.has_flag(TEXTURE_ASPECT_STENCIL_BIT);
				if (!depth.stencilReadOnly) {
					depth.stencilLoadOp = WGPULoadOp_Clear;
					depth.stencilStoreOp = WGPUStoreOp_Store;
					depth.stencilClearValue = p_stencil;
				}
			}
			WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
			desc.depthStencilAttachment = &depth;
			RenderPassEncoderHandle pass(wgpuCommandEncoderBeginRenderPass(cmd.encoder.get(), &desc));
			ERR_FAIL_COND(!pass);
			wgpuRenderPassEncoderEnd(pass.get());
		}
	}
	cmd.keep(texture);
}
