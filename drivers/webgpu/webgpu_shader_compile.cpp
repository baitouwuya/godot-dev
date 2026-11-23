#include "webgpu_shader_resources.h"
#include "webgpu_perf_stats.h"

#include "core/object/worker_thread_pool.h"

#include <cstring>

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

CompileOptions variant_options(const ShaderResource &p_shader, VectorView<RDD::PipelineSpecializationConstant> p_constants, CompileOptions options) {
	for (uint32_t i = 0; i < p_constants.size(); ++i) {
		const auto &value = p_constants[i];
		uint32_t bits = value.int_value;
		if (value.type == RDD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL) {
			bits = value.bool_value;
		} else if (value.type == RDD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_FLOAT) {
			std::memcpy(&bits, &value.float_value, sizeof(bits));
		}
		options.specialization[value.constant_id] = bits;
	}
	// Reuse sampler descriptors only when they are the same native immutable sampler object.
	std::map<const SamplerResource *, uint32_t> equivalence;
	for (const auto &[key, sampler] : p_shader.immutable) {
		auto descriptor = equivalence.try_emplace(sampler.get(), equivalence.size());
		options.immutable_samplers[key] = descriptor.first->second;
	}
	return options;
}

struct PrepareJob {
	Ref<RenderingShaderContainerWebGPU> source;
	CompileOptions options;
	static void run(void *p_data) {
		std::unique_ptr<PrepareJob> job(static_cast<PrepareJob *>(p_data));
		job->source->prepare(job->options);
	}
};

} // namespace

const StageOutput &ShaderVariant::stage(Stage p_stage) const {
	for (const StageOutput &stage : prepared->plan.stages) {
		if (stage.stage == p_stage) {
			return stage;
		}
	}
	CRASH_NOW_MSG("WebGPU pipeline has no requested shader stage.");
}

} // namespace godot::webgpu

RDD::ShaderID RenderingDeviceDriverWebGPU::shader_create_from_container(const Ref<RenderingShaderContainer> &p_container, const Vector<ImmutableSampler> &p_samplers) {
	auto shader = std::make_shared<ShaderResource>();
	shader->source = p_container;
	ERR_FAIL_COND_V(shader->source.is_null(), {});
	shader->reflection = p_container->get_shader_reflection();
	const String name = String::utf8(p_container->shader_name.get_data());
	const String shader_class = name.get_slicec(':', 0);
	shader->options.non_uniform_derivatives = shader_class.ends_with("ShaderRD");
	if (shader_class == "SceneForwardClusteredShaderRD") {
		shader->options.subgroup_lowering = SubgroupLowering::CLUSTERED_SCENE;
	} else if (shader_class == "ClusterRenderShaderRD") {
		shader->options.subgroup_lowering = SubgroupLowering::CLUSTER_RASTER;
		shader->options.specialization[0] = 0; // WGSL has no HelperInvocation builtin.
	} else if (shader_class == "SdfgiPreprocessShaderRD") {
		shader->options.texture_barrier = state->has_read_write_storage_textures;
	}
	for (const auto &sampler : p_samplers) {
		std::optional<uint32_t> found_set;
		for (uint32_t s = 0; s < shader->reflection.uniform_sets.size(); ++s) {
			for (const auto &uniform : shader->reflection.uniform_sets[s]) {
				if (uniform.binding == sampler.binding && uniform.type == sampler.type) {
					ERR_FAIL_COND_V_MSG(found_set, {}, "Immutable sampler is ambiguous across logical sets; Godot's driver descriptor does not contain a set index.");
					found_set = s;
				}
			}
		}
		ERR_FAIL_COND_V_MSG(!found_set, {}, "Immutable sampler does not match shader reflection.");
		for (uint32_t i = 0; i < sampler.ids.size(); ++i) {
			shader->immutable[{ *found_set, sampler.binding, i, Role::SAMPLER }] = resource<SamplerResource>(sampler.ids[i]);
		}
	}
	// On the calling thread, wait for first use so the real constants and bound view formats are known.
#ifdef THREADS_ENABLED
	if (shader_class.ends_with("ShaderRD")) {
		// ShaderRD can call here from its GLSL worker group. This separate CPU
		// job must not access driver state or wait on browser callbacks.
		shader->prepare_task = WorkerThreadPool::get_singleton()->add_native_task(PrepareJob::run,
				new PrepareJob{ shader->source, variant_options(*shader, {}, shader->options) }, true, "Prepare WebGPU shader");
	}
#endif // THREADS_ENABLED
	return resource_id<ShaderID>(std::move(shader));
}

Error RenderingDeviceDriverWebGPU::set_shader_compile_options(ShaderID p_shader, const CompileOptions &p_options) {
	state->assert_owner();
	auto &shader = *resource<ShaderResource>(p_shader);
	shader.options = p_options;
	return OK;
}

int64_t RenderingDeviceDriverWebGPU::schedule_shader_prepare(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_constants) {
	state->assert_owner();
	const auto &shader = *resource<ShaderResource>(p_shader);
	return WorkerThreadPool::get_singleton()->add_native_task(PrepareJob::run, new PrepareJob{ shader.source, variant_options(shader, p_constants, shader.options) }, true, "Prepare WebGPU shader");
}

std::shared_ptr<ShaderVariant> RenderingDeviceDriverWebGPU::realize_shader(const std::shared_ptr<ShaderResource> &p_shader, VectorView<PipelineSpecializationConstant> p_constants, const CompileOptions &p_options) {
	state->assert_owner();
	if (p_shader->prepare_task != -1) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(p_shader->prepare_task);
		p_shader->prepare_task = -1;
	}
	const CompileKey key(variant_options(*p_shader, p_constants, p_options));
	if (auto it = p_shader->variants.find(key); it != p_shader->variants.end()) {
		return it->second;
	}
#ifdef WEB_ENABLED
	const bool perf_active = webgpu_perf_is_enabled() && webgpu_perf_stats.in_raf;
	const double perf_start_ms = perf_active ? emscripten_get_now() : 0.0;
#endif
	auto prepared = p_shader->source->prepare(key.options);
	const ShaderPlan &plan = prepared->plan;
	ERR_FAIL_COND_V_MSG(!plan.ok(), {}, String::utf8(plan.diagnostic.c_str()));
	if (auto it = p_shader->variants.find(prepared->key); it != p_shader->variants.end()) {
		return p_shader->variants.try_emplace(key, it->second).first->second;
	}
	auto variant = std::make_shared<ShaderVariant>();
	variant->prepared = std::move(prepared);
	variant->immutable = p_shader->immutable;
	for (uint32_t s = 0; s < p_shader->reflection.uniform_sets.size(); ++s) {
		for (const auto &uniform : p_shader->reflection.uniform_sets[s]) {
			if (uniform.type == UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC || uniform.type == UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC) {
				variant->dynamic_buffers.insert({ s, uniform.binding, 0, uniform.type == UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC ? Role::UNIFORM_BUFFER : Role::STORAGE_BUFFER });
			}
		}
	}
	if (!create_pipeline_layout(*state, *variant)) {
		return {};
	}
	for (const StageOutput &stage : plan.stages) {
		WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
		wgsl.code = wg_string(stage.wgsl);
		WGPUShaderModuleDescriptor desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		desc.nextInChain = &wgsl.chain;
		desc.label = wg_string(p_shader->label);
		desc.label = { p_shader->source->shader_name.get_data(), static_cast<size_t>(p_shader->source->shader_name.length()) };
		auto &module = variant->modules[static_cast<size_t>(stage.stage)];
		module.reset(wgpuDeviceCreateShaderModule(state->device.get(), &desc));
		ERR_FAIL_COND_V(!module, {});
	}
#ifdef WEB_ENABLED
	if (perf_active) {
		webgpu_perf_stats.frame.work.shader_module_calls++;
		webgpu_perf_stats.frame.work.shader_module_ms += emscripten_get_now() - perf_start_ms;
	}
#endif
	p_shader->variants.try_emplace(variant->prepared->key, variant);
	p_shader->variants.try_emplace(key, variant);
	return variant;
}

void RenderingDeviceDriverWebGPU::shader_destroy_modules(ShaderID p_shader) {
	state->assert_owner();
	resource<ShaderResource>(p_shader)->variants.clear(); // Live pipelines hold their own variant lease.
}

void RenderingDeviceDriverWebGPU::shader_free(ShaderID p_shader) {
	state->assert_owner();
	const auto &shader = resource<ShaderResource>(p_shader);
	if (shader->prepare_task != -1) {
		WorkerThreadPool::get_singleton()->wait_for_task_completion(shader->prepare_task);
		shader->prepare_task = -1;
	}
	free_id<ShaderResource>(p_shader);
}

const RenderingShaderContainerFormat &RenderingDeviceDriverWebGPU::get_shader_container_format() const {
	static const RenderingShaderContainerFormatWebGPU format;
	return format;
}
