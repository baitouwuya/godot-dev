#include "rendering_shader_container_webgpu.h"

#include "tint_wrapper.h"

#include <algorithm>
#include <optional>

#ifdef WEBGPU_PRECOMPILED_SHADERS
#include "core/crypto/crypto_core.h"
#include "webgpu_shader_cache.gen.h"
#endif

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

auto key_fields(const CompileOptions &p) {
	return std::tie(p.specialization, p.raw_depth, p.unfilterable_float, p.storage_formats, p.atomic_images, p.immutable_samplers,
			p.uniform_memory_fences, p.texture_barrier, p.promote_narrow_storage, p.non_uniform_derivatives, p.flip_vertex_y, p.subgroup_lowering);
}

using Common = RenderingDeviceCommons;
std::optional<Stage> shader_stage(Common::ShaderStage p_stage) {
	switch (p_stage) {
		case Common::SHADER_STAGE_VERTEX:
			return Stage::VERTEX;
		case Common::SHADER_STAGE_FRAGMENT:
			return Stage::FRAGMENT;
		case Common::SHADER_STAGE_COMPUTE:
			return Stage::COMPUTE;
		default:
			return {};
	}
}

} // namespace

CompileKey::CompileKey(CompileOptions p_options) : options(std::move(p_options)) {
	for (auto *keys : { &options.raw_depth, &options.unfilterable_float }) {
		for (BindingKey &key : *keys) {
			key.role = Role::TEXTURE;
		}
		std::sort(keys->begin(), keys->end());
		keys->erase(std::unique(keys->begin(), keys->end()), keys->end());
	}
}

bool CompileKey::operator<(const CompileKey &p_other) const {
	return key_fields(options) < key_fields(p_other.options);
}

} // namespace godot::webgpu

uint32_t RenderingShaderContainerWebGPU::_format() const {
	return 0x55504757u; // WGPU fourcc.
}

uint32_t RenderingShaderContainerWebGPU::_format_version() const {
	return SHADER_PLAN_VERSION;
}

bool RenderingShaderContainerWebGPU::_set_code_from_spirv(const ReflectShader &p_shader) {
	for (const auto &stage : p_shader.shader_stages) {
		ERR_FAIL_COND_V_MSG(!shader_stage(stage.shader_stage), false, "The WebGPU backend supports only vertex, fragment and compute SPIR-V stages.");
	}
	{
		std::lock_guard<std::mutex> lock(prepared_mutex);
		prepared.clear();
	}
	shaders.resize(p_shader.shader_stages.size());
	for (uint32_t i = 0; i < p_shader.shader_stages.size(); ++i) {
		const auto &input = p_shader.shader_stages[i];
		Shader &output = shaders.write[i];
		output.shader_stage = input.shader_stage;
		output.code_compressed_bytes = input.spirv_data();
		output.code_compression_flags = 0;
		output.code_decompressed_size = output.code_compressed_bytes.size();
	}
	return true;
}

std::shared_ptr<const PreparedShader> RenderingShaderContainerWebGPU::prepare(const CompileOptions &p_options) const {
	CompileKey key(p_options);
	{
		std::lock_guard<std::mutex> lock(prepared_mutex);
		if (const auto cached = prepared.find(key); cached != prepared.end()) {
			return cached->second;
		}
	}
	std::vector<StageInput> stages;
	ShaderPlan result;
	for (const Shader &shader : shaders) {
		const auto stage = shader_stage(shader.shader_stage);
		if (!stage || shader.code_decompressed_size % sizeof(uint32_t) || !shader.code_decompressed_size || shader.code_compressed_bytes.is_empty() ||
				(!shader.code_compression_flags && shader.code_compressed_bytes.size() != shader.code_decompressed_size)) {
			result.diagnostic = "Invalid SPIR-V stage in WebGPU shader container";
			break;
		}
		StageInput input{ *stage, std::vector<uint32_t>(shader.code_decompressed_size / sizeof(uint32_t)) };
		if (!decompress_code(shader.code_compressed_bytes.ptr(), shader.code_compressed_bytes.size(), shader.code_compression_flags,
					reinterpret_cast<uint8_t *>(input.spirv.data()), input.spirv.size() * sizeof(uint32_t))) {
			result.diagnostic = "Cannot decode WebGPU shader container bytecode";
			break;
		}
		stages.push_back(std::move(input));
	}
	if (result.ok()) {
#ifdef WEBGPU_PRECOMPILED_SHADERS
		std::array<std::string, SHADER_STAGE_COUNT> hashes;
		for (const auto &stage : stages) {
			uint8_t digest[32];
			const auto *data = reinterpret_cast<const uint8_t *>(stage.spirv.data());
			const Error err = CryptoCore::sha256(data, stage.spirv.size() * sizeof(uint32_t), digest);
			DEV_ASSERT(err == OK);
			(void)err;
			const CharString hex = String::hex_encode_buffer(digest, sizeof(digest)).ascii();
			hashes[static_cast<size_t>(stage.stage)] = hex.get_data();
		}
		for (const auto &entry : embedded::entries) {
			if (hashes[0] != entry.sha256[0] || hashes[1] != entry.sha256[1] || hashes[2] != entry.sha256[2]) {
				continue;
			}
			const CompileKey compiled(entry.options());
			if (!(key < compiled) && !(compiled < key)) {
				seed_verified_plan(key.options, entry.plan());
				std::lock_guard<std::mutex> lock(prepared_mutex);
				return prepared.find(key)->second;
			}
		}
#endif
		result = compile_shader(stages, key.options);
	}
	auto value = std::make_shared<const PreparedShader>(PreparedShader{ std::move(result), key });
	std::lock_guard<std::mutex> lock(prepared_mutex);
	return prepared.emplace(std::move(key), std::move(value)).first->second;
}

void RenderingShaderContainerWebGPU::seed_verified_plan(const CompileOptions &p_options, ShaderPlan p_plan) const {
	ERR_FAIL_COND(!p_plan.ok() || p_plan.stages.empty());
	CompileKey key(p_options);
	auto value = std::make_shared<const PreparedShader>(PreparedShader{ std::move(p_plan), key });
	std::lock_guard<std::mutex> lock(prepared_mutex);
	prepared.try_emplace(std::move(key), std::move(value));
}

Ref<RenderingShaderContainer> RenderingShaderContainerFormatWebGPU::create_container() const {
	Ref<RenderingShaderContainerWebGPU> container;
	container.instantiate();
	return container;
}
