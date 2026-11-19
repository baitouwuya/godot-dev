#pragma once

#include <cstdint>

namespace godot::webgpu {

inline constexpr uint32_t SHADER_PLAN_VERSION = 1;
inline constexpr uint32_t FIRST_FOLDED_GROUP = 3;
inline constexpr uint32_t SHADER_STAGE_COUNT = 3;
inline constexpr uint32_t SPIRV_VULKAN_1_1 = (1u << 22) | (1u << 12);
inline constexpr uint32_t SPIRV_VERSION_1_3 = (1u << 16) | (3u << 8);
inline constexpr uint32_t GLSL_DEFAULT_VERSION = 100;
inline constexpr uint32_t SPIRV_WORD_BYTES = sizeof(uint32_t);
inline constexpr uint32_t ATOMIC_IMAGE_HEADER_BYTES = 4 * SPIRV_WORD_BYTES;
inline constexpr uint32_t COPY_ROW_ALIGNMENT = 256;
inline constexpr uint32_t BUFFER_COPY_ALIGNMENT = 4;
inline constexpr uint32_t PUSH_PAGE_BYTES = 64 * 1024;
inline constexpr uint32_t MAX_PUSH_CONSTANT_BYTES = 256;
inline constexpr uint32_t QUERY_RESOLVE_ALIGNMENT = 256;
inline constexpr uint32_t MAX_DRIVER_FRAMES = 4; // RenderingDevice frame-index packing contract.
inline constexpr uint32_t DYNAMIC_FRAME_BITS = 4;
inline constexpr uint32_t VERTEX_FRAME_BITS = 2;
inline constexpr uint16_t MAX_SAMPLER_ANISOTROPY = 16;
inline constexpr uint32_t CUBE_LAYERS = 6;

} // namespace godot::webgpu
