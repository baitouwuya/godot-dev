#include "spirv_module.h"

#include "source/opt/eliminate_dead_constant_pass.h"

#include <set>

namespace godot::webgpu::spirv {
namespace {

using Op = spv::Op;

bool nonfinite(const Instruction &p_inst, IRContext &p_ir) {
	if (p_inst.opcode() != Op::OpConstant) {
		return false;
	}
	const Instruction &type = *def(p_ir, p_inst.type_id());
	if (type.opcode() != Op::OpTypeFloat) {
		return false;
	}
	const uint32_t width = type.GetSingleWordInOperand(0);
	const auto &payload = p_inst.GetInOperand(0).words;
	if (width == 64) {
		return (payload[1] & 0x7ff00000u) == 0x7ff00000u;
	}
	const uint32_t exponent = width == 16 ? 0x7c00u : 0x7f800000u;
	return (payload[0] & exponent) == exponent;
}

uint32_t materialize(IRContext &p_ir, Instruction &r_before, uint32_t p_value, const std::set<uint32_t> &p_nonfinite,
		std::map<std::pair<uint32_t, uint32_t>, uint32_t> &r_locals) {
	if (!p_nonfinite.count(p_value)) {
		return p_value;
	}
	const Instruction &inst = *def(p_ir, p_value);
	if (inst.opcode() == Op::OpConstantComposite) {
		Operands parts;
		for (uint32_t j = 0; j < inst.NumInOperands(); ++j) {
			parts.push_back(id(materialize(p_ir, r_before, inst.GetSingleWordInOperand(j), p_nonfinite, r_locals)));
		}
		return insert_value(p_ir, r_before, Op::OpCompositeConstruct, inst.type_id(), std::move(parts));
	}
	auto *fn = p_ir.get_instr_block(&r_before)->GetParent();
	auto [stored, fresh] = r_locals.emplace(std::make_pair(fn->result_id(), p_value), 0);
	uint32_t type = p_ir.get_type_mgr()->GetUIntTypeId();
	if (fresh) {
		GODOT_WEBGPU_ASSERT(def(p_ir, inst.type_id())->GetSingleWordInOperand(0) == 32);
		const auto sc = spv::StorageClass::Function;
		uint32_t ptr = p_ir.get_type_mgr()->FindPointerToType(type, sc);
		auto &first = *fn->entry()->begin();
		stored->second = insert_value(p_ir, first, Op::OpVariable, ptr,
				{ { SPV_OPERAND_TYPE_STORAGE_CLASS, { bits(sc) } }, id(uint_constant(p_ir, inst.GetSingleWordInOperand(0))) });
	}
	uint32_t value = insert_value(p_ir, r_before, Op::OpLoad, type, { id(stored->second) });
	return insert_value(p_ir, r_before, Op::OpBitcast, inst.type_id(), { id(value) });
}

} // namespace

bool lower_nonfinite_constants(Module &r_module) {
	IRContext &ir = *r_module.ir;
	std::set<uint32_t> special;
	for (const auto &inst : ir.types_values()) {
		if (nonfinite(inst, ir)) {
			if (def(ir, inst.type_id())->GetSingleWordInOperand(0) != 32) {
				return r_module.fail("Only runtime materialization of non-finite f32 constants is supported");
			}
			special.insert(inst.result_id());
		} else if (inst.opcode() == Op::OpConstantComposite) {
			inst.ForEachInId([&](const uint32_t *operand) {
				if (special.count(*operand)) {
					special.insert(inst.result_id());
				}
			});
		} else if (inst.opcode() == Op::OpVariable && inst.NumInOperands() > 1 && special.count(inst.GetSingleWordInOperand(1))) {
			return r_module.fail("Non-finite SPIR-V global initializer cannot be a WGSL creation-time constant");
		}
	}
	if (special.empty()) {
		return true;
	}
	std::vector<Instruction *> work;
	for (auto &fn : *ir.module()) {
		fn.ForEachInst([&](Instruction *i) { work.push_back(i); });
	}
	std::map<std::pair<uint32_t, uint32_t>, uint32_t> locals;
	for (Instruction *i : work) {
		for (uint32_t j = 0; j < i->NumInOperands(); ++j) {
			const auto &op = i->GetInOperand(j);
			if (op.type != SPV_OPERAND_TYPE_ID || !special.count(op.words[0])) {
				continue;
			}
			if (i->opcode() == Op::OpPhi || i->opcode() == Op::OpVariable) {
				return r_module.fail("Non-finite phi or function initializer needs edge materialization");
			}
			uint32_t value = materialize(ir, *i, op.words[0], special, locals);
			replace_operand(ir, *i, j, value);
		}
	}
	spvtools::opt::EliminateDeadConstantPass pass;
	return r_module.run(pass);
}

} // namespace godot::webgpu::spirv
