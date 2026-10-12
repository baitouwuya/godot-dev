#!/usr/bin/env python3

"""Verify opt-in, metadata-only traces through an ordinary MCP Host and stdio bridge."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator, Sequence, cast

from mcp_smoke_test_utils import (
    HostProcess,
    ProcessRunner,
    SmokeFailure,
    invoke_stdio,
    remove_tree_with_retry,
    request,
    require,
    tool_call,
)
from test_mcp_cli_smoke import new_smoke_project, structured_result, tool_error_code, wait_for_discovery, wait_for_file
from test_mcp_editor_lifecycle import isolated_editor_environment

TOOL_NAME = "godot.project.get_setting"
ARGUMENT_SENTINEL = "TRACE_PRIVATE_ARGUMENT_5b189e67"
RESULT_SENTINEL = "TRACE_PRIVATE_RESULT_c22f43ad"
ERROR_SENTINEL = "TRACE_PRIVATE_ERROR_37adb9c4"
METHOD_SENTINEL = "TRACE_PRIVATE_METHOD_b8e43dc1"
TOOL_SENTINEL = "TRACE_PRIVATE_TOOL_0d59d8fa"
TEXT_SENTINEL = "TRACE_PRIVATE_TEXT_f583602e"
PATH_SENTINEL = "TRACE_PRIVATE_PATH_7a6ce228"
REQUEST_ID = 730011234567890
SETTING_NAME = f"mcp_trace_smoke/{ARGUMENT_SENTINEL}"
EVENT_FIELDS = {
    "seq",
    "tsUnixUsec",
    "monoUsec",
    "sessionId",
    "category",
    "event",
    "severity",
    "correlationId",
    "data",
    "payloads",
    "error",
}
METHOD_NAMES = {"initialize", "notifications/initialized", "tools/call", "unknown"}
BASE_DATA_FIELDS = {"name", "sessionHash", "parentId", "rootId"}
FINISHED_DATA_FIELDS = BASE_DATA_FIELDS | {"durationUsec", "outcome", "status", "isError"}


@contextmanager
def trace_environment(value: str | None, directory: Path) -> Iterator[None]:
    values = {"GODOT_MCP_TRACE": value, "GODOT_MCP_TRACE_DIR": str(directory)}
    previous = {name: os.environ.get(name) for name in values}
    try:
        for name, setting in values.items():
            if setting is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = setting
        yield
    finally:
        for name, setting in previous.items():
            if setting is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = setting


def new_trace_project(path: Path) -> None:
    new_smoke_project(path, "MCP Trace Smoke")
    with (path / "project.godot").open("a", encoding="utf-8") as project:
        project.write(f'\n[mcp_trace_smoke]\n{ARGUMENT_SENTINEL}="{RESULT_SENTINEL}"\n')


def exercise_stdio(runner: ProcessRunner, host: HostProcess, protocol: str) -> None:
    # invoke_stdio checks every stdout line is one expected JSON-RPC response and
    # keeps stdin open between calls. No Harness job or runtime launch is needed.
    responses = invoke_stdio(
        runner,
        host.project_path,
        protocol,
        [
            tool_call(REQUEST_ID, TOOL_NAME, {"name": SETTING_NAME}),
            tool_call(REQUEST_ID + 1, TOOL_NAME, {"name": ""}),
            tool_call(REQUEST_ID + 2, TOOL_NAME, {"name": ERROR_SENTINEL}),
            request(REQUEST_ID + 3, METHOD_SENTINEL, {"value": TEXT_SENTINEL}),
            tool_call(REQUEST_ID + 4, TOOL_SENTINEL, {"value": TEXT_SENTINEL}),
        ],
        host.label,
    )
    require(
        structured_result(responses, REQUEST_ID, "get_setting success").get("value") == RESULT_SENTINEL,
        "get_setting did not return the private fixture value.",
    )
    require(
        tool_error_code(responses, REQUEST_ID + 1, "get_setting empty name") == "INVALID_ARGUMENTS",
        "The empty-name request did not exercise a business failure.",
    )
    require(
        tool_error_code(responses, REQUEST_ID + 2, "get_setting missing name") == "SETTING_NOT_FOUND",
        "The missing setting did not exercise an error containing private input.",
    )
    for offset, code in ((3, -32601), (4, -32602)):
        require(
            responses[REQUEST_ID + offset].get("error", {}).get("code") == code,
            "The unknown method/tool did not exercise the expected protocol failure.",
        )
    host.require_running()


def read_trace(directory: Path) -> tuple[Path, dict[str, Any], list[dict[str, Any]]]:
    manifests = sorted(directory.glob("trace-*/manifest.json"))
    require(len(manifests) == 1, f"Expected one Host trace manifest, found {len(manifests)} in {directory}.")
    manifest_path = manifests[0]
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    require(isinstance(manifest, dict), "Trace manifest is not an object.")
    require(manifest.get("schemaVersion") == 1, "Unexpected trace manifest schema.")
    event_files = manifest.get("eventFiles")
    require(isinstance(event_files, list) and 1 <= len(event_files) <= 4, "Trace event files are missing or unbounded.")
    events: list[dict[str, Any]] = []
    for filename in event_files:
        require(
            isinstance(filename, str) and re.fullmatch(r"events-[0-9]+\.jsonl", filename),
            "Manifest event file is not a local JSONL filename.",
        )
        path = manifest_path.parent / filename
        require(path.stat().st_size <= 8 * 1024 * 1024, "Trace event file exceeded its byte limit.")
        for line in path.read_text(encoding="utf-8").splitlines():
            require(bool(line), "Trace contains an empty JSONL record.")
            event = json.loads(line)
            require(isinstance(event, dict), "Trace event is not an object.")
            events.append(event)
    require(bool(events), "Ordinary MCP calls produced no structured trace events.")
    require(
        set(manifest_path.parent.glob("events-*.jsonl"))
        == {manifest_path.parent / filename for filename in event_files},
        "The trace manifest does not account for all event files.",
    )
    return manifest_path, cast(dict[str, Any], manifest), events


def require_private_trace(directory: Path) -> None:
    sentinels = (
        ARGUMENT_SENTINEL,
        RESULT_SENTINEL,
        ERROR_SENTINEL,
        METHOD_SENTINEL,
        TOOL_SENTINEL,
        TEXT_SENTINEL,
        PATH_SENTINEL,
        "A non-empty setting name is required.",
        "Project setting was not found:",
        "Unknown method:",
        "Unknown tool:",
        *(str(REQUEST_ID + offset) for offset in range(5)),
    )
    for path in directory.rglob("*"):
        if path.is_file():
            contents = path.read_text(encoding="utf-8")
            for sentinel in sentinels:
                require(sentinel not in contents, f"Private request/result/error data leaked into {path.name}.")
    require(not any(directory.glob("trace-*/payloads/*")), "Ordinary tracing wrote payload sidecars.")


def require_correlated_spans(events: list[dict[str, Any]], sessions: int) -> None:
    started: dict[str, dict[str, Any]] = {}
    finished: dict[str, dict[str, Any]] = {}
    for sequence, event in enumerate(events, 1):
        require(set(event) == EVENT_FIELDS, "Trace escaped its event-envelope field allowlist.")
        require(event.get("seq") == sequence, "Trace sequence is not ordered and complete.")
        require(event.get("category") in {"mcp", "tool"}, "Ordinary tracing emitted an unexpected category.")
        require(event.get("payloads") == {} and event.get("error") is None, "Trace persisted payload/error content.")
        span_value = event.get("correlationId")
        require(
            isinstance(span_value, str) and span_value.isdecimal(), "Trace span ID is not an internal decimal string."
        )
        span_id = cast(str, span_value)
        data_value = event.get("data")
        require(isinstance(data_value, dict), "Trace event has no metadata object.")
        data = cast(dict[str, Any], data_value)
        phase = event.get("event")
        require(phase in {"started", "finished"}, "Trace omitted the started/finished lifecycle.")
        require(
            set(data) == (BASE_DATA_FIELDS if phase == "started" else FINISHED_DATA_FIELDS),
            "Ordinary trace data escaped its metadata-only field allowlist.",
        )
        require(
            data["name"] in (METHOD_NAMES if event["category"] == "mcp" else {TOOL_NAME, "unknown"}),
            "Trace persisted an unexpected method/tool name.",
        )
        session_hash = data["sessionHash"]
        require(
            isinstance(session_hash, str)
            and (re.fullmatch(r"[a-f0-9]{16}", session_hash) is not None or session_hash == ""),
            "Trace session ID is not a bounded hash.",
        )
        if phase == "started":
            require(span_id not in started, "Trace reused a span ID.")
            started[span_id] = event
        else:
            require(span_id in started and span_id not in finished, "Trace finish has no unique prior start.")
            start = started[span_id]
            require(start["category"] == event["category"], "Span category changed at finish.")
            require(all(data[key] == start["data"][key] for key in BASE_DATA_FIELDS), "Span correlation changed.")
            require(
                type(data["durationUsec"]) in (int, float) and data["durationUsec"] >= 0,
                "Finished span has no nonnegative duration.",
            )
            require(type(data["status"]) in (int, float), "Finished span status is not numeric.")
            require(type(data["isError"]) is bool, "Finished span isError is not boolean.")
            require(
                data["outcome"] in {"success", "business_error", "protocol_error", "notification"},
                "Finished span has an unknown outcome.",
            )
            finished[span_id] = event
    require(set(started) == set(finished), "Completed ordinary requests left unfinished spans.")

    setting_spans = [e for e in finished.values() if e["category"] == "tool" and e["data"]["name"] == TOOL_NAME]
    require(len(setting_spans) == 3 * sessions, "Expected all successful and failed get_setting spans.")
    hashes = {event["data"]["sessionHash"] for event in setting_spans}
    require(len(hashes) == sessions and "" not in hashes, "Independent MCP sessions were not separately hashed.")
    for session_hash in hashes:
        calls = [event for event in setting_spans if event["data"]["sessionHash"] == session_hash]
        require(
            sorted(event["data"]["outcome"] for event in calls) == ["business_error", "business_error", "success"],
            "Business errors were classified as successful protocol responses.",
        )
    for event in finished.values():
        data = event["data"]
        span_id = event["correlationId"]
        if event["category"] == "mcp":
            require(data["parentId"] == "" and data["rootId"] == span_id, "Protocol root correlation is invalid.")
            continue
        parent_id = data["parentId"]
        require(parent_id in started, "Tool span has no protocol parent.")
        parent = started[parent_id]
        require(parent["category"] == "mcp" and parent["data"]["name"] == "tools/call", "Wrong tool parent.")
        require(data["rootId"] == parent_id, "Tool span lost its protocol root ID.")
        require(data["sessionHash"] == parent["data"]["sessionHash"], "Tool and protocol session hashes differ.")
        require(
            parent["seq"] < started[span_id]["seq"] < event["seq"] < finished[parent_id]["seq"], "Spans are not nested."
        )
        require(finished[parent_id]["data"]["outcome"] == data["outcome"], "Protocol and tool outcomes disagree.")
        require(data["isError"] == (data["outcome"] != "success"), "Tool isError disagrees with its outcome.")
        if data["name"] == TOOL_NAME:
            require(data["status"] == 0, "Business results should preserve the successful Registry call status.")
    unknown_methods = [e for e in finished.values() if e["category"] == "mcp" and e["data"]["name"] == "unknown"]
    unknown_tools = [e for e in finished.values() if e["category"] == "tool" and e["data"]["name"] == "unknown"]
    require(len(unknown_methods) == sessions and len(unknown_tools) == sessions, "Unknown names were not normalized.")
    for event in unknown_methods:
        require(event["data"]["status"] == -32601, "Protocol error code was not preserved.")
        require(event["data"]["outcome"] == "protocol_error", "Unknown method was not a protocol error.")
    for event in unknown_tools:
        require(event["data"]["status"] != 0, "Failed Registry call lost its status.")
        require(finished[event["data"]["parentId"]]["data"]["status"] == -32602, "Tool protocol code was lost.")


def check_host(runner: ProcessRunner, root: Path, value: str | None, enabled: bool) -> None:
    label = "trace-enabled" if enabled else f"trace-disabled-{value or 'unset'}"
    project = root / f"{label}-{PATH_SENTINEL}"
    trace_root = root / f"{label}-records"
    new_trace_project(project)
    with trace_environment(value, trace_root):
        host = HostProcess(runner.host_binary, project, label, root)
        try:
            discovery = wait_for_discovery(runner, host, runner.timeout_seconds)
            wait_for_file(host, ".mcp-smoke-scene-ready", runner.timeout_seconds)
            exercise_stdio(runner, host, str(discovery["protocolVersion"]))
            if not enabled:
                require(not trace_root.exists(), "Opted-out Host created trace storage.")
                result = host.stop(require_graceful=True)
                require(not trace_root.exists(), "Opted-out Host wrote a trace during shutdown.")
                require("MCP_TRACE_" not in result.stderr + result.stdout, "Opted-out Host emitted a trace diagnostic.")
                return

            manifest_path, manifest, events = read_trace(trace_root)
            require(not manifest.get("endedAtUtc"), "Live Host trace was already closed at bridge EOF.")
            require_correlated_spans(events, 1)
            require_private_trace(trace_root)
            # A second bridge/session must append to the existing Host trace.
            exercise_stdio(runner, host, str(discovery["protocolVersion"]))
            second_path, second_manifest, before_shutdown = read_trace(trace_root)
            require(second_path == manifest_path, "A new stdio session restarted the Host trace.")
            require(before_shutdown[: len(events)] == events, "A new session replaced earlier trace events.")
            require(not second_manifest.get("endedAtUtc"), "Second bridge EOF closed the Host trace.")
            require_correlated_spans(before_shutdown, 2)
            result = host.stop(require_graceful=True)
            final_path, final_manifest, after_shutdown = read_trace(trace_root)
            require(final_path == manifest_path, "Shutdown replaced the Host trace.")
            require(bool(final_manifest.get("endedAtUtc")), "Graceful shutdown did not finalize the manifest.")
            require(after_shutdown == before_shutdown, "Graceful shutdown lost or changed completed events.")
            require_private_trace(trace_root)
            require(result.stderr.count("MCP_TRACE_ENABLED") == 1, "Host must report opt-in once on stderr.")
            require("MCP_TRACE_FAILED" not in result.stderr, "Trace failed during ordinary Host use.")
            require("MCP_TRACE_" not in result.stdout, "Trace diagnostic polluted stdout.")
        finally:
            host.stop()


def run_trace_smoke(binary: str, timeout: int, keep_temporary_projects: bool) -> None:
    executable = ProcessRunner.resolve_binary(binary)
    root = Path(tempfile.mkdtemp(prefix="godot-mcp-trace-smoke-"))
    success = False
    print(f"Temporary fixture root: {root}", flush=True)
    try:
        with isolated_editor_environment(root / "environment"):
            runner = ProcessRunner(executable, root, timeout)
            print("[1/3] Ordinary MCP requests do not trace by default", flush=True)
            check_host(runner, root, None, False)
            print("[2/3] Non-opt-in values keep ordinary tracing disabled", flush=True)
            check_host(runner, root, "off", False)
            print(
                "[3/3] Opt-in traces correlate success/errors without payloads across bridge EOF and Host shutdown",
                flush=True,
            )
            check_host(runner, root, "1", True)
        success = True
        print("MCP ordinary trace regression test passed.", flush=True)
    finally:
        if keep_temporary_projects or not success:
            print(f"Temporary projects and logs kept at: {root}", flush=True)
        else:
            remove_tree_with_retry(root)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="Godot editor binary built with MCP support.")
    parser.add_argument("--timeout-seconds", type=int, default=45, help="Per-operation timeout (5-300 seconds).")
    parser.add_argument(
        "--keep-temporary-projects", action="store_true", help="Keep fixtures after a successful run too."
    )
    args = parser.parse_args(argv)
    if not 5 <= args.timeout_seconds <= 300:
        parser.error("--timeout-seconds must be between 5 and 300")
    try:
        run_trace_smoke(args.binary, args.timeout_seconds, args.keep_temporary_projects)
    except (SmokeFailure, OSError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("MCP trace smoke test interrupted.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
