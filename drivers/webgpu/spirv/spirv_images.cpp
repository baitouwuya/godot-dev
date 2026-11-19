#include "spirv_module.h"
#include "spirv_storage_formats.h"

#include <spirv/unified1/GLSL.std.450.h>

#include "source/opt/remove_duplicates_pass.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace godot::webgpu::spirv {
namespace {

using Op = spv::Op;
using Format = spv::ImageFormat;
using Deco = spv::Decoration;

bool comparison(Op p_op) {
	switch (p_op) {
		case Op::OpImageSampleDrefImplicitLod:
		case Op::OpImageSampleDrefExplicitLod:
		case Op::OpImageSampleProjDrefImplicitLod:
		case Op::OpImageSampleProjDrefExplicitLod:
		case Op::OpImageDrefGather: return true;
		default: return false;
	}
}

bool sampling(Op p_op) {
	return comparison(p_op) || p_op == Op::OpImageSampleImplicitLod || p_op == Op::OpImageSampleExplicitLod ||
			p_op == Op::OpImageSampleProjImplicitLod || p_op == Op::OpImageSampleProjExplicitLod || p_op == Op::OpImageGather;
}

// SPIRV-Tools can leave opaque calls that have early returns. A texture can
// therefore acquire its depth type from a comparison in a callee (Mobile PCF).
class ImageOrigins {
	IRContext &ir;
	std::map<uint32_t, std::vector<uint32_t>> arguments;

	void trace(uint32_t p_id, bool p_sampler, std::set<uint32_t> &r_seen, std::set<uint32_t> &r_roots) {
		if (!r_seen.insert(p_id).second) {
			return;
		}
		const auto *inst = def(ir, p_id);
		switch (inst->opcode()) {
			case Op::OpVariable:
				r_roots.insert(p_id);
				break;
			case Op::OpLoad:
			case Op::OpAccessChain:
			case Op::OpInBoundsAccessChain:
			case Op::OpCopyObject:
			case Op::OpImage:
			case Op::OpSampledImage:
				trace(inst->GetSingleWordInOperand(p_sampler && inst->opcode() == Op::OpSampledImage ? 1 : 0), p_sampler, r_seen, r_roots);
				break;
			case Op::OpFunctionParameter:
				if (auto it = arguments.find(p_id); it != arguments.end()) {
					for (uint32_t argument : it->second) {
						trace(argument, p_sampler, r_seen, r_roots);
					}
				}
				break;
			default:
				break;
		}
	}

public:
	explicit ImageOrigins(IRContext &p_ir) :
			ir(p_ir) {
		ir.module()->ForEachInst([&](Instruction *p_inst) {
			if (p_inst->opcode() != Op::OpFunctionCall) {
				return;
			}
			const auto *function = ir.GetFunction(p_inst->GetSingleWordInOperand(0));
			GODOT_WEBGPU_ASSERT(function);
			uint32_t operand = 1;
			function->ForEachParam([&](const Instruction *p_param) {
				const uint32_t argument = p_inst->GetSingleWordInOperand(operand++);
				const auto *type = def(ir, p_param->type_id());
				if (type->opcode() == Op::OpTypePointer) {
					type = def(ir, type->GetSingleWordInOperand(1));
				}
				if (type->opcode() == Op::OpTypeImage || type->opcode() == Op::OpTypeSampledImage || type->opcode() == Op::OpTypeSampler) {
					arguments[p_param->result_id()].push_back(argument);
				}
			});
		});
	}

	std::set<uint32_t> find(uint32_t p_id, bool p_sampler = false) {
		std::set<uint32_t> seen, roots;
		trace(p_id, p_sampler, seen, roots);
		return roots;
	}
};

Operands operands_of(const Instruction &p_inst) {
	Operands operands;
	for (uint32_t i = 0; i < p_inst.NumInOperands(); ++i) {
		operands.push_back(p_inst.GetInOperand(i));
	}
	return operands;
}

// After opaque inlining and descriptor scalarization there are no opaque phi
// nodes. Change the type of the handle, not other variables sharing its type.
bool retype_image(Module &r_module, Instruction &r_var, Operands p_operands) {
	IRContext &ir = *r_module.ir;
	const uint32_t image = type_inst(ir, r_var, Op::OpTypeImage, p_operands);
	const uint32_t pointer = type_inst(ir, r_var, Op::OpTypePointer, { r_var.GetInOperand(0), id(image) });
	std::vector<Instruction *> work{ &r_var };
	std::set<Instruction *> seen;
	while (!work.empty()) {
		Instruction *inst = work.back();
		work.pop_back();
		if (!seen.insert(inst).second) {
			continue;
		}
		uint32_t new_type;
		switch (inst->opcode()) {
			case Op::OpVariable: new_type = pointer; break;
			case Op::OpLoad:
			case Op::OpImage: new_type = image; break;
			case Op::OpSampledImage: new_type = type_inst(ir, r_var, Op::OpTypeSampledImage, { id(image) }); break;
			case Op::OpCopyObject: {
				new_type = def(ir, inst->GetSingleWordInOperand(0))->type_id();
				break;
			}
			case Op::OpFunctionCall:
			case Op::OpPhi:
			case Op::OpSelect: return r_module.fail("Remapped image escapes the statically typed resource chain");
			default: continue;
		}
		change_type(ir, *inst, new_type);
		ir.get_def_use_mgr()->ForEachUser(inst, [&](Instruction *use) { work.push_back(use); });
	}
	return true;
}

uint32_t scalar_constant(IRContext &p_ir, uint32_t p_type, float p_value) {
	const auto *type = p_ir.get_type_mgr()->GetType(p_type);
	uint32_t payload = static_cast<uint32_t>(p_value);
	if (type->AsFloat()) {
		static_assert(sizeof(payload) == sizeof(p_value));
		std::memcpy(&payload, &p_value, sizeof(payload));
	}
	const auto *value = p_ir.get_constant_mgr()->GetConstant(type, { payload });
	return p_ir.get_constant_mgr()->GetDefiningInstruction(value)->result_id();
}

uint32_t quantize_norm(IRContext &p_ir, Instruction &r_at, uint32_t p_scalar, uint32_t p_value, const FormatPlan &p_plan) {
	const bool signed_norm = p_plan.conversion == FormatConversion::SNORM;
	const float max = static_cast<float>((1u << (p_plan.component_bits - signed_norm)) - 1u);
	const uint32_t limit = scalar_constant(p_ir, p_scalar, max);
	const uint32_t lo = scalar_constant(p_ir, p_scalar, signed_norm ? -1.0f : 0.0f);
	const uint32_t hi = scalar_constant(p_ir, p_scalar, 1.0f);
	uint32_t glsl = p_ir.get_feature_mgr()->GetExtInstImportId_GLSLstd450();
	if (!glsl) {
		p_ir.AddExtInstImport("GLSL.std.450");
		glsl = p_ir.get_feature_mgr()->GetExtInstImportId_GLSLstd450();
	}
	p_value = insert_value(p_ir, r_at, Op::OpExtInst, p_scalar, { id(glsl), word(GLSLstd450FClamp), id(p_value), id(lo), id(hi) });
	p_value = insert_value(p_ir, r_at, Op::OpFMul, p_scalar, { id(p_value), id(limit) });
	p_value = insert_value(p_ir, r_at, Op::OpExtInst, p_scalar, { id(glsl), word(GLSLstd450RoundEven), id(p_value) });
	return insert_value(p_ir, r_at, Op::OpFDiv, p_scalar, { id(p_value), id(limit) });
}

void preserve_channels(IRContext &p_ir, Instruction &r_io, const FormatPlan &p_plan) {
	const bool store = r_io.opcode() == Op::OpImageWrite;
	const uint32_t value = store ? r_io.GetSingleWordInOperand(2) : r_io.result_id();
	const uint32_t type = def(p_ir, value)->type_id();
	const uint32_t scalar = def(p_ir, type)->GetSingleWordInOperand(0);
	GODOT_WEBGPU_ASSERT(def(p_ir, type)->GetSingleWordInOperand(1) == 4);
	Instruction &at = store ? r_io : *r_io.NextNode();
	std::vector<Instruction *> old_uses;
	if (!store) {
		p_ir.get_def_use_mgr()->ForEachUser(&r_io, [&](Instruction *use) { old_uses.push_back(use); });
	}
	Operands components;
	for (uint32_t c = 0; c < 4; ++c) {
		uint32_t channel;
		if (c < p_plan.channels) {
			channel = insert_value(p_ir, at, Op::OpCompositeExtract, scalar, { id(value), word(c) });
			if (store && (p_plan.conversion == FormatConversion::UNORM || p_plan.conversion == FormatConversion::SNORM)) {
				channel = quantize_norm(p_ir, at, scalar, channel, p_plan);
			}
		} else {
			channel = scalar_constant(p_ir, scalar, c == 3 ? 1.0f : 0.0f);
		}
		components.push_back(id(channel));
	}
	const auto result = insert_value(p_ir, at, Op::OpCompositeConstruct, type, std::move(components));
	if (store) {
		replace_operand(p_ir, r_io, 2, result);
	} else {
		for (Instruction *use : old_uses) {
			p_ir.ForgetUses(use);
			use->ForEachInId([&](uint32_t *operand) { if (*operand == value) { *operand = result; } });
			p_ir.AnalyzeUses(use);
		}
	}
}

bool contains(const std::vector<BindingKey> &p_bindings, const BindingKey &p_key) {
	return std::find_if(p_bindings.begin(), p_bindings.end(), [&](const auto &key) {
		return key.set == p_key.set && key.binding == p_key.binding && key.element == p_key.element;
	}) != p_bindings.end();
}

} // namespace

bool lower_images(Module &r_module, const CompileOptions &p_options) {
	IRContext &ir = *r_module.ir;
	ImageOrigins origins(ir);
	std::map<uint32_t, std::vector<Instruction *>> image_uses;
	std::map<uint32_t, std::vector<std::pair<uint32_t, bool>>> sampler_uses;
	bool known_storage_uses = true;
	ir.module()->ForEachInst([&](Instruction *i) {
		const Op opcode = i->opcode();
		const bool sampled = sampling(opcode);
		if (!sampled && opcode != Op::OpImageRead && opcode != Op::OpImageSparseRead && opcode != Op::OpImageWrite && opcode != Op::OpImageTexelPointer) {
			return;
		}
		const uint32_t operand = i->GetSingleWordInOperand(0);
		const auto textures = origins.find(operand);
		for (uint32_t texture : textures) {
			image_uses[texture].push_back(i);
		}
		if (sampled) {
			for (uint32_t sampler : origins.find(operand, true)) {
				if (textures.empty()) {
					sampler_uses[sampler].emplace_back(0, comparison(opcode));
				} else {
					for (uint32_t texture : textures) {
						sampler_uses[sampler].emplace_back(texture, comparison(opcode));
					}
				}
			}
		} else {
			known_storage_uses &= !textures.empty();
		}
	});
	std::map<uint32_t, SampleType> sample_types;
	for (Resource &resource : r_module.resources) {
		BindingPlan &binding = resource.binding;
		if (binding.key.role != Role::TEXTURE && binding.key.role != Role::STORAGE_TEXTURE) {
			continue;
		}
		Instruction &var = *def(ir, resource.id);
		const Instruction &image = *pointee(ir, var);
		const auto &uses = image_uses[resource.id];
		FormatPlan &format = binding.format;
		format.dimension = image.GetSingleWordInOperand(1);
		format.arrayed = image.GetSingleWordInOperand(3) != 0;
		format.multisampled = image.GetSingleWordInOperand(4) != 0;
		auto operands = operands_of(image);
		if (binding.key.role == Role::TEXTURE) {
			const bool dref = std::any_of(uses.begin(), uses.end(), [](const Instruction *i) { return comparison(i->opcode()); });
			const auto &type = *def(ir, image.GetSingleWordInOperand(0));
			const bool explicit_depth = contains(p_options.raw_depth, binding.key);
			if (dref || explicit_depth || image.GetSingleWordInOperand(2) == 1) {
				format.sample_type = SampleType::DEPTH;
				format.depth_only_view = true;
				// Tint infers depth on the whole call chain for comparison
				// sampling. Only raw depth without a Dref needs retyping.
				if (explicit_depth && !dref && image.GetSingleWordInOperand(2) != 1) {
					operands[2].words = { 1 };
					if (!retype_image(r_module, var, std::move(operands))) {
						return false;
					}
				}
			} else if (type.opcode() == Op::OpTypeInt) {
				format.sample_type = type.GetSingleWordInOperand(1) ? SampleType::SINT : SampleType::UINT;
			} else {
				format.sample_type = format.multisampled || contains(p_options.unfilterable_float, binding.key) ? SampleType::UNFILTERABLE_FLOAT : SampleType::FLOAT;
			}
			sample_types[resource.id] = format.sample_type;
			continue;
		}
		format.logical_format = image.GetSingleWordInOperand(6);
		if (format.logical_format == bits(Format::Unknown)) {
			auto supplied = p_options.storage_formats.find(binding.key);
			if (supplied == p_options.storage_formats.end() || supplied->second == bits(Format::Unknown)) {
				return r_module.fail("Formatless storage image needs the real view format (set " + std::to_string(binding.key.set) +
						", binding " + std::to_string(binding.key.binding) + ")");
			}
			format.logical_format = supplied->second;
		}
		if (std::any_of(uses.begin(), uses.end(), [](const Instruction *i) { return i->opcode() == Op::OpImageTexelPointer; })) {
			if (!lower_image_atomics(r_module, resource, uses)) {
				return false;
			}
			continue;
		}
		bool non_writable = decoration(ir, resource.id, Deco::NonWritable);
		bool non_readable = decoration(ir, resource.id, Deco::NonReadable);
		if (!non_writable && !non_readable && known_storage_uses) {
			// GLSL allows a declaration shared by variants that use different
			// access modes. Keep only what the scalarized WGSL binding needs.
			const bool reads = std::any_of(uses.begin(), uses.end(), [](const Instruction *i) {
				return i->opcode() == Op::OpImageRead || i->opcode() == Op::OpImageSparseRead;
			});
			const bool writes = std::any_of(uses.begin(), uses.end(), [](const Instruction *i) { return i->opcode() == Op::OpImageWrite; });
			if (reads != writes) {
				non_writable = reads;
				non_readable = writes;
				decorate(ir, resource.id, reads ? Deco::NonWritable : Deco::NonReadable);
			}
		}
		format.access = non_writable ? ImageAccess::READ : non_readable ? ImageAccess::WRITE : ImageAccess::READ_WRITE;
		binding.read_only = non_writable;
		format.physical_format = format.logical_format;
		if (p_options.promote_narrow_storage) {
			if (const auto *rule = storage_promotion(static_cast<Format>(format.logical_format))) {
				format.physical_format = bits(rule->physical);
				format.channels = rule->channels;
				format.component_bits = rule->precision;
				format.conversion = rule->conversion;
			}
		}
		if (image.GetSingleWordInOperand(6) != format.physical_format) {
			operands[6].words = { format.physical_format };
			if (!retype_image(r_module, var, std::move(operands))) {
				return false;
			}
		}
		if (format.channels) {
			for (Instruction *i : uses) {
				if (i->opcode() == Op::OpImageWrite || i->opcode() == Op::OpImageRead) {
					preserve_channels(ir, *i, format);
				}
			}
		}
	}
	for (Resource &resource : r_module.resources) {
		if (resource.binding.key.role != Role::SAMPLER) {
			continue;
		}
		bool compare = false, regular = false, unfilterable = false;
		for (const auto &[tex, dref] : sampler_uses[resource.id]) {
			compare |= dref;
			regular |= !dref;
			if (auto type = sample_types.find(tex); type != sample_types.end()) {
				unfilterable |= type->second == SampleType::UNFILTERABLE_FLOAT || (type->second == SampleType::DEPTH && !dref);
			}
		}
		if (compare && regular) {
			return r_module.fail("One logical sampler cannot be both comparison and ordinary");
		}
		resource.binding.sampler_type = compare ? SamplerType::COMPARISON : unfilterable ? SamplerType::NON_FILTERING : SamplerType::FILTERING;
	}
	ir.InvalidateAnalysesExceptFor(IRContext::kAnalysisNone);
	spvtools::opt::RemoveDuplicatesPass duplicates;
	return r_module.run(duplicates);
}

} // namespace godot::webgpu::spirv
