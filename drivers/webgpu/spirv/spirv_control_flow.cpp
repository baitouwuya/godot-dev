#include "spirv_module.h"

namespace godot::webgpu::spirv {
namespace {

bool needs_image_fence(IRContext &p_ir) {
	bool needed = false;
	p_ir.module()->ForEachInst([&](Instruction *p_inst) {
		switch (p_inst->opcode()) {
			case spv::Op::OpImageRead:
			case spv::Op::OpImageSparseRead: {
				const auto *image = global_origin(p_ir, p_inst->GetSingleWordInOperand(0));
				needed |= !image || !decoration(p_ir, image->result_id(), spv::Decoration::NonWritable);
				break;
			}
			case spv::Op::OpImageTexelPointer:
				needed = true;
				break;
			default:
				break;
		}
	});
	return needed;
}

} // namespace

bool lower_control_flow(Module &r_module, const CompileOptions &p_options) {
	IRContext &ir = *r_module.ir;
	std::vector<Instruction *> barriers;
	ir.module()->ForEachInst([&](Instruction *i) {
		if (i->opcode() == spv::Op::OpMemoryBarrier || i->opcode() == spv::Op::OpControlBarrier) {
			barriers.push_back(i);
		}
	});
	constexpr uint32_t workgroup = bits(spv::Scope::Workgroup);
	constexpr uint32_t ordering = bits(spv::MemorySemanticsMask::AcquireRelease);
	constexpr uint32_t workgroup_memory = bits(spv::MemorySemanticsMask::WorkgroupMemory);
	constexpr uint32_t buffer_memory = bits(spv::MemorySemanticsMask::UniformMemory);
	constexpr uint32_t image_memory = bits(spv::MemorySemanticsMask::ImageMemory);
	constexpr uint32_t atomic_counter = bits(spv::MemorySemanticsMask::AtomicCounterMemory);
	std::optional<bool> image_fence_needed;
	for (Instruction *inst : barriers) {
		const bool fence = inst->opcode() == spv::Op::OpMemoryBarrier;
		const auto scope = constant_u32(ir, inst->GetSingleWordInOperand(fence ? 0 : 1));
		auto semantic = constant_u32(ir, inst->GetSingleWordInOperand(fence ? 1 : 2));
		if (!scope || !semantic || r_module.stage != Stage::COMPUTE ||
				(!fence && constant_u32(ir, inst->GetSingleWordInOperand(0)) != workgroup)) {
			return r_module.fail("Only workgroup execution barriers are representable in WGSL compute");
		}
		if (*scope == workgroup) {
			// groupMemoryBarrier() includes all GLSL memory classes even when
			// the shader cannot access them. Vulkan forbids AtomicCounter;
			// WebGPU also forbids aliasing writable texture subresources.
			*semantic &= ~atomic_counter;
			if (*semantic & image_memory) {
				if (!image_fence_needed.has_value()) {
					image_fence_needed = needs_image_fence(ir);
				}
				if (!*image_fence_needed) {
					*semantic &= ~image_memory;
				}
			}
		}
		if (fence) {
			Instruction *next = inst->NextNode();
			while (next && (next->opcode() == spv::Op::OpLine || next->opcode() == spv::Op::OpNoLine)) {
				next = next->NextNode();
			}
			// Fuse with the existing rendezvous, never introduce one where
			// convergence is unknown. Shared storage is workgroup-local
			// even when memoryBarrierShared() was emitted with Device scope.
			const bool local_scope = *scope == workgroup ||
					(*scope == bits(spv::Scope::Device) && *semantic == (ordering | workgroup_memory));
			if (next && next->opcode() == spv::Op::OpControlBarrier && local_scope &&
					!(*semantic & ~(ordering | workgroup_memory | buffer_memory | image_memory)) &&
					constant_u32(ir, next->GetSingleWordInOperand(0)) == workgroup &&
					constant_u32(ir, next->GetSingleWordInOperand(1)) == workgroup) {
				auto next_semantics = constant_u32(ir, next->GetSingleWordInOperand(2));
				if (next_semantics) {
					replace_operand(ir, *next, 2, uint_constant(ir, *semantic | *next_semantics));
					ir.KillInst(inst);
					continue;
				}
			}
		}
		if (fence && !p_options.uniform_memory_fences) {
			return r_module.fail("OpMemoryBarrier has no WGSL memory-only equivalent; a proven convergent fence is required");
		}
		if (*scope != workgroup || (*semantic & ~(ordering | workgroup_memory | buffer_memory | image_memory))) {
			return r_module.fail("SPIR-V barrier memory scope or semantics have no workgroup-scoped WGSL equivalent");
		}
		if ((*semantic & image_memory) && !p_options.texture_barrier) {
			return r_module.fail("Image memory barrier requires the texture_barrier capability");
		}
		r_module.uses_texture_barrier |= (*semantic & image_memory) != 0;
		uint32_t normalized = *semantic | ordering;
		if (!(normalized & (workgroup_memory | buffer_memory | image_memory))) {
			normalized |= workgroup_memory; // A rendezvous still must execute.
		}
		uint32_t workgroup_id = uint_constant(ir, workgroup);
		uint32_t semantics_id = uint_constant(ir, normalized);
		if (fence) {
			insert_inst(ir, *inst, spv::Op::OpControlBarrier, { id(workgroup_id), id(workgroup_id), id(semantics_id) });
			ir.KillInst(inst);
		} else {
			replace_operand(ir, *inst, 2, semantics_id);
		}
	}
	return true;
}

} // namespace godot::webgpu::spirv
