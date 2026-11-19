#include "webgpu_shader_resources.h"

#include <algorithm>
#include <cmath>

using namespace godot::webgpu;

namespace godot::webgpu {
namespace {

WGPUPrimitiveTopology topology(Rdc::RenderPrimitive p_primitive) {
	switch (p_primitive) {
		case Rdc::RENDER_PRIMITIVE_POINTS:
			return WGPUPrimitiveTopology_PointList;
		case Rdc::RENDER_PRIMITIVE_LINES:
			return WGPUPrimitiveTopology_LineList;
		case Rdc::RENDER_PRIMITIVE_LINESTRIPS:
			return WGPUPrimitiveTopology_LineStrip;
		case Rdc::RENDER_PRIMITIVE_TRIANGLES:
			return WGPUPrimitiveTopology_TriangleList;
		case Rdc::RENDER_PRIMITIVE_TRIANGLE_STRIPS:
		case Rdc::RENDER_PRIMITIVE_TRIANGLE_STRIPS_WITH_RESTART_INDEX:
			return WGPUPrimitiveTopology_TriangleStrip;
		default:
			return WGPUPrimitiveTopology_Undefined;
	}
}

WGPUStencilFaceState stencil_face(const Rdc::PipelineDepthStencilState::StencilOperationState &p_stencil) {
	constexpr WGPUStencilOperation op[] = { WGPUStencilOperation_Keep, WGPUStencilOperation_Zero, WGPUStencilOperation_Replace,
		WGPUStencilOperation_IncrementClamp, WGPUStencilOperation_DecrementClamp, WGPUStencilOperation_Invert,
		WGPUStencilOperation_IncrementWrap, WGPUStencilOperation_DecrementWrap };
	WGPUStencilFaceState face = WGPU_STENCIL_FACE_STATE_INIT;
	face.compare = compare_function(p_stencil.compare);
	face.failOp = op[p_stencil.fail];
	face.depthFailOp = op[p_stencil.depth_fail];
	face.passOp = op[p_stencil.pass];
	return face;
}

WGPUBlendComponent blend_component(Rdc::BlendFactor p_src, Rdc::BlendFactor p_dst, Rdc::BlendOperation p_op) {
	constexpr WGPUBlendFactor factors[] = { WGPUBlendFactor_Zero, WGPUBlendFactor_One, WGPUBlendFactor_Src, WGPUBlendFactor_OneMinusSrc,
		WGPUBlendFactor_Dst, WGPUBlendFactor_OneMinusDst, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha,
		WGPUBlendFactor_DstAlpha, WGPUBlendFactor_OneMinusDstAlpha, WGPUBlendFactor_Constant, WGPUBlendFactor_OneMinusConstant,
		WGPUBlendFactor_Constant, WGPUBlendFactor_OneMinusConstant, WGPUBlendFactor_SrcAlphaSaturated,
		WGPUBlendFactor_Src1, WGPUBlendFactor_OneMinusSrc1, WGPUBlendFactor_Src1Alpha, WGPUBlendFactor_OneMinusSrc1Alpha };
	constexpr WGPUBlendOperation ops[] = { WGPUBlendOperation_Add, WGPUBlendOperation_Subtract, WGPUBlendOperation_ReverseSubtract,
		WGPUBlendOperation_Min, WGPUBlendOperation_Max };
	WGPUBlendComponent out = WGPU_BLEND_COMPONENT_INIT;
	out.operation = ops[p_op];
	out.srcFactor = factors[p_src];
	out.dstFactor = factors[p_dst];
	if (p_op == Rdc::BLEND_OP_MINIMUM || p_op == Rdc::BLEND_OP_MAXIMUM) {
		out.srcFactor = out.dstFactor = WGPUBlendFactor_One; // Vulkan ignores factors for min/max; WebGPU requires One.
	}
	return out;
}

} // namespace

WGPURenderPipeline PipelineResource::render_variant(WGPUIndexFormat p_index) const {
	if (!is_strip || p_index == WGPUIndexFormat_Undefined) {
		return raster[0].get();
	}
	DEV_ASSERT(p_index == WGPUIndexFormat_Uint16 || p_index == WGPUIndexFormat_Uint32);
	return raster[p_index == WGPUIndexFormat_Uint16 ? 1 : 2].get();
}

} // namespace godot::webgpu

RDD::SamplerID RenderingDeviceDriverWebGPU::sampler_create(const SamplerState &p_sampler) {
	state->assert_owner();
	ERR_FAIL_COND_V_MSG(p_sampler.unnormalized_uvw, {}, "WebGPU samplers require normalized texture coordinates.");
	ERR_FAIL_COND_V_MSG(p_sampler.repeat_u >= SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER ||
					p_sampler.repeat_v >= SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER || p_sampler.repeat_w >= SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER,
			{},
			"WebGPU does not support clamp-to-border or mirror-clamp-to-edge sampler addressing.");
	ERR_FAIL_COND_V_MSG(!std::isfinite(p_sampler.lod_bias) || !std::isfinite(p_sampler.min_lod) || !std::isfinite(p_sampler.max_lod),
			{}, "Sampler LOD bias and clamps must be finite.");
	if (p_sampler.lod_bias != 0.0f) {
		WARN_PRINT_ONCE("WebGPU does not support sampler LOD bias. Using an unbiased sampler.");
	}
	constexpr WGPUAddressMode addresses[] = { WGPUAddressMode_Repeat, WGPUAddressMode_MirrorRepeat, WGPUAddressMode_ClampToEdge };
	WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
	desc.addressModeU = addresses[p_sampler.repeat_u];
	desc.addressModeV = addresses[p_sampler.repeat_v];
	desc.addressModeW = addresses[p_sampler.repeat_w];
	desc.magFilter = p_sampler.mag_filter == SAMPLER_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	desc.minFilter = p_sampler.min_filter == SAMPLER_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
	desc.mipmapFilter = p_sampler.mip_filter == SAMPLER_FILTER_LINEAR ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
	desc.lodMinClamp = MAX(0.0f, p_sampler.min_lod);
	desc.lodMaxClamp = MAX(desc.lodMinClamp, p_sampler.max_lod);
	desc.compare = p_sampler.enable_compare ? compare_function(p_sampler.compare_op) : WGPUCompareFunction_Undefined;
	if (p_sampler.use_anisotropy) {
		ERR_FAIL_COND_V_MSG(!std::isfinite(p_sampler.anisotropy_max), {}, "Sampler maximum anisotropy must be finite.");
		// Clamp first, then round down to the integer anisotropy limit.
		desc.maxAnisotropy = static_cast<uint16_t>(std::clamp(p_sampler.anisotropy_max, 1.0f, float(MAX_SAMPLER_ANISOTROPY)));
		const bool all_linear = desc.magFilter == WGPUFilterMode_Linear && desc.minFilter == WGPUFilterMode_Linear &&
				desc.mipmapFilter == WGPUMipmapFilterMode_Linear;
		if (desc.maxAnisotropy > 1 && !all_linear) {
			WARN_PRINT_ONCE("WebGPU anisotropy requires all filters to be linear. Keeping the requested filters at 1x.");
			desc.maxAnisotropy = 1;
		}
	}
	auto sampler = std::make_shared<SamplerResource>();
	sampler->sampler.reset(wgpuDeviceCreateSampler(state->device.get(), &desc));
	ERR_FAIL_COND_V(!sampler->sampler, {});
	const bool linear = desc.magFilter == WGPUFilterMode_Linear || desc.minFilter == WGPUFilterMode_Linear || desc.mipmapFilter == WGPUMipmapFilterMode_Linear;
	sampler->type = p_sampler.enable_compare ? SamplerType::COMPARISON : linear ? SamplerType::FILTERING
																				: SamplerType::NON_FILTERING;
	if (sampler->type == SamplerType::FILTERING) {
		desc.magFilter = desc.minFilter = WGPUFilterMode_Nearest;
		desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
		desc.maxAnisotropy = 1;
		sampler->non_filtering.reset(wgpuDeviceCreateSampler(state->device.get(), &desc));
		ERR_FAIL_COND_V(!sampler->non_filtering, {});
	}
	return resource_id<SamplerID>(std::move(sampler));
}

void RenderingDeviceDriverWebGPU::sampler_free(SamplerID p_sampler) {
	state->assert_owner();
	free_id<SamplerResource>(p_sampler);
}

bool RenderingDeviceDriverWebGPU::sampler_is_format_supported_for_filter(DataFormat p_format, SamplerFilter p_filter) {
	const auto info = texture_format(*state, p_format, false);
	return info.valid() && (p_filter == SAMPLER_FILTER_NEAREST || info.sample_type == SampleType::FLOAT || info.sample_type == SampleType::DEPTH);
}

RDD::VertexFormatID RenderingDeviceDriverWebGPU::vertex_format_create(Span<VertexAttribute> p_attributes, const VertexAttributeBindingsMap &p_bindings) {
	std::map<uint32_t, VertexBinding> sorted;
	for (uint32_t i = 0; i < p_attributes.size(); ++i) {
		const VertexAttribute &src = p_attributes[i];
		const auto format = vertex_data_format(src.format);
		ERR_FAIL_COND_V_MSG(!format, {}, "WebGPU has no corresponding vertex attribute format.");
		const uint32_t index = src.binding == UINT32_MAX ? i : src.binding;
		VertexBinding &binding = sorted[index];
		binding.index = index;
		binding.layout = p_bindings[index];
		WGPUVertexAttribute attr = WGPU_VERTEX_ATTRIBUTE_INIT;
		attr.format = format;
		attr.offset = src.offset;
		attr.shaderLocation = src.location;
		binding.attributes.push_back(attr);
	}
	if (sorted.size() > state->limits.maxVertexBuffers || p_attributes.size() > state->limits.maxVertexAttributes) {
		ERR_FAIL_V_MSG({}, "Vertex format exceeds enabled WebGPU device limits.");
	}
	auto fmt = std::make_shared<VertexFormatResource>();
	for (auto &entry : sorted) {
		fmt->bindings.push_back(std::move(entry.second));
	}
	return resource_id<VertexFormatID>(std::move(fmt));
}

void RenderingDeviceDriverWebGPU::vertex_format_free(VertexFormatID p_format) {
	free_id<VertexFormatResource>(p_format);
}

RDD::PipelineID RenderingDeviceDriverWebGPU::compute_pipeline_create(ShaderID p_shader, VectorView<PipelineSpecializationConstant> p_constants) {
	// Store only a CPU recipe; the device object is realized at the first dispatch.
	auto pipeline = std::make_shared<PipelineResource>();
	pipeline->source = resource<ShaderResource>(p_shader);
	for (uint32_t i = 0; i < p_constants.size(); ++i) {
		pipeline->constants.push_back(p_constants[i]);
	}
	pipeline->create = [this](PipelineResource &r_variant, const ShaderVariant &p_shader) {
		WGPUComputePipelineDescriptor desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		desc.label = wg_string(r_variant.label);
		desc.layout = p_shader.layout.get();
		desc.compute.module = p_shader.modules[static_cast<size_t>(Stage::COMPUTE)].get();
		desc.compute.entryPoint = wg_string(p_shader.stage(Stage::COMPUTE).entry_point);
		r_variant.compute.reset(wgpuDeviceCreateComputePipeline(state->device.get(), &desc));
		return bool(r_variant.compute);
	};
	return resource_id<PipelineID>(std::move(pipeline));
}

RDD::PipelineID RenderingDeviceDriverWebGPU::render_pipeline_create(ShaderID p_shader, VertexFormatID p_format, RenderPrimitive p_primitive,
		PipelineRasterizationState p_raster, PipelineMultisampleState p_msaa, PipelineDepthStencilState p_depth,
		PipelineColorBlendState p_blend, VectorView<int32_t> p_colors, BitField<PipelineDynamicStateFlags> p_dynamic,
		RenderPassID p_pass, uint32_t p_subpass, VectorView<PipelineSpecializationConstant> p_constants) {
	// Pipeline precaching may run on a worker. No WebGPU objects are created here.
	const auto &pass = *resource<RenderPassLayout>(p_pass);
	const auto &subpass = pass.subpasses[p_subpass];
	auto pipeline = std::make_shared<PipelineResource>();
	WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	desc.primitive.topology = topology(p_primitive);
	ERR_FAIL_COND_V_MSG(desc.primitive.topology == WGPUPrimitiveTopology_Undefined || p_raster.wireframe || p_raster.discard_primitives ||
					p_raster.line_width != 1.0f || p_msaa.enable_alpha_to_one || p_msaa.enable_sample_shading || p_depth.enable_depth_range || p_blend.enable_logic_op ||
					p_dynamic.has_flag(DYNAMIC_STATE_LINE_WIDTH) || p_dynamic.has_flag(DYNAMIC_STATE_DEPTH_BIAS) || p_dynamic.has_flag(DYNAMIC_STATE_DEPTH_BOUNDS) ||
					p_dynamic.has_flag(DYNAMIC_STATE_STENCIL_COMPARE_MASK) || p_dynamic.has_flag(DYNAMIC_STATE_STENCIL_WRITE_MASK),
			{}, "This fixed-function state has no equivalent WebGPU pipeline state.");
	ERR_FAIL_COND_V(p_raster.enable_depth_clamp && !state->has(WGPUFeatureName_DepthClipControl), {});
	desc.primitive.unclippedDepth = p_raster.enable_depth_clamp;
	desc.primitive.frontFace = (p_raster.front_face == POLYGON_FRONT_FACE_CLOCKWISE) ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
	constexpr WGPUCullMode culling[] = { WGPUCullMode_None, WGPUCullMode_Front, WGPUCullMode_Back };
	desc.primitive.cullMode = culling[p_raster.cull_mode];
	desc.multisample.count = 1u << p_msaa.sample_count;
	pipeline->sample_count = desc.multisample.count;
	ERR_FAIL_COND_V(desc.multisample.count != 1 && desc.multisample.count != 4, {});
	desc.multisample.alphaToCoverageEnabled = p_msaa.enable_alpha_to_coverage;
	if (!p_msaa.sample_mask.is_empty()) {
		desc.multisample.mask = p_msaa.sample_mask[0];
	}
	pipeline->synthetic_attachment = pass.requires_synthetic_target(p_subpass);
	WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
	const uint32_t depth_index = subpass.depth_stencil_reference.attachment;
	if (depth_index != AttachmentReference::UNUSED) {
		const auto format = texture_format(*state, pass.attachments[depth_index].format, false);
		ERR_FAIL_COND_V(!format.valid() || (!format.depth && !format.stencil), {});
		depth.format = format.format;
		if (format.depth) {
			depth.depthCompare = p_depth.enable_depth_test ? compare_function(p_depth.depth_compare_operator) : WGPUCompareFunction_Always;
			depth.depthWriteEnabled = p_depth.enable_depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
		}
		if (p_raster.depth_bias_enabled) {
			ERR_FAIL_COND_V(std::round(p_raster.depth_bias_constant_factor) != p_raster.depth_bias_constant_factor, {});
			depth.depthBias = p_raster.depth_bias_constant_factor;
			depth.depthBiasSlopeScale = p_raster.depth_bias_slope_factor;
			depth.depthBiasClamp = p_raster.depth_bias_clamp;
		}
		if (p_depth.enable_stencil) {
			ERR_FAIL_COND_V_MSG(!format.stencil || p_depth.front_op.compare_mask != p_depth.back_op.compare_mask ||
							p_depth.front_op.write_mask != p_depth.back_op.write_mask || p_depth.front_op.reference != p_depth.back_op.reference,
					{},
					"WebGPU stencil masks and reference are shared across both faces.");
			depth.stencilFront = stencil_face(p_depth.front_op);
			depth.stencilBack = stencil_face(p_depth.back_op);
			depth.stencilReadMask = p_depth.front_op.compare_mask;
			depth.stencilWriteMask = p_depth.front_op.write_mask;
			pipeline->stencil_reference = p_depth.front_op.reference;
			pipeline->dynamic_stencil_reference = p_dynamic.has_flag(DYNAMIC_STATE_STENCIL_REFERENCE);
		}
		desc.depthStencil = &depth;
	}
	std::vector<WGPUColorTargetState> targets(pipeline->synthetic_attachment ? 1 : p_colors.size());
	std::vector<WGPUBlendState> blends(targets.size());
	bool use_constant_alpha = false, use_constant_color = false;
	for (size_t i = 0; i < targets.size(); ++i) {
		WGPUColorTargetState &target = targets[i];
		target = WGPU_COLOR_TARGET_STATE_INIT;
		if (pipeline->synthetic_attachment) {
			target.format = WGPUTextureFormat_R8Unorm;
			target.writeMask = WGPUColorWriteMask_None;
			continue;
		}
		if (p_colors[i] == ATTACHMENT_UNUSED) {
			target.writeMask = WGPUColorWriteMask_None;
			continue;
		}
		const auto info = texture_format(*state, pass.attachments[p_colors[i]].format, false);
		ERR_FAIL_COND_V(!info.valid() || !info.renderable, {});
		target.format = info.format;
		const auto &blend = p_blend.attachments[i];
		target.writeMask = (blend.write_r ? WGPUColorWriteMask_Red : 0) | (blend.write_g ? WGPUColorWriteMask_Green : 0) |
				(blend.write_b ? WGPUColorWriteMask_Blue : 0) | (blend.write_a ? WGPUColorWriteMask_Alpha : 0);
		if (blend.enable_blend) {
			for (const auto factor : { blend.src_color_blend_factor, blend.dst_color_blend_factor }) {
				use_constant_alpha |= factor == BLEND_FACTOR_CONSTANT_ALPHA || factor == BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
				use_constant_color |= factor == BLEND_FACTOR_CONSTANT_COLOR || factor == BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
			}
			blends[i].color = blend_component(blend.src_color_blend_factor, blend.dst_color_blend_factor, blend.color_blend_op);
			blends[i].alpha = blend_component(blend.src_alpha_blend_factor, blend.dst_alpha_blend_factor, blend.alpha_blend_op);
			target.blend = &blends[i];
		}
	}
	ERR_FAIL_COND_V_MSG(use_constant_alpha && use_constant_color, {}, "WebGPU cannot apply both constant RGB and constant-alpha factors to RGB.");
	pipeline->blend_constant = use_constant_alpha ? Color(p_blend.blend_constant.a, p_blend.blend_constant.a, p_blend.blend_constant.a, p_blend.blend_constant.a) : p_blend.blend_constant;
	pipeline->source = resource<ShaderResource>(p_shader);
	for (uint32_t i = 0; i < p_constants.size(); ++i) {
		pipeline->constants.push_back(p_constants[i]);
	}
	const auto vertex_format = p_format ? resource<VertexFormatResource>(p_format) : nullptr;
	std::vector<WGPUVertexBufferLayout> vertex_layouts;
	if (vertex_format) {
		for (const auto &binding : vertex_format->bindings) {
			WGPUVertexBufferLayout layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
			layout.arrayStride = binding.layout.stride;
			layout.stepMode = binding.layout.frequency == VERTEX_FREQUENCY_INSTANCE ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
			layout.attributeCount = binding.attributes.size();
			layout.attributes = binding.attributes.data();
			vertex_layouts.push_back(layout);
			pipeline->vertex_bindings.push_back(binding.index);
		}
	}
	pipeline->is_strip = desc.primitive.topology == WGPUPrimitiveTopology_LineStrip || desc.primitive.topology == WGPUPrimitiveTopology_TriangleStrip;
	std::vector<bool> has_blend;
	for (auto &target : targets) {
		has_blend.push_back(target.blend != nullptr);
		target.blend = nullptr;
	}
	const bool has_depth = desc.depthStencil != nullptr;
	desc.depthStencil = nullptr;
	pipeline->create = [this, desc, depth, has_depth, vertex_format, vertex_layouts = std::move(vertex_layouts), targets = std::move(targets),
						   has_blend = std::move(has_blend), blends = std::move(blends)](PipelineResource &r_variant, const ShaderVariant &p_shader) {
		ERR_FAIL_COND_V(vertex_layouts.size() + p_shader.groups.size() > state->limits.maxBindGroupsPlusVertexBuffers, false);
		WGPURenderPipelineDescriptor info = desc;
		info.label = wg_string(r_variant.label);
		info.depthStencil = has_depth ? &depth : nullptr;
		info.layout = p_shader.layout.get();
		info.vertex.module = p_shader.modules[static_cast<size_t>(Stage::VERTEX)].get();
		info.vertex.entryPoint = wg_string(p_shader.stage(Stage::VERTEX).entry_point);
		info.vertex.bufferCount = vertex_layouts.size();
		info.vertex.buffers = vertex_layouts.data(); // Attribute arrays are owned by vertex_format.
		auto color_targets = targets;
		for (size_t i = 0; i < color_targets.size(); ++i) {
			color_targets[i].blend = has_blend[i] ? &blends[i] : nullptr;
		}
		WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
		if (p_shader.modules[static_cast<size_t>(Stage::FRAGMENT)]) {
			fragment.module = p_shader.modules[static_cast<size_t>(Stage::FRAGMENT)].get();
			fragment.entryPoint = wg_string(p_shader.stage(Stage::FRAGMENT).entry_point);
			fragment.targetCount = color_targets.size();
			fragment.targets = color_targets.data();
			info.fragment = &fragment;
		} else {
			ERR_FAIL_COND_V(!color_targets.empty(), false);
		}
		constexpr WGPUIndexFormat index_formats[] = { WGPUIndexFormat_Undefined, WGPUIndexFormat_Uint16, WGPUIndexFormat_Uint32 };
		const uint32_t variant_count = r_variant.is_strip ? std::size(index_formats) : 1;
		for (uint32_t i = 0; i < variant_count; ++i) {
			info.primitive.stripIndexFormat = index_formats[i];
			r_variant.raster[i].reset(wgpuDeviceCreateRenderPipeline(state->device.get(), &info));
			ERR_FAIL_COND_V(!r_variant.raster[i], false);
		}
		return true;
	};
	return resource_id<PipelineID>(std::move(pipeline));
}

std::shared_ptr<PipelineResource> RenderingDeviceDriverWebGPU::materialize_pipeline(PipelineResource &r_pipeline, const std::map<uint32_t, std::shared_ptr<LogicalUniformSet>> &p_sets) {
	CompileOptions options = r_pipeline.source->options;
	for (const auto &[index, set] : p_sets) {
		for (const auto &[key, binding] : set->bindings) {
			if (!binding.texture) {
				continue;
			}
			const auto &texture = *binding.texture;
			if (key.role == Role::TEXTURE && texture.format.depth) {
				options.raw_depth.push_back(key);
			} else if (key.role == Role::TEXTURE && texture.format.sample_type == SampleType::UNFILTERABLE_FLOAT) {
				options.unfilterable_float.push_back(key);
			} else if (key.role == Role::STORAGE_TEXTURE) {
				options.storage_formats[key] = static_cast<uint32_t>(texture.format.storage_format);
			} else if (key.role == Role::STORAGE_BUFFER) {
				// The set stores the physical role; the compiler looks up the SPIR-V image.
				options.atomic_images[{ key.set, key.binding, key.element, Role::STORAGE_TEXTURE }] = static_cast<uint32_t>(texture.format.storage_format);
			}
		}
	}
	const auto variant = realize_shader(r_pipeline.source, VectorView<PipelineSpecializationConstant>(r_pipeline.constants.data(), r_pipeline.constants.size()), options);
	if (!variant) {
		return {};
	}
	const auto &key = variant->prepared->key;
	const auto it = r_pipeline.variants.find(key);
	if (it != r_pipeline.variants.end()) {
		return it->second;
	}
	auto gpu = std::make_shared<PipelineResource>();
	gpu->shader = variant;
	gpu->vertex_bindings = r_pipeline.vertex_bindings;
	gpu->is_strip = r_pipeline.is_strip;
	gpu->label = r_pipeline.label;
	ERR_FAIL_COND_V(!r_pipeline.create(*gpu, *variant), {});
	r_pipeline.variants.emplace(key, gpu);
	return gpu;
}

void RenderingDeviceDriverWebGPU::pipeline_free(PipelineID p_pipeline) {
	state->assert_owner();
	free_id<PipelineResource>(p_pipeline);
}
