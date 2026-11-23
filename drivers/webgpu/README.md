# WebGPU Rendering Backend & Web Asset Streaming

**English** | [简体中文](README.zh-CN.md)

This document covers the custom WebGPU `RenderingDevice` driver (`drivers/webgpu/`) and Web streaming runtime (`platform/web/`) built on top of **Godot 4.7.3-rc**.

## Build & Toolchain Versions

| Component | Version / Requirement | Location |
| --- | --- | --- |
| **Godot Base** | `4.7.3-rc` (`upstream/4.7` @ `fde53e7e59`) | `version.py` |
| **Emscripten SDK** | `>= 4.0.11` (verified on `4.0.11`, `--use-port=emdawnwebgpu`) | `platform/web/detect.py` |
| **SPIRV-Tools** | `v2026.2` (`3605cce5b11f6a085107fd400f1721cd2a59c49e`) | `thirdparty/spirv-tools/` |
| **Dawn Tint** | Upstream `main` extract + 6 Godot patches (`C++20`) | `thirdparty/tint/` |

## Capabilities

### Rendering (`drivers/webgpu/`)
- **Full `RenderingDevice` Driver**: Implements Forward+ and Mobile pipelines over WebGPU (`emdawnwebgpu`), including MSAA, TAA, SSAO, SSIL, SSR, Volumetric Fog, cubemap/array slice views, and async-GPU `MeshStorage` surface readback.
- **3-Tier Runtime Negotiation**: Browser startup automatically probes adapter/device limits and selects `WebGPU Forward+ → WebGPU Mobile → WebGL2` (`platform/web/js/engine/config.js`).
- **SPIR-V → WGSL Pipeline**: Embeds SPIRV-Tools preprocessing and Dawn Tint WGSL translation; supports build-time host precompilation of built-in engine shader plans (`webgpu_precompile_shaders=yes`).
- **Bind Group & Pass Optimization**: Compatible `BindGroupRecipe` interning across material shaders, specialized pipeline variant caching, encoder state dirty-tracking, and flat subresource hazard checks.
- **GDExtension Support**: Supports `dlink_enabled=yes` dynamic WebAssembly side-module exports on WebGPU.

### Streaming & Web Startup (`platform/web/`, `scene/resources/`, `modules/dds/`)
- **HTTP `206` Range `.pck` Streaming**: Parses PCK v2/v3/v4 headers over HTTP Range requests with strong `ETag` / `If-Range` validation, reading directly from JS `Uint8Array` slabs into WASM with zero `MEMFS` copy (`web_pack_stream.{h,cpp}`, `preloader.js`).
- **Progressive Texture Mip Streaming**: Opt-in via `progressiveTextureMips` (enabled in `misc/dist/html/full-size.html` for single-threaded builds). Boots with verified block-aligned tail mips for large `.ctex` / `.dds` textures and upgrades full mip chains asynchronously over frame-budgeted post-iteration slices (`RS::texture_replace`) before evicting JS slabs.
- **Multi-Phase Startup Shell & Server**: Splits `Main::setup` / `setup2` / `start` / first frame across browser rAF ticks with normalized progress (`window.godotStartupStatus`) and live texture stream state (`window.godotStreamState`). `platform/web/serve.py` provides a multi-threaded HTTP/1.1 server with RFC 7233 `Range` and strong `ETag` support.

## Building Export Templates

Activate Emscripten `>= 4.0.11` in your shell first, then run SCons from the repository root:

```bash
# Single-threaded WebGPU release template with precompiled engine shaders
scons platform=web target=template_release threads=no dlink_enabled=no webgpu=yes webgpu_precompile_shaders=yes optimize=speed

# WebGPU template with GDExtension dynamic linking enabled
scons platform=web target=template_release threads=no dlink_enabled=yes webgpu=yes webgpu_precompile_shaders=yes optimize=speed
```

### SCons Options
- `webgpu=yes|no`: Enables `WEBGPU_ENABLED`, `RD_ENABLED`, and `--use-port=emdawnwebgpu` (Web platform only; disables `proxy_to_pthread`).
- `webgpu_precompile_shaders=yes|no`: Runs `drivers/webgpu/wgsl_precompile.py` on the host to generate and embed `webgpu_shader_cache.gen.h` (`WEBGPU_PRECOMPILED_SHADERS`).
- `dlink_enabled=yes|no`: Enables WebAssembly dynamic linking for GDExtensions.

### Local Serving (with HTTP Range & ETag)

```bash
python platform/web/serve.py --root <export_dir> --port 8076 --no-browser
```

## Runtime Diagnostics

- **Startup & Streaming Telemetry**: Inspect `window.godotStartupStatus` and `window.godotStreamState` (or subscribe via `window.onGodotStreamProgress`).
- **Opt-in Performance Telemetry**: Append `?webgpu_perf=1` to the exported HTML URL to emit 1 Hz `[WebGPU-Perf]` summaries, `[WebGPU-Caps]` device features, and `>= 40 ms` `[WebGPU-Hitch]` diagnostics to the browser console (zero overhead when omitted).

## Architecture Map

| Area | Key Files |
| --- | --- |
| Driver & Submission | `drivers/webgpu/rendering_device_driver_webgpu.*`, `rendering_context_driver_webgpu.*`, `webgpu_submission.cpp` |
| Pipelines, Bind Groups & Passes | `drivers/webgpu/webgpu_pipelines.cpp`, `webgpu_bind_groups.cpp`, `webgpu_passes.cpp`, `webgpu_draw_dispatch.cpp` |
| Buffers, Textures & Formats | `drivers/webgpu/webgpu_buffers.cpp`, `webgpu_textures.cpp`, `webgpu_formats.cpp` |
| Shader Compiler & Precompiler | `drivers/webgpu/tint_wrapper.cpp`, `spirv_preprocess.cpp`, `wgsl_precompile.py`, `rendering_shader_container_webgpu.cpp` |
| Web Streaming & Shell | `platform/web/web_pack_stream.*`, `platform/web/js/engine/{preloader,config,engine}.js`, `platform/web/serve.py`, `misc/dist/html/full-size.html` |
| Vendored Tint Patches | [thirdparty/tint/patches/README.md](../../thirdparty/tint/patches/README.md) |
