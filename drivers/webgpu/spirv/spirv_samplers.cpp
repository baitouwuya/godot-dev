#include "spirv_module.h"

#include "source/opt/desc_sroa.h"
#include "source/opt/inline_opaque_pass.h"
#include "source/opt/replace_desc_array_access_using_var_index.h"
#include "source/opt/split_combined_image_sampler_pass.h"

#include <limits>
#include <set>

namespace godot::webgpu::spirv {
namespace {

struct DescriptorRange {
	BindingKey key;
	uint32_t count;
};

bool dynamic_descriptor(IRContext &p_ir, uint32_t p_id) {
	Instruction *inst = def(p_ir, p_id);
	switch (inst->opcode()) {
		case spv::Op::OpAccessChain:
		case spv::Op::OpInBoundsAccessChain: {
			const Instruction *base = def(p_ir, inst->GetSingleWordInOperand(0));
			return base->opcode() == spv::Op::OpVariable &&
					base->GetSingleWordInOperand(0) == bits(spv::StorageClass::UniformConstant) &&
					pointee(p_ir, *base)->opcode() == spv::Op::OpTypeArray &&
					!constant_u32(p_ir, inst->GetSingleWordInOperand(1));
		}
		case spv::Op::OpSampledImage:
			return dynamic_descriptor(p_ir, inst->GetSingleWordInOperand(0)) || dynamic_descriptor(p_ir, inst->GetSingleWordInOperand(1));
		case spv::Op::OpLoad:
		case spv::Op::OpImage:
		case spv::Op::OpCopyObject:
			return dynamic_descriptor(p_ir, inst->GetSingleWordInOperand(0));
		default: return false;
	}
}

bool prepare_dispatch(Module &r_module, std::vector<Instruction *> &r_dynamic_stores) {
	IRContext &ir = *r_module.ir;
	std::vector<Instruction *> samples;
	ir.module()->ForEachInst([&](Instruction *i) {
		switch (i->opcode()) {
			case spv::Op::OpImageSampleImplicitLod:
			case spv::Op::OpImageSampleDrefImplicitLod:
			case spv::Op::OpImageSampleProjImplicitLod:
			case spv::Op::OpImageSampleProjDrefImplicitLod:
			case spv::Op::OpImageGather:
			case spv::Op::OpImageDrefGather:
			case spv::Op::OpImageTexelPointer:
				if (dynamic_descriptor(ir, i->GetSingleWordInOperand(0))) {
					samples.push_back(i);
				}
				break;
			case spv::Op::OpImageWrite:
				if (dynamic_descriptor(ir, i->GetSingleWordInOperand(0))) {
					r_dynamic_stores.push_back(i);
				}
				break;
			default: break;
		}
	});
	for (Instruction *sample : samples) {
		// WGSL has no explicit-gradient equivalent for compare or gather.
		if (sample->opcode() != spv::Op::OpImageSampleImplicitLod || sample->NumInOperands() != 2) {
			return r_module.fail("Dynamic descriptor dispatch cannot lower this implicit-LOD, gather, or side-effecting operation");
		}
		uint32_t sampled = sample->GetSingleWordInOperand(0);
		uint32_t coords = sample->GetSingleWordInOperand(1);
		const Instruction *combined = def(ir, def(ir, sampled)->type_id());
		const Instruction *image = def(ir, combined->GetSingleWordInOperand(0));
		uint32_t vec_type = def(ir, coords)->type_id();
		if (image->GetSingleWordInOperand(3)) { // Array coordinate includes the non-spatial layer.
			const Instruction *vec = def(ir, vec_type);
			uint32_t width = vec->GetSingleWordInOperand(1) - 1;
			if (width < 2) {
				return r_module.fail("Dynamic implicit sampling of an arrayed 1D texture cannot be represented in WGSL");
			}
			vec_type = ir.get_type_mgr()->GetFloatVectorTypeId(width);
			Operands operands{ id(coords), id(coords) };
			for (uint32_t j = 0; j < width; ++j) {
				operands.push_back(word(j));
			}
			coords = insert_value(ir, *sample, spv::Op::OpVectorShuffle, vec_type, std::move(operands));
		}
		uint32_t dx = insert_value(ir, *sample, spv::Op::OpDPdx, vec_type, { id(coords) });
		uint32_t dy = insert_value(ir, *sample, spv::Op::OpDPdy, vec_type, { id(coords) });
		ir.ForgetUses(sample);
		sample->SetOpcode(spv::Op::OpImageSampleExplicitLod);
		sample->AddOperand({ SPV_OPERAND_TYPE_IMAGE, { bits(spv::ImageOperandsMask::Grad) } });
		sample->AddOperand(id(dx));
		sample->AddOperand(id(dy));
		ir.AnalyzeUses(sample);
	}
	return true;
}

bool lower_multisampled_fetches(Module &r_module) {
	IRContext &ir = *r_module.ir;
	std::vector<Instruction *> extracts;
	bool supported = true;
	ir.module()->ForEachInst([&](Instruction *p_inst) {
		if (p_inst->opcode() != spv::Op::OpSampledImage) {
			return;
		}
		const auto *combined = def(ir, p_inst->type_id());
		const auto *image = def(ir, combined->GetSingleWordInOperand(0));
		if (!image->GetSingleWordInOperand(4)) {
			return;
		}
		ir.get_def_use_mgr()->ForEachUser(p_inst, [&](Instruction *p_use) {
			if (p_use->opcode() == spv::Op::OpImage) {
				extracts.push_back(p_use);
			} else if (p_use->opcode() != spv::Op::OpName && !p_use->IsDecoration()) {
				supported = false;
			}
		});
	});
	if (!supported) {
		return r_module.fail("A multisampled texture cannot use a sampler in WGSL");
	}
	for (Instruction *extract : extracts) {
		// sampler2DMS texelFetch consumes only the image, not the sampler.
		const auto *combined = def(ir, extract->GetSingleWordInOperand(0));
		ir.ReplaceAllUsesWith(extract->result_id(), combined->GetSingleWordInOperand(0));
		ir.KillInst(extract);
	}
	return true;
}

Role resource_role(const Instruction &p_var, const Instruction &p_type) {
	if (p_type.opcode() == spv::Op::OpTypeImage) {
		return p_type.GetSingleWordInOperand(5) == 2 ? Role::STORAGE_TEXTURE : Role::TEXTURE;
	}
	if (p_type.opcode() == spv::Op::OpTypeSampler) {
		return Role::SAMPLER;
	}
	return p_var.GetSingleWordInOperand(0) == bits(spv::StorageClass::StorageBuffer) ? Role::STORAGE_BUFFER : Role::UNIFORM_BUFFER;
}

bool is_non_writable(IRContext &p_ir, const Instruction &p_var, const Instruction &p_type) {
	if (decoration(p_ir, p_var.result_id(), spv::Decoration::NonWritable)) {
		return true;
	}
	std::set<uint32_t> readonly;
	for (const Instruction *d : p_ir.get_decoration_mgr()->GetDecorationsFor(p_type.result_id(), false)) {
		if (d->opcode() == spv::Op::OpMemberDecorate && d->GetSingleWordInOperand(2) == bits(spv::Decoration::NonWritable)) {
			readonly.insert(d->GetSingleWordInOperand(1));
		}
	}
	return readonly.size() == p_type.NumInOperands();
}

bool storage_pointer_is_read_only(IRContext &p_ir, uint32_t p_pointer, std::set<uint32_t> &r_seen) {
	if (!r_seen.insert(p_pointer).second) {
		return true;
	}
	return p_ir.get_def_use_mgr()->WhileEachUser(p_pointer, [&](Instruction *p_user) {
		if (p_user->IsDecoration() || p_user->opcode() == spv::Op::OpName || p_user->opcode() == spv::Op::OpEntryPoint) {
			return true;
		}
		switch (p_user->opcode()) {
			case spv::Op::OpLoad:
			case spv::Op::OpArrayLength:
				return p_user->GetSingleWordInOperand(0) == p_pointer;
			case spv::Op::OpAccessChain:
			case spv::Op::OpInBoundsAccessChain:
			case spv::Op::OpCopyObject:
				return p_user->GetSingleWordInOperand(0) == p_pointer && storage_pointer_is_read_only(p_ir, p_user->result_id(), r_seen);
			default:
				return false;
		}
	});
}

} // namespace

bool lower_bindings(Module &r_module, const CompileOptions &) {
	IRContext &ir = *r_module.ir;
	spvtools::opt::InlineOpaquePass inline_opaque;
	if (!r_module.run(inline_opaque) || !prune(r_module)) {
		return false;
	}
	std::map<uint32_t, DescriptorRange> ranges;
	uint32_t next = 0;
	bool has_arrays = false;
	for (Instruction &var : ir.types_values()) {
		if (var.opcode() != spv::Op::OpVariable) {
			continue;
		}
		Instruction *set = decoration(ir, var.result_id(), spv::Decoration::DescriptorSet);
		Instruction *binding = decoration(ir, var.result_id(), spv::Decoration::Binding);
		if (!set || !binding) {
			continue;
		}
		const Instruction *type = pointee(ir, var);
		if (type->opcode() == spv::Op::OpTypeRuntimeArray) {
			return r_module.fail("Runtime-sized descriptor arrays require a concrete binding count");
		}
		uint32_t size = 1;
		if (type->opcode() == spv::Op::OpTypeArray) {
			const Instruction *element = def(ir, type->GetSingleWordInOperand(0));
			if (element->opcode() != spv::Op::OpTypeImage && element->opcode() != spv::Op::OpTypeSampler && element->opcode() != spv::Op::OpTypeSampledImage) {
				return r_module.fail("Only one-dimensional fixed-size texture and sampler descriptor arrays are supported");
			}
			const auto count = constant_u32(ir, type->GetSingleWordInOperand(1));
			if (!count || !*count || *count > std::numeric_limits<uint32_t>::max() - next) {
				return r_module.fail("Descriptor array has an invalid or unfrozen length");
			}
			size = *count;
			has_arrays = true;
		}
		if (next == std::numeric_limits<uint32_t>::max()) {
			return r_module.fail("Descriptor provenance space exhausted");
		}
		ranges.emplace(next, DescriptorRange{ { set->GetSingleWordInOperand(2), binding->GetSingleWordInOperand(2) }, size });
		// SROA offsets the input binding by the element index. Assign non-overlapping
		// temporary ranges so adjacent Vulkan bindings never lose their provenance.
		replace_operand(ir, *binding, 2, next);
		next += size;
	}
	if (has_arrays) {
		spvtools::opt::ReplaceDescArrayAccessUsingVarIndex dispatch;
		spvtools::opt::DescriptorScalarReplacement scalar(false, true);
		std::vector<Instruction *> dynamic_stores;
		if (!prepare_dispatch(r_module, dynamic_stores) || !r_module.run(dispatch)) {
			return false;
		}
		for (Instruction *store : dynamic_stores) {
			// Singleton arrays are rewritten in place. For larger arrays the
			// dispatch pass clones the store in each case and leaves the original.
			if (dynamic_descriptor(ir, store->GetSingleWordInOperand(0))) {
				ir.KillInst(store);
			}
		}
		if (!prune(r_module) || !r_module.run(scalar)) {
			return false;
		}
	}
	spvtools::opt::SplitCombinedImageSamplerPass split;
	if (!r_module.run(split) || !lower_multisampled_fetches(r_module) || !prune(r_module)) {
		return false;
	}
	for (Instruction &var : ir.types_values()) {
		if (var.opcode() != spv::Op::OpVariable) {
			continue;
		}
		const Instruction *binding = decoration(ir, var.result_id(), spv::Decoration::Binding);
		if (!binding) {
			continue;
		}
		uint32_t tag = binding->GetSingleWordInOperand(2);
		auto range = ranges.upper_bound(tag);
		GODOT_WEBGPU_ASSERT(range != ranges.begin());
		--range;
		const auto *type = pointee(ir, var);
		BindingPlan plan;
		plan.key = range->second.key;
		plan.key.element = tag - range->first;
		GODOT_WEBGPU_ASSERT(plan.key.element < range->second.count);
		plan.key.role = resource_role(var, *type);
		plan.stages = stage_mask(r_module.stage);
		plan.read_only = plan.key.role != Role::STORAGE_BUFFER || is_non_writable(ir, var, *type);
		if (!plan.read_only && r_module.stage == Stage::VERTEX) {
			// VoxelGI debug declares a writable buffer but only loads from it.
			std::set<uint32_t> seen;
			if (!storage_pointer_is_read_only(ir, var.result_id(), seen)) {
				return r_module.fail("Writable storage buffer access is not supported in the WGSL vertex stage");
			}
			decorate(ir, var.result_id(), spv::Decoration::NonWritable);
			plan.read_only = true;
		}
		if (plan.key.role == Role::SAMPLER) {
			plan.sampler_type = SamplerType::FILTERING;
		}
		r_module.resources.push_back({ var.result_id(), plan });
	}
	return true;
}

} // namespace godot::webgpu::spirv
