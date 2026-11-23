# WebGPU 渲染后端与 Web 资产流式加载

[English](README.md) | **简体中文**

本文档说明基于 **Godot 4.7.3-rc** 构建的自定义 WebGPU `RenderingDevice` 渲染驱动（`drivers/webgpu/`）及 Web 流式加载运行时（`platform/web/`）。

## 构建与工具链版本

| 组件 | 版本 / 要求 | 位置 |
| --- | --- | --- |
| **Godot 上游基线** | `4.7.3-rc`（`upstream/4.7` @ `fde53e7e59`） | `version.py` |
| **Emscripten SDK** | `>= 4.0.11`（已验证 `4.0.11`，启用 `--use-port=emdawnwebgpu`） | `platform/web/detect.py` |
| **SPIRV-Tools** | `v2026.2`（`3605cce5b11f6a085107fd400f1721cd2a59c49e`） | `thirdparty/spirv-tools/` |
| **Dawn Tint** | 上游 `main` 精简提取 + 6 个 Godot 定制补丁（`C++20`） | `thirdparty/tint/` |

## 核心功能

### 渲染后端（`drivers/webgpu/`）
- **完整 `RenderingDevice` 驱动**：基于 `emdawnwebgpu` 实现 Forward+ 与 Mobile 管线，支持 MSAA、TAA、SSAO、SSIL、SSR、体积雾（Volumetric Fog）、CubeMap/Array 切片视图以及异步 GPU `MeshStorage` 表面回读。
- **三级运行时能力协商**：启动时自动探测浏览器适配器/设备限制，按 `WebGPU Forward+ → WebGPU Mobile → WebGL2` 顺序无缝选择最佳后端（`platform/web/js/engine/config.js`）。
- **SPIR-V → WGSL 编译链**：内嵌 SPIRV-Tools 预处理与 Dawn Tint WGSL 翻译；支持通过 `webgpu_precompile_shaders=yes` 在构建期于主机端预编译引擎内置着色器计划。
- **BindGroup 与 RenderPass 热路径优化**：跨材质着色器复用兼容 `BindGroupRecipe` 与布局、特化管线变体缓存、编码器状态脏标记去重以及扁平子资源读写冲突检测。
- **GDExtension 动态链接**：支持在 WebGPU 导出中开启 `dlink_enabled=yes` 加载 WASM 侧模块扩展。

### 流式加载与 Web 启动（`platform/web/`、`scene/resources/`、`modules/dds/`）
- **HTTP `206` Range `.pck` 流式加载**：通过 HTTP Range 请求解析 PCK v2/v3/v4 目录并执行强 `ETag` / `If-Range` 一致性校验，直接从 JS `Uint8Array` 分片读入 WASM（零 `MEMFS` 复制，`web_pack_stream.{h,cpp}`、`preloader.js`）。
- **渐进式纹理 Mip 流式升级**：通过 `progressiveTextureMips` 开启（单线程模板在 `misc/dist/html/full-size.html` 默认启用）。首帧仅加载经块对齐校验的 `.ctex`/`.dds` 尾级 mip 快速出图，首帧后按帧预算切片异步拉取并热替换完整 mip 链（`RS::texture_replace`），完成后即时释放 JS 分片内存。
- **多阶段启动进度与本地开发服务**：将 `Main::setup` / `setup2` / `start` / 首帧渲染拆分到多个浏览器 rAF 周期，暴露归一化进度 `window.godotStartupStatus` 与实时流式状态 `window.godotStreamState`；`platform/web/serve.py` 提供支持 RFC 7233 `Range` 与强 `ETag` 的多线程 HTTP/1.1 本地服务。

## 构建导出模板

请先在终端激活 Emscripten `>= 4.0.11` 环境，然后在仓库根目录执行：

```bash
# 单线程 WebGPU Release 导出模板（含内置着色器预编译）
scons platform=web target=template_release threads=no dlink_enabled=no webgpu=yes webgpu_precompile_shaders=yes optimize=speed

# 启用 GDExtension 动态链接的 WebGPU 模板
scons platform=web target=template_release threads=no dlink_enabled=yes webgpu=yes webgpu_precompile_shaders=yes optimize=speed
```

### SCons 构建选项
- `webgpu=yes|no`：启用 `WEBGPU_ENABLED`、`RD_ENABLED` 与 `--use-port=emdawnwebgpu`（仅限 Web 平台，自动禁用 `proxy_to_pthread`）。
- `webgpu_precompile_shaders=yes|no`：构建期运行 `drivers/webgpu/wgsl_precompile.py` 生成并内嵌 `webgpu_shader_cache.gen.h`（`WEBGPU_PRECOMPILED_SHADERS`）。
- `dlink_enabled=yes|no`：开启 WebAssembly 动态链接以支持 GDExtension。

### 本地预览服务（支持 HTTP Range 与 ETag）

```bash
python platform/web/serve.py --root <导出目录> --port 8076 --no-browser
```

## 运行时诊断与遥测

- **启动与流式进度**：可通过 `window.godotStartupStatus`、`window.godotStreamState` 或回调 `window.onGodotStreamProgress` 观察实时状态。
- **按需性能遥测**：在导出页面 URL 后追加 `?webgpu_perf=1`，可在浏览器控制台输出启动能力摘要 `[WebGPU-Caps]`、每秒一次的 `[WebGPU-Perf]` 聚合指标及 `>= 40 ms` 卡顿帧的 `[WebGPU-Hitch]` 归因报告（不加参数时零输出、零时钟采样开销）。

## 源码架构索引

| 模块 | 核心文件 |
| --- | --- |
| 驱动与提交管线 | `drivers/webgpu/rendering_device_driver_webgpu.*`、`rendering_context_driver_webgpu.*`、`webgpu_submission.cpp` |
| 管线、BindGroup 与 Pass | `drivers/webgpu/webgpu_pipelines.cpp`、`webgpu_bind_groups.cpp`、`webgpu_passes.cpp`、`webgpu_draw_dispatch.cpp` |
| 缓冲、纹理与格式 | `drivers/webgpu/webgpu_buffers.cpp`、`webgpu_textures.cpp`、`webgpu_formats.cpp` |
| 着色器编译与预编译 | `drivers/webgpu/tint_wrapper.cpp`、`spirv_preprocess.cpp`、`wgsl_precompile.py`、`rendering_shader_container_webgpu.cpp` |
| Web 流式加载与 HTML 壳 | `platform/web/web_pack_stream.*`、`platform/web/js/engine/{preloader,config,engine}.js`、`platform/web/serve.py`、`misc/dist/html/full-size.html` |
| 内嵌 Tint 补丁详情 | [thirdparty/tint/patches/README.md](../../thirdparty/tint/patches/README.md) |
