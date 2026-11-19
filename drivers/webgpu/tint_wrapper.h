#pragma once

#include "webgpu_shader_plan.h"

namespace godot::webgpu {

// No WebGPU, Tint or SPIRV-Tools headers cross this API. Result and inputs can
// be owned by different worker tasks; the function creates no device objects.
ShaderPlan compile_shader(const std::vector<StageInput> &p_stages, const CompileOptions &p_options = {});

} // namespace godot::webgpu
