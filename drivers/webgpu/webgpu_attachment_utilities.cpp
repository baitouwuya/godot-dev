#include "webgpu_commands.h"

using namespace godot::webgpu;

namespace {
	constexpr char fullscreen_vertex[] = R"(
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let xy = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(2.0 * xy - 1.0, 0.0, 1.0);
})";
	constexpr char depth_resolve_fragment[] = R"(
@group(0) @binding(0) var src: texture_depth_multisampled_2d;
@fragment fn fs(@builtin(position) pos: vec4f) -> @builtin(frag_depth) f32 {
    let coord = vec2i(pos.xy);
    var depth = textureLoad(src, coord, 0);
    for (var sample = 1u; sample < textureNumSamples(src); sample++) {
        depth = max(depth, textureLoad(src, coord, i32(sample)));
    }
    return depth;
})";

	RenderPipelineHandle make_utility_pipeline(DeviceState &p_state, const std::string &p_fragment, const WGPUDepthStencilState *p_depth,
			const std::vector<WGPUColorTargetState> &p_targets, uint32_t p_samples) {
		const std::string code = fullscreen_vertex + p_fragment;
		WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
		wgsl.code = wg_string(code);
		WGPUShaderModuleDescriptor shader = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
		shader.nextInChain = &wgsl.chain;
		ShaderModuleHandle module(wgpuDeviceCreateShaderModule(p_state.device.get(), &shader));
		ERR_FAIL_COND_V(!module, {});
		WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
		fragment.module = module.get();
		fragment.targetCount = p_targets.size();
		fragment.targets = p_targets.data();
		WGPURenderPipelineDescriptor desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
		desc.vertex.module = module.get();
		desc.fragment = &fragment;
		desc.depthStencil = p_depth;
		desc.multisample.count = p_samples;
		return RenderPipelineHandle(wgpuDeviceCreateRenderPipeline(p_state.device.get(), &desc));
	}

} // namespace

bool RenderingDeviceDriverWebGPU::resolve_depth(CommandRecording &r_cmd, const TextureResource &p_src, WGPUTextureView p_src_view,
		const TextureResource &p_dst, WGPUTextureView p_dst_view) {
	ERR_FAIL_COND_V_MSG(p_src.backing->desc.samples != TEXTURE_SAMPLES_4 || !(p_src.backing->usage & WGPUTextureUsage_TextureBinding),
			false, "Depth resolve requires a sampled four-sample WebGPU depth texture.");
	const std::vector<uint32_t> key{ 0, uint32_t(p_dst.format.format) };
	auto &pipeline = utility_pipelines[key];
	if (!pipeline) {
		WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
		depth.format = p_dst.format.format;
		depth.depthWriteEnabled = WGPUOptionalBool_True;
		depth.depthCompare = WGPUCompareFunction_Always;
		pipeline = make_utility_pipeline(*state, depth_resolve_fragment, &depth, {}, 1);
		ERR_FAIL_COND_V(!pipeline, false);
	}
	BindGroupLayoutHandle layout(wgpuRenderPipelineGetBindGroupLayout(pipeline.get(), 0));
	WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
	entry.textureView = p_src_view;
	WGPUBindGroupDescriptor group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	group_desc.layout = layout.get();
	group_desc.entryCount = 1;
	group_desc.entries = &entry;
	BindGroupHandle group(wgpuDeviceCreateBindGroup(state->device.get(), &group_desc));
	ERR_FAIL_COND_V(!group, false);
	WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	depth.view = p_dst_view;
	depth.depthLoadOp = WGPULoadOp_Load;
	depth.depthStoreOp = WGPUStoreOp_Store;
	depth.stencilReadOnly = p_dst.format.stencil; // Godot resolves reverse-Z depth with MAX, not stencil.
	WGPURenderPassDescriptor desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	desc.depthStencilAttachment = &depth;
	RenderPassEncoderHandle pass(wgpuCommandEncoderBeginRenderPass(r_cmd.encoder.get(), &desc));
	ERR_FAIL_COND_V(!pass, false);
	wgpuRenderPassEncoderSetPipeline(pass.get(), pipeline.get());
	wgpuRenderPassEncoderSetBindGroup(pass.get(), 0, group.get(), 0, nullptr);
	wgpuRenderPassEncoderDraw(pass.get(), 3, 1, 0, 0);
	wgpuRenderPassEncoderEnd(pass.get());
	return true;
}

void RenderingDeviceDriverWebGPU::command_render_clear_attachments(CommandBufferID p_buffer, VectorView<AttachmentClear> p_clears, VectorView<Rect2i> p_rects) {
	auto &cmd = recording(*state, p_buffer);
	DEV_ASSERT(cmd.layout);
	const Rect2i full(0, 0, cmd.framebuffer->width, cmd.framebuffer->height);
	cmd.open_render();
	ERR_FAIL_COND(!cmd.render);
	if (p_rects.size() == 1 && p_rects[0] == full && cmd.area == full) {
		cmd.end_render();
		cmd.open_render(p_clears);
		return;
	}

	const auto &subpass = cmd.layout->subpasses[cmd.subpass];
	const uint32_t depth_index = subpass.depth_stencil_reference.attachment;
	const auto depth_texture = depth_index == AttachmentReference::UNUSED ? nullptr : cmd.framebuffer->attachments[depth_index];
	const uint32_t color_count = subpass.color_references.size();
	std::vector<bool> color_clear(color_count, false);
	bool clear_depth = false, clear_stencil = false;
	uint32_t stencil = 0;
	std::vector<std::array<float, 4>> values(color_count + 1);
	for (uint32_t i = 0; i < p_clears.size(); ++i) {
		const auto &clear = p_clears[i];
		if (clear.aspect.has_flag(TEXTURE_ASPECT_COLOR_BIT)) {
			DEV_ASSERT(clear.color_attachment < color_count);
			const Color &c = clear.value.color;
			values[clear.color_attachment] = { c.r, c.g, c.b, c.a };
			color_clear[clear.color_attachment] = true;
		}
		if (clear.aspect.has_flag(TEXTURE_ASPECT_DEPTH_BIT)) {
			values.back()[0] = clear.value.depth;
			clear_depth = true;
		}
		if (clear.aspect.has_flag(TEXTURE_ASPECT_STENCIL_BIT)) {
			stencil = clear.value.stencil;
			clear_stencil = true;
		}
	}
	const bool read_only = subpass.depth_stencil_reference.layout == TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	ERR_FAIL_COND_MSG(read_only && (clear_depth || clear_stencil), "Cannot clear a read-only depth/stencil attachment.");
	WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
	uint32_t samples = 1;
	if (depth_texture) {
		depth.format = depth_texture->format.format;
		samples = 1u << depth_texture->backing->desc.samples;
		if (depth_texture->format.depth) {
			depth.depthWriteEnabled = clear_depth ? WGPUOptionalBool_True : WGPUOptionalBool_False;
			depth.depthCompare = WGPUCompareFunction_Always;
		}
		if (clear_stencil) {
			depth.stencilFront.passOp = depth.stencilBack.passOp = WGPUStencilOperation_Replace;
			depth.stencilFront.compare = depth.stencilBack.compare = WGPUCompareFunction_Always;
			depth.stencilWriteMask = UINT8_MAX;
		}
	}
	std::vector<WGPUColorTargetState> targets(color_count);
	if (cmd.layout->requires_synthetic_target(cmd.subpass)) {
		targets.resize(1, WGPU_COLOR_TARGET_STATE_INIT);
		targets[0].format = WGPUTextureFormat_R8Unorm;
		targets[0].writeMask = WGPUColorWriteMask_None;
		samples = cmd.render_samples;
	}
	std::vector<uint32_t> key{ 1, uint32_t(depth.format), clear_depth, clear_stencil };
	std::string fields, outputs;
	for (uint32_t i = 0; i < color_count; ++i) {
		auto &target = targets[i];
		target = WGPU_COLOR_TARGET_STATE_INIT;
		target.writeMask = WGPUColorWriteMask_None;
		const uint32_t index = subpass.color_references[i].attachment;
		if (index != AttachmentReference::UNUSED) {
			const auto &texture = *cmd.framebuffer->attachments[index];
			target.format = texture.format.format;
			samples = 1u << texture.backing->desc.samples;
			if (color_clear[i]) {
				target.writeMask = WGPUColorWriteMask_All;
				const std::string slot = std::to_string(i);
				const std::string type = texture.format.sample_type == SampleType::UINT ? "vec4u" : texture.format.sample_type == SampleType::SINT ? "vec4i" : "vec4f";
				fields += "@location(" + slot + ") c" + slot + ": " + type + ",\n";
				outputs += "out.c" + slot + " = " + type + "(values[" + slot + "]);\n";
			}
		}
		key.insert(key.end(), { uint32_t(target.format), uint32_t(target.writeMask) });
	}
	if (clear_depth) {
		fields += "@builtin(frag_depth) depth: f32,\n";
		outputs += "out.depth = values[" + std::to_string(color_count) + "].x;\n";
	}
	key.push_back(samples);
	auto &pipeline = utility_pipelines[key];
	if (!pipeline) {
		const std::string code = fields.empty() ? "\n@fragment fn fs() {}\n" : "\n@group(0) @binding(0) var<uniform> values: array<vec4f, " +
					std::to_string(values.size()) + ">;\nstruct Outputs {\n" + fields + "}\n@fragment fn fs() -> Outputs { var out: Outputs;\n" + outputs + "return out; }\n";
		pipeline = make_utility_pipeline(*state, code, depth_texture ? &depth : nullptr, targets, samples);
		ERR_FAIL_COND(!pipeline);
	}
	BindGroupHandle group;
	if (!fields.empty()) {
		const uint64_t size = values.size() * sizeof(values[0]);
		const auto buffer = make_transfer_buffer(*state, size, WGPUBufferUsage_Uniform, true);
		ERR_FAIL_COND(!buffer);
		void *data = wgpuBufferGetMappedRange(buffer->buffer.get(), 0, size);
		ERR_FAIL_NULL(data);
		std::memcpy(data, values.data(), size);
		wgpuBufferUnmap(buffer->buffer.get());
		BindGroupLayoutHandle layout(wgpuRenderPipelineGetBindGroupLayout(pipeline.get(), 0));
		WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
		entry.buffer = buffer->buffer.get();
		entry.size = size;
		WGPUBindGroupDescriptor desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		desc.layout = layout.get();
		desc.entryCount = 1;
		desc.entries = &entry;
		group.reset(wgpuDeviceCreateBindGroup(state->device.get(), &desc));
		ERR_FAIL_COND(!group);
		cmd.keep(buffer);
	}
	const auto pass = cmd.render.get();
	wgpuRenderPassEncoderSetPipeline(pass, pipeline.get());
	if (group) {
		wgpuRenderPassEncoderSetBindGroup(pass, 0, group.get(), 0, nullptr);
	}
	wgpuRenderPassEncoderSetViewport(pass, 0, 0, full.size.x, full.size.y, 0.f, 1.f);
	wgpuRenderPassEncoderSetStencilReference(pass, stencil);
	for (uint32_t i = 0; i < p_rects.size(); ++i) {
		const Rect2i rect = p_rects[i].intersection(full).intersection(cmd.area);
		if (rect.has_area()) {
			wgpuRenderPassEncoderSetScissorRect(pass, rect.position.x, rect.position.y, rect.size.x, rect.size.y);
			wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
		}
	}
	// The next ordinary draw replays viewport, scissor, pipeline, and bindings.
}
