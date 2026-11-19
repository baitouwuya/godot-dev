#pragma once

#include "webgpu_shader_plan.h"

namespace godot::webgpu::host {

bool compile_glsl(Stage p_stage, const std::string &p_source, uint32_t p_vulkan_version,
		uint32_t p_spirv_version, bool p_debug, std::vector<uint32_t> &r_words, std::string &r_error);

} // namespace godot::webgpu::host
