#include "spirv_module.h"

#include "../tint_wrapper.h"

#include <algorithm>

namespace godot::webgpu {

bool emit_wgsl(const std::vector<uint32_t> &, const CompileOptions &, StageOutput &, std::string &);

ShaderPlan compile_shader(const std::vector<StageInput> &p_stages, const CompileOptions &p_options) {
	ShaderPlan plan;
	std::vector<std::unique_ptr<spirv::Module>> modules;
	uint8_t seen = 0;
	for (const StageInput &stage : p_stages) {
		if (static_cast<uint32_t>(stage.stage) >= SHADER_STAGE_COUNT || (seen & stage_mask(stage.stage))) {
			plan.diagnostic = "Invalid or duplicate shader stage";
			return plan;
		}
		seen |= stage_mask(stage.stage);
		auto module = std::make_unique<spirv::Module>(stage.stage);
		if (!module->open(stage.spirv) || !spirv::specialize(*module, p_options) || !spirv::lower_control_flow(*module, p_options) ||
				!spirv::lower_bindings(*module, p_options) || !spirv::lower_images(*module, p_options) ||
				!spirv::lower_push_constant(*module) || !spirv::lower_builtins(*module, p_options)) {
			plan.diagnostic = "stage " + std::to_string(static_cast<unsigned>(stage.stage)) + ": " + module->error;
			return plan;
		}
		plan.uses_texture_barrier |= module->uses_texture_barrier;
		modules.push_back(std::move(module));
	}
	if (!seen || ((seen & stage_mask(Stage::COMPUTE)) && seen != stage_mask(Stage::COMPUTE))) {
		plan.diagnostic = "Expected a compute pipeline or one or more graphics stages";
		return plan;
	}
	if (!spirv::plan_layout(modules, p_options, plan)) {
		return plan;
	}
	std::sort(modules.begin(), modules.end(), [](const auto &a, const auto &b) { return a->stage < b->stage; });
	for (const auto &module : modules) {
		StageOutput output{ module->stage, module->entry, {} };
		std::string error;
		if (!emit_wgsl(module->binary(), p_options, output, error)) {
			plan.diagnostic = "Tint stage " + std::to_string(static_cast<unsigned>(module->stage)) + ": " + error;
			break;
		}
		plan.stages.push_back(std::move(output));
	}
	return plan;
}

} // namespace godot::webgpu
