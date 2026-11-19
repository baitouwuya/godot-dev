#pragma once

#include "rendering_context_driver_webgpu.h"
#include "spirv/spirv_storage_formats.h"

#include "servers/rendering/rendering_device_driver.h"

namespace godot::webgpu {

using Rdc = RenderingDeviceCommons;

enum class TexelEncoding : uint8_t { COPY,
	CHANNELS,
	UNORM16_FLOAT,
	SNORM16_FLOAT,
	LUMINANCE,
	LUMINANCE_ALPHA };

struct TextureFormatInfo {
	WGPUTextureFormat format = WGPUTextureFormat_Undefined;
	spv::ImageFormat storage_format = spv::ImageFormat::Unknown; // Logical shader format.
	SampleType sample_type = SampleType::NONE;
	TexelEncoding encoding = TexelEncoding::COPY;
	uint8_t block_width = 1;
	uint8_t block_height = 1;
	uint8_t logical_bytes = 0;
	uint8_t physical_bytes = 0;
	uint32_t alpha_one = 0; // Native component bit-pattern, independent of byte width.
	bool depth = false;
	bool stencil = false;
	bool renderable = false;
	bool multisample = false;
	bool srgb = false;
	bool compressed = false;
	bool valid() const { return format != WGPUTextureFormat_Undefined; }
};

struct CopyLayout {
	uint32_t row_size = 0;
	uint32_t row_pitch = 0;
	uint32_t rows = 0;
	uint32_t images = 0;
	uint64_t size() const { return uint64_t(row_pitch) * rows * images; }
};

inline uint64_t align_up(uint64_t p_size, uint64_t p_align) {
	DEV_ASSERT(p_align && (p_align & (p_align - 1)) == 0);
	return (p_size + p_align - 1) & ~(p_align - 1);
}

TextureFormatInfo texture_format(const DeviceState &p_device, Rdc::DataFormat p_format, bool p_storage);
bool bake_texture_swizzle(TextureFormatInfo &r_format, const RDD::TextureView &p_view, bool p_storage);
WGPUTextureFormat storage_texture_format(spv::ImageFormat p_format);
WGPUTextureFormat srgb_partner(WGPUTextureFormat p_format);
WGPUTextureViewDimension shader_view_dimension(const FormatPlan &p_format);
WGPUCompareFunction compare_function(Rdc::CompareOperator p_compare);
WGPUVertexFormat vertex_data_format(Rdc::DataFormat p_format);
CopyLayout copy_layout(const TextureFormatInfo &p_format, WGPUExtent3D p_extent, bool p_physical, bool p_aligned);
void pack_texture_rows(const TextureFormatInfo &p_format, WGPUExtent3D p_extent, const uint8_t *p_src, uint32_t p_src_pitch, uint8_t *p_dst, uint32_t p_dst_pitch);
void unpack_texture_rows(const TextureFormatInfo &p_format, WGPUExtent3D p_extent, const uint8_t *p_src, uint32_t p_src_pitch, uint8_t *p_dst, uint32_t p_dst_pitch);

} // namespace godot::webgpu
