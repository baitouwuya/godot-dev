#pragma once

#include "../webgpu_shader_plan.h"

#include "source/opt/ir_context.h"
#include "source/opt/pass.h"

#include <cassert>
#include <optional>

namespace godot::webgpu::spirv {

using spvtools::opt::IRContext;
using spvtools::opt::Instruction;
using spvtools::opt::Operand;
using Operands = std::vector<Operand>;

// Host builds do not include core/error/error_macros.h.
#if defined(WEBGPU_GODOT_BUILD)
#define GODOT_WEBGPU_ASSERT(m_condition) DEV_ASSERT(m_condition)
#else
#define GODOT_WEBGPU_ASSERT(m_condition) assert(m_condition)
#endif

struct Resource {
	uint32_t id;
	BindingPlan binding;
};

struct Module {
	Stage stage;
	std::string entry;
	std::string error;
	std::unique_ptr<IRContext> ir;
	std::vector<Resource> resources;
	uint32_t push_size = 0;
	bool uses_texture_barrier = false;

	explicit Module(Stage p_stage) : stage(p_stage) {}
	Module(const Module &) = delete;
	Module &operator=(const Module &) = delete;
	bool open(const std::vector<uint32_t> &p_words);
	bool run(spvtools::opt::Pass &p_pass);
	bool fail(const std::string &p_text);
	std::vector<uint32_t> binary() const;
};

inline Operand id(uint32_t p_id) { return { SPV_OPERAND_TYPE_ID, { p_id } }; }
inline Operand word(uint32_t p_word) { return { SPV_OPERAND_TYPE_LITERAL_INTEGER, { p_word } }; }
template <class E>
constexpr uint32_t bits(E p_enum) { return static_cast<uint32_t>(p_enum); }

Instruction *def(IRContext &p_ir, uint32_t p_id);
Instruction *pointee(IRContext &p_ir, const Instruction &p_inst);
Instruction *global_origin(IRContext &p_ir, uint32_t p_id);
void replace_operand(IRContext &p_ir, Instruction &r_inst, uint32_t p_index, uint32_t p_word);
uint32_t insert_value(IRContext &p_ir, Instruction &r_before, spv::Op p_op, uint32_t p_type, Operands p_operands);
void insert_inst(IRContext &p_ir, Instruction &r_before, spv::Op p_op, Operands p_operands);
uint32_t type_inst(IRContext &p_ir, Instruction &r_before, spv::Op p_opcode, const Operands &p_operands);
void change_type(IRContext &p_ir, Instruction &r_inst, uint32_t p_type);
Instruction *decoration(IRContext &p_ir, uint32_t p_id, spv::Decoration p_kind);
void decorate(IRContext &p_ir, uint32_t p_id, spv::Decoration p_kind, std::optional<uint32_t> p_value = {});
std::optional<uint32_t> constant_u32(IRContext &p_ir, uint32_t p_id);
uint32_t uint_constant(IRContext &p_ir, uint32_t p_value);
bool prune(Module &r_module);
bool specialize(Module &r_module, const CompileOptions &p_options);
bool lower_bindings(Module &r_module, const CompileOptions &p_options);
bool lower_push_constant(Module &r_module);
bool plan_layout(const std::vector<std::unique_ptr<Module>> &p_modules, const CompileOptions &p_options, ShaderPlan &r_plan);
bool lower_images(Module &r_module, const CompileOptions &p_options);
bool lower_image_atomics(Module &r_module, Resource &r_resource, const std::vector<Instruction *> &p_uses);
bool lower_builtins(Module &r_module, const CompileOptions &p_options);
bool lower_nonfinite_constants(Module &r_module);
bool lower_control_flow(Module &r_module, const CompileOptions &p_options);
bool normalize_memory_qualifiers(Module &r_module);

} // namespace godot::webgpu::spirv
