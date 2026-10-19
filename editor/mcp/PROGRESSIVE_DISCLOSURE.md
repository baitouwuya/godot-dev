# Progressive disclosure of embedded MCP tools

Design delivered in chat before source edits, against
`c173101c043a57c019302d4486dafb31629a9d38`. Stage 1 adds catalog inspection;
stage 2 now implements opt-in compact advertisement and host measurement support.
Client integration and revision-aware caching remain proposed. No capability is
removed. Stage 2 runtime behavior and savings remain pending host verification.

## Inventory and cost

Inspected the ordered manifest, all 18 Provider registration blocks, their schema
builders, shared tool utilities, built-in composition and feature lifecycle,
registry, protocol, schema validator, trace observer, Host/HTTP dispatch, README,
ARCHITECTURE, CONTRIBUTING, and the relevant registry, Provider and CLI tests.

`MCPBuiltinFeatures` orders the Providers. `MCPEditorFeatureSet` coordinates their
registration and reverse cleanup. Each Provider uses `MCPToolUtils::register_tools`
to install definitions and handlers into the single `MCPToolRegistry`.
Registration validates schemas, normalizes definitions, and invalidates the
definition cache. In the baseline and current full mode, `MCPProtocol` returns
all cached definitions, deeply copied, on every `tools/list`; there is no
pagination. Each Host session owns
its protocol. HTTP and stdio do not own another business registry.

The baseline full build has 111 tools:

| Provider | Count | Namespace family |
| --- | ---: | --- |
| Automation | 1 | automation |
| Class | 2 | class |
| Debug | 4 | debug |
| Runtime | 37 | runtime |
| Harness | 4 | runtime.harness |
| Editor | 3 | editor |
| Editor UI | 2 | editor.ui |
| Project | 7 | project |
| GDExtension | 3 | gdextension |
| Autoload | 3 | autoload |
| Input Map | 3 | input |
| File | 1 | file |
| Resource | 5 | resource |
| Scene | 4 | scene |
| Node | 10 | node |
| Node structure | 6 | node |
| Script | 6 | script |
| GDScript | 10 | gdscript |

Aggregate family counts: runtime 41, node 16, gdscript 10, project 7, script 6,
editor 5, resource 5, debug 4, scene 4, autoload 3, input 3, gdextension 3,
class 2, automation 1, file 1. Script and GDScript depend on the optional
GDScript/LSP build surface; the manifest describes the full configuration.

Definitions contain names, descriptions, input/output schemas and four behavior
hints. Shared C++ schema builders still expand separately in every advertised
definition. The principal source-based bulk candidates are the runtime family's
repeated selectors, rectangles, session/generation fields and job results;
GDScript document/position alternatives and rich output schemas; node/resource
property metadata; editor UI snapshots; and repeated Harness summaries.
The open Harness script object does not advertise its entire internal language.

This is a structural assessment, not a measured byte ranking. The manifest has
names only. Exact bytes, tokens, per-tool ranking, timing and savings are pending
host verification. Save a real `tools/list` result and count compact UTF-8 JSON
bytes separately for descriptions, input schemas, output schemas and annotations,
grouped by tool and family. Report serializer and protocol version. Measure
complete workflows as well as initial advertisement; result payload size is a
separate cost.

## Contract

Stage 1 adds ordinary read-only, idempotent tools, with `openWorldHint=false`:

| Tool | Closed input object | Successful structuredContent |
| --- | --- | --- |
| `godot.catalog.search` | Optional `query`: string, default empty, at most 128 characters; `prefix`: string, default empty, at most 128; `offset`: integer, default 0, range 0..2147483647; `limit`: integer, default 20, range 1..50. | `{tools: [{name, description, annotations}], total, nextOffset}` |
| `godot.catalog.describe` | Required nonempty string `name`: exact registered name. | `{tool: <complete current registry definition>}` |

Search combines a case-sensitive name prefix with a case-insensitive substring
query over name or full description. Empty filters enumerate all live callable
tools in registration order. Descriptions in summaries are capped at 256
characters. Search returns no schemas. `total` counts all matches before paging;
`nextOffset` is the next match offset or -1 at exhaustion. An offset beyond the
end returns an empty page. Unmatched searches succeed with an empty array.

Describe returns the complete definition, including unchanged input/output schemas
and annotations, without truncation. Unknown or no-longer-callable targets return
a tool error `TOOL_NOT_FOUND`. Malformed arguments use `INVALID_ARGUMENTS` and the
existing registry validator. An unregistered catalog Provider returns
`CATALOG_UNAVAILABLE`. These are catalog lookup errors, not changes to registry
call statuses. `structuredContent` is authoritative, with existing text-content
compatibility behavior.

Invocation remains the existing protocol contract:

```json
{"method":"tools/call","params":{"name":"godot.node.rename","arguments":{"path":"OldName","name":"NewName"}}}
```

Use the described schema to form arguments. There is no generic invoke wrapper.
Canonical names remain visible to client authorization and ordinary tracing.

## Compact mode implemented in stage 2

Always advertise `godot.catalog.search`, `godot.catalog.describe`,
`godot.editor.get_state`, `godot.runtime.get_state`, and `godot.project.inspect`.
All other tools remain registered and available through discovery and canonical
`tools/call`. The three existing core tools orient the client to editor, runtime
and configuration state.

Compact mode requires an explicit Godot extension in initialize capabilities:

```json
{"experimental":{"io.godot/tool-discovery":{"version":1}}}
```

The server acknowledges the same extension in its capabilities only for numeric
version 1 (integer 1 or floating-point 1.0), and only if all five named tools are
currently registered and callable. Missing or malformed nested objects, absent
version, strings, booleans, or unsupported versions fall back to the unchanged
full advertisement without acknowledging the extension. Existing validation of
the required initialize envelope remains unchanged. The client must observe the
acknowledgement before using compact mode. A supporting client reads structuredContent, loads the
definition returned by describe into its model's callable tool set, and forwards
canonical calls. Describe does not mutate the server's advertised list.
Ordinary clients, including clients that cannot load definitions, keep full
advertisement. Do not assume ordinary MCP clients support this extension.

The negotiated flag belongs to `MCPProtocol`, not to the registry or Provider,
and is cleared by `reset_session`. Full and compact sessions can share a registry.
Compact listing deep-copies exactly the five live definitions in the order above;
full listing retains the original path and registration order. No call dispatch,
schema validation, error-status mapping, or trace-observer path is changed.
If a core tool disappears after negotiation, listing returns `INTERNAL_ERROR`
and asks for a new session rather than returning a partial list. Negotiation of
that new session falls back to full mode if the core is still incomplete.

The orienting tools were inspected and retained without changes. Each has a
closed empty input schema and a structured object output schema:

- Editor state supplies the current scene, unsaved scenes, open/unsaved scripts,
  and undo/redo availability.
- Runtime state supplies playing state, scene/process identity, and debugger
  sessions with generation information when available.
- Project inspect supplies the configuration path/revision, file existence,
  managed/ordinary setting counts, and dedicated configuration tool names.

Their existing summaries accurately describe these outputs. They orient a client
without introducing new tool names or changing existing result schemas.

Stage 1 preserves the original 111 definitions and order, appending two catalog
tools for 113 full-build definitions. Default advertisement is therefore larger
in this stage. It enables client-side lazy loading and catalog inspection; it
does not claim initial-advertisement savings. Optional builds discover only their
actual registry. The manifest remains a compatibility fixture, never registration
or discovery input.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Wrong editor/runtime tool selected | Preserve namespaces, descriptions and annotations; describe unfamiliar tools before calling. |
| Missing capability or weak lexical search | Empty-query prefix browsing and exhaustive pagination; explicit totals and end marker; tests compare enumeration with the whole registry. |
| Stale discovery | Query live entries, exclude invalid handlers, refresh after reconnect/schema errors, and always validate invocation against current definitions. |
| Registry changes between pages | Pages are live, not an atomic snapshot. Restart enumeration after registration changes. No cache is added by compact mode; revision-aware caching remains deferred. |
| Extra round trips | Unknown tool: search, describe, call. Known name skips search. Host/session-scoped caching needs invalidation; measure end-to-end workloads before rollout. |
| Client cannot invoke discovered tools | Negotiate compact mode explicitly; keep full mode as the default and fallback. |
| Weakened schema validation or authorization | Discovery reads definitions only; canonical calls retain current validation, status mapping and client policy. No invoke wrapper. |
| Trace changes or query leakage | Use ordinary catalog spans; do not add arguments to trace metadata. Target calls keep their existing names, statuses and nesting. |
| Excessive catalog output/allocation | Search copies schema-free metadata and emits at most 50 summaries; describe copies one definition; no retained jobs or session store. |
| Optional builds and teardown | Editor MCP/core-only dependency; composition owns the Provider, shared registration rolls back, and unregister is idempotent. |

The catalog Provider owns only registration and a non-owning registry pointer.
It has no per-session state. Schema parsing and pure query construction live in
`mcp_catalog_tool_utils`; no extra service object is needed. Composition allocates
it last, appends it after optional Providers, and deletes it first after feature
cleanup. Session removal and shutdown have no retained catalog work to cancel.

## Stages and verification

1. Add live registry summaries and exact-definition access, then the catalog
   Provider and schemas, preserving full advertisement and direct invocation.
   Add focused C++ tests and real-stdio smoke assertions. This is the implemented
   source stage; execution remains pending host verification.
2. Implement negotiated compact advertisement without adding a schema cache.
   Protocol tests cover fallback, acknowledgement, exact definitions/order,
   shared-registry sessions, canonical dispatch, reset, and a missing core tool.
   Catalog tests cover real describe calls in both modes without list mutation.
   The CLI smoke checks the same contract against a live Host and calls all three
   orienting tools. Source implemented; execution remains host-pending.
3. Run the supplied payload measurement on the host, measure workflow round trips,
   then tune the core and search guidance using evidence. Revision-aware discovery
   consistency, cached client definition loading, and client integration remain
   proposed; this stage does not promise cached definitions remain current.

Stage 1 fixture changes: append catalog names to
`tests/editor/mcp/data/mcp_tool_manifest.json`; change the static validator and
CLI smoke's expected count to 113; extend the smoke with discovery, exact schema
equality, exhaustive enumeration, errors and canonical calls. Extend
`tests/core/mcp/test_mcp_tool_registry.cpp` for isolated/live introspection and add
`tests/editor/mcp/test_mcp_catalog_provider.cpp` for bounds, filtering, paging,
description completeness, errors, registration rollback and cleanup. Existing
domain assertions and fixtures stay intact.

Stage 2 leaves `tests/editor/mcp/data/mcp_tool_manifest.json` and
`misc/scripts/validate_mcp_tool_manifest.py` unchanged. They still describe and
validate all 113 registered full-build tools in their existing order. No tool is
added or removed. Compact mode has separate exact five-name assertions in C++
and Python; the complete baseline mapping below is preserved.

### Host payload comparison

From the checkout on the host, with `$EditorBinary` pointing at the new editor
and `$ProjectPath` pointing at an existing project inside this checkout already
running an MCP Host from that build:

```powershell
py -3 tests/editor/mcp/test_mcp_cli_smoke.py --binary "$EditorBinary" --measure-advertisement --project "$ProjectPath"
```

This opens three sequential stdio sessions (full, unsupported version 2, compact
version 1) against the same Host instance. It starts no editor, changes no project
content, and invokes only read-only tools. It validates negotiation, exact
definitions, hidden canonical calls, orienting tools, and list stability, then
prints JSON with protocol version, binary path, instance ID, tool counts and:

- `toolsListResponseBytes`: UTF-8 bytes of the actual JSON-RPC response line for
  `tools/list` request ID 2, excluding the line terminator and transport headers.
- `toolsListResultBytes`: UTF-8 bytes of the result object reserialized using
  Python `json.dumps(ensure_ascii=False, separators=(",", ":"))`.
- `summedDefinitionBytes`: sum of the UTF-8 sizes of the individual definitions
  using the same Python serializer, excluding the array and response envelope.

The primary comparison is full versus compact `toolsListResponseBytes` from this
same run. For the earlier host-reported baseline of 111 tools and 210,575 result
bytes, compare result bytes only after confirming the same serializer and byte
boundary; raw response bytes include the JSON-RPC envelope and cannot be compared
directly to that result-only number. The current full mode also contains the two
stage 1 catalog tools. No payload counts, timings, or savings were measured in
the VM. End-to-end discovery round-trip cost and client behavior remain unmeasured.

Required host gate: focused C++ protocol/registry/catalog tests, complete `[MCP]*` suite,
real CLI and trace smokes, SCons configuration check, and the documented MCP
enabled/disabled, SCU/non-SCU and debug-template build matrix. No execution-based
conclusion is established merely by source inspection or static validators.

Stage 1 historical VM verification record: the dependency, build-surface and 113-tool manifest
validators passed; all 27 Python unit tests for those validators passed. Python
syntax, the unchanged 111-entry manifest prefix, the complete mapping below,
header/file-format scripts and `git diff --check` were checked locally. No C++
tests, editor, transport smoke or performance measurements ran. The SCons config
check could not start SCons because that executable is unavailable. Its first
attempt created an automatically cleaned profile in the OS temporary directory;
this was a temporary-file scope deviation. Subsequent test temporary directories
were explicitly placed inside this checkout. No other checkout was accessed.

Stage 2 VM work is limited to targeted source inspection, edits and diff review.
No compiler, SCons, test suite, editor, or measurement command was run for stage 2.
Static validators, Python formatting/syntax gates, the focused C++ suites and
the complete suite, CLI/trace smokes, optional build configurations, and payload
measurement all remain pending host verification. C++ tests require `tests=yes`.

The branch uses explicit per-command commit identity `Codex <codex@localhost>`
because the VM has no Git author configured; repository/global configuration was
not changed. The owner's two baseline files are excluded from all commits.

## Complete baseline mapping

For every deferred row: search with the listed prefix (or an empty query),
describe the exact name, then use canonical `tools/call`. Core rows also remain
searchable. All rows continue to be advertised directly in stage 1 and in full
mode. No canonical name, arguments or result shape is changed.

| Existing tool | Provider | Search prefix | Compact-mode exposure |
| --- | --- | --- | --- |
| `godot.automation.batch` | automation | `godot.automation.` | Search -> describe -> canonical call |
| `godot.class.search` | class | `godot.class.` | Search -> describe -> canonical call |
| `godot.class.get_documentation` | class | `godot.class.` | Search -> describe -> canonical call |
| `godot.debug.get_logs` | debug | `godot.debug.` | Search -> describe -> canonical call |
| `godot.debug.get_errors` | debug | `godot.debug.` | Search -> describe -> canonical call |
| `godot.debug.get_stack` | debug | `godot.debug.` | Search -> describe -> canonical call |
| `godot.debug.get_latest_log` | debug | `godot.debug.` | Search -> describe -> canonical call |
| `godot.runtime.get_state` | runtime | `godot.runtime.` | Core + canonical call |
| `godot.runtime.play` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.stop` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.pause` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.resume` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.next_frame` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.debug.break` | runtime | `godot.runtime.debug.` | Search -> describe -> canonical call |
| `godot.runtime.debug.continue` | runtime | `godot.runtime.debug.` | Search -> describe -> canonical call |
| `godot.runtime.debug.step_into` | runtime | `godot.runtime.debug.` | Search -> describe -> canonical call |
| `godot.runtime.debug.step_over` | runtime | `godot.runtime.debug.` | Search -> describe -> canonical call |
| `godot.runtime.debug.step_out` | runtime | `godot.runtime.debug.` | Search -> describe -> canonical call |
| `godot.runtime.get_tree` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.node.get_properties` | runtime | `godot.runtime.node.` | Search -> describe -> canonical call |
| `godot.runtime.node.set_property` | runtime | `godot.runtime.node.` | Search -> describe -> canonical call |
| `godot.runtime.input.send` | runtime | `godot.runtime.input.` | Search -> describe -> canonical call |
| `godot.runtime.input.sequence` | runtime | `godot.runtime.input.` | Search -> describe -> canonical call |
| `godot.runtime.input.sequence_status` | runtime | `godot.runtime.input.` | Search -> describe -> canonical call |
| `godot.runtime.input.sequence_cancel` | runtime | `godot.runtime.input.` | Search -> describe -> canonical call |
| `godot.runtime.input.release_all` | runtime | `godot.runtime.input.` | Search -> describe -> canonical call |
| `godot.runtime.get_screenshot` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.get_viewport_summary` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.query_nodes` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.get_interactables` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.get_node_snapshot` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.click_target` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.double_click_target` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.hover_target` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.focus_target` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.drag_target_to_target` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.type_text` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.scroll_view` | runtime | `godot.runtime.` | Search -> describe -> canonical call |
| `godot.runtime.wait.start` | runtime | `godot.runtime.wait.` | Search -> describe -> canonical call |
| `godot.runtime.wait.status` | runtime | `godot.runtime.wait.` | Search -> describe -> canonical call |
| `godot.runtime.wait.cancel` | runtime | `godot.runtime.wait.` | Search -> describe -> canonical call |
| `godot.runtime.performance.start` | runtime | `godot.runtime.performance.` | Search -> describe -> canonical call |
| `godot.runtime.performance.status` | runtime | `godot.runtime.performance.` | Search -> describe -> canonical call |
| `godot.runtime.performance.stop` | runtime | `godot.runtime.performance.` | Search -> describe -> canonical call |
| `godot.runtime.harness.start` | harness | `godot.runtime.harness.` | Search -> describe -> canonical call |
| `godot.runtime.harness.status` | harness | `godot.runtime.harness.` | Search -> describe -> canonical call |
| `godot.runtime.harness.cancel` | harness | `godot.runtime.harness.` | Search -> describe -> canonical call |
| `godot.runtime.harness.get_report` | harness | `godot.runtime.harness.` | Search -> describe -> canonical call |
| `godot.editor.get_state` | editor | `godot.editor.` | Core + canonical call |
| `godot.editor.undo` | editor | `godot.editor.` | Search -> describe -> canonical call |
| `godot.editor.redo` | editor | `godot.editor.` | Search -> describe -> canonical call |
| `godot.editor.ui.get_actions` | editor_ui | `godot.editor.ui.` | Search -> describe -> canonical call |
| `godot.editor.ui.perform` | editor_ui | `godot.editor.ui.` | Search -> describe -> canonical call |
| `godot.project.inspect` | project | `godot.project.` | Core + canonical call |
| `godot.project.get_settings` | project | `godot.project.` | Search -> describe -> canonical call |
| `godot.project.get_setting` | project | `godot.project.` | Search -> describe -> canonical call |
| `godot.project.set_setting` | project | `godot.project.` | Search -> describe -> canonical call |
| `godot.project.erase_setting` | project | `godot.project.` | Search -> describe -> canonical call |
| `godot.project.apply` | project | `godot.project.` | Search -> describe -> canonical call |
| `godot.project.save` | project | `godot.project.` | Search -> describe -> canonical call |
| `godot.gdextension.build` | gdextension | `godot.gdextension.` | Search -> describe -> canonical call |
| `godot.gdextension.build_status` | gdextension | `godot.gdextension.` | Search -> describe -> canonical call |
| `godot.gdextension.build_cancel` | gdextension | `godot.gdextension.` | Search -> describe -> canonical call |
| `godot.autoload.get_all` | autoload | `godot.autoload.` | Search -> describe -> canonical call |
| `godot.autoload.add` | autoload | `godot.autoload.` | Search -> describe -> canonical call |
| `godot.autoload.remove` | autoload | `godot.autoload.` | Search -> describe -> canonical call |
| `godot.input.get_actions` | input_map | `godot.input.` | Search -> describe -> canonical call |
| `godot.input.set_action` | input_map | `godot.input.` | Search -> describe -> canonical call |
| `godot.input.remove_action` | input_map | `godot.input.` | Search -> describe -> canonical call |
| `godot.file.create` | file | `godot.file.` | Search -> describe -> canonical call |
| `godot.resource.get_properties` | resource | `godot.resource.` | Search -> describe -> canonical call |
| `godot.resource.set_property` | resource | `godot.resource.` | Search -> describe -> canonical call |
| `godot.resource.save` | resource | `godot.resource.` | Search -> describe -> canonical call |
| `godot.resource.import_options` | resource | `godot.resource.` | Search -> describe -> canonical call |
| `godot.resource.import` | resource | `godot.resource.` | Search -> describe -> canonical call |
| `godot.scene.open` | scene | `godot.scene.` | Search -> describe -> canonical call |
| `godot.scene.get_tree` | scene | `godot.scene.` | Search -> describe -> canonical call |
| `godot.scene.get_selection` | scene | `godot.scene.` | Search -> describe -> canonical call |
| `godot.scene.save` | scene | `godot.scene.` | Search -> describe -> canonical call |
| `godot.node.get_properties` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.create` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.set_property` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.attach_script` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.get_groups` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.add_to_group` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.remove_from_group` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.get_signal_connections` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.connect_signal` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.disconnect_signal` | node | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.delete` | node_structure | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.rename` | node_structure | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.reparent` | node_structure | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.move` | node_structure | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.duplicate` | node_structure | `godot.node.` | Search -> describe -> canonical call |
| `godot.node.instantiate_scene` | node_structure | `godot.node.` | Search -> describe -> canonical call |
| `godot.script.create` | script | `godot.script.` | Search -> describe -> canonical call |
| `godot.script.open` | script | `godot.script.` | Search -> describe -> canonical call |
| `godot.script.get` | script | `godot.script.` | Search -> describe -> canonical call |
| `godot.script.usages` | script | `godot.script.` | Search -> describe -> canonical call |
| `godot.script.edit` | script | `godot.script.` | Search -> describe -> canonical call |
| `godot.script.save` | script | `godot.script.` | Search -> describe -> canonical call |
| `godot.gdscript.diagnostics` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
| `godot.gdscript.symbols` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
| `godot.gdscript.completion` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
| `godot.gdscript.hover` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
| `godot.gdscript.definition` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
| `godot.gdscript.declaration` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
| `godot.gdscript.references` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
| `godot.gdscript.signature_help` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
| `godot.gdscript.rename` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
| `godot.gdscript.apply_workspace_edit` | gdscript | `godot.gdscript.` | Search -> describe -> canonical call |
