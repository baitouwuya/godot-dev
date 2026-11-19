#include "spirv_module.h"

#include "source/opt/fold_spec_constant_op_and_composite_pass.h"
#include "source/opt/freeze_spec_constant_value_pass.h"
#include "source/opt/simplification_pass.h"

namespace godot::webgpu::spirv {

bool specialize(Module &r_module, const CompileOptions &p_options) {
	IRContext &ir = *r_module.ir;
	for (Instruction &inst : ir.types_values()) {
		if (inst.opcode() != spv::Op::OpSpecConstant && inst.opcode() != spv::Op::OpSpecConstantTrue && inst.opcode() != spv::Op::OpSpecConstantFalse) {
			continue;
		}
		const Instruction *spec = decoration(ir, inst.result_id(), spv::Decoration::SpecId);
		if (!spec) {
			continue;
		}
		auto value = p_options.specialization.find(spec->GetSingleWordInOperand(2));
		if (value == p_options.specialization.end()) {
			continue;
		}
		if (inst.opcode() == spv::Op::OpSpecConstant) {
			if (inst.GetInOperand(0).words.size() != 1) {
				return r_module.fail("Only 32-bit specialization payloads are supported");
			}
			replace_operand(ir, inst, 0, value->second);
		} else {
			inst.SetOpcode(value->second ? spv::Op::OpSpecConstantTrue : spv::Op::OpSpecConstantFalse);
		}
	}
	spvtools::opt::FreezeSpecConstantValuePass freeze;
	spvtools::opt::FoldSpecConstantOpAndCompositePass fold;
	spvtools::opt::SimplificationPass simplify;
	if (!r_module.run(freeze) || !r_module.run(fold) || !r_module.run(simplify)) {
		return false;
	}
	return prune(r_module);
}

} // namespace godot::webgpu::spirv
