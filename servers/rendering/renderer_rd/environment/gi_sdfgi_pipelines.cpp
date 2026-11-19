/**************************************************************************/
/*  gi_sdfgi_pipelines.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gi.h"

#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"

using namespace RendererRD;

void GI::_init_sdfgi_pipelines(SkyRD *p_sky) {
	DEV_ASSERT(sdfgi_supported);
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();

	{
		Vector<String> preprocess_modes;
		preprocess_modes.push_back("\n#define MODE_SCROLL\n");
		preprocess_modes.push_back("\n#define MODE_SCROLL_OCCLUSION\n");
		preprocess_modes.push_back("\n#define MODE_INITIALIZE_JUMP_FLOOD\n");
		preprocess_modes.push_back("\n#define MODE_INITIALIZE_JUMP_FLOOD_HALF\n");
		preprocess_modes.push_back("\n#define MODE_JUMPFLOOD\n");
		preprocess_modes.push_back("\n#define MODE_JUMPFLOOD_OPTIMIZED\n");
		preprocess_modes.push_back("\n#define MODE_UPSCALE_JUMP_FLOOD\n");
		preprocess_modes.push_back("\n#define MODE_OCCLUSION\n");
		preprocess_modes.push_back("\n#define MODE_STORE\n");
		String defines = "\n#define OCCLUSION_SIZE " + itos(SDFGI::CASCADE_SIZE / SDFGI::PROBE_DIVISOR) + "\n";
		sdfgi_shader.preprocess.initialize(preprocess_modes, defines);
		sdfgi_shader.preprocess_shader = sdfgi_shader.preprocess.version_create();
		for (int i = 0; i < SDFGIShader::PRE_PROCESS_MAX; i++) {
			sdfgi_shader.preprocess_pipeline[i].create_compute_pipeline(sdfgi_shader.preprocess.version_get_shader(sdfgi_shader.preprocess_shader, i));
		}
	}

	{
		//calculate tables
		String defines = "\n#define OCT_SIZE " + itos(SDFGI::LIGHTPROBE_OCT_SIZE) + "\n";

		Vector<String> direct_light_modes;
		direct_light_modes.push_back("\n#define MODE_PROCESS_STATIC\n");
		direct_light_modes.push_back("\n#define MODE_PROCESS_DYNAMIC\n");
		sdfgi_shader.direct_light.initialize(direct_light_modes, defines);
		sdfgi_shader.direct_light_shader = sdfgi_shader.direct_light.version_create();
		for (int i = 0; i < SDFGIShader::DIRECT_LIGHT_MODE_MAX; i++) {
			sdfgi_shader.direct_light_pipeline[i].create_compute_pipeline(sdfgi_shader.direct_light.version_get_shader(sdfgi_shader.direct_light_shader, i));
		}
	}

	{
		//calculate tables
		String defines = "\n#define OCT_SIZE " + itos(SDFGI::LIGHTPROBE_OCT_SIZE) + "\n";
		defines += "\n#define SH_SIZE " + itos(SDFGI::SH_SIZE) + "\n";
		if (p_sky->sky_use_octmap_array) {
			defines += "\n#define USE_OCTMAP_ARRAY\n";
		}

		Vector<String> integrate_modes;
		integrate_modes.push_back("\n#define MODE_PROCESS\n");
		integrate_modes.push_back("\n#define MODE_STORE\n");
		integrate_modes.push_back("\n#define MODE_SCROLL\n");
		integrate_modes.push_back("\n#define MODE_SCROLL_STORE\n");
		sdfgi_shader.integrate.initialize(integrate_modes, defines);
		sdfgi_shader.integrate_shader = sdfgi_shader.integrate.version_create();

		for (int i = 0; i < SDFGIShader::INTEGRATE_MODE_MAX; i++) {
			sdfgi_shader.integrate_pipeline[i].create_compute_pipeline(sdfgi_shader.integrate.version_get_shader(sdfgi_shader.integrate_shader, i));
		}

		{
			Vector<RD::Uniform> uniforms;

			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_TEXTURE;
				u.binding = 0;
				if (p_sky->sky_use_octmap_array) {
					u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE));
				} else {
					u.append_id(texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE));
				}
				uniforms.push_back(u);
			}
			{
				RD::Uniform u;
				u.uniform_type = RD::UNIFORM_TYPE_SAMPLER;
				u.binding = 1;
				u.append_id(material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));
				uniforms.push_back(u);
			}

			sdfgi_shader.integrate_default_sky_uniform_set = RD::get_singleton()->uniform_set_create(uniforms, sdfgi_shader.integrate.version_get_shader(sdfgi_shader.integrate_shader, 0), 1);
		}
	}

	{
		String defines = "\n#define OCT_SIZE " + itos(SDFGI::LIGHTPROBE_OCT_SIZE) + "\n";
		Vector<String> debug_modes;
		debug_modes.push_back("");
		sdfgi_shader.debug.initialize(debug_modes, defines);
		sdfgi_shader.debug_shader = sdfgi_shader.debug.version_create();
		sdfgi_shader.debug_shader_version = sdfgi_shader.debug.version_get_shader(sdfgi_shader.debug_shader, 0);
		sdfgi_shader.debug_pipeline.create_compute_pipeline(sdfgi_shader.debug_shader_version);
	}
	{
		String defines = "\n#define OCT_SIZE " + itos(SDFGI::LIGHTPROBE_OCT_SIZE) + "\n";

		Vector<String> versions;
		versions.push_back("\n#define MODE_PROBES\n");
		versions.push_back("\n#define MODE_PROBES\n#define USE_MULTIVIEW\n");
		versions.push_back("\n#define MODE_VISIBILITY\n");
		versions.push_back("\n#define MODE_VISIBILITY\n#define USE_MULTIVIEW\n");

		sdfgi_shader.debug_probes.initialize(versions, defines);

		// TODO disable multiview versions if turned off

		sdfgi_shader.debug_probes_shader = sdfgi_shader.debug_probes.version_create();

		{
			RD::PipelineRasterizationState rs;
			rs.cull_mode = RD::POLYGON_CULL_DISABLED;
			RD::PipelineDepthStencilState ds;
			ds.enable_depth_test = true;
			ds.enable_depth_write = true;
			ds.depth_compare_operator = RD::COMPARE_OP_GREATER_OR_EQUAL;
			for (int i = 0; i < SDFGIShader::PROBE_DEBUG_MAX; i++) {
				// TODO check if version is enabled

				RID debug_probes_shader_version = sdfgi_shader.debug_probes.version_get_shader(sdfgi_shader.debug_probes_shader, i);
				sdfgi_shader.debug_probes_pipeline[i].setup(debug_probes_shader_version, RD::RENDER_PRIMITIVE_TRIANGLE_STRIPS, rs, RD::PipelineMultisampleState(), ds, RD::PipelineColorBlendState::create_disabled(), 0);
			}
		}
	}
}
