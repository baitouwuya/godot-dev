/**************************************************************************/
/*  library_godot_webgpu.js                                               */
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

const GodotWebGPU = {
	// emdawnwebgpu v20250531 omits WGPUTextureViewDescriptor.usage in wgpuTextureCreateView,
	// preventing narrowed sRGB sampling/attachment views on StorageBinding textures.
	godot_webgpu_texture_create_view__deps: ['$WebGPU', 'emwgpuCreateTextureView'],
	godot_webgpu_texture_create_view__sig: 'ppiiiiiiii',
	godot_webgpu_texture_create_view: function (texturePtr, format, dimension, baseMipLevel, mipLevelCount, baseArrayLayer, arrayLayerCount, aspect, usage) {
		const mipCount = mipLevelCount >>> 0;
		const layerCount = arrayLayerCount >>> 0;
		const viewUsage = usage >>> 0;
		const desc = {
			'format': WebGPU.TextureFormat[format >>> 0],
			'dimension': WebGPU.TextureViewDimension[dimension >>> 0],
			'baseMipLevel': baseMipLevel >>> 0,
			'mipLevelCount': mipCount === 0xffffffff ? undefined : mipCount,
			'baseArrayLayer': baseArrayLayer >>> 0,
			'arrayLayerCount': layerCount === 0xffffffff ? undefined : layerCount,
			'aspect': WebGPU.TextureAspect[aspect >>> 0],
		};
		if (viewUsage !== 0) {
			desc['usage'] = viewUsage;
		}
		const texture = WebGPU.getJsObject(texturePtr);
		const view = texture.createView(desc);
		const ptr = _emwgpuCreateTextureView(0);
		WebGPU.Internals.jsObjectInsert(ptr, view);
		return ptr;
	},
};

mergeInto(LibraryManager.library, GodotWebGPU);
