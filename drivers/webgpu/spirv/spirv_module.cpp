#include "spirv_module.h"

#include <spirv-tools/libspirv.hpp>

#include "source/opt/aggressive_dead_code_elim_pass.h"
#include "source/opt/build_module.h"
#include "source/opt/dead_branch_elim_pass.h"
#include "source/opt/dead_variable_elimination.h"
#include "source/opt/eliminate_dead_constant_pass.h"
#include "source/opt/eliminate_dead_functions_pass.h"
#include "source/opt/flatten_decoration_pass.h"
#include "source/opt/remove_unused_interface_variables_pass.h"
#include "source/opt/strip_nonsemantic_info_pass.h"

#include <algorithm>

namespace godot::webgpu::spirv {

bool Module::fail(const std::string &p_text) {
	if (!error.empty()) {
		error += '\n';
	}
	error += p_text;
	return false;
}

bool Module::open(const std::vector<uint32_t> &p_words) {
	constexpr spv_target_env env = SPV_ENV_VULKAN_1_1;
	auto report = [this](spv_message_level_t p_level, const char *, const spv_position_t &p_pos, const char *p_msg) {
		if (p_level <= SPV_MSG_ERROR) {
			fail("SPIR-V at " + std::to_string(p_pos.index) + ": " + p_msg);
		}
	};
	spvtools::SpirvTools validator(env);
	validator.SetMessageConsumer(report);
	if (!validator.Validate(p_words)) {
		return fail("Input does not validate as Vulkan 1.1 SPIR-V");
	}
	ir = spvtools::BuildModule(env, report, p_words.data(), p_words.size());
	if (!ir || ir->module()->entry_points().empty()) {
		return fail("No SPIR-V entry point");
	}
	constexpr std::array models{ spv::ExecutionModel::Vertex, spv::ExecutionModel::Fragment, spv::ExecutionModel::GLCompute };
	const auto points = ir->module()->entry_points();
	const auto &entry_inst = *points.begin();
	if (std::next(points.begin()) != points.end() || entry_inst.GetSingleWordInOperand(0) != bits(models.at(static_cast<unsigned>(stage)))) {
		return fail("Expected exactly one SPIR-V entry point for the selected stage");
	}
	entry = entry_inst.GetInOperand(2).AsString();
	spvtools::opt::FlattenDecorationPass flatten;
	spvtools::opt::StripNonSemanticInfoPass nonsemantic;
	if (!run(flatten) || !run(nonsemantic)) {
		return false;
	}
	if (!normalize_memory_qualifiers(*this)) {
		return false;
	}
	std::vector<Instruction *> hint_decorations;
	std::vector<uint32_t> storage_blocks, storage_vars;
	for (const auto &var : ir->types_values()) {
		if (var.opcode() == spv::Op::OpVariable && var.GetSingleWordInOperand(0) == bits(spv::StorageClass::StorageBuffer)) {
			storage_blocks.push_back(pointee(*ir, var)->result_id());
			storage_vars.push_back(var.result_id());
		}
	}
	for (auto &inst : ir->annotations()) {
		const bool member = inst.opcode() == spv::Op::OpMemberDecorate;
		if (member || inst.opcode() == spv::Op::OpDecorate) {
			uint32_t deco = inst.GetSingleWordInOperand(member ? 2 : 1);
			const auto &storage = member ? storage_blocks : storage_vars;
			if (deco == bits(spv::Decoration::NonReadable) && std::find(storage.begin(), storage.end(), inst.GetSingleWordInOperand(0)) != storage.end()) {
				hint_decorations.push_back(&inst);
			}
		}
	}
	for (auto *inst : hint_decorations) {
		// Write-only buffers are represented as read_write,
		// with the original (validated) stores unchanged.
		ir->KillInst(inst);
	}
	return true;
}

bool Module::run(spvtools::opt::Pass &p_pass) {
	GODOT_WEBGPU_ASSERT(ir);
	p_pass.SetMessageConsumer([this](spv_message_level_t p_level, const char *, const spv_position_t &, const char *p_msg) {
		if (p_level <= SPV_MSG_ERROR) {
			fail(p_msg);
		}
	});
	return p_pass.Run(ir.get()) != spvtools::opt::Pass::Status::Failure || fail(std::string("SPIR-V pass failed: ") + p_pass.name());
}

std::vector<uint32_t> Module::binary() const {
	GODOT_WEBGPU_ASSERT(ir);
	std::vector<uint32_t> output;
	ir->module()->ToBinary(&output, false);
	return output;
}

Instruction *def(IRContext &p_ir, uint32_t p_id) {
	return p_ir.get_def_use_mgr()->GetDef(p_id);
}

Instruction *pointee(IRContext &p_ir, const Instruction &p_inst) {
	Instruction *pointer = def(p_ir, p_inst.type_id());
	GODOT_WEBGPU_ASSERT(pointer->opcode() == spv::Op::OpTypePointer);
	return def(p_ir, pointer->GetSingleWordInOperand(1));
}

Instruction *global_origin(IRContext &p_ir, uint32_t p_id) {
	for (Instruction *i = def(p_ir, p_id); i; i = def(p_ir, i->GetSingleWordInOperand(0))) {
		switch (i->opcode()) {
			case spv::Op::OpVariable: return i;
			case spv::Op::OpLoad:
			case spv::Op::OpAccessChain:
			case spv::Op::OpInBoundsAccessChain:
			case spv::Op::OpCopyObject:
			case spv::Op::OpImage:
			case spv::Op::OpSampledImage: break;
			default: return nullptr;
		}
	}
	return nullptr;
}

void replace_operand(IRContext &p_ir, Instruction &r_inst, uint32_t p_index, uint32_t p_word) {
	p_ir.ForgetUses(&r_inst);
	r_inst.SetInOperand(p_index, { p_word });
	p_ir.AnalyzeUses(&r_inst);
}

static void insert(IRContext &p_ir, Instruction &r_before, std::unique_ptr<Instruction> p_inst) {
	p_ir.get_def_use_mgr()->AnalyzeInstDefUse(p_inst.get());
	p_ir.set_instr_block(p_inst.get(), p_ir.get_instr_block(&r_before));
	r_before.InsertBefore(std::move(p_inst));
}

uint32_t insert_value(IRContext &p_ir, Instruction &r_before, spv::Op p_op, uint32_t p_type, Operands p_operands) {
	uint32_t result = p_ir.TakeNextId();
	insert(p_ir, r_before, std::make_unique<Instruction>(&p_ir, p_op, p_type, result, std::move(p_operands)));
	return result;
}

void insert_inst(IRContext &p_ir, Instruction &r_before, spv::Op p_op, Operands p_operands) {
	insert(p_ir, r_before, std::make_unique<Instruction>(&p_ir, p_op, 0, 0, std::move(p_operands)));
}

uint32_t type_inst(IRContext &p_ir, Instruction &r_before, spv::Op p_opcode, const Operands &p_operands) {
	for (const Instruction &i : p_ir.types_values()) {
		if (&i == &r_before) {
			break;
		}
		if (i.opcode() != p_opcode || i.NumInOperands() != p_operands.size()) {
			continue;
		}
		uint32_t j = 0;
		while (j < p_operands.size() && i.GetInOperand(j).words == p_operands[j].words) {
			++j;
		}
		if (j == p_operands.size()) {
			return i.result_id();
		}
	}
	uint32_t result = p_ir.TakeNextId();
	auto type = std::make_unique<Instruction>(&p_ir, p_opcode, 0, result, p_operands);
	p_ir.get_def_use_mgr()->AnalyzeInstDefUse(type.get());
	r_before.InsertBefore(std::move(type));
	p_ir.InvalidateAnalyses(IRContext::kAnalysisTypes);
	return result;
}

void change_type(IRContext &p_ir, Instruction &r_inst, uint32_t p_type) {
	p_ir.ForgetUses(&r_inst);
	r_inst.SetResultType(p_type);
	p_ir.AnalyzeUses(&r_inst);
}

Instruction *decoration(IRContext &p_ir, uint32_t p_id, spv::Decoration p_kind) {
	for (Instruction *inst : p_ir.get_decoration_mgr()->GetDecorationsFor(p_id, false)) {
		if (inst->opcode() == spv::Op::OpDecorate && inst->GetSingleWordInOperand(1) == bits(p_kind)) {
			return inst;
		}
	}
	return nullptr;
}

void decorate(IRContext &p_ir, uint32_t p_id, spv::Decoration p_kind, std::optional<uint32_t> p_value) {
	Operands operands{ id(p_id), { SPV_OPERAND_TYPE_DECORATION, { bits(p_kind) } } };
	if (p_value) {
		operands.push_back(word(*p_value));
	}
	p_ir.AddAnnotationInst(std::make_unique<Instruction>(&p_ir, spv::Op::OpDecorate, 0, 0, std::move(operands)));
}

std::optional<uint32_t> constant_u32(IRContext &p_ir, uint32_t p_id) {
	const auto *constant = p_ir.get_constant_mgr()->FindDeclaredConstant(p_id);
	if (!constant || !constant->AsIntConstant() || constant->type()->AsInteger()->width() != 32) {
		return {};
	}
	return constant->GetU32();
}

uint32_t uint_constant(IRContext &p_ir, uint32_t p_value) {
	auto *types = p_ir.get_type_mgr();
	const auto *constant = p_ir.get_constant_mgr()->GetConstant(types->GetType(types->GetUIntTypeId()), { p_value });
	return p_ir.get_constant_mgr()->GetDefiningInstruction(constant)->result_id();
}

bool prune(Module &r_module) {
	spvtools::opt::EliminateDeadFunctionsPass functions;
	spvtools::opt::DeadBranchElimPass branches;
	spvtools::opt::AggressiveDCEPass instructions;
	spvtools::opt::RemoveUnusedInterfaceVariablesPass interface_vars;
	spvtools::opt::DeadVariableElimination variables;
	spvtools::opt::EliminateDeadConstantPass constants;
	return r_module.run(functions) && r_module.run(branches) && r_module.run(instructions) &&
			r_module.run(interface_vars) && r_module.run(variables) && r_module.run(constants);
}

} // namespace godot::webgpu::spirv
