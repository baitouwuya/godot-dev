#pragma once

#include "../webgpu_shader_plan.h"

#include <spirv/unified1/spirv.hpp11>

namespace godot::webgpu {

struct StoragePromotion {
	spv::ImageFormat logical;
	spv::ImageFormat physical;
	uint8_t channels;
	uint8_t precision = 0;
	FormatConversion conversion = FormatConversion::NONE;
};

inline constexpr StoragePromotion storage_promotions[] = {
	{ spv::ImageFormat::R8, spv::ImageFormat::Rgba8, 1 },
	{ spv::ImageFormat::Rg8, spv::ImageFormat::Rgba8, 2 },
	{ spv::ImageFormat::R8Snorm, spv::ImageFormat::Rgba8Snorm, 1 },
	{ spv::ImageFormat::Rg8Snorm, spv::ImageFormat::Rgba8Snorm, 2 },
	{ spv::ImageFormat::R8i, spv::ImageFormat::Rgba8i, 1 },
	{ spv::ImageFormat::Rg8i, spv::ImageFormat::Rgba8i, 2 },
	{ spv::ImageFormat::R8ui, spv::ImageFormat::Rgba8ui, 1 },
	{ spv::ImageFormat::Rg8ui, spv::ImageFormat::Rgba8ui, 2 },
	{ spv::ImageFormat::R16f, spv::ImageFormat::Rgba16f, 1 },
	{ spv::ImageFormat::Rg16f, spv::ImageFormat::Rgba16f, 2 },
	{ spv::ImageFormat::R16i, spv::ImageFormat::Rgba16i, 1 },
	{ spv::ImageFormat::Rg16i, spv::ImageFormat::Rgba16i, 2 },
	{ spv::ImageFormat::R16ui, spv::ImageFormat::Rgba16ui, 1 },
	{ spv::ImageFormat::Rg16ui, spv::ImageFormat::Rgba16ui, 2 },
	{ spv::ImageFormat::R16, spv::ImageFormat::Rgba32f, 1, 16, FormatConversion::UNORM },
	{ spv::ImageFormat::Rg16, spv::ImageFormat::Rgba32f, 2, 16, FormatConversion::UNORM },
	{ spv::ImageFormat::Rgba16, spv::ImageFormat::Rgba32f, 4, 16, FormatConversion::UNORM },
	{ spv::ImageFormat::R16Snorm, spv::ImageFormat::Rgba32f, 1, 16, FormatConversion::SNORM },
	{ spv::ImageFormat::Rg16Snorm, spv::ImageFormat::Rgba32f, 2, 16, FormatConversion::SNORM },
	{ spv::ImageFormat::Rgba16Snorm, spv::ImageFormat::Rgba32f, 4, 16, FormatConversion::SNORM },
};

constexpr const StoragePromotion *storage_promotion(spv::ImageFormat p_format) {
	for (const auto &rule : storage_promotions) {
		if (rule.logical == p_format) {
			return &rule;
		}
	}
	return nullptr;
}

} // namespace godot::webgpu
