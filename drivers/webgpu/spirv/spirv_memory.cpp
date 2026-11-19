#include "spirv_module.h"

#include <set>

namespace godot::webgpu::spirv {
namespace {

using Op = spv::Op;
using Deco = spv::Decoration;

bool has_type(IRContext &p_ir, uint32_t p_id, uint32_t p_target) {
	if (p_id == p_target) {
		return true;
	}
	const Instruction &type = *def(p_ir, p_id);
	switch (type.opcode()) {
		case Op::OpTypeArray:
		case Op::OpTypeRuntimeArray:
			return has_type(p_ir, type.GetSingleWordInOperand(0), p_target);
		case Op::OpTypeStruct:
			for (uint32_t i = 0; i < type.NumInOperands(); ++i) {
				if (has_type(p_ir, type.GetSingleWordInOperand(i), p_target)) {
					return true;
				}
			}
			return false;
		default:
			return false;
	}
}

bool only_atomic_accesses(IRContext &p_ir, uint32_t p_id, std::set<uint32_t> &r_seen) {
	if (!r_seen.insert(p_id).second) {
		return true;
	}
	return p_ir.get_def_use_mgr()->WhileEachUser(p_id, [&](Instruction *p_user) {
		if (p_user->IsDecoration() || p_user->opcode() == Op::OpName || p_user->opcode() == Op::OpEntryPoint) {
			return true;
		}
		if (p_user->IsAtomicOp()) {
			return p_user->GetSingleWordInOperand(0) == p_id;
		}
		switch (p_user->opcode()) {
			case Op::OpAccessChain:
			case Op::OpInBoundsAccessChain:
			case Op::OpImageTexelPointer:
			case Op::OpCopyObject:
				return p_user->GetSingleWordInOperand(0) == p_id && only_atomic_accesses(p_ir, p_user->result_id(), r_seen);
			default:
				return false;
		}
	});
}

} // namespace

bool normalize_memory_qualifiers(Module &r_module) {
	IRContext &ir = *r_module.ir;
	std::vector<Instruction *> decorations;
	for (Instruction &inst : ir.annotations()) {
		const bool member = inst.opcode() == Op::OpMemberDecorate;
		if (!member && inst.opcode() != Op::OpDecorate) {
			continue;
		}
		const auto deco = static_cast<Deco>(inst.GetSingleWordInOperand(member ? 2 : 1));
		if (deco == Deco::Restrict || deco == Deco::Aliased) {
			// WGSL has no SPIR-V aliasing promise. Do not change memory accesses.
			decorations.push_back(&inst);
		} else if (deco == Deco::Volatile) {
			const uint32_t target = inst.GetSingleWordInOperand(0);
			std::set<uint32_t> seen;
			for (const Instruction &var : ir.types_values()) {
				if (var.opcode() == Op::OpVariable && (member ? has_type(ir, pointee(ir, var)->result_id(), target) : var.result_id() == target) &&
						!only_atomic_accesses(ir, var.result_id(), seen)) {
					return r_module.fail("Volatile non-atomic memory access has no WGSL equivalent (SPIR-V ID " + std::to_string(var.result_id()) + ")");
				}
			}
			// Fog's accesses are already SPIR-V atomics. WGSL atomics execute
			// explicitly as well; this does not remove a load, store or fence.
			decorations.push_back(&inst);
		}
	}
	for (Instruction *inst : decorations) {
		ir.KillInst(inst);
	}
	return true;
}

} // namespace godot::webgpu::spirv
