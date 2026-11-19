#pragma once

#include "webgpu_shader_plan.h"

#include "servers/rendering/rendering_shader_container.h"

#include <memory>
#include <mutex>

namespace godot::webgpu {

struct CompileKey {
	CompileOptions options;
	explicit CompileKey(CompileOptions p_options);
	bool operator<(const CompileKey &p_other) const;
};

struct PreparedShader {
	const ShaderPlan plan;
	const CompileKey key;
};

} // namespace godot::webgpu

class RenderingShaderContainerWebGPU final : public RenderingShaderContainer {
	GDSOFTCLASS(RenderingShaderContainerWebGPU, RenderingShaderContainer);

	mutable std::mutex prepared_mutex;
	mutable std::map<godot::webgpu::CompileKey, std::shared_ptr<const godot::webgpu::PreparedShader>> prepared;

protected:
	uint32_t _format() const override;
	uint32_t _format_version() const override;
	bool _set_code_from_spirv(const ReflectShader &p_shader) override;

public:
	// Pure CPU operation. Capture a Ref when scheduling on WorkerThreadPool;
	// no context/device is needed, and the result may cross threads.
	std::shared_ptr<const godot::webgpu::PreparedShader> prepare(const godot::webgpu::CompileOptions &p_options) const;
	// The precompiled loader must validate the exact input SPIR-V SHA-256s and
	// compiler fingerprint against its manifest before installing a candidate.
	void seed_verified_plan(const godot::webgpu::CompileOptions &p_options, godot::webgpu::ShaderPlan p_plan) const;
};

class RenderingShaderContainerFormatWebGPU final : public RenderingShaderContainerFormat {
	GDSOFTCLASS(RenderingShaderContainerFormatWebGPU, RenderingShaderContainerFormat);

public:
	Ref<RenderingShaderContainer> create_container() const override;
	ShaderLanguageVersion get_shader_language_version() const override { return SHADER_LANGUAGE_VULKAN_VERSION_1_1; }
	ShaderSpirvVersion get_shader_spirv_version() const override { return SHADER_SPIRV_VERSION_1_3; }
};
