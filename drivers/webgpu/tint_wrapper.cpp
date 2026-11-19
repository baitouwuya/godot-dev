#include "tint_wrapper.h"

#include "src/tint/api/tint.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/writer/writer.h"

#include <mutex>

namespace godot::webgpu {

bool emit_wgsl(const std::vector<uint32_t> &p_words, const CompileOptions &p_options, StageOutput &r_output, std::string &r_error) {
	static std::once_flag initialized;
	std::call_once(initialized, tint::Initialize);
	tint::wgsl::writer::Options options;
	options.allowed_features = tint::wgsl::AllowedFeatures::Everything();
	options.allow_non_uniform_derivatives = p_options.non_uniform_derivatives;
	auto result = tint::SpirvToWgsl(p_words, options);
	if (result != tint::Success) {
		r_error = result.Failure().reason;
		return false;
	}
	r_output.wgsl = std::move(result.Get());
	return true;
}

} // namespace godot::webgpu
