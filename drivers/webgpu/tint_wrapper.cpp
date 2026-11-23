#include "tint_wrapper.h"

#include "src/tint/api/tint.h"
#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/ast/module.h"
#include "src/tint/lang/wgsl/program/program.h"
#include "src/tint/lang/wgsl/sem/function.h"
#include "src/tint/lang/wgsl/writer/writer.h"

#include <mutex>

namespace godot::webgpu {

bool emit_wgsl(const std::vector<uint32_t> &p_words, const CompileOptions &p_options, StageOutput &r_output, std::string &r_error) {
	static std::once_flag initialized;
	std::call_once(initialized, tint::Initialize);
	tint::wgsl::writer::Options options;
	options.allowed_features = tint::wgsl::AllowedFeatures::Everything();
	options.allow_non_uniform_derivatives = p_options.non_uniform_derivatives;
	auto ir = tint::spirv::reader::ReadIR(p_words);
	if (ir != tint::Success) {
		r_error = ir.Failure().reason;
		return false;
	}
	auto program = tint::wgsl::writer::ProgramFromIR(ir.Get(), options);
	if (program != tint::Success) {
		r_error = program.Failure().reason;
		return false;
	}
	const auto &wgsl_program = program.Get();
	for (const auto *function : wgsl_program.AST().Functions()) {
		if (!function->IsEntryPoint()) {
			continue;
		}
		for (const auto *global : wgsl_program.Sem().Get(function)->TransitivelyReferencedGlobals()) {
			const auto id = global->Attributes().override_id;
			if (id) {
				const auto requested = p_options.specialization.find(id->value);
				if (requested != p_options.specialization.end() && !requested->second.has_value()) {
					r_output.override_ids.push_back(id->value);
				}
			}
		}
	}
	auto result = tint::wgsl::writer::Generate(wgsl_program, options);
	if (result != tint::Success) {
		r_error = result.Failure().reason;
		return false;
	}
	r_output.wgsl = std::move(result.Get().wgsl);
	return true;
}

} // namespace godot::webgpu
