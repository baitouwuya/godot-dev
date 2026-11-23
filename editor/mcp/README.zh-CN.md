# Godot 嵌入式 MCP Host 与 CLI 传输适配器

[English](README.md) | **简体中文**

Godot 将 MCP 作为**按需启用的嵌入式编辑器 Host** 与两个终端 CLI 传输适配器提供。编辑器 Host、MCP 协议层、Tool Registry 与各 Provider 是唯一业务功能来源；CLI 仅负责项目发现与标准 `stdio` 转发，不维护独立业务注册表也不直接调用 Provider。普通编辑器或游戏进程默认不启动 MCP。

所有对外暴露的工具均包含对象级 `outputSchema` 及标准 MCP 注解（`readOnlyHint`、`destructiveHint`、`idempotentHint`、`openWorldHint`，其中 `openWorldHint` 固定为 `false`）。

> 架构分层与扩展边界见 [ARCHITECTURE.md](ARCHITECTURE.md)；新增 Feature / Provider / 工具请遵循 [CONTRIBUTING.md](CONTRIBUTING.md)；精简工具发现完整设计见 [PROGRESSIVE_DISCLOSURE.md](PROGRESSIVE_DISCLOSURE.md)。

## 版本与构建选项

- **MCP 协议版本**：`2025-11-25`（兼容 `2025-06-18`、`2025-03-26`）；`2025-03-26` 支持 JSON-RPC 批处理，新版本使用 `godot.automation.batch`。
- **工具发现扩展**：`io.godot/tool-discovery` `version: 1`（服务端版本上报 `VERSION_FULL_CONFIG`）。
- **SCons 构建控制**：`mcp=auto|yes|no`（`auto` 在编辑器构建中默认开启；`template_release` 严格禁止启用 MCP）。

## 工具发现与渐进式披露

完整目录默认共暴露 **113 个工具**（111 个业务工具 + 2 个目录检索工具）：
- `godot.catalog.search`：按名称前缀（`prefix`）或关键字（`query`）分页检索不含完整 schema 的工具摘要（默认 `limit=20`，范围 `1..50`）。
- `godot.catalog.describe`：按精确 `name` 获取单个工具的完整定义（位于 `structuredContent.tool`），随后可直接以规范名称发起 `tools/call`。

若客户端支持动态加载工具定义，可在 `initialize.params.capabilities` 中声明精简模式扩展：

```json
{"experimental":{"io.godot/tool-discovery":{"version":1}}}
```

协商成功后，`tools/list` 仅返回 5 个入口工具（`godot.catalog.search`、`godot.catalog.describe`、`godot.editor.get_state`、`godot.runtime.get_state`、`godot.project.inspect`），其余工具仍可通过目录检索后按规范名调用。测量完整/精简模式负载字节差异：

```powershell
py -3 tests/editor/mcp/test_mcp_cli_smoke.py --binary "$EditorBinary" --measure-advertisement --project "$ProjectPath"
```

## 启动、发现与连接项目 Host

Host 归属于项目编辑器进程，而非单次工具调用或 stdio 会话；会话断开不会关闭编辑器或正在运行的游戏。

```powershell
# 1. 显式启动启用 MCP 的项目编辑器（--mcp 需配合 --editor 与 --path）
godot --editor --path C:\projects\my-game --mcp
# 可选指定固定端口：--mcp-port <port>（0 表示随机临时端口）

# 2. 发现已运行的项目 Host（stdout 仅输出单行公开 JSON，不含 authSecret）
godot --mcp-discover --path C:\projects\my-game

# 3. 持久化 stdio 桥接（供 MCP 客户端配置）
godot --mcp-stdio --path C:\projects\my-game
```

CLI 生命周期选项（`--mcp-discover` 与 `--mcp-stdio` 均支持）：
- `--connect-only`（默认）：仅附着已有 Host，绝不启动新编辑器。
- `--allow-start`：当项目无活跃编辑器/Host 时，允许执行一次独立编辑器启动（不继承桥接标准流，Windows 下脱离父 Job）。
- `--wait-ms <0..120000>`：启动与就绪等待上限（默认 `10000`）。

> `--restart` 会被刻意拒绝，以避免未经确认丢弃未保存修改；多项目按规范化项目根路径隔离路由，请为每个项目单独配置客户端入口。

### 常见诊断码（stderr）
- `MCP_DISABLED`：普通编辑器已打开该项目（未启用 MCP），不会静默接管或重复启动，需用户显式重启并加 `--mcp`。
- `ALREADY_RUNNING` / `HOST_STARTING` / `HOST_NOT_READY`：复用到已有所有者或等待就绪。
- `HOST_NOT_RUNNING`：默认 `connect-only` 未发现可用 Host（如需拉起请显式加 `--allow-start`）。
- `OWNERSHIP_UNCERTAIN` / `DISCOVERY_INVALID` / `HOST_IDENTITY_MISMATCH`：元数据或身份校验异常，按 fail-closed 停止，切勿手动删除锁文件。
- `LAUNCH_UNAVAILABLE` / `LAUNCH_EXITED`：单次显式启动失败，不会盲目重试。

## 核心工具能力矩阵

| 能力域 | 代表工具 | 关键契约与安全边界 |
| --- | --- | --- |
| **类文档检索** | `godot.class.search`、`godot.class.get_documentation` | 合并引擎原生类、扩展类、内置 Variant 与全局 `class_name` 脚本；项目脚本引导改用 `godot.script.get` 避免读取过时缓存 |
| **场景与节点编辑** | `godot.scene.open/save`、`godot.node.{get_properties,set_property,delete,rename,reparent,move,duplicate,instantiate_scene}`、分组与信号工具 | 全部接入 `EditorUndoRedoManager` 且默认不自动保存；仅允许编辑当前场景拥有的节点，复用路径重写机制 |
| **编辑器 UI 操作** | `godot.editor.ui.get_actions`、`godot.editor.ui.perform` | 基于会话隔离的语义快照（`snapshotId` + `targetId`），屏蔽密码框内容，弹窗开启时限定作用域 |
| **项目配置与 Autoload / Input** | `godot.project.{inspect,get_settings,get_setting,set_setting,erase_setting,apply,save}`、`godot.autoload.*`、`godot.input.*` | 基于 `projectRevision` 乐观锁与 Undo/Redo 事务；Variant 线协议 v2；禁止通过通用文件写入绕过 `project.godot` |
| **脚本与 GDScript LSP** | `godot.script.{open,get,usages}`、`godot.gdscript.{rename,apply_workspace_edit}` | 直接同步未保存 ScriptEditor 缓冲区进行语义查找与 UTF-16 LSP 重构，按文档校验 revision/SHA-256 |
| **资源与导入** | `godot.resource.{get_properties,set_property,save,import}` | 操作 `ResourceLoader` 缓存对象并接入 Undo/Redo；导入失败仅回滚已证明归属路径 |
| **GDExtension 构建** | `godot.gdextension.{build,build_status,build_cancel}` | 仅允许构建项目内 `.gdextension` 对应的固定 `scons` 命令，每项目单任务隔离并支持失败回滚与 SHA-256 校验 |
| **调试日志与堆栈** | `godot.debug.{get_logs,get_latest_log,get_errors,get_stack}` | 全局去重、按 `runtimeGeneration` 隔离运行代次，支持增量 `sinceSequence` 分页与受限日志摘要 |
| **运行时控制与自动化** | `godot.runtime.{get_state,play,stop,pause,resume,next_frame,get_tree}`、`debug.*`、`input.{send,sequence,release_all}`、`wait.*`、`performance.*`、`harness.*` | 复用内置远程调试通道；通过 `runtimeGeneration` 防串代；输入序列具备步数/内存/心跳超时自动释放保护 |
| **同步批处理** | `godot.automation.batch` | 单会话顺序预校验并执行最多 64 个工具调用（默认 fail-fast，禁止递归）；长时运行时任务使用异步 Harness |

## 普通请求追踪策略（Trace）

默认关闭。需在**启动编辑器 Host 前**设置 `GODOT_MCP_TRACE=1`（可选绝对路径 `GODOT_MCP_TRACE_DIR` 指定落盘目录）：

```bash
GODOT_MCP_TRACE=1 godot --editor --path /absolute/project --mcp
```

追踪仅记录白名单方法/工具名、span 层级、16 位会话哈希、耗时与结果分类（`success` / `business_error` / `protocol_error` / `notification`），严格排除参数、返回值、凭证与项目路径；单 Host 最多保留 4 个 8 MiB 事件文件，启动清理保留最多 30 个目录及 14 天内记录。

## 回归与冒烟测试

构建前静态校验与跨平台冒烟测试：

```bash
# 依赖方向、构建面与工具清单快速校验
python3 misc/scripts/validate_mcp_dependencies.py --root .
python3 misc/scripts/validate_mcp_build_surfaces.py --root .
python3 misc/scripts/validate_mcp_tool_manifest.py --root .

# 核心 CLI / Host 跨平台冒烟测试（Windows 可使用 py -3 与 *.console.exe）
python3 tests/editor/mcp/test_mcp_cli_smoke.py --binary <editor-binary>

# 追踪策略、持久化生命周期与外部变更确认回归（桌面环境可附加 --gui）
python3 tests/editor/mcp/test_mcp_trace_smoke.py --binary <editor-binary>
python3 tests/editor/mcp/test_mcp_editor_lifecycle.py --binary <editor-binary>
python3 tests/editor/mcp/test_mcp_external_changes_smoke.py --binary <editor-binary>
```

关于会话恢复边界、私有记录目录权限（Unix `0700/0600` / Windows 用户独占 ACL）及编辑器托管文件冲突确认细节，请参阅英文完整版 [README.md](README.md) 与 [ARCHITECTURE.md](ARCHITECTURE.md)。
