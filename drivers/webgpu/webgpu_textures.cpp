#include "rendering_device_driver_webgpu.h"

#include <algorithm>
#include <cstring>

#ifdef WEB_ENABLED
extern "C" WGPUTextureView godot_webgpu_texture_create_view(
		WGPUTexture p_texture,
		uint32_t p_format,
		uint32_t p_dimension,
		uint32_t p_base_mip_level,
		uint32_t p_mip_level_count,
		uint32_t p_base_array_layer,
		uint32_t p_array_layer_count,
		uint32_t p_aspect,
		uint32_t p_usage);
#endif

using namespace godot::webgpu;

namespace godot::webgpu {

TextureViewHandle create_texture_view(WGPUTexture p_texture, const WGPUTextureViewDescriptor &p_desc) {
	DEV_ASSERT(p_texture && !p_desc.nextInChain);
#ifdef WEB_ENABLED
	return TextureViewHandle(godot_webgpu_texture_create_view(
			p_texture,
			static_cast<uint32_t>(p_desc.format),
			static_cast<uint32_t>(p_desc.dimension),
			p_desc.baseMipLevel,
			p_desc.mipLevelCount,
			p_desc.baseArrayLayer,
			p_desc.arrayLayerCount,
			static_cast<uint32_t>(p_desc.aspect),
			static_cast<uint32_t>(p_desc.usage)));
#else
	return TextureViewHandle(wgpuTextureCreateView(p_texture, &p_desc));
#endif
}

namespace {

WGPUTextureViewDimension view_dimension(Rdc::TextureType p_type) {
	switch (p_type) {
		case Rdc::TEXTURE_TYPE_1D:
			return WGPUTextureViewDimension_1D;
		case Rdc::TEXTURE_TYPE_2D:
			return WGPUTextureViewDimension_2D;
		case Rdc::TEXTURE_TYPE_3D:
			return WGPUTextureViewDimension_3D;
		case Rdc::TEXTURE_TYPE_CUBE:
			return WGPUTextureViewDimension_Cube;
		case Rdc::TEXTURE_TYPE_CUBE_ARRAY:
			return WGPUTextureViewDimension_CubeArray;
		case Rdc::TEXTURE_TYPE_2D_ARRAY:
			return WGPUTextureViewDimension_2DArray;
		default:
			return WGPUTextureViewDimension_Undefined;
	}
}

WGPUTextureDimension base_dimension(Rdc::TextureType p_type) {
	return p_type == Rdc::TEXTURE_TYPE_3D ? WGPUTextureDimension_3D : p_type == Rdc::TEXTURE_TYPE_1D ? WGPUTextureDimension_1D
																									 : WGPUTextureDimension_2D;
}

TextureFormatInfo view_format(const DeviceState &p_state, const TextureBacking &p_backing, const RDD::TextureView &p_view) {
	if (p_view.format == Rdc::DATA_FORMAT_MAX || p_view.format == p_backing.desc.format) {
		const bool storage = (p_backing.usage & WGPUTextureUsage_StorageBinding) != 0;
		TextureFormatInfo info = texture_format(p_state, p_backing.desc.format, storage);
		return info.valid() && bake_texture_swizzle(info, p_view, storage) &&
				info.format == p_backing.storage.format && info.encoding == p_backing.storage.encoding ? info : TextureFormatInfo{};
	}
	TextureFormatInfo info = texture_format(p_state, p_view.format, false);
	if (!info.valid() || !bake_texture_swizzle(info, p_view, false) || info.encoding != p_backing.storage.encoding ||
			info.logical_bytes != p_backing.storage.logical_bytes || std::find(p_backing.view_formats.begin(), p_backing.view_formats.end(), info.format) == p_backing.view_formats.end()) {
		return {};
	}
	return info;
}

bool create_atomic_backing(DeviceState &p_state, TextureBacking &r_backing) {
	const auto &format = r_backing.desc;
	if (format.format != Rdc::DATA_FORMAT_R32_UINT && format.format != Rdc::DATA_FORMAT_R32_SINT) {
		return false;
	}
	const uint32_t slices = format.texture_type == Rdc::TEXTURE_TYPE_3D ? format.depth : format.array_layers;
	const uint64_t texels = uint64_t(format.width) * format.height * slices;
	WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	desc.size = ATOMIC_IMAGE_HEADER_BYTES + texels * SPIRV_WORD_BYTES;
	if (desc.size > p_state.limits.maxStorageBufferBindingSize || desc.size > p_state.limits.maxBufferSize || desc.size > SIZE_MAX) {
		return false;
	}
	desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
	desc.mappedAtCreation = true;
	r_backing.atomic_buffer.reset(wgpuDeviceCreateBuffer(p_state.device.get(), &desc));
	ERR_FAIL_COND_V(!r_backing.atomic_buffer, false);
	const uint32_t extent[] = { format.width, format.height, slices, 0 };
	void *mapped = wgpuBufferGetMappedRange(r_backing.atomic_buffer.get(), 0, desc.size);
	ERR_FAIL_NULL_V(mapped, false);
	std::memcpy(mapped, extent, sizeof(extent));
	wgpuBufferUnmap(r_backing.atomic_buffer.get());
	r_backing.atomic_size = desc.size;
	return true;
}

} // namespace

WGPUExtent3D TextureResource::extent(uint32_t p_mip, uint32_t p_layers) const {
	DEV_ASSERT(p_mip < mip_count);
	const auto &desc = backing->desc;
	const uint32_t mip = base_mip + p_mip;
	return { MAX(1u, desc.width >> mip), MAX(1u, desc.height >> mip), backing->dimension == WGPUTextureDimension_3D ? MAX(1u, desc.depth >> mip) : p_layers };
}

Error TextureResource::initialize_views() {
	if (!backing->texture) {
		DEV_ASSERT(backing->atomic_buffer);
		return OK;
	}
	WGPUTextureViewDescriptor desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	desc.format = format.format;
	desc.dimension = dimension;
	desc.baseMipLevel = base_mip;
	desc.mipLevelCount = mip_count;
	desc.baseArrayLayer = base_layer;
	desc.arrayLayerCount = dimension == WGPUTextureViewDimension_3D ? 1 : layer_count;
	if (backing->usage & WGPUTextureUsage_TextureBinding) {
		WGPUTextureViewDescriptor sample = desc;
		sample.usage = WGPUTextureUsage_TextureBinding;
		if (format.depth || format.stencil) {
			sample.format = WGPUTextureFormat_Undefined; // Uses the aspect-specific format for depth/stencil.
			sample.aspect = format.depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_StencilOnly;
		}
		TextureViewHandle view = create_texture_view(backing->texture.get(), sample);
		ERR_FAIL_COND_V(!view, ERR_CANT_CREATE);
		(format.depth ? depth_sampling : sampling) = std::move(view);
	}
	if ((backing->usage & WGPUTextureUsage_StorageBinding) && !format.srgb && format.format == backing->storage.format) {
		WGPUTextureViewDescriptor storage_desc = desc;
		storage_desc.usage = WGPUTextureUsage_StorageBinding;
		storage_desc.mipLevelCount = 1;
		if (dimension == WGPUTextureViewDimension_Cube || dimension == WGPUTextureViewDimension_CubeArray) {
			storage_desc.dimension = WGPUTextureViewDimension_2DArray;
		}
		storage = create_texture_view(backing->texture.get(), storage_desc);
		ERR_FAIL_COND_V(!storage, ERR_CANT_CREATE);
	}
	if ((backing->usage & WGPUTextureUsage_RenderAttachment) && format.renderable) {
		WGPUTextureViewDescriptor render_desc = desc;
		render_desc.usage = WGPUTextureUsage_RenderAttachment;
		render_desc.dimension = WGPUTextureViewDimension_2D;
		render_desc.arrayLayerCount = 1;
		render_desc.mipLevelCount = 1;
		attachment = create_texture_view(backing->texture.get(), render_desc);
		ERR_FAIL_COND_V(!attachment, ERR_CANT_CREATE);
	}
	return OK;
}

} // namespace godot::webgpu

RDD::TextureID RenderingDeviceDriverWebGPU::texture_create(const TextureFormat &p_format, const TextureView &p_view) {
	state->assert_owner();
	auto backing = std::make_shared<TextureBacking>();
	backing->desc = p_format;
	const bool atomic = p_format.usage_bits & TEXTURE_USAGE_STORAGE_ATOMIC_BIT;
	const bool storage = p_format.usage_bits & TEXTURE_USAGE_STORAGE_BIT;
	TextureFormatInfo &info = backing->storage;
	info = texture_format(*state, p_format.format, storage);
	ERR_FAIL_COND_V_MSG(!info.valid(), {}, "Texture format/usage is not supported by the enabled WebGPU device features.");
	ERR_FAIL_COND_V_MSG(!bake_texture_swizzle(info, p_view, storage), {}, "This WebGPU texture swizzle requires a shader view transform.");
	const uint32_t samples = 1u << p_format.samples;
	const auto dimension = view_dimension(p_format.texture_type);
	backing->dimension = base_dimension(p_format.texture_type);
	const uint32_t attachment_bits = TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | TEXTURE_USAGE_DEPTH_RESOLVE_ATTACHMENT_BIT;
	const bool render = (p_format.usage_bits & attachment_bits) != 0;
	ERR_FAIL_COND_V_MSG(render && storage && info.format != texture_format(*state, p_format.format, false).format, {},
			"A physically promoted storage texture cannot also be used as an attachment of the logical format.");
	ERR_FAIL_COND_V_MSG(dimension == WGPUTextureViewDimension_Undefined || p_format.is_subsampled || (render && !info.renderable) ||
					(render && backing->dimension != WGPUTextureDimension_2D) || (samples != 1 && (samples != 4 || !info.multisample || storage || atomic || p_format.mipmaps != 1 || p_format.array_layers != 1 || dimension != WGPUTextureViewDimension_2D)),
			{}, "Unsupported WebGPU texture dimensions, sample count, or attachment usage.");
	if (info.compressed && backing->dimension != WGPUTextureDimension_2D) {
		ERR_FAIL_V_MSG({}, "This backend currently supports only 2D-family block-compressed textures.");
	}
	if (p_format.usage_bits & (TEXTURE_USAGE_SAMPLING_BIT | TEXTURE_USAGE_INPUT_ATTACHMENT_BIT)) {
		backing->usage |= WGPUTextureUsage_TextureBinding;
	}
	if (p_format.usage_bits & TEXTURE_USAGE_CAN_COPY_FROM_BIT) {
		backing->usage |= WGPUTextureUsage_CopySrc;
	}
	if (p_format.usage_bits & (TEXTURE_USAGE_CAN_UPDATE_BIT | TEXTURE_USAGE_CAN_COPY_TO_BIT)) {
		backing->usage |= WGPUTextureUsage_CopyDst;
	}
	if (render) {
		backing->usage |= WGPUTextureUsage_RenderAttachment;
	}
	if (samples == 1 && backing->dimension == WGPUTextureDimension_2D && info.renderable && !info.depth && (backing->usage & WGPUTextureUsage_CopyDst) &&
			(!storage || info.format == texture_format(*state, p_format.format, false).format)) {
		backing->usage |= WGPUTextureUsage_RenderAttachment; // Also supports typed clears of non-framebuffer textures.
	}
	if (storage) {
		backing->usage |= WGPUTextureUsage_StorageBinding;
	}
	if (samples > 1) {
		ERR_FAIL_COND_V(!render, {});
		// RD uses CAN_COPY_FROM on MSAA resources to permit resolves, not buffer copies.
		backing->usage &= ~(WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst);
	}
	auto add_srgb_partner = [&](WGPUTextureFormat p_candidate) {
		if (p_candidate != WGPUTextureFormat_Undefined && p_candidate != info.format &&
				std::find(backing->view_formats.begin(), backing->view_formats.end(), p_candidate) == backing->view_formats.end()) {
			backing->view_formats.push_back(p_candidate);
		}
	};
	for (DataFormat format : p_format.shareable_formats) {
		TextureFormatInfo alias = texture_format(*state, format, false);
		if (alias.valid() && alias.format == srgb_partner(info.format) && alias.encoding == info.encoding) {
			add_srgb_partner(alias.format);
		}
	}
	// Texture2DRD retroactively adds sRGB sharing via _texture_ensure_shareable_format().
	if ((backing->usage & WGPUTextureUsage_TextureBinding) && info.encoding == TexelEncoding::COPY) {
		add_srgb_partner(srgb_partner(info.format));
	}
	TextureFormatInfo primary_format = info;
	if (p_view.format != DATA_FORMAT_MAX && p_view.format != p_format.format) {
		primary_format = view_format(*state, *backing, p_view);
		ERR_FAIL_COND_V_MSG(!primary_format.valid() || render || storage || atomic, {}, "The requested primary WebGPU view format is not compatible with this texture's usage.");
	}
	if (atomic) {
		// The current plan has a single buffer for all atomic operations. Do not
		// create a stale mirror texture; 2B-2 must add an explicit handoff for mixed uses.
		ERR_FAIL_COND_V_MSG(p_format.mipmaps != 1 || (backing->usage & ~(WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst)) || !create_atomic_backing(*state, *backing), {},
				"Atomic WebGPU images currently require single-mip R32i/R32ui storage-only backing.");
	} else {
		WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
		desc.usage = backing->usage;
		desc.dimension = backing->dimension;
		desc.size = { p_format.width, p_format.height, desc.dimension == WGPUTextureDimension_3D ? p_format.depth : p_format.array_layers };
		desc.format = info.format;
		desc.mipLevelCount = p_format.mipmaps;
		desc.sampleCount = samples;
		desc.viewFormats = backing->view_formats.data();
		desc.viewFormatCount = backing->view_formats.size();
		ERR_FAIL_COND_V(!desc.usage, {});
		backing->texture.reset(wgpuDeviceCreateTexture(state->device.get(), &desc));
		ERR_FAIL_COND_V(!backing->texture, {});
	}
	auto texture = std::make_shared<TextureResource>();
	texture->backing = std::move(backing);
	texture->format = primary_format;
	texture->dimension = dimension;
	texture->mip_count = p_format.mipmaps;
	texture->layer_count = p_format.array_layers;
	ERR_FAIL_COND_V(texture->initialize_views() != OK, {});
	return resource_id<TextureID>(std::move(texture));
}

RDD::TextureID RenderingDeviceDriverWebGPU::texture_create_from_extension(uint64_t p_native_texture, TextureType p_type, DataFormat p_format, uint32_t p_array_layers, bool, uint32_t p_mipmaps) {
	state->assert_owner();
	WGPUTexture native = reinterpret_cast<WGPUTexture>(p_native_texture);
	ERR_FAIL_NULL_V(native, {});
	const WGPUTextureUsage usage = wgpuTextureGetUsage(native);
	const auto info = texture_format(*state, p_format, (usage & WGPUTextureUsage_StorageBinding) != 0);
	const auto dim = view_dimension(p_type);
	ERR_FAIL_COND_V(!info.valid() || dim == WGPUTextureViewDimension_Undefined || wgpuTextureGetFormat(native) != info.format ||
					wgpuTextureGetDimension(native) != base_dimension(p_type) || p_mipmaps > wgpuTextureGetMipLevelCount(native) ||
					p_array_layers > wgpuTextureGetDepthOrArrayLayers(native),
			{});
	auto backing = std::make_shared<TextureBacking>();
	wgpuTextureAddRef(native);
	backing->texture.reset(native);
	backing->dimension = base_dimension(p_type);
	backing->usage = usage;
	backing->storage = info;
	backing->desc.format = p_format;
	backing->desc.texture_type = p_type;
	backing->desc.width = wgpuTextureGetWidth(native);
	backing->desc.height = wgpuTextureGetHeight(native);
	backing->desc.depth = p_type == TEXTURE_TYPE_3D ? wgpuTextureGetDepthOrArrayLayers(native) : 1;
	backing->desc.array_layers = p_array_layers;
	backing->desc.mipmaps = p_mipmaps;
	const uint32_t samples = wgpuTextureGetSampleCount(native);
	backing->desc.samples = samples == 4 ? TEXTURE_SAMPLES_4 : TEXTURE_SAMPLES_1;
	auto texture = std::make_shared<TextureResource>();
	texture->backing = std::move(backing);
	texture->format = info;
	texture->dimension = dim;
	texture->mip_count = p_mipmaps;
	texture->layer_count = p_array_layers;
	ERR_FAIL_COND_V(texture->initialize_views() != OK, {});
	return resource_id<TextureID>(std::move(texture));
}

RDD::TextureID RenderingDeviceDriverWebGPU::texture_create_shared(TextureID p_texture, const TextureView &p_view) {
	state->assert_owner();
	const auto &src = *resource<TextureResource>(p_texture);
	auto texture = std::make_shared<TextureResource>();
	texture->backing = src.backing;
	texture->format = view_format(*state, *src.backing, p_view);
	ERR_FAIL_COND_V_MSG(!texture->format.valid() || !src.backing->texture, {}, "No compatible WebGPU view for the requested format/swizzle.");
	texture->dimension = src.dimension;
	texture->mip_count = src.mip_count;
	texture->layer_count = src.layer_count;
	ERR_FAIL_COND_V(texture->initialize_views() != OK, {});
	return resource_id<TextureID>(std::move(texture));
}

RDD::TextureID RenderingDeviceDriverWebGPU::texture_create_shared_from_slice(TextureID p_texture, const TextureView &p_view,
		TextureSliceType p_type, uint32_t p_layer, uint32_t p_layers, uint32_t p_mip, uint32_t p_mips) {
	state->assert_owner();
	const auto &src = *resource<TextureResource>(p_texture);
	constexpr WGPUTextureViewDimension dims[] = { WGPUTextureViewDimension_2D, WGPUTextureViewDimension_Cube, WGPUTextureViewDimension_3D, WGPUTextureViewDimension_2DArray };
	DEV_ASSERT(p_type < TEXTURE_SLICE_MAX && p_mip + p_mips <= src.mip_count);
	ERR_FAIL_COND_V_MSG(!src.backing->texture || (src.dimension == WGPUTextureViewDimension_3D && p_type != TEXTURE_SLICE_3D), {}, "A 3D texture cannot be aliased as a 2D WebGPU view.");
	auto texture = std::make_shared<TextureResource>();
	texture->backing = src.backing;
	texture->format = view_format(*state, *src.backing, p_view);
	ERR_FAIL_COND_V(!texture->format.valid(), {});
	texture->dimension = dims[p_type];
	texture->base_mip = p_mip;
	texture->mip_count = p_mips;
	texture->base_layer = p_type == TEXTURE_SLICE_3D ? 0 : p_layer;
	texture->layer_count = p_type == TEXTURE_SLICE_CUBEMAP ? CUBE_LAYERS : p_layers;
	ERR_FAIL_COND_V(texture->initialize_views() != OK, {});
	return resource_id<TextureID>(std::move(texture));
}

void RenderingDeviceDriverWebGPU::texture_free(TextureID p_texture) {
	state->assert_owner();
	free_id<TextureResource>(p_texture);
}

uint64_t RenderingDeviceDriverWebGPU::texture_get_allocation_size(TextureID p_texture) {
	const auto &tex = *resource<TextureResource>(p_texture);
	uint64_t bytes = tex.backing->atomic_size;
	for (uint32_t mip = 0; !tex.backing->atomic_buffer && mip < tex.mip_count; ++mip) {
		bytes += copy_layout(tex.format, tex.extent(mip, tex.layer_count), true, false).size() << tex.backing->desc.samples;
	}
	return bytes;
}

void RenderingDeviceDriverWebGPU::texture_get_copyable_layout(TextureID p_texture, const TextureSubresource &p_subresource, TextureCopyableLayout *r_layout) {
	const auto &tex = *resource<TextureResource>(p_texture);
	auto format = tex.format;
	if (p_subresource.aspect == TEXTURE_ASPECT_STENCIL) {
		format.logical_bytes = format.stencil ? 1 : 0;
	}
	const auto layout = copy_layout(format, tex.extent(p_subresource.mipmap, 1), false, true);
	*r_layout = { layout.size(), layout.row_pitch };
}

Vector<uint8_t> RenderingDeviceDriverWebGPU::texture_get_data(TextureID, uint32_t) {
	ERR_FAIL_V_MSG({}, "Synchronous WebGPU texture readback cannot wait on the browser owner thread; use an asynchronous readback ticket.");
}

BitField<RDD::TextureUsageBits> RenderingDeviceDriverWebGPU::texture_get_usages_supported_by_format(DataFormat p_format, bool p_cpu_readable) {
	const TextureFormatInfo fmt = texture_format(*state, p_format, false);
	if (!fmt.valid() || (p_cpu_readable && !fmt.logical_bytes)) {
		return {};
	}
	BitField<TextureUsageBits> usages = TEXTURE_USAGE_SAMPLING_BIT;
	if (fmt.logical_bytes) {
		usages.set_flag(TEXTURE_USAGE_CPU_READ_BIT | TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		if (!fmt.depth) {
			usages.set_flag(TEXTURE_USAGE_CAN_UPDATE_BIT | TEXTURE_USAGE_CAN_COPY_TO_BIT);
		}
	}
	if (fmt.renderable) {
		usages.set_flag(fmt.depth || fmt.stencil ? TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);
		usages.set_flag(TEXTURE_USAGE_TRANSIENT_BIT);
		if (fmt.depth) {
			usages.set_flag(TEXTURE_USAGE_DEPTH_RESOLVE_ATTACHMENT_BIT | TEXTURE_USAGE_CAN_COPY_FROM_BIT | TEXTURE_USAGE_CAN_COPY_TO_BIT);
		}
	}
	if (texture_format(*state, p_format, true).valid()) {
		usages.set_flag(TEXTURE_USAGE_STORAGE_BIT);
	}
	if (p_format == DATA_FORMAT_R32_SINT || p_format == DATA_FORMAT_R32_UINT) {
		usages.set_flag(TEXTURE_USAGE_STORAGE_ATOMIC_BIT);
	}
	return usages;
}

bool RenderingDeviceDriverWebGPU::texture_can_make_shared_with_format(TextureID p_texture, DataFormat p_format, bool &r_raw_reinterpretation) {
	const auto &tex = *resource<TextureResource>(p_texture);
	TextureView view;
	view.format = p_format;
	r_raw_reinterpretation = false;
	return tex.backing->texture && view_format(*state, *tex.backing, view).valid();
}
