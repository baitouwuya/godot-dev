#include "spirv_module.h"

#include "source/opt/fold_spec_constant_op_and_composite_pass.h"
#include "source/opt/freeze_spec_constant_value_pass.h"
#include "source/opt/simplification_pass.h"

#include <algorithm>

namespace godot::webgpu::spirv {
namespace {

using Op = spv::Op;

bool can_defer(IRContext &r_ir, const Instruction &p_constant, const Instruction *p_spec) {
	const auto *type = def(r_ir, p_constant.type_id());
	if (p_constant.opcode() != Op::OpSpecConstant || type->opcode() != Op::OpTypeInt ||
			type->GetSingleWordInOperand(0) != 32 || type->GetSingleWordInOperand(1) != 0) {
		return false;
	}
	// Forward+ reads the two packed words via parameterless u32 getters. Keeping
	// the constant behind that call preserves resource pruning: InlineOpaque
	// does not inline these calls, and folding cannot see through them.
	return r_ir.get_def_use_mgr()->WhileEachUser(&p_constant, [&](Instruction *p_user) {
		if (p_user == p_spec || p_user->opcode() == Op::OpName) {
			return true;
		}
		if (p_user->opcode() != Op::OpReturnValue) {
			return false;
		}
		const auto *block = r_ir.get_instr_block(p_user);
		const auto *function = block->GetParent();
		const auto *signature = def(r_ir, function->DefInst().GetSingleWordInOperand(1));
		return signature->NumInOperands() == 1 && block == function->entry().get() &&
				block == function->tail() && &*block->begin() == p_user;
	});
}

} // namespace

bool specialize(Module &r_module, const CompileOptions &p_options) {
	IRContext &ir = *r_module.ir;
	const bool partial = std::any_of(p_options.specialization.begin(), p_options.specialization.end(),
			[](const auto &p_entry) { return !p_entry.second.has_value(); });
	for (Instruction &inst : ir.types_values()) {
		if (inst.opcode() != Op::OpSpecConstant && inst.opcode() != Op::OpSpecConstantTrue &&
				inst.opcode() != Op::OpSpecConstantFalse && inst.opcode() != Op::OpSpecConstantDataKHR) {
			continue;
		}
		Instruction *spec = decoration(ir, inst.result_id(), spv::Decoration::SpecId);
		const auto value = spec ? p_options.specialization.find(spec->GetSingleWordInOperand(2)) :
								  p_options.specialization.end();
		if (value != p_options.specialization.end()) {
			if (!value->second) {
				if (!can_defer(ir, inst, spec)) {
					return r_module.fail("Pipeline override is not a runtime u32 getter");
				}
				continue;
			}
			if (inst.opcode() == Op::OpSpecConstant) {
				if (inst.GetInOperand(0).words.size() != 1) {
					return r_module.fail("Only 32-bit specialization payloads are supported");
				}
				replace_operand(ir, inst, 0, *value->second);
			} else if (inst.opcode() != Op::OpSpecConstantDataKHR) {
				inst.SetOpcode(*value->second ? Op::OpSpecConstantTrue : Op::OpSpecConstantFalse);
			}
		}
		if (partial) {
			switch (inst.opcode()) {
				case Op::OpSpecConstant: inst.SetOpcode(Op::OpConstant); break;
				case Op::OpSpecConstantTrue: inst.SetOpcode(Op::OpConstantTrue); break;
				case Op::OpSpecConstantFalse: inst.SetOpcode(Op::OpConstantFalse); break;
				case Op::OpSpecConstantDataKHR: inst.SetOpcode(Op::OpConstantDataKHR); break;
				default: break;
			}
			if (spec) {
				ir.KillInst(spec);
			}
		}
	}
	spvtools::opt::FreezeSpecConstantValuePass freeze;
	if (partial) {
		ir.InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
	} else if (!r_module.run(freeze)) {
		return false;
	}
	spvtools::opt::FoldSpecConstantOpAndCompositePass fold;
	spvtools::opt::SimplificationPass simplify;
	if (!r_module.run(fold) || !r_module.run(simplify)) {
		return false;
	}
	return prune(r_module);
}

} // namespace godot::webgpu::spirv
