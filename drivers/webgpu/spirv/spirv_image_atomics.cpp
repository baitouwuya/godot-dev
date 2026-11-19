#include "spirv_module.h"

namespace godot::webgpu::spirv {
namespace {

using Op = spv::Op;
using Deco = spv::Decoration;
using SC = spv::StorageClass;
constexpr uint32_t extent_member = 0;
constexpr uint32_t texels_member = 1;

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

uint32_t linear_index(IRContext &p_ir, Instruction &r_at, uint32_t p_extent, uint32_t p_coordinates, uint32_t p_dimensions, uint32_t p_uint, uint32_t p_bool) {
	uint32_t index = uint_constant(p_ir, 0);
	uint32_t valid = 0;
	const auto coord_type = def(p_ir, p_coordinates)->type_id();
	const auto scalar = p_dimensions == 1 ? coord_type : def(p_ir, coord_type)->GetSingleWordInOperand(0);
	for (uint32_t j = p_dimensions; j-- > 0;) {
		const uint32_t size = insert_value(p_ir, r_at, Op::OpCompositeExtract, p_uint, { id(p_extent), word(j) });
		uint32_t c = p_dimensions == 1 ? p_coordinates : insert_value(p_ir, r_at, Op::OpCompositeExtract, scalar, { id(p_coordinates), word(j) });
		if (scalar != p_uint) {
			c = insert_value(p_ir, r_at, Op::OpBitcast, p_uint, { id(c) }); // Negative coordinates become out of range.
		}
		uint32_t axis_ok = insert_value(p_ir, r_at, Op::OpULessThan, p_bool, { id(c), id(size) });
		valid = valid ? insert_value(p_ir, r_at, Op::OpLogicalAnd, p_bool, { id(valid), id(axis_ok) }) : axis_ok;
		index = insert_value(p_ir, r_at, Op::OpIMul, p_uint, { id(index), id(size) });
		index = insert_value(p_ir, r_at, Op::OpIAdd, p_uint, { id(index), id(c) });
	}
	return insert_value(p_ir, r_at, Op::OpSelect, p_uint, { id(valid), id(index), id(uint_constant(p_ir, UINT32_MAX)) });
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
			(format.logical_format != bits(spv::ImageFormat::R32i) && format.logical_format != bits(spv::ImageFormat::R32ui))) {
		return r_module.fail("Atomic image buffer lowering requires non-multisampled signed or unsigned R32 texels");
	}
	if (!ir.get_def_use_mgr()->WhileEachUser(&var, [](Instruction *use) {
			return use->opcode() == Op::OpImageTexelPointer || use->opcode() == Op::OpDecorate || use->opcode() == Op::OpName;
		})) {
		return r_module.fail("Atomic image also uses a texture view; a shared representation is required for all its accesses");
	}
	const uint32_t scalar = image.GetSingleWordInOperand(0);
	const uint32_t storage = bits(SC::StorageBuffer);
	const Operand sc{ SPV_OPERAND_TYPE_STORAGE_CLASS, { storage } };
	const uint32_t u32 = type_inst(ir, var, Op::OpTypeInt, { word(32), word(0) });
	const uint32_t bool_type = type_inst(ir, var, Op::OpTypeBool, {});
	const uint32_t uvec4 = type_inst(ir, var, Op::OpTypeVector, { id(u32), word(4) });
	const uint32_t array = decorated_type(ir, var, Op::OpTypeRuntimeArray, { id(scalar) });
	decorate(ir, array, Deco::ArrayStride, SPIRV_WORD_BYTES);
	const uint32_t block = decorated_type(ir, var, Op::OpTypeStruct, { id(uvec4), id(array) });
	decorate(ir, block, Deco::Block);
	offset(ir, block, extent_member, 0);
	offset(ir, block, texels_member, ATOMIC_IMAGE_HEADER_BYTES);
	const uint32_t block_ptr = type_inst(ir, var, Op::OpTypePointer, { sc, id(block) });
	const uint32_t extent_ptr = type_inst(ir, var, Op::OpTypePointer, { sc, id(uvec4) });
	const uint32_t atomic_ptr = type_inst(ir, var, Op::OpTypePointer, { sc, id(scalar) });
	if (Instruction *d = decoration(ir, var.result_id(), Deco::NonReadable)) {
		ir.KillInst(d);
	}
	if (Instruction *d = decoration(ir, var.result_id(), Deco::NonWritable)) {
		ir.KillInst(d);
	}
	change_type(ir, var, block_ptr);
	replace_operand(ir, var, 0, storage);
	const uint32_t dims = (dim == spv::Dim::Dim1D ? 1 : dim == spv::Dim::Dim2D ? 2 : 3) + format.arrayed;
	for (Instruction *use : p_uses) {
		GODOT_WEBGPU_ASSERT(use->opcode() == Op::OpImageTexelPointer);
		uint32_t at = insert_value(ir, *use, Op::OpAccessChain, extent_ptr, { id(r_resource.id), id(uint_constant(ir, extent_member)) });
		uint32_t extent = insert_value(ir, *use, Op::OpLoad, uvec4, { id(at) });
		uint32_t index = linear_index(ir, *use, extent, use->GetSingleWordInOperand(1), dims, u32, bool_type);
		uint32_t ptr = insert_value(ir, *use, Op::OpAccessChain, atomic_ptr, { id(r_resource.id), id(uint_constant(ir, texels_member)), id(index) });
		ir.ReplaceAllUsesWith(use->result_id(), ptr);
		ir.KillInst(use);
	}
	format.atomic_buffer = true;
	format.access = ImageAccess::READ_WRITE;
	binding.read_only = false;
	binding.key.role = Role::STORAGE_BUFFER;
	return true;
}

} // namespace godot::webgpu::spirv
