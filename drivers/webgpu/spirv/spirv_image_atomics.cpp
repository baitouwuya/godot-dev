#include "spirv_module.h"

#include <set>

namespace godot::webgpu::spirv {
namespace {

using Op = spv::Op;
using Deco = spv::Decoration;
using SC = spv::StorageClass;
using spvtools::opt::BasicBlock;
using spvtools::opt::Function;

constexpr uint32_t texels_member = ATOMIC_IMAGE_HEADER_BYTES / SPIRV_WORD_BYTES;

uint32_t decorated_type(IRContext &p_ir, Instruction &r_var, Op p_op, Operands p_operands) {
	uint32_t result = p_ir.TakeNextId();
	auto inst = std::make_unique<Instruction>(&p_ir, p_op, 0, result, std::move(p_operands));
	p_ir.get_def_use_mgr()->AnalyzeInstDefUse(inst.get());
	r_var.InsertBefore(std::move(inst));
	return result;
}

void offset(IRContext &p_ir, uint32_t p_type, uint32_t p_member, uint32_t p_offset) {
	p_ir.AddAnnotationInst(std::make_unique<Instruction>(&p_ir, Op::OpMemberDecorate, 0, 0,
			Operands{ id(p_type), word(p_member), { SPV_OPERAND_TYPE_DECORATION, { bits(Deco::Offset) } }, word(p_offset) }));
}

bool texel_atomic(Op p_op) {
	switch (p_op) {
		case Op::OpAtomicLoad:
		case Op::OpAtomicStore:
		case Op::OpAtomicExchange:
		case Op::OpAtomicCompareExchange:
		case Op::OpAtomicIIncrement:
		case Op::OpAtomicIDecrement:
		case Op::OpAtomicIAdd:
		case Op::OpAtomicISub:
		case Op::OpAtomicSMin:
		case Op::OpAtomicUMin:
		case Op::OpAtomicSMax:
		case Op::OpAtomicUMax:
		case Op::OpAtomicAnd:
		case Op::OpAtomicOr:
		case Op::OpAtomicXor: return true;
		default: return false;
	}
}

bool interface_use(Op p_op) {
	return p_op == Op::OpEntryPoint || p_op == Op::OpName || p_op == Op::OpDecorate;
}

uint32_t emit(IRContext &p_ir, BasicBlock &r_block, Op p_op, uint32_t p_type, Operands p_operands = {}) {
	const uint32_t result = p_type ? p_ir.TakeNextId() : 0;
	r_block.AddInstruction(std::make_unique<Instruction>(&p_ir, p_op, p_type, result, std::move(p_operands)));
	return result;
}

struct ImageBuffer {
	uint32_t variable;
	uint32_t dimensions;
	uint32_t scalar;
	uint32_t u32;
	uint32_t bool_type;
	uint32_t extent_ptr;
	uint32_t texel_ptr;
	bool atomic;
};

struct AccessSignature {
	Op opcode;
	uint32_t result_type;
	std::vector<uint32_t> parameter_types;
	std::vector<uint32_t> memory_operands;

	bool operator<(const AccessSignature &p_other) const {
		return std::tie(opcode, result_type, parameter_types, memory_operands) <
				std::tie(p_other.opcode, p_other.result_type, p_other.parameter_types, p_other.memory_operands);
	}
};

uint32_t guarded_access(IRContext &p_ir, const ImageBuffer &p_image, const AccessSignature &p_access, uint32_t p_return_type) {
	Operands function_type{ id(p_return_type) };
	for (uint32_t type : p_access.parameter_types) {
		function_type.push_back(id(type));
	}
	const uint32_t type_id = p_ir.TakeNextId();
	p_ir.AddType(std::make_unique<Instruction>(&p_ir, Op::OpTypeFunction, 0, type_id, std::move(function_type)));
	const uint32_t function_id = p_ir.TakeNextId();
	auto function = std::make_unique<Function>(std::make_unique<Instruction>(&p_ir, Op::OpFunction, p_return_type, function_id,
			Operands{ { SPV_OPERAND_TYPE_FUNCTION_CONTROL, { bits(spv::FunctionControlMask::Inline) } }, id(type_id) }));
	std::vector<uint32_t> parameters;
	for (uint32_t type : p_access.parameter_types) {
		parameters.push_back(p_ir.TakeNextId());
		function->AddParameter(std::make_unique<Instruction>(&p_ir, Op::OpFunctionParameter, type, parameters.back(), Operands{}));
	}
	auto new_block = [&]() {
		return std::make_unique<BasicBlock>(std::make_unique<Instruction>(&p_ir, Op::OpLabel, 0, p_ir.TakeNextId(), Operands{}));
	};
	auto entry = new_block();
	auto inside = new_block();
	auto done = new_block();
	const uint32_t zero = uint_constant(p_ir, 0);
	const uint32_t coord_type = p_access.parameter_types.front();
	const uint32_t coord_scalar = p_image.dimensions == 1 ? coord_type : def(p_ir, coord_type)->GetSingleWordInOperand(0);
	const bool signed_coord = def(p_ir, coord_scalar)->GetSingleWordInOperand(1) != 0;
	const uint32_t signed_zero = signed_coord ? emit(p_ir, *entry, Op::OpBitcast, coord_scalar, { id(zero) }) : 0;
	uint32_t index = zero;
	uint32_t valid = 0;
	for (uint32_t axis = p_image.dimensions; axis-- > 0;) {
		const uint32_t extent_ptr = emit(p_ir, *entry, Op::OpAccessChain, p_image.extent_ptr,
				{ id(p_image.variable), id(uint_constant(p_ir, axis)) });
		const uint32_t size = emit(p_ir, *entry, Op::OpLoad, p_image.u32, { id(extent_ptr) });
		uint32_t c = p_image.dimensions == 1 ? parameters.front() : emit(p_ir, *entry, Op::OpCompositeExtract, coord_scalar,
				{ id(parameters.front()), word(axis) });
		const uint32_t nonnegative = signed_coord ? emit(p_ir, *entry, Op::OpSGreaterThanEqual, p_image.bool_type, { id(c), id(signed_zero) }) : 0;
		if (coord_scalar != p_image.u32) {
			c = emit(p_ir, *entry, Op::OpBitcast, p_image.u32, { id(c) });
		}
		uint32_t axis_ok = emit(p_ir, *entry, Op::OpULessThan, p_image.bool_type, { id(c), id(size) });
		if (signed_coord) {
			axis_ok = emit(p_ir, *entry, Op::OpLogicalAnd, p_image.bool_type, { id(nonnegative), id(axis_ok) });
		}
		if (valid) {
			valid = emit(p_ir, *entry, Op::OpLogicalAnd, p_image.bool_type, { id(valid), id(axis_ok) });
			index = emit(p_ir, *entry, Op::OpIMul, p_image.u32, { id(index), id(size) });
			index = emit(p_ir, *entry, Op::OpIAdd, p_image.u32, { id(index), id(c) });
		} else {
			valid = axis_ok;
			index = c;
		}
	}
	// Buffer robustness may clamp an invalid index to a valid texel. Form the
	// texel pointer only in the in-bounds branch, including for atomic RMWs.
	emit(p_ir, *entry, Op::OpSelectionMerge, 0, { id(done->id()), { SPV_OPERAND_TYPE_SELECTION_CONTROL, { 0 } } });
	emit(p_ir, *entry, Op::OpBranchConditional, 0, { id(valid), id(inside->id()), id(done->id()) });
	const uint32_t ptr = emit(p_ir, *inside, Op::OpAccessChain, p_image.texel_ptr,
			{ id(p_image.variable), id(uint_constant(p_ir, texels_member)), id(index) });
	uint32_t result = 0;
	if (p_access.opcode == Op::OpImageRead || p_access.opcode == Op::OpImageWrite) {
		Operands operands{ id(ptr) };
		if (p_image.atomic) {
			operands.push_back(id(uint_constant(p_ir, bits(spv::Scope::Device))));
			operands.push_back(id(zero)); // Relaxed, as for the original image access.
		}
		if (p_access.opcode == Op::OpImageRead) {
			result = emit(p_ir, *inside, p_image.atomic ? Op::OpAtomicLoad : Op::OpLoad, p_image.scalar, std::move(operands));
			if (const Instruction *type = def(p_ir, p_access.result_type); type->opcode() == Op::OpTypeVector) {
				const uint32_t count = type->GetSingleWordInOperand(1);
				const uint32_t one = uint_constant(p_ir, 1);
				Operands channels{ id(result) };
				for (uint32_t i = 1; i < count; ++i) {
					const uint32_t fill = i == 3 ? one : zero;
					channels.push_back(id(p_image.scalar == p_image.u32 ? fill : emit(p_ir, *inside, Op::OpBitcast, p_image.scalar, { id(fill) })));
				}
				result = emit(p_ir, *inside, Op::OpCompositeConstruct, p_access.result_type, std::move(channels));
			}
		} else {
			const uint32_t value = parameters[1];
			const uint32_t x = def(p_ir, p_access.parameter_types[1])->opcode() == Op::OpTypeVector ?
					emit(p_ir, *inside, Op::OpCompositeExtract, p_image.scalar, { id(value), word(0) }) : value;
			operands.push_back(id(x));
			emit(p_ir, *inside, p_image.atomic ? Op::OpAtomicStore : Op::OpStore, 0, std::move(operands));
		}
	} else {
		Operands operands{ id(ptr) };
		for (uint32_t mem : p_access.memory_operands) {
			operands.push_back(id(mem));
		}
		for (size_t i = 1; i < parameters.size(); ++i) {
			operands.push_back(id(parameters[i]));
		}
		result = emit(p_ir, *inside, p_access.opcode, p_access.result_type, std::move(operands));
	}
	if (result) {
		emit(p_ir, *inside, Op::OpReturnValue, 0, { id(result) });
		const uint32_t zero_result = p_ir.TakeNextId();
		p_ir.AddGlobalValue(std::make_unique<Instruction>(&p_ir, Op::OpConstantNull, p_access.result_type, zero_result, Operands{}));
		emit(p_ir, *done, Op::OpReturnValue, 0, { id(zero_result) });
	} else {
		emit(p_ir, *inside, Op::OpBranch, 0, { id(done->id()) });
		emit(p_ir, *done, Op::OpReturn, 0);
	}
	function->AddBasicBlock(std::move(entry));
	function->AddBasicBlock(std::move(inside));
	function->AddBasicBlock(std::move(done));
	function->SetFunctionEnd(std::make_unique<Instruction>(&p_ir, Op::OpFunctionEnd, 0, 0, Operands{}));
	// Register all definitions before their uses, including forward branch labels.
	auto *def_use = p_ir.get_def_use_mgr();
	function->ForEachInst([&](Instruction *p_inst) { def_use->AnalyzeInstDef(p_inst); });
	function->ForEachInst([&](Instruction *p_inst) { def_use->AnalyzeInstUse(p_inst); });
	p_ir.AddFunction(std::move(function));
	return function_id;
}

} // namespace

bool lower_image_atomics(Module &r_module, Resource &r_resource, const std::vector<Instruction *> &p_uses) {
	IRContext &ir = *r_module.ir;
	Instruction &var = *def(ir, r_resource.id);
	const Instruction &image = *pointee(ir, var);
	BindingPlan &binding = r_resource.binding;
	FormatPlan &format = binding.format;
	const auto dim = static_cast<spv::Dim>(format.dimension);
	if (format.multisampled || (dim != spv::Dim::Dim1D && dim != spv::Dim::Dim2D && dim != spv::Dim::Dim3D) ||
			(format.arrayed && dim != spv::Dim::Dim2D) ||
			(format.logical_format != bits(spv::ImageFormat::R32i) && format.logical_format != bits(spv::ImageFormat::R32ui))) {
		return r_module.fail("Atomic image buffers require non-multisampled R32i/R32ui 1D, 2D, 2D-array, or 3D images");
	}
	const std::set<Instruction *> uses(p_uses.begin(), p_uses.end());
	std::vector<Instruction *> handles{ &var };
	for (size_t i = 0; i < handles.size(); ++i) {
		Instruction *handle = handles[i];
		if (!ir.get_def_use_mgr()->WhileEachUser(handle, [&](Instruction *use) {
				if (interface_use(use->opcode())) {
					return true;
				}
				if (use->opcode() == Op::OpLoad || use->opcode() == Op::OpCopyObject) {
					handles.push_back(use);
					return true;
				}
				return uses.count(use) && use->GetSingleWordInOperand(0) == handle->result_id();
			})) {
			return r_module.fail("Atomic image escapes its statically typed resource chain");
		}
	}
	std::vector<std::pair<Instruction *, Instruction *>> accesses;
	std::vector<Instruction *> texel_pointers;
	std::vector<Instruction *> size_queries;
	bool has_atomics = false;
	bool writes = false;
	for (Instruction *use : p_uses) {
		switch (use->opcode()) {
			case Op::OpImageTexelPointer:
				if (!ir.get_def_use_mgr()->WhileEachUser(use, [&](Instruction *p_user) {
						if (interface_use(p_user->opcode())) {
							return true;
						}
						if (!texel_atomic(p_user->opcode()) || p_user->GetSingleWordInOperand(0) != use->result_id()) {
							return false;
						}
						has_atomics = true;
						accesses.emplace_back(p_user, use);
						return true;
					})) {
					return r_module.fail("Unsupported atomic image texel pointer use");
				}
				texel_pointers.push_back(use);
				break;
			case Op::OpImageRead:
			case Op::OpImageWrite: {
				const bool write = use->opcode() == Op::OpImageWrite;
				const uint32_t required = write ? 3 : 2;
				const uint32_t extend = bits(spv::ImageOperandsMask::SignExtend) | bits(spv::ImageOperandsMask::ZeroExtend);
				if (use->NumInOperands() > required && (use->GetSingleWordInOperand(required) & ~extend)) {
					return r_module.fail("Atomic image reads and writes do not support SPIR-V image memory operands");
				}
				writes |= write;
				accesses.emplace_back(use, use);
			} break;
			case Op::OpImageQuerySize:
				size_queries.push_back(use);
				break;
			default: return r_module.fail("Unsupported image operation on an atomic image buffer");
		}
	}
	const uint32_t dims = (dim == spv::Dim::Dim1D ? 1 : dim == spv::Dim::Dim2D ? 2 : 3) + format.arrayed;
	const uint32_t scalar = image.GetSingleWordInOperand(0);
	for (const auto &[inst, image_op] : accesses) {
		const uint32_t coord = image_op->GetSingleWordInOperand(1);
		const Instruction &coord_type = *def(ir, def(ir, coord)->type_id());
		const bool vector = coord_type.opcode() == Op::OpTypeVector;
		const Instruction &coord_scalar = vector ? *def(ir, coord_type.GetSingleWordInOperand(0)) : coord_type;
		if ((vector ? coord_type.GetSingleWordInOperand(1) : 1) != dims || coord_scalar.opcode() != Op::OpTypeInt || coord_scalar.GetSingleWordInOperand(0) != 32) {
			return r_module.fail("Atomic image buffer access requires 32-bit integer coordinates of the image dimension");
		}
		if (inst->opcode() == Op::OpImageRead || inst->opcode() == Op::OpImageWrite) {
			const uint32_t type = inst->opcode() == Op::OpImageRead ? inst->type_id() : def(ir, inst->GetSingleWordInOperand(2))->type_id();
			const Instruction &texel = *def(ir, type);
			if ((texel.opcode() == Op::OpTypeVector ? texel.GetSingleWordInOperand(0) : type) != scalar) {
				return r_module.fail("Atomic image buffer texel type does not match the image's sample type");
			}
		}
	}
	const uint32_t storage = bits(SC::StorageBuffer);
	const Operand sc{ SPV_OPERAND_TYPE_STORAGE_CLASS, { storage } };
	const uint32_t u32 = type_inst(ir, var, Op::OpTypeInt, { word(32), word(0) });
	const uint32_t bool_type = type_inst(ir, var, Op::OpTypeBool, {});
	const uint32_t void_type = type_inst(ir, var, Op::OpTypeVoid, {});
	const uint32_t array = decorated_type(ir, var, Op::OpTypeRuntimeArray, { id(scalar) });
	decorate(ir, array, Deco::ArrayStride, SPIRV_WORD_BYTES);
	// Keep the existing 16-byte extent header, with scalar alignment so the
	// minimum storage binding size also fits a single-texel backing.
	Operands members(texels_member, id(u32));
	members.push_back(id(array));
	const uint32_t block = decorated_type(ir, var, Op::OpTypeStruct, std::move(members));
	decorate(ir, block, Deco::Block);
	for (uint32_t member = 0; member <= texels_member; ++member) {
		offset(ir, block, member, member * SPIRV_WORD_BYTES);
	}
	const uint32_t block_ptr = type_inst(ir, var, Op::OpTypePointer, { sc, id(block) });
	const uint32_t extent_ptr = type_inst(ir, var, Op::OpTypePointer, { sc, id(u32) });
	const uint32_t texel_ptr = type_inst(ir, var, Op::OpTypePointer, { sc, id(scalar) });
	const bool read_only = !has_atomics && !writes;
	if (Instruction *d = decoration(ir, var.result_id(), Deco::NonReadable)) {
		ir.KillInst(d);
	}
	if (Instruction *d = decoration(ir, var.result_id(), Deco::NonWritable); d && !read_only) {
		ir.KillInst(d);
	} else if (!d && read_only) {
		decorate(ir, var.result_id(), Deco::NonWritable);
	}
	change_type(ir, var, block_ptr);
	replace_operand(ir, var, 0, storage);
	const ImageBuffer layout{ var.result_id(), dims, scalar, u32, bool_type, extent_ptr, texel_ptr, has_atomics };
	// Share helpers by signature, rather than splitting arbitrary caller blocks
	// (which can be loop headers or already-structured selections).
	std::map<AccessSignature, uint32_t> functions;
	for (const auto &[inst, image_op] : accesses) {
		const uint32_t coord = image_op->GetSingleWordInOperand(1);
		AccessSignature signature{ inst->opcode(), inst->type_id(), { def(ir, coord)->type_id() }, {} };
		Operands args{ id(coord) };
		uint32_t first = inst->NumInOperands();
		if (signature.opcode == Op::OpImageWrite) {
			const uint32_t texel = inst->GetSingleWordInOperand(2);
			args.push_back(id(texel));
			signature.parameter_types.push_back(def(ir, texel)->type_id());
		} else if (texel_atomic(signature.opcode)) {
			first = signature.opcode == Op::OpAtomicCompareExchange ? 4 : 3;
			for (uint32_t i = 1; i < first; ++i) {
				signature.memory_operands.push_back(inst->GetSingleWordInOperand(i));
			}
		}
		for (uint32_t i = first; i < inst->NumInOperands(); ++i) {
			const uint32_t arg = inst->GetSingleWordInOperand(i);
			args.push_back(id(arg));
			signature.parameter_types.push_back(def(ir, arg)->type_id());
		}
		const uint32_t return_type = signature.result_type ? signature.result_type : void_type;
		auto [fn, fresh] = functions.try_emplace(signature, 0);
		if (fresh) {
			fn->second = guarded_access(ir, layout, signature, return_type);
		}
		args.insert(args.begin(), id(fn->second));
		const uint32_t result = insert_value(ir, *inst, Op::OpFunctionCall, return_type, std::move(args));
		if (inst->result_id()) {
			ir.ReplaceAllUsesWith(inst->result_id(), result);
		}
		ir.KillInst(inst);
	}
	for (Instruction *use : size_queries) {
		const uint32_t type = use->type_id();
		const uint32_t component = dims == 1 ? type : def(ir, type)->GetSingleWordInOperand(0);
		Operands extents;
		for (uint32_t axis = 0; axis < dims; ++axis) {
			const uint32_t ptr = insert_value(ir, *use, Op::OpAccessChain, extent_ptr, { id(r_resource.id), id(uint_constant(ir, axis)) });
			uint32_t size = insert_value(ir, *use, Op::OpLoad, u32, { id(ptr) });
			if (component != u32) {
				size = insert_value(ir, *use, Op::OpBitcast, component, { id(size) });
			}
			extents.push_back(id(size));
		}
		const uint32_t result = dims == 1 ? extents.front().words[0] : insert_value(ir, *use, Op::OpCompositeConstruct, type, std::move(extents));
		ir.ReplaceAllUsesWith(use->result_id(), result);
		ir.KillInst(use);
	}
	for (Instruction *ptr : texel_pointers) {
		ir.KillInst(ptr);
	}
	while (handles.size() > 1) {
		ir.KillInst(handles.back());
		handles.pop_back();
	}
	format.atomic_buffer = true;
	format.access = read_only ? ImageAccess::READ : ImageAccess::READ_WRITE;
	binding.read_only = read_only;
	binding.key.role = Role::STORAGE_BUFFER;
	return true;
}

} // namespace godot::webgpu::spirv
