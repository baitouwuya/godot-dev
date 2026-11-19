#include "webgpu_formats.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace godot::webgpu {
namespace {

using F = spv::ImageFormat;
using DF = Rdc::DataFormat;

bool is_identity(Rdc::TextureSwizzle p_swizzle, Rdc::TextureSwizzle p_component) {
	return p_swizzle == p_component || p_swizzle == Rdc::TEXTURE_SWIZZLE_IDENTITY;
}

struct FormatRow {
	DF logical;
	WGPUTextureFormat physical;
	F storage;
	SampleType sample;
	uint8_t bytes;
	bool render;
	bool msaa;
	bool srgb = false;
	uint8_t expansion_bytes = 0;
	uint32_t one = 0;
};

#define COLOR(m_rd, m_wg, m_spv, m_type, m_bytes, m_rt, m_ms) \
	{ Rdc::DATA_FORMAT_##m_rd, WGPUTextureFormat_##m_wg, F::m_spv, SampleType::m_type, m_bytes, m_rt, m_ms }
#define SRGB(m_rd, m_wg) \
	{ Rdc::DATA_FORMAT_##m_rd, WGPUTextureFormat_##m_wg, F::Unknown, SampleType::FLOAT, 4, true, true, true }
#define RGB(m_rd, m_wg, m_type, m_bytes, m_one, m_srgb) \
	{ Rdc::DATA_FORMAT_##m_rd, WGPUTextureFormat_##m_wg, F::Unknown, SampleType::m_type, m_bytes * 3, false, false, m_srgb, m_bytes * 4, m_one }
constexpr FormatRow colors[] = {
	COLOR(R8_UNORM, R8Unorm, R8, FLOAT, 1, true, true),
	COLOR(R8_SNORM, R8Snorm, R8Snorm, FLOAT, 1, false, false),
	COLOR(R8_UINT, R8Uint, R8ui, UINT, 1, true, true),
	COLOR(R8_SINT, R8Sint, R8i, SINT, 1, true, true),
	COLOR(R8G8_UNORM, RG8Unorm, Rg8, FLOAT, 2, true, true),
	COLOR(R8G8_SNORM, RG8Snorm, Rg8Snorm, FLOAT, 2, false, false),
	COLOR(R8G8_UINT, RG8Uint, Rg8ui, UINT, 2, true, true),
	COLOR(R8G8_SINT, RG8Sint, Rg8i, SINT, 2, true, true),
	COLOR(R8G8B8A8_UNORM, RGBA8Unorm, Rgba8, FLOAT, 4, true, true),
	COLOR(R8G8B8A8_SNORM, RGBA8Snorm, Rgba8Snorm, FLOAT, 4, false, false),
	COLOR(R8G8B8A8_UINT, RGBA8Uint, Rgba8ui, UINT, 4, true, true),
	COLOR(R8G8B8A8_SINT, RGBA8Sint, Rgba8i, SINT, 4, true, true),
	SRGB(R8G8B8A8_SRGB, RGBA8UnormSrgb),
	COLOR(B8G8R8A8_UNORM, BGRA8Unorm, Unknown, FLOAT, 4, true, true),
	SRGB(B8G8R8A8_SRGB, BGRA8UnormSrgb),
	RGB(R8G8B8_UNORM, RGBA8Unorm, FLOAT, 1, 255, false),
	RGB(R8G8B8_SRGB, RGBA8UnormSrgb, FLOAT, 1, 255, true),
	RGB(R8G8B8_SNORM, RGBA8Snorm, FLOAT, 1, 127, false),
	RGB(R8G8B8_UINT, RGBA8Uint, UINT, 1, 1, false),
	RGB(R8G8B8_SINT, RGBA8Sint, SINT, 1, 1, false),
	COLOR(R16_UNORM, Undefined, R16, FLOAT, 2, false, false),
	COLOR(R16_SNORM, Undefined, R16Snorm, FLOAT, 2, false, false),
	COLOR(R16_UINT, R16Uint, R16ui, UINT, 2, true, true),
	COLOR(R16_SINT, R16Sint, R16i, SINT, 2, true, true),
	COLOR(R16_SFLOAT, R16Float, R16f, FLOAT, 2, true, true),
	COLOR(R16G16_UNORM, Undefined, Rg16, FLOAT, 4, false, false),
	COLOR(R16G16_SNORM, Undefined, Rg16Snorm, FLOAT, 4, false, false),
	COLOR(R16G16_UINT, RG16Uint, Rg16ui, UINT, 4, true, true),
	COLOR(R16G16_SINT, RG16Sint, Rg16i, SINT, 4, true, true),
	COLOR(R16G16_SFLOAT, RG16Float, Rg16f, FLOAT, 4, true, true),
	COLOR(R16G16B16A16_UNORM, Undefined, Rgba16, FLOAT, 8, false, false),
	COLOR(R16G16B16A16_SNORM, Undefined, Rgba16Snorm, FLOAT, 8, false, false),
	COLOR(R16G16B16A16_UINT, RGBA16Uint, Rgba16ui, UINT, 8, true, true),
	COLOR(R16G16B16A16_SINT, RGBA16Sint, Rgba16i, SINT, 8, true, true),
	COLOR(R16G16B16A16_SFLOAT, RGBA16Float, Rgba16f, FLOAT, 8, true, true),
	RGB(R16G16B16_SFLOAT, RGBA16Float, FLOAT, 2, 0x3c00, false),
	RGB(R16G16B16_UINT, RGBA16Uint, UINT, 2, 1, false),
	RGB(R16G16B16_SINT, RGBA16Sint, SINT, 2, 1, false),
	COLOR(R32_UINT, R32Uint, R32ui, UINT, 4, true, true),
	COLOR(R32_SINT, R32Sint, R32i, SINT, 4, true, true),
	COLOR(R32_SFLOAT, R32Float, R32f, UNFILTERABLE_FLOAT, 4, true, true),
	COLOR(R32G32_UINT, RG32Uint, Rg32ui, UINT, 8, true, false),
	COLOR(R32G32_SINT, RG32Sint, Rg32i, SINT, 8, true, false),
	COLOR(R32G32_SFLOAT, RG32Float, Rg32f, UNFILTERABLE_FLOAT, 8, true, false),
	COLOR(R32G32B32A32_UINT, RGBA32Uint, Rgba32ui, UINT, 16, true, false),
	COLOR(R32G32B32A32_SINT, RGBA32Sint, Rgba32i, SINT, 16, true, false),
	COLOR(R32G32B32A32_SFLOAT, RGBA32Float, Rgba32f, UNFILTERABLE_FLOAT, 16, true, false),
	RGB(R32G32B32_SFLOAT, RGBA32Float, UNFILTERABLE_FLOAT, 4, 0x3f800000, false),
	RGB(R32G32B32_UINT, RGBA32Uint, UINT, 4, 1, false),
	RGB(R32G32B32_SINT, RGBA32Sint, SINT, 4, 1, false),
	COLOR(A2B10G10R10_UNORM_PACK32, RGB10A2Unorm, Unknown, FLOAT, 4, true, true),
	COLOR(A2B10G10R10_UINT_PACK32, RGB10A2Uint, Unknown, UINT, 4, true, true),
	COLOR(B10G11R11_UFLOAT_PACK32, RG11B10Ufloat, Unknown, FLOAT, 4, true, true),
	COLOR(E5B9G9R9_UFLOAT_PACK32, RGB9E5Ufloat, Unknown, FLOAT, 4, false, false),
};
#undef RGB
#undef SRGB
#undef COLOR

struct BlockFormat {
	DF logical;
	WGPUTextureFormat physical;
	WGPUFeatureName feature;
	uint8_t bytes;
	uint8_t width;
	uint8_t height;
	bool srgb;
};

#define BLOCK(m_rd, m_wg, m_family, m_bytes, m_srgb) \
	{ Rdc::DATA_FORMAT_##m_rd##_BLOCK, WGPUTextureFormat_##m_wg, WGPUFeatureName_TextureCompression##m_family, m_bytes, 4, 4, m_srgb }
#define ASTC(m_size, m_w, m_h) \
	{ Rdc::DATA_FORMAT_ASTC_##m_size##_UNORM_BLOCK, WGPUTextureFormat_ASTC##m_size##Unorm, WGPUFeatureName_TextureCompressionASTC, 16, m_w, m_h, false }, \
	{ \
		Rdc::DATA_FORMAT_ASTC_##m_size##_SRGB_BLOCK, WGPUTextureFormat_ASTC##m_size##UnormSrgb, WGPUFeatureName_TextureCompressionASTC, 16, m_w, m_h, true \
	}
constexpr BlockFormat blocks[] = {
	// BC1 RGB is not interchangeable with BC1 RGBA when the fourth selector is used.
	BLOCK(BC1_RGBA_UNORM, BC1RGBAUnorm, BC, 8, false),
	BLOCK(BC1_RGBA_SRGB, BC1RGBAUnormSrgb, BC, 8, true),
	BLOCK(BC2_UNORM, BC2RGBAUnorm, BC, 16, false),
	BLOCK(BC2_SRGB, BC2RGBAUnormSrgb, BC, 16, true),
	BLOCK(BC3_UNORM, BC3RGBAUnorm, BC, 16, false),
	BLOCK(BC3_SRGB, BC3RGBAUnormSrgb, BC, 16, true),
	BLOCK(BC4_UNORM, BC4RUnorm, BC, 8, false),
	BLOCK(BC4_SNORM, BC4RSnorm, BC, 8, false),
	BLOCK(BC5_UNORM, BC5RGUnorm, BC, 16, false),
	BLOCK(BC5_SNORM, BC5RGSnorm, BC, 16, false),
	BLOCK(BC6H_UFLOAT, BC6HRGBUfloat, BC, 16, false),
	BLOCK(BC6H_SFLOAT, BC6HRGBFloat, BC, 16, false),
	BLOCK(BC7_UNORM, BC7RGBAUnorm, BC, 16, false),
	BLOCK(BC7_SRGB, BC7RGBAUnormSrgb, BC, 16, true),
	BLOCK(ETC2_R8G8B8_UNORM, ETC2RGB8Unorm, ETC2, 8, false),
	BLOCK(ETC2_R8G8B8_SRGB, ETC2RGB8UnormSrgb, ETC2, 8, true),
	BLOCK(ETC2_R8G8B8A1_UNORM, ETC2RGB8A1Unorm, ETC2, 8, false),
	BLOCK(ETC2_R8G8B8A1_SRGB, ETC2RGB8A1UnormSrgb, ETC2, 8, true),
	BLOCK(ETC2_R8G8B8A8_UNORM, ETC2RGBA8Unorm, ETC2, 16, false),
	BLOCK(ETC2_R8G8B8A8_SRGB, ETC2RGBA8UnormSrgb, ETC2, 16, true),
	BLOCK(EAC_R11_UNORM, EACR11Unorm, ETC2, 8, false),
	BLOCK(EAC_R11_SNORM, EACR11Snorm, ETC2, 8, false),
	BLOCK(EAC_R11G11_UNORM, EACRG11Unorm, ETC2, 16, false),
	BLOCK(EAC_R11G11_SNORM, EACRG11Snorm, ETC2, 16, false),
	ASTC(4x4, 4, 4),
	ASTC(5x4, 5, 4),
	ASTC(5x5, 5, 5),
	ASTC(6x5, 6, 5),
	ASTC(6x6, 6, 6),
	ASTC(8x5, 8, 5),
	ASTC(8x6, 8, 6),
	ASTC(8x8, 8, 8),
	ASTC(10x5, 10, 5),
	ASTC(10x6, 10, 6),
	ASTC(10x8, 10, 8),
	ASTC(10x10, 10, 10),
	ASTC(12x10, 12, 10),
	ASTC(12x12, 12, 12),
};
#undef BLOCK
#undef ASTC

template <class T>
T load(const uint8_t *p_bytes) {
	T value;
	std::memcpy(&value, p_bytes, sizeof(T));
	return value;
}

template <class T>
void store(uint8_t *r_bytes, T p_value) {
	std::memcpy(r_bytes, &p_value, sizeof(T));
}

void convert_texel(const TextureFormatInfo &p_format, const uint8_t *p_src, uint8_t *r_dst, bool p_pack) {
	using E = TexelEncoding;
	if (p_format.encoding == E::UNORM16_FLOAT || p_format.encoding == E::SNORM16_FLOAT) {
		const bool signed_norm = p_format.encoding == E::SNORM16_FLOAT;
		const auto channels = p_format.logical_bytes / sizeof(uint16_t);
		const float max = signed_norm ? INT16_MAX : UINT16_MAX;
		if (p_pack) {
			for (size_t c = 0; c < 4; ++c) {
				float value = c == 3 ? 1.0f : 0.0f;
				if (c < channels) {
					value = signed_norm ? MAX(-1.0f, load<int16_t>(p_src + c * sizeof(uint16_t)) / max) : load<uint16_t>(p_src + c * sizeof(uint16_t)) / max;
				}
				store(r_dst + c * sizeof(float), value);
			}
		} else {
			for (size_t c = 0; c < channels; ++c) {
				float value = load<float>(p_src + c * sizeof(float));
				value = std::isnan(value) ? 0.0f : std::clamp(value, signed_norm ? -1.0f : 0.0f, 1.0f);
				const int32_t q = std::lround(value * max);
				store(r_dst + c * sizeof(uint16_t), static_cast<uint16_t>(q));
			}
		}
		return;
	}
	if (!p_pack) {
		if (p_format.encoding == E::LUMINANCE_ALPHA) {
			r_dst[0] = p_src[0];
			r_dst[1] = p_src[3];
		} else {
			std::memcpy(r_dst, p_src, p_format.logical_bytes);
		}
		return;
	}
	if (p_format.encoding == E::LUMINANCE || p_format.encoding == E::LUMINANCE_ALPHA) {
		r_dst[0] = r_dst[1] = r_dst[2] = p_src[0];
		r_dst[3] = p_format.encoding == E::LUMINANCE_ALPHA ? p_src[1] : UINT8_MAX;
		return;
	}
	const uint8_t component_bytes = p_format.physical_bytes / 4;
	std::memcpy(r_dst, p_src, p_format.logical_bytes);
	std::memset(r_dst + p_format.logical_bytes, 0, p_format.physical_bytes - p_format.logical_bytes);
	std::memcpy(r_dst + 3 * component_bytes, &p_format.alpha_one, component_bytes); // WASM is little endian.
}

void convert_rows(const TextureFormatInfo &p_format, WGPUExtent3D p_extent, const uint8_t *p_src, uint32_t p_src_pitch,
		uint8_t *r_dst, uint32_t p_dst_pitch, bool p_pack) {
	const auto layout = copy_layout(p_format, p_extent, false, false);
	const auto src_stride = p_pack ? p_format.logical_bytes : p_format.physical_bytes;
	const auto dst_stride = p_pack ? p_format.physical_bytes : p_format.logical_bytes;
	DEV_ASSERT(p_src_pitch >= (p_pack ? layout.row_size : copy_layout(p_format, p_extent, true, false).row_size));
	for (uint64_t y = 0; y < uint64_t(layout.rows) * layout.images; ++y) {
		const auto *src = p_src + y * p_src_pitch;
		auto *dst = r_dst + y * p_dst_pitch;
		if (p_format.encoding == TexelEncoding::COPY) {
			std::memcpy(dst, src, layout.row_size);
		} else {
			for (uint32_t x = 0; x < p_extent.width; ++x) {
				convert_texel(p_format, src + x * src_stride, dst + x * dst_stride, p_pack);
			}
		}
	}
}

} // namespace

WGPUTextureFormat storage_texture_format(F p_format) {
	if (p_format != F::Unknown) {
		for (const FormatRow &row : colors) {
			if (row.storage == p_format) {
				return row.physical;
			}
		}
	}
	return WGPUTextureFormat_Undefined;
}

TextureFormatInfo texture_format(const DeviceState &p_device, DF p_format, bool p_storage) {
	TextureFormatInfo info;
	info.logical_format = p_format;
	for (const FormatRow &row : colors) {
		if (row.logical != p_format) {
			continue;
		}
		info.format = row.physical;
		info.storage_format = row.storage;
		info.sample_type = row.sample;
		info.logical_bytes = row.bytes;
		info.physical_bytes = row.expansion_bytes ? row.expansion_bytes : row.bytes;
		info.encoding = row.expansion_bytes ? TexelEncoding::CHANNELS : TexelEncoding::COPY;
		info.alpha_one = row.one;
		info.renderable = row.render;
		info.multisample = row.msaa;
		info.srgb = row.srgb;
		const auto *rule = storage_promotion(row.storage);
		const bool normalized_16 = rule && rule->precision == 16;
		if (p_storage && row.storage == F::Unknown) {
			return {};
		}
		if (rule && (p_storage || normalized_16)) {
			info.format = storage_texture_format(rule->physical);
			info.physical_bytes = normalized_16 ? 4 * sizeof(float) : row.bytes / rule->channels * 4;
			info.encoding = !normalized_16 ? TexelEncoding::CHANNELS : rule->conversion == FormatConversion::SNORM ? TexelEncoding::SNORM16_FLOAT
																												   : TexelEncoding::UNORM16_FLOAT;
			info.alpha_one = (row.storage == F::R8 || row.storage == F::Rg8) ? UINT8_MAX : (row.storage == F::R8Snorm || row.storage == F::Rg8Snorm) ? INT8_MAX
					: (row.storage == F::R16f || row.storage == F::Rg16f)																			 ? 0x3c00
																																					 : 1;
			if (normalized_16) {
				info.sample_type = SampleType::UNFILTERABLE_FLOAT;
				info.renderable = false; // A color attachment would not quantize to UNORM16/SNORM16.
				info.multisample = false;
			}
		}
		if (info.sample_type == SampleType::UNFILTERABLE_FLOAT && p_device.has(WGPUFeatureName_Float32Filterable)) {
			info.sample_type = SampleType::FLOAT;
		}
		if (row.physical == WGPUTextureFormat_RG11B10Ufloat && !p_device.has(WGPUFeatureName_RG11B10UfloatRenderable)) {
			info.renderable = info.multisample = false;
		}
		return info;
	}
	if (p_storage) {
		return {};
	}
	for (const BlockFormat &row : blocks) {
		if (row.logical == p_format && p_device.has(row.feature)) {
			info.format = row.physical;
			info.sample_type = SampleType::FLOAT;
			info.block_width = row.width;
			info.block_height = row.height;
			info.logical_bytes = info.physical_bytes = row.bytes;
			info.srgb = row.srgb;
			info.compressed = true;
			return info;
		}
	}
	switch (p_format) {
		case Rdc::DATA_FORMAT_D16_UNORM:
			info.format = WGPUTextureFormat_Depth16Unorm;
			info.logical_bytes = sizeof(uint16_t);
			break;
		case Rdc::DATA_FORMAT_D32_SFLOAT:
			info.format = WGPUTextureFormat_Depth32Float;
			info.logical_bytes = sizeof(float);
			break;
		case Rdc::DATA_FORMAT_X8_D24_UNORM_PACK32:
			info.format = WGPUTextureFormat_Depth24Plus;
			break;
		case Rdc::DATA_FORMAT_D24_UNORM_S8_UINT:
			info.format = WGPUTextureFormat_Depth24PlusStencil8;
			info.stencil = true;
			break;
		case Rdc::DATA_FORMAT_D32_SFLOAT_S8_UINT:
			if (!p_device.has(WGPUFeatureName_Depth32FloatStencil8)) {
				return {};
			}
			info.format = WGPUTextureFormat_Depth32FloatStencil8;
			info.stencil = true;
			info.logical_bytes = sizeof(float);
			break;
		case Rdc::DATA_FORMAT_S8_UINT:
			info.format = WGPUTextureFormat_Stencil8;
			info.stencil = true;
			info.logical_bytes = 1;
			break;
		default:
			return {};
	}
	info.depth = p_format != Rdc::DATA_FORMAT_S8_UINT;
	info.physical_bytes = info.logical_bytes;
	info.sample_type = info.depth ? SampleType::DEPTH : SampleType::UINT;
	info.renderable = info.multisample = true;
	return info;
}

bool texture_swizzle_is_identity(const RDD::TextureView &p_view) {
	const auto &v = p_view;
	return is_identity(v.swizzle_r, Rdc::TEXTURE_SWIZZLE_R) && is_identity(v.swizzle_g, Rdc::TEXTURE_SWIZZLE_G) &&
			is_identity(v.swizzle_b, Rdc::TEXTURE_SWIZZLE_B) && is_identity(v.swizzle_a, Rdc::TEXTURE_SWIZZLE_A);
}

bool bake_texture_swizzle(TextureFormatInfo &r_format, const RDD::TextureView &p_view, bool p_storage) {
	const auto &v = p_view;
	if (texture_swizzle_is_identity(v)) {
		return true;
	}
	// Native 1/2/3-channel color formats synthesize missing RGB as 0 and A as 1 without mutating backing.
	if (r_format.encoding == TexelEncoding::COPY && is_identity(v.swizzle_r, Rdc::TEXTURE_SWIZZLE_R) && v.swizzle_a == Rdc::TEXTURE_SWIZZLE_ONE) {
		if (v.swizzle_b == Rdc::TEXTURE_SWIZZLE_ZERO) {
			switch (r_format.format) {
				case WGPUTextureFormat_R8Unorm:
				case WGPUTextureFormat_R8Snorm:
				case WGPUTextureFormat_R8Uint:
				case WGPUTextureFormat_R8Sint:
				case WGPUTextureFormat_R16Uint:
				case WGPUTextureFormat_R16Sint:
				case WGPUTextureFormat_R16Float:
				case WGPUTextureFormat_R32Uint:
				case WGPUTextureFormat_R32Sint:
				case WGPUTextureFormat_R32Float:
				case WGPUTextureFormat_BC4RUnorm:
				case WGPUTextureFormat_BC4RSnorm:
				case WGPUTextureFormat_EACR11Unorm:
				case WGPUTextureFormat_EACR11Snorm:
					return v.swizzle_g == Rdc::TEXTURE_SWIZZLE_ZERO;
				case WGPUTextureFormat_RG8Unorm:
				case WGPUTextureFormat_RG8Snorm:
				case WGPUTextureFormat_RG8Uint:
				case WGPUTextureFormat_RG8Sint:
				case WGPUTextureFormat_RG16Uint:
				case WGPUTextureFormat_RG16Sint:
				case WGPUTextureFormat_RG16Float:
				case WGPUTextureFormat_RG32Uint:
				case WGPUTextureFormat_RG32Sint:
				case WGPUTextureFormat_RG32Float:
				case WGPUTextureFormat_BC5RGUnorm:
				case WGPUTextureFormat_BC5RGSnorm:
				case WGPUTextureFormat_EACRG11Unorm:
				case WGPUTextureFormat_EACRG11Snorm:
					return is_identity(v.swizzle_g, Rdc::TEXTURE_SWIZZLE_G);
				default:
					break;
			}
		} else if (is_identity(v.swizzle_g, Rdc::TEXTURE_SWIZZLE_G) && is_identity(v.swizzle_b, Rdc::TEXTURE_SWIZZLE_B)) {
			switch (r_format.format) {
				case WGPUTextureFormat_RG11B10Ufloat:
				case WGPUTextureFormat_RGB9E5Ufloat:
				case WGPUTextureFormat_BC6HRGBUfloat:
				case WGPUTextureFormat_BC6HRGBFloat:
				case WGPUTextureFormat_ETC2RGB8Unorm:
				case WGPUTextureFormat_ETC2RGB8UnormSrgb:
					return true;
				default:
					break;
			}
		}
	}
	if (p_storage) {
		return false;
	}
	if ((r_format.encoding == TexelEncoding::UNORM16_FLOAT || r_format.encoding == TexelEncoding::SNORM16_FLOAT) &&
			is_identity(v.swizzle_r, Rdc::TEXTURE_SWIZZLE_R) && v.swizzle_a == Rdc::TEXTURE_SWIZZLE_ONE) {
		if (r_format.logical_bytes == 2) {
			return v.swizzle_g == Rdc::TEXTURE_SWIZZLE_ZERO && v.swizzle_b == Rdc::TEXTURE_SWIZZLE_ZERO;
		}
		if (r_format.logical_bytes == 4) {
			return is_identity(v.swizzle_g, Rdc::TEXTURE_SWIZZLE_G) && v.swizzle_b == Rdc::TEXTURE_SWIZZLE_ZERO;
		}
		if (r_format.logical_bytes == 6) {
			return is_identity(v.swizzle_g, Rdc::TEXTURE_SWIZZLE_G) && is_identity(v.swizzle_b, Rdc::TEXTURE_SWIZZLE_B);
		}
	}
	const bool gray = is_identity(v.swizzle_r, Rdc::TEXTURE_SWIZZLE_R) && v.swizzle_g == Rdc::TEXTURE_SWIZZLE_R && v.swizzle_b == Rdc::TEXTURE_SWIZZLE_R;
	if (gray && ((r_format.format == WGPUTextureFormat_R8Unorm && v.swizzle_a == Rdc::TEXTURE_SWIZZLE_ONE) || (r_format.format == WGPUTextureFormat_RG8Unorm && v.swizzle_a == Rdc::TEXTURE_SWIZZLE_G))) {
		r_format.encoding = r_format.logical_bytes == 1 ? TexelEncoding::LUMINANCE : TexelEncoding::LUMINANCE_ALPHA;
		r_format.format = WGPUTextureFormat_RGBA8Unorm;
		r_format.physical_bytes = 4;
		r_format.renderable = r_format.multisample = false;
		return true;
	}
	if (v.swizzle_a == Rdc::TEXTURE_SWIZZLE_ONE && is_identity(v.swizzle_r, Rdc::TEXTURE_SWIZZLE_R) &&
			is_identity(v.swizzle_g, Rdc::TEXTURE_SWIZZLE_G) && is_identity(v.swizzle_b, Rdc::TEXTURE_SWIZZLE_B) &&
			r_format.encoding == TexelEncoding::CHANNELS && r_format.logical_bytes * 4 == r_format.physical_bytes * 3) {
		return true; // RGB expansion already writes one in alpha.
	}
	return false;
}

WGPUTextureFormat srgb_partner(WGPUTextureFormat p_format) {
	switch (p_format) {
		case WGPUTextureFormat_RGBA8Unorm:
			return WGPUTextureFormat_RGBA8UnormSrgb;
		case WGPUTextureFormat_RGBA8UnormSrgb:
			return WGPUTextureFormat_RGBA8Unorm;
		case WGPUTextureFormat_BGRA8Unorm:
			return WGPUTextureFormat_BGRA8UnormSrgb;
		case WGPUTextureFormat_BGRA8UnormSrgb:
			return WGPUTextureFormat_BGRA8Unorm;
		default:
			break;
	}
	for (size_t i = 0; i < std::size(blocks); ++i) {
		if (blocks[i].physical == p_format) {
			const size_t paired = blocks[i].srgb ? i - 1 : i + 1;
			if (paired < std::size(blocks) && blocks[paired].srgb != blocks[i].srgb &&
					int(blocks[paired].physical) - int(p_format) == (blocks[i].srgb ? -1 : 1)) {
				return blocks[paired].physical;
			}
			break;
		}
	}
	return WGPUTextureFormat_Undefined;
}

WGPUTextureViewDimension shader_view_dimension(const FormatPlan &p_format) {
	switch (static_cast<spv::Dim>(p_format.dimension)) {
		case spv::Dim::Dim1D:
			return p_format.arrayed ? WGPUTextureViewDimension_Undefined : WGPUTextureViewDimension_1D;
		case spv::Dim::Dim2D:
			return p_format.arrayed ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
		case spv::Dim::Dim3D:
			return WGPUTextureViewDimension_3D;
		case spv::Dim::Cube:
			return p_format.arrayed ? WGPUTextureViewDimension_CubeArray : WGPUTextureViewDimension_Cube;
		default:
			return WGPUTextureViewDimension_Undefined;
	}
}

WGPUCompareFunction compare_function(Rdc::CompareOperator p_compare) {
	constexpr WGPUCompareFunction table[] = { WGPUCompareFunction_Never, WGPUCompareFunction_Less, WGPUCompareFunction_Equal,
		WGPUCompareFunction_LessEqual, WGPUCompareFunction_Greater, WGPUCompareFunction_NotEqual, WGPUCompareFunction_GreaterEqual, WGPUCompareFunction_Always };
	DEV_ASSERT(p_compare < Rdc::COMPARE_OP_MAX);
	return table[p_compare];
}

WGPUVertexFormat vertex_data_format(DF p_format) {
#define VF(m_rd, m_wg) \
	case Rdc::DATA_FORMAT_##m_rd: \
		return WGPUVertexFormat_##m_wg
	switch (p_format) {
		VF(R32_SFLOAT, Float32);
		VF(R32G32_SFLOAT, Float32x2);
		VF(R32G32B32_SFLOAT, Float32x3);
		VF(R32G32B32A32_SFLOAT, Float32x4);
		VF(R16G16_SFLOAT, Float16x2);
		VF(R16G16B16A16_SFLOAT, Float16x4);
		VF(R32_UINT, Uint32);
		VF(R32G32_UINT, Uint32x2);
		VF(R32G32B32_UINT, Uint32x3);
		VF(R32G32B32A32_UINT, Uint32x4);
		VF(R32_SINT, Sint32);
		VF(R32G32_SINT, Sint32x2);
		VF(R32G32B32_SINT, Sint32x3);
		VF(R32G32B32A32_SINT, Sint32x4);
		VF(R8G8_UNORM, Unorm8x2);
		VF(R8G8B8A8_UNORM, Unorm8x4);
		VF(R8G8_SNORM, Snorm8x2);
		VF(R8G8B8A8_SNORM, Snorm8x4);
		VF(R8G8_UINT, Uint8x2);
		VF(R8G8B8A8_UINT, Uint8x4);
		VF(R8G8_SINT, Sint8x2);
		VF(R8G8B8A8_SINT, Sint8x4);
		VF(R16G16_UINT, Uint16x2);
		VF(R16G16B16A16_UINT, Uint16x4);
		VF(R16G16_SINT, Sint16x2);
		VF(R16G16B16A16_SINT, Sint16x4);
		VF(R16G16_UNORM, Unorm16x2);
		VF(R16G16B16A16_UNORM, Unorm16x4);
		VF(R16G16_SNORM, Snorm16x2);
		VF(R16G16B16A16_SNORM, Snorm16x4);
		VF(A2B10G10R10_UNORM_PACK32, Unorm10_10_10_2);
		default:
			return static_cast<WGPUVertexFormat>(0); // This ABI has no named invalid vertex format.
	}
#undef VF
}

CopyLayout copy_layout(const TextureFormatInfo &p_format, WGPUExtent3D p_extent, bool p_physical, bool p_aligned) {
	const uint32_t columns = (p_extent.width + p_format.block_width - 1) / p_format.block_width;
	const uint32_t rows = (p_extent.height + p_format.block_height - 1) / p_format.block_height;
	const uint32_t bytes = columns * (p_physical ? p_format.physical_bytes : p_format.logical_bytes);
	return { bytes, static_cast<uint32_t>(p_aligned ? align_up(bytes, COPY_ROW_ALIGNMENT) : bytes), rows, p_extent.depthOrArrayLayers };
}

void pack_texture_rows(const TextureFormatInfo &p_format, WGPUExtent3D p_extent, const uint8_t *p_src, uint32_t p_src_pitch, uint8_t *p_dst, uint32_t p_dst_pitch) {
	convert_rows(p_format, p_extent, p_src, p_src_pitch, p_dst, p_dst_pitch, true);
}

void unpack_texture_rows(const TextureFormatInfo &p_format, WGPUExtent3D p_extent, const uint8_t *p_src, uint32_t p_src_pitch, uint8_t *p_dst, uint32_t p_dst_pitch) {
	convert_rows(p_format, p_extent, p_src, p_src_pitch, p_dst, p_dst_pitch, false);
}

} // namespace godot::webgpu
