# godot-dev

[English](README.md) | **简体中文**

基于 [Godot Engine](https://godotengine.org) 的定制开发分支，内置 **嵌入式 MCP Host / CLI** 与 **WebGPU RenderingDevice 渲染后端及 HTTP Range 资产流式加载**。

上游引擎通用介绍、教程与标准编译指南请直接参阅 [Godot 官方文档](https://docs.godotengine.org) 及 [CONTRIBUTING.md](CONTRIBUTING.md)，本文档不再复述。

## 版本与构建基线

| 组件 | 版本 / 基线 | 说明 |
| --- | --- | --- |
| **上游 Godot 基线** | `4.7.3-rc` | 合并自 `upstream/4.7`（`fde53e7e59`） |
| **嵌入式 MCP 模块** | 协议 `2025-11-25`（兼容 `2025-06-18`、`2025-03-26`）· 渐进发现扩展 `io.godot/tool-discovery v1` | 服务端上报 `VERSION_FULL_CONFIG`；SCons 选项 `mcp=auto\|yes\|no`（`template_release` 禁用） |
| **WebGPU 渲染后端** | Emscripten `>= 4.0.11`（`--use-port=emdawnwebgpu`，已验证 `4.0.11`） | SCons 选项 `webgpu=yes`、`webgpu_precompile_shaders=yes`、`dlink_enabled=yes\|no` |
| **WGSL / SPIR-V 工具链** | SPIRV-Tools `v2026.2`（`3605cce5`）· Dawn Tint（`main` + 6 个自研补丁） | 内嵌于 `drivers/webgpu/` 与 `thirdparty/{tint,spirv-tools,spirv-headers}` |

## 自定义模块概览

### 1. 嵌入式 MCP Host 与 CLI（`core/mcp/`、`editor/mcp/`、`main/mcp_cli*`）
- 按需启用的持久化编辑器 MCP Host（`--editor --mcp`），支持项目级租约独占与安全生命周期恢复。
- 提供无头终端桥接命令（`--mcp-discover`、`--mcp-stdio`），通过本机回环与私有 Bearer 凭证转发。
- 内置 113 个结构化工具（覆盖编辑器 UI、场景结构、项目设置、资源、GDScript LSP 语义分析、GDExtension 构建与运行时调试控制），均带 `outputSchema` 与行为安全注解。
- 支持按会话协商的精简渐进式工具发现（`godot.catalog.search` / `godot.catalog.describe`）。

**延伸文档（渐进式披露）：**
- 模块手册：[MCP 使用与 CLI 指南](editor/mcp/README.zh-CN.md)（[English](editor/mcp/README.md)）
- 进阶设计：[架构设计](editor/mcp/ARCHITECTURE.md) · [渐进式披露设计](editor/mcp/PROGRESSIVE_DISCLOSURE.md) · [贡献规范](editor/mcp/CONTRIBUTING.md) · [GDExtension 构建方案](editor/mcp/GDEXTENSION_BUILD_PLAN.md)

### 2. WebGPU 渲染后端与 Web 流式加载（`drivers/webgpu/`、`platform/web/`）
- 完整的 `RenderingDevice` WebGPU 驱动，在浏览器端支持 **Forward+** 与 **Mobile** 渲染器，并具备自动三级回退协商（`WebGPU Forward+ → WebGPU Mobile → WebGL2`）。
- 内嵌 SPIR-V → Dawn Tint WGSL 编译器，支持构建期主机预编译引擎内置着色器计划（`webgpu_precompile_shaders=yes`）。
- 支持 WebGPU 导出下的 GDExtension 动态链接（`dlink_enabled=yes`）。
- 内置 HTTP `206` Range `.pck` 流式加载（零 `MEMFS` 整包拷贝）、`.ctex`/`.dds` 渐进式 mip-tail 流式升级、多阶段 HTML5 启动进度壳及多线程 RFC 7233 本地服务（`platform/web/serve.py`）。

**延伸文档（渐进式披露）：**
- 模块手册：[WebGPU 后端与流式加载指南](drivers/webgpu/README.zh-CN.md)（[English](drivers/webgpu/README.md)）
- 进阶细节：[内嵌 Tint 补丁说明](thirdparty/tint/patches/README.md)

## 快速构建参考

```bash
# 桌面编辑器（默认通过 mcp=auto 启用嵌入式 MCP）
scons platform=windows target=editor

# WebGPU Release 导出模板（启用引擎内置着色器预编译）
scons platform=web target=template_release threads=no webgpu=yes webgpu_precompile_shaders=yes optimize=speed
```

完整构建矩阵、CLI 参数、运行时选项与自动化测试请查看上述各模块专门文档。
