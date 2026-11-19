#include "spirv_module.h"

#include "source/opt/remove_duplicates_pass.h"

#include <algorithm>
#include <limits>

namespace godot::webgpu::spirv {
namespace {

using Deco = spv::Decoration;
using Op = spv::Op;
using SC = spv::StorageClass;

std::optional<uint32_t> member_decoration(IRContext &p_ir, uint32_t p_struct, uint32_t p_member, Deco p_decoration) {
	for (const Instruction *d : p_ir.get_decoration_mgr()->GetDecorationsFor(p_struct, false)) {
		if (d->opcode() == Op::OpMemberDecorate && d->GetSingleWordInOperand(1) == p_member && d->GetSingleWordInOperand(2) == bits(p_decoration)) {
			return d->NumInOperands() > 3 ? d->GetSingleWordInOperand(3) : 1;
		}
	}
	return {};
}

// Byte range touched by the Vulkan block, not the size of a host C++ struct.
uint64_t block_extent(IRContext &p_ir, uint32_t p_type, uint32_t p_matrix_stride = 0, bool p_row_major = false) {
	const Instruction &type = *def(p_ir, p_type);
	auto at = [&type](uint32_t i) { return type.GetSingleWordInOperand(i); };
	switch (type.opcode()) {
		case Op::OpTypeInt:
		case Op::OpTypeFloat: return at(0) / 8;
		case Op::OpTypeVector: return block_extent(p_ir, at(0)) * at(1);
		case Op::OpTypeMatrix: {
			const Instruction &column = *def(p_ir, at(0));
			return uint64_t(p_matrix_stride) * (p_row_major ? column.GetSingleWordInOperand(1) : at(1));
		}
		case Op::OpTypeArray: {
			const Instruction *stride = decoration(p_ir, p_type, Deco::ArrayStride);
			return stride ? uint64_t(stride->GetSingleWordInOperand(2)) * constant_u32(p_ir, at(1)).value_or(0) : 0;
		}
		case Op::OpTypeStruct: {
			uint64_t extent = 0;
			for (uint32_t i = 0; i < type.NumInOperands(); ++i) {
				auto offset = member_decoration(p_ir, p_type, i, Deco::Offset);
				uint64_t size = block_extent(p_ir, at(i), member_decoration(p_ir, p_type, i, Deco::MatrixStride).value_or(0),
						member_decoration(p_ir, p_type, i, Deco::RowMajor).has_value());
				if (!offset || !size) {
					return 0;
				}
				extent = std::max(extent, *offset + size);
			}
			return extent;
		}
		default: return 0;
	}
}

bool same_format(const FormatPlan &a, const FormatPlan &b) {
	auto order = [](const FormatPlan &f) {
		return std::tie(f.logical_format, f.physical_format, f.dimension, f.arrayed, f.multisampled, f.depth_only_view,
				f.access, f.sample_type, f.conversion, f.component_bits, f.channels, f.atomic_buffer);
	};
	return order(a) == order(b);
}

uint32_t group_for(const BindingKey &p_key) {
	return std::min(p_key.set, FIRST_FOLDED_GROUP);
}

void count_binding(StageLimits &r_stage, Role p_role) {
	switch (p_role) {
		case Role::TEXTURE: ++r_stage.sampled_textures; break;
		case Role::SAMPLER: ++r_stage.samplers; break;
		case Role::PUSH_CONSTANT:
		case Role::STORAGE_BUFFER: ++r_stage.storage_buffers; break;
		case Role::STORAGE_TEXTURE: ++r_stage.storage_textures; break;
		case Role::UNIFORM_BUFFER: ++r_stage.uniform_buffers; break;
	}
}

bool remove_unused_global(IRContext &p_ir, Instruction &p_var) {
	// Entry-point interface lists and metadata do not make a resource statically used.
	if (!p_ir.get_def_use_mgr()->WhileEachUser(&p_var, [](const Instruction *p_use) {
			return p_use->opcode() == Op::OpEntryPoint || p_use->opcode() == Op::OpName || p_use->opcode() == Op::OpDecorate;
		})) {
		return false;
	}
	const uint32_t result_id = p_var.result_id();
	for (Instruction &entry : p_ir.module()->entry_points()) {
		// The interface follows the execution model, function ID and entry-point name.
		for (uint32_t i = 3; i < entry.NumInOperands(); ++i) {
			if (entry.GetSingleWordInOperand(i) == result_id) {
				p_ir.ForgetUses(&entry);
				entry.RemoveInOperand(i);
				p_ir.AnalyzeUses(&entry);
				break;
			}
		}
	}
	p_ir.KillNamesAndDecorates(result_id);
	p_ir.KillInst(&p_var);
	return true;
}

} // namespace

bool lower_push_constant(Module &r_module) {
	IRContext &ir = *r_module.ir;
	Instruction *var = nullptr;
	for (Instruction &inst : ir.types_values()) {
		if (inst.opcode() == Op::OpVariable && inst.GetSingleWordInOperand(0) == bits(SC::PushConstant)) {
			GODOT_WEBGPU_ASSERT(var == nullptr);
			var = &inst;
		}
	}
	if (!var || remove_unused_global(ir, *var)) {
		return true;
	}
	uint64_t size = block_extent(ir, pointee(ir, *var)->result_id());
	if (!size || size > std::numeric_limits<uint32_t>::max()) {
		return r_module.fail("Push constant block has no finite, explicit byte layout");
	}
	r_module.push_size = static_cast<uint32_t>(size);
	for (Instruction &inst : ir.types_values()) {
		if (inst.opcode() == Op::OpTypePointer && inst.GetSingleWordInOperand(0) == bits(SC::PushConstant)) {
			replace_operand(ir, inst, 0, bits(SC::StorageBuffer));
		}
	}
	replace_operand(ir, *var, 0, bits(SC::StorageBuffer));
	decorate(ir, var->result_id(), Deco::NonWritable);
	BindingPlan binding;
	binding.key = { FIRST_FOLDED_GROUP, 0, 0, Role::PUSH_CONSTANT };
	binding.read_only = true;
	binding.stages = stage_mask(r_module.stage);
	r_module.resources.push_back({ var->result_id(), binding });
	ir.InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
	spvtools::opt::RemoveDuplicatesPass duplicates;
	return r_module.run(duplicates);
}

bool plan_layout(const std::vector<std::unique_ptr<Module>> &p_modules, const CompileOptions &p_options, ShaderPlan &r_plan) {
	std::map<BindingKey, BindingPlan> logical;
	for (auto &module : p_modules) {
		IRContext &ir = *module->ir;
		auto &resources = module->resources;
		resources.erase(std::remove_if(resources.begin(), resources.end(), [&](const Resource &p_resource) {
			if (!remove_unused_global(ir, *def(ir, p_resource.id))) {
				return false;
			}
			if (p_resource.binding.key.role == Role::PUSH_CONSTANT) {
				module->push_size = 0;
			}
			return true;
		}), resources.end());
		r_plan.push_constant.size = std::max(r_plan.push_constant.size, module->push_size);
		for (const auto &resource : resources) {
			const BindingPlan &incoming = resource.binding;
			auto [slot, fresh] = logical.emplace(incoming.key, incoming);
			if (!fresh && (slot->second.read_only != incoming.read_only || slot->second.sampler_type != incoming.sampler_type ||
							  !same_format(slot->second.format, incoming.format))) {
				r_plan.diagnostic = "Incompatible binding across stages: set=" + std::to_string(incoming.key.set) +
						" binding=" + std::to_string(incoming.key.binding) + " element=" + std::to_string(incoming.key.element);
				return false;
			}
			slot->second.stages |= incoming.stages;
		}
	}
	std::map<std::tuple<uint32_t, uint32_t, SamplerType>, size_t> immutable;
	std::map<BindingKey, size_t> indices;
	std::array<uint32_t, FIRST_FOLDED_GROUP + 1> next{};
	for (auto &[key, binding] : logical) {
		if (key.role == Role::PUSH_CONSTANT) {
			continue; // Last in group 3, after expanded material/folded bindings.
		}
		binding.group = group_for(key);
		auto sampler = p_options.immutable_samplers.find(key);
		if (key.role == Role::SAMPLER && sampler != p_options.immutable_samplers.end()) {
			auto [candidate, fresh] = immutable.emplace(std::make_tuple(binding.group, sampler->second, binding.sampler_type), r_plan.bindings.size());
			if (!fresh) {
				BindingPlan &primary = r_plan.bindings[candidate->second];
				primary.aliases.push_back(key);
				primary.stages |= binding.stages;
				indices.emplace(key, candidate->second);
				continue;
			}
		}
		binding.binding = next[binding.group]++;
		indices.emplace(key, r_plan.bindings.size());
		r_plan.bindings.push_back(binding);
	}
	if (auto pc = logical.find({ FIRST_FOLDED_GROUP, 0, 0, Role::PUSH_CONSTANT }); pc != logical.end()) {
		auto &binding = pc->second;
		binding.group = FIRST_FOLDED_GROUP;
		binding.binding = next[binding.group]++;
		r_plan.push_constant.group = binding.group;
		r_plan.push_constant.binding = binding.binding;
		r_plan.push_constant.stages = binding.stages;
		indices.emplace(pc->first, r_plan.bindings.size());
		r_plan.bindings.push_back(binding);
	}
	for (const BindingPlan &binding : r_plan.bindings) {
		for (uint32_t s = 0; s < SHADER_STAGE_COUNT; ++s) {
			if (binding.stages & stage_mask(static_cast<Stage>(s))) {
				count_binding(r_plan.limits[s], binding.key.role);
			}
		}
	}
	for (auto &module : p_modules) {
		IRContext &ir = *module->ir;
		std::map<size_t, uint32_t> primaries;
		for (const Resource &resource : module->resources) {
			const size_t idx = indices.at(resource.binding.key);
			const auto [primary, fresh] = primaries.emplace(idx, resource.id);
			if (!fresh) {
				ir.KillNamesAndDecorates(resource.id);
				ir.ReplaceAllUsesWith(resource.id, primary->second);
				ir.KillDef(resource.id);
				continue;
			}
			const BindingPlan &binding = r_plan.bindings[idx];
			for (const auto &[deco, value] : { std::pair{ Deco::DescriptorSet, binding.group }, std::pair{ Deco::Binding, binding.binding } }) {
				if (Instruction *d = decoration(ir, resource.id, deco)) {
					replace_operand(ir, *d, 2, value);
				} else {
					decorate(ir, resource.id, deco, value);
				}
			}
		}
	}
	return true;
}

} // namespace godot::webgpu::spirv
