# godot-dev

**English** | [简体中文](README.zh-CN.md)

Custom [Godot Engine](https://godotengine.org) development fork adding an embedded **MCP Host/CLI** and a **WebGPU RenderingDevice backend with HTTP Range asset streaming**.

For upstream engine overview, tutorials, and general compilation guides, see the [official Godot documentation](https://docs.godotengine.org) and [CONTRIBUTING.md](CONTRIBUTING.md).

## Versions & Build Baseline

| Component | Version / Baseline | Notes |
| --- | --- | --- |
| **Upstream Godot** | `4.7.3-rc` | Merged from `upstream/4.7` (`fde53e7e59`) |
| **Embedded MCP** | Protocol `2025-11-25` (compat `2025-06-18`, `2025-03-26`) · Discovery ext `io.godot/tool-discovery v1` | Server reports `VERSION_FULL_CONFIG`; SCons `mcp=auto\|yes\|no` (excluded from `template_release`) |
| **WebGPU Backend** | Emscripten `>= 4.0.11` (`--use-port=emdawnwebgpu`, verified on `4.0.11`) | SCons `webgpu=yes`, `webgpu_precompile_shaders=yes`, `dlink_enabled=yes\|no` |
| **WGSL / SPIR-V Toolchain** | SPIRV-Tools `v2026.2` (`3605cce5`) · Dawn Tint (`main` + 6 vendored patches) | Embedded in `drivers/webgpu/` + `thirdparty/{tint,spirv-tools,spirv-headers}` |

## Custom Modules

### 1. Embedded MCP Host & CLI (`core/mcp/`, `editor/mcp/`, `main/mcp_cli*`)
- Opt-in persistent editor MCP Host (`--editor --mcp`) with project-lease ownership and safe lifecycle recovery.
- Headless terminal adapters (`--mcp-discover`, `--mcp-stdio`) with bearer-authenticated loopback forwarding.
- 111+ structured editor, scene, project, GDScript LSP analysis, and runtime debugger tools with `outputSchema` and safety annotations.
- Session-negotiated compact progressive tool discovery (`godot.catalog.search`, `godot.catalog.describe`).

**Documentation (Progressive Disclosure):**
- Start here: [MCP Usage & CLI Guide](editor/mcp/README.md)
- Deep dives: [Architecture](editor/mcp/ARCHITECTURE.md) · [Progressive Disclosure](editor/mcp/PROGRESSIVE_DISCLOSURE.md) · [Contributing](editor/mcp/CONTRIBUTING.md) · [GDExtension Build Plan](editor/mcp/GDEXTENSION_BUILD_PLAN.md)

### 2. WebGPU Rendering Backend & Web Streaming (`drivers/webgpu/`, `platform/web/`)
- Native `RenderingDevice` WebGPU driver supporting **Forward+** and **Mobile** renderers on Web with automatic 3-tier fallback (`WebGPU Forward+ → WebGPU Mobile → WebGL2`).
- Embedded SPIR-V → Tint WGSL compilation plus optional host-side build-time shader precompilation (`webgpu_precompile_shaders=yes`).
- GDExtension dynamic linking (`dlink_enabled=yes`) support on WebGPU exports.
- HTTP `206` Range `.pck` streaming (zero `MEMFS` duplication), progressive `.ctex`/`.dds` mip-tail streaming, multi-phase HTML5 startup progress shell, and threaded RFC 7233 dev server (`platform/web/serve.py`).

**Documentation (Progressive Disclosure):**
- Start here: [WebGPU Backend & Streaming Guide](drivers/webgpu/README.md)
- Deep dive: [Vendored Tint Patches](thirdparty/tint/patches/README.md)

## Quick Build Reference

```bash
# Desktop Editor (with embedded MCP enabled by default via mcp=auto)
scons platform=windows target=editor

# WebGPU Release Export Template (with precompiled engine shaders)
scons platform=web target=template_release threads=no webgpu=yes webgpu_precompile_shaders=yes optimize=speed
```

See the linked module documentation above for full build matrices, CLI options, runtime flags, and test suites.
