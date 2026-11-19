#include "spirv_module.h"

namespace godot::webgpu::spirv {
namespace {

using Op = spv::Op;

struct Position {
	uint32_t variable;
	uint32_t value_type;
	std::optional<uint32_t> member;
};

std::optional<Position> find_position(IRContext &p_ir) {
	const auto is_position = [](const Instruction *d, uint32_t i) {
		return d->GetSingleWordInOperand(i) == bits(spv::Decoration::BuiltIn) &&
				d->GetSingleWordInOperand(i + 1) == bits(spv::BuiltIn::Position);
	};
	for (Instruction &var : p_ir.types_values()) {
		if (var.opcode() != Op::OpVariable || var.GetSingleWordInOperand(0) != bits(spv::StorageClass::Output)) {
			continue;
		}
		const auto *type = pointee(p_ir, var);
		if (const auto *d = decoration(p_ir, var.result_id(), spv::Decoration::BuiltIn); d && is_position(d, 1)) {
			return Position{ var.result_id(), type->result_id(), {} };
		}
		for (const Instruction *d : p_ir.get_decoration_mgr()->GetDecorationsFor(type->result_id(), false)) {
			if (d->opcode() == Op::OpMemberDecorate && is_position(d, 2)) {
				uint32_t member = d->GetSingleWordInOperand(1);
				return Position{ var.result_id(), type->GetSingleWordInOperand(member), member };
			}
		}
	}
	return {};
}

void flip_output_position(IRContext &p_ir, const Position &p_pos) {
	uint32_t entry = p_ir.module()->entry_points().begin()->GetSingleWordInOperand(1);
	uint32_t component = def(p_ir, p_pos.value_type)->GetSingleWordInOperand(0);
	constexpr uint32_t y_index = 1;
	auto *function = p_ir.GetFunction(entry);
	function->ForEachInst([&](Instruction *ret) {
		if (ret->opcode() != Op::OpReturn) {
			return;
		}
		uint32_t ptr = p_pos.variable;
		if (p_pos.member) {
			uint32_t type = p_ir.get_type_mgr()->FindPointerToType(p_pos.value_type, spv::StorageClass::Output);
			ptr = insert_value(p_ir, *ret, Op::OpAccessChain, type, { id(ptr), id(uint_constant(p_ir, *p_pos.member)) });
		}
		uint32_t pos = insert_value(p_ir, *ret, Op::OpLoad, p_pos.value_type, { id(ptr) });
		uint32_t y = insert_value(p_ir, *ret, Op::OpCompositeExtract, component, { id(pos), word(y_index) });
		y = insert_value(p_ir, *ret, Op::OpFNegate, component, { id(y) });
		pos = insert_value(p_ir, *ret, Op::OpCompositeInsert, p_pos.value_type, { id(y), id(pos), word(y_index) });
		insert_inst(p_ir, *ret, Op::OpStore, { id(ptr), id(pos) });
	});
}

bool lower_nonfinite_tests(Module &r_module) {
	IRContext &ir = *r_module.ir;
	std::vector<Instruction *> tests;
	ir.module()->ForEachInst([&](Instruction *i) {
		if (i->opcode() == Op::OpIsInf || i->opcode() == Op::OpIsNan) {
			tests.push_back(i);
		}
	});
	for (Instruction *test : tests) {
		uint32_t input = test->GetSingleWordInOperand(0);
		const Instruction *type = def(ir, def(ir, input)->type_id());
		const bool vector = type->opcode() == Op::OpTypeVector;
		const Instruction *scalar = vector ? def(ir, type->GetSingleWordInOperand(0)) : type;
		if (scalar->GetSingleWordInOperand(0) != 32) {
			return r_module.fail("Nonfinite test lowering requires f32");
		}
		const uint32_t integer_type = vector ? ir.get_type_mgr()->GetUIntVectorTypeId(type->GetSingleWordInOperand(1)) : ir.get_type_mgr()->GetUIntTypeId();
		auto splat = [&](uint32_t p_bits) -> uint32_t {
			const uint32_t component = uint_constant(ir, p_bits);
			if (!vector) {
				return component;
			}
			return insert_value(ir, *test, Op::OpCompositeConstruct, integer_type, Operands(type->GetSingleWordInOperand(1), id(component)));
		};
		constexpr uint32_t magnitude = 0x7fffffffu;
		constexpr uint32_t infinity = 0x7f800000u;
		uint32_t bits_id = insert_value(ir, *test, Op::OpBitcast, integer_type, { id(input) });
		bits_id = insert_value(ir, *test, Op::OpBitwiseAnd, integer_type, { id(bits_id), id(splat(magnitude)) });
		uint32_t result = insert_value(ir, *test, test->opcode() == Op::OpIsInf ? Op::OpIEqual : Op::OpUGreaterThan, test->type_id(),
				{ id(bits_id), id(splat(infinity)) });
		ir.ReplaceAllUsesWith(test->result_id(), result);
		ir.KillInst(test);
	}
	return true;
}

bool lower_cluster_subgroups(Module &r_module, SubgroupLowering p_mode) {
	if (p_mode == SubgroupLowering::NONE || r_module.stage != Stage::FRAGMENT) {
		return true;
	}
	IRContext &ir = *r_module.ir;
	std::vector<Instruction *> calls;
	ir.module()->ForEachInst([&](Instruction *i) {
		if (i->opcode() >= Op::OpGroupNonUniformElect && i->opcode() <= Op::OpGroupNonUniformQuadSwap) {
			calls.push_back(i);
		}
	});
	for (Instruction *inst : calls) {
		uint32_t value = 0;
		switch (inst->opcode()) {
			case Op::OpGroupNonUniformBroadcastFirst:
				value = inst->GetSingleWordInOperand(1);
				break;
			case Op::OpGroupNonUniformUMin:
			case Op::OpGroupNonUniformUMax:
			case Op::OpGroupNonUniformBitwiseOr:
				if (inst->GetSingleWordInOperand(1) == bits(spv::GroupOperation::Reduce)) {
					value = inst->GetSingleWordInOperand(2);
				}
				break;
			case Op::OpGroupNonUniformBallot:
				if (p_mode == SubgroupLowering::CLUSTER_RASTER) {
					uint32_t zero = uint_constant(ir, 0);
					uint32_t lo = insert_value(ir, *inst, Op::OpSelect, ir.get_type_mgr()->GetUIntTypeId(),
							{ id(inst->GetSingleWordInOperand(1)), id(uint_constant(ir, 1)), id(zero) });
					value = insert_value(ir, *inst, Op::OpCompositeConstruct, inst->type_id(), { id(lo), id(zero), id(zero), id(zero) });
				}
				break;
			case Op::OpGroupNonUniformBallotBitCount:
				if (p_mode == SubgroupLowering::CLUSTER_RASTER && inst->GetSingleWordInOperand(1) == bits(spv::GroupOperation::ExclusiveScan)) {
					value = uint_constant(ir, 0); // Single-lane subgroup has no preceding lanes.
				}
				break;
			default: break;
		}
		if (!value) {
			return r_module.fail("Unexpected subgroup operation in the lane-local Godot cluster variant");
		}
		ir.ReplaceAllUsesWith(inst->result_id(), value);
		ir.KillInst(inst);
	}
	// Scene loops retain their own item bounds, mask, and ordering. Cluster raster
	// commits each fragment's own two atomic ORs instead of combining lanes.
	return true;
}

} // namespace

bool lower_builtins(Module &r_module, const CompileOptions &p_options) {
	IRContext &ir = *r_module.ir;
	if (r_module.stage == Stage::VERTEX && p_options.flip_vertex_y) {
		if (auto position = find_position(ir)) {
			flip_output_position(ir, *position);
		}
	}
	for (const Instruction &inst : ir.annotations()) {
		const bool member = inst.opcode() == Op::OpMemberDecorate;
		if ((member || inst.opcode() == Op::OpDecorate) &&
				inst.GetSingleWordInOperand(member ? 2 : 1) == bits(spv::Decoration::BuiltIn) &&
				inst.GetSingleWordInOperand(member ? 3 : 2) == bits(spv::BuiltIn::HelperInvocation)) {
			return r_module.fail("HelperInvocation has no WGSL builtin; select a shader variant that does not observe helper lanes");
		}
	}
	return lower_nonfinite_tests(r_module) && lower_nonfinite_constants(r_module) && lower_cluster_subgroups(r_module, p_options.subgroup_lowering);
}

} // namespace godot::webgpu::spirv
