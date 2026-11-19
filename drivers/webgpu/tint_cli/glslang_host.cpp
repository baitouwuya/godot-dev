#include "glslang_host.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace godot::webgpu::host {

bool compile_glsl(Stage p_stage, const std::string &p_source, uint32_t p_vulkan_version,
		uint32_t p_spirv_version, bool p_debug, std::vector<uint32_t> &r_words, std::string &r_error) {
	static const bool initialized = glslang::InitializeProcess();
	if (!initialized) {
		r_error = "glslang::InitializeProcess failed";
		return false;
	}
	constexpr std::array stages{ EShLangVertex, EShLangFragment, EShLangCompute };
	const EShLanguage stage = stages.at(static_cast<unsigned>(p_stage));
	const char *source = p_source.c_str();
	glslang::TShader shader(stage);
	shader.setStrings(&source, 1);
	// Match modules/glslang/register_types.cpp::compile_glslang_shader.
	shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, GLSL_DEFAULT_VERSION);
	shader.setEnvClient(glslang::EShClientVulkan, static_cast<glslang::EShTargetClientVersion>(p_vulkan_version));
	shader.setEnvTarget(glslang::EShTargetSpv, static_cast<glslang::EShTargetLanguageVersion>(p_spirv_version));
	const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | (p_debug ? EShMsgDebugInfo : 0));
	if (!shader.parse(GetDefaultResources(), GLSL_DEFAULT_VERSION, false, messages)) {
		r_error = std::string("glslang parse: ") + shader.getInfoLog() + "\n" + shader.getInfoDebugLog();
		return false;
	}
	glslang::TProgram program;
	program.addShader(&shader);
	if (!program.link(messages)) {
		r_error = std::string("glslang link: ") + program.getInfoLog() + "\n" + program.getInfoDebugLog();
		return false;
	}
	spv::SpvBuildLogger logger;
	glslang::SpvOptions options;
	options.generateDebugInfo = p_debug;
	options.emitNonSemanticShaderDebugInfo = p_debug;
	options.emitNonSemanticShaderDebugSource = p_debug;
	glslang::GlslangToSpv(*program.getIntermediate(stage), r_words, &logger, &options);
	if (r_words.empty()) {
		r_error = "glslang emitted no SPIR-V: " + logger.getAllMessages();
		return false;
	}
	return true;
}

} // namespace godot::webgpu::host
