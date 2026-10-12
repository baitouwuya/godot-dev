#!/usr/bin/env python3

"""Real-process MCP editor ownership checks; --gui also tests detached startup."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator, Mapping, Sequence, cast

from mcp_smoke_test_utils import (
    CommandResult,
    HostProcess,
    ProcessRunner,
    SmokeFailure,
    describe_command_result,
    json_line,
    notification,
    parse_discovery,
    parse_jsonrpc_responses,
    remove_tree_with_retry,
    request,
    require,
    tool_call,
    wait_until,
)
from test_mcp_cli_smoke import new_smoke_project, structured_result, wait_for_discovery, wait_for_file, write_text

START_PREFIX = ".mcp-lifecycle-start-"
EXIT_PREFIX = ".mcp-lifecycle-exit-"


@contextmanager
def isolated_editor_environment(root: Path) -> Iterator[None]:
    """Keep editor settings/discovery outside the user's HOME and XDG directories."""
    directories = {
        "HOME": root / "home",
        "USERPROFILE": root / "home",
        "APPDATA": root / "appdata",
        "LOCALAPPDATA": root / "localappdata",
        "XDG_CONFIG_HOME": root / "config",
        "XDG_DATA_HOME": root / "data",
        "XDG_CACHE_HOME": root / "cache",
        "XDG_STATE_HOME": root / "state",
    }
    previous = {name: os.environ.get(name) for name in directories}
    try:
        for name, path in directories.items():
            path.mkdir(parents=True, exist_ok=True)
            os.environ[name] = str(path)
        yield
    finally:
        for name, value in previous.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


def new_lifecycle_project(path: Path, name: str) -> None:
    new_smoke_project(path, name)
    plugin = path / "addons/mcp_smoke_cleanup/plugin.gd"
    source = plugin.read_text(encoding="utf-8")
    source = source.replace(
        "func _enter_tree() -> void:\n\tset_process(true)",
        """func _enter_tree() -> void:
\tvar marker := "res://.mcp-lifecycle-start-%d" % OS.get_process_id()
\tvar mode := FileAccess.READ_WRITE if FileAccess.file_exists(marker) else FileAccess.WRITE
\tvar started := FileAccess.open(marker, mode)
\tif started:
\t\tstarted.seek_end()
\t\tstarted.store_line(str(OS.get_process_id()))
\t\tstarted.close()
\tset_process(true)

func _exit_tree() -> void:
\tvar exited := FileAccess.open("res://.mcp-lifecycle-exit-%d" % OS.get_process_id(), FileAccess.WRITE)
\tif exited:
\t\texited.store_string("exited")
""",
    )
    require(START_PREFIX in source, "fixture plugin initialization instrumentation was not installed.")
    write_text(plugin, source)


def initialization_counts(project: Path) -> dict[int, int]:
    return {
        int(marker.name.removeprefix(START_PREFIX)): len(marker.read_text(encoding="utf-8").splitlines())
        for marker in project.glob(f"{START_PREFIX}*")
    }


def project_snapshot(project: Path) -> tuple[str, int]:
    settings = project / "project.godot"
    return hashlib.sha256(settings.read_bytes()).hexdigest(), settings.stat().st_mtime_ns


def stable_snapshot(host: HostProcess, timeout: int) -> tuple[str, int]:
    previous = project_snapshot(host.project_path)
    unchanged_since = time.monotonic()

    def attempt() -> tuple[str, int] | None:
        nonlocal previous, unchanged_since
        host.require_running()
        current = project_snapshot(host.project_path)
        if current != previous:
            previous, unchanged_since = current, time.monotonic()
        return current if time.monotonic() - unchanged_since >= 0.5 else None

    return cast(tuple[str, int], wait_until(timeout, attempt, "stable project.godot hash and modification time"))


def lifecycle_arguments(command: str, project: Path, timeout: int, allow_start: bool = False) -> list[str]:
    return [
        command,
        "--path",
        str(project),
        "--allow-start" if allow_start else "--connect-only",
        "--wait-ms",
        str(min(timeout * 1000, 120000)),
    ]


def require_identity(actual: Mapping[str, Any], expected: Mapping[str, Any], label: str) -> None:
    for field in ("pid", "instanceId", "projectId", "endpoint", "protocolVersion"):
        require(actual.get(field) == expected.get(field), f"{label} changed {field}: {actual!r} != {expected!r}")


def probe_stdio(runner: ProcessRunner, project: Path, discovery: Mapping[str, Any], allow_start: bool) -> None:
    messages = [
        request(
            1,
            "initialize",
            {
                "protocolVersion": discovery["protocolVersion"],
                "capabilities": {},
                "clientInfo": {"name": "godot-mcp-editor-lifecycle", "version": "1"},
            },
        ),
        notification("notifications/initialized", {}),
        tool_call(2, "godot.editor.get_state", {}),
    ]
    result = runner.invoke(
        lifecycle_arguments("--mcp-stdio", project, runner.timeout_seconds, allow_start),
        [json_line(message) for message in messages],
    )
    responses = parse_jsonrpc_responses(result, [1, 2], "short-lived lifecycle stdio bridge")
    require("result" in responses[1], f"stdio initialize failed: {responses[1]}")
    structured_result(responses, 2, "editor/get_state before bridge EOF")


def check_preserved(host: HostProcess, baseline: tuple[str, int]) -> None:
    host.require_running()
    require(
        initialization_counts(host.project_path) == {host.process.pid: 1},
        f"{host.label} initialized another editor/plugin: {initialization_counts(host.project_path)}",
    )
    require(project_snapshot(host.project_path) == baseline, f"{host.label} project.godot hash or mtime changed.")


def check_host_reuse(runner: ProcessRunner, host: HostProcess, timeout: int) -> None:
    wait_for_file(host, ".mcp-smoke-scene-ready", timeout)
    discovery = wait_for_discovery(runner, host, timeout)
    require(discovery["pid"] == host.process.pid, "public discovery did not identify the editor process.")
    baseline = stable_snapshot(host, timeout)
    # Exercise actual process liveness, not only the registration startup grace.
    time.sleep(5.1)
    for iteration in range(3):
        for allow_start in (False, True):
            result = runner.invoke(lifecycle_arguments("--mcp-discover", host.project_path, timeout, allow_start))
            require_identity(parse_discovery(result, host.label), discovery, f"discovery iteration {iteration + 1}")
            probe_stdio(runner, host.project_path, discovery, allow_start)
            # The bridge has exited and deleted its session; the editor must remain.
            require_identity(wait_for_discovery(runner, host, timeout), discovery, "discovery after bridge EOF")
            check_preserved(host, baseline)

    print("[2/4] Rejecting a second managed editor before plugins and project.godot writes", flush=True)
    duplicate = runner.invoke(host.arguments[1:], executable=runner.host_binary)
    require(duplicate.returncode != 0, "a second --editor --mcp unexpectedly succeeded.")
    require(
        "ALREADY_RUNNING" in duplicate.stderr,
        f"duplicate managed startup did not identify its owner: {describe_command_result(duplicate)}",
    )
    check_preserved(host, baseline)
    require_identity(wait_for_discovery(runner, host, timeout), discovery, "original Host after rejected startup")


def check_ordinary_editor(runner: ProcessRunner, editor: HostProcess, timeout: int) -> None:
    wait_for_file(editor, ".mcp-smoke-scene-ready", timeout)
    baseline = stable_snapshot(editor, timeout)
    # A broken non-child PID probe must not be hidden by a fresh presence record.
    time.sleep(5.1)
    commands = [[*editor.arguments[1:], "--mcp", "--mcp-port", "0"]]
    for command in ("--mcp-discover", "--mcp-stdio"):
        for allow_start in (False, True):
            commands.append(lifecycle_arguments(command, editor.project_path, timeout, allow_start))
    for arguments in commands:
        result = runner.invoke(
            arguments, input_lines=[], executable=runner.host_binary if "--editor" in arguments else None
        )
        require(result.returncode != 0, f"MCP access to an ordinary editor unexpectedly succeeded: {arguments}")
        require("MCP_DISABLED" in result.stderr, f"expected MCP_DISABLED: {describe_command_result(result)}")
        require(str(editor.process.pid) in result.stderr, "MCP_DISABLED omitted the preserved editor's PID.")
        check_preserved(editor, baseline)
    logs = editor.result()
    require("MCP Host started" not in logs.stdout + logs.stderr, "the ordinary editor enabled MCP implicitly.")


def process_running(pid: int) -> bool:
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE, without permission to terminate.
        if not handle:
            return ctypes.get_last_error() != 87  # ERROR_INVALID_PARAMETER means the process is gone.
        try:
            return bool(kernel.WaitForSingleObject(handle, 0) != 0)  # WAIT_OBJECT_0 means exited.
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    if sys.platform.startswith("linux"):
        try:
            if (Path("/proc") / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()[0] == "Z":
                return False
        except FileNotFoundError:
            return False
    return True


def check_concurrent_start(runner: ProcessRunner, project: Path, timeout: int) -> None:
    """Exercise the production GUI launch path; no wrapper or fake launcher."""
    arguments = [str(runner.binary), *lifecycle_arguments("--mcp-discover", project, timeout, allow_start=True)]
    callers: list[subprocess.Popen[str]] = []
    pids: set[int] = set()
    results: list[CommandResult] = []
    try:
        for _ in range(2):
            callers.append(
                subprocess.Popen(
                    arguments,
                    cwd=runner.working_directory,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                )
            )
        for caller in callers:
            try:
                stdout, stderr = caller.communicate(timeout=timeout + 5)
            except subprocess.TimeoutExpired:
                caller.kill()
                stdout, stderr = caller.communicate(timeout=5)
                results.append(CommandResult(caller.returncode, stdout, stderr, arguments))
                raise SmokeFailure("concurrent allow-start caller timed out (possibly inherited editor stdio).")
            results.append(CommandResult(caller.returncode, stdout, stderr, arguments))
        discoveries = [parse_discovery(result, "concurrent allow-start") for result in results]
        discovery = discoveries[0]
        pids.add(discovery["pid"])
        require_identity(discoveries[1], discovery, "two concurrent allow-start callers")
        require(
            sum(result.stderr.count("STARTED_EDITOR pid=") for result in results) == 1,
            f"concurrent callers launched something other than one editor: {results!r}",
        )
        wait_until(timeout, lambda: (project / ".mcp-smoke-scene-ready").is_file(), "detached editor fixture ready")
        for _ in range(3):
            probe_stdio(runner, project, discovery, allow_start=True)
            result = runner.invoke(lifecycle_arguments("--mcp-discover", project, timeout))
            require_identity(parse_discovery(result, "detached Host after callers exit"), discovery, "detached Host")
            require(initialization_counts(project) == {discovery["pid"]: 1}, "detached startup initialized twice.")
            require(process_running(discovery["pid"]), "the Host exited with its launching callers.")
    finally:
        for caller in callers:
            if caller.poll() is None:
                caller.kill()
                stdout, stderr = caller.communicate(timeout=5)
                results.append(CommandResult(caller.returncode, stdout, stderr, arguments))
        for result in results:
            pids.update(int(pid) for pid in re.findall(r"STARTED_EDITOR pid=(\d+)", result.stderr))
        pids.update(initialization_counts(project))
        (project / ".mcp-smoke-stop").write_text("stop", encoding="utf-8")
        wait_until(timeout, lambda: not any(process_running(pid) for pid in pids), f"fixture editor cleanup {pids}")
        for pid in initialization_counts(project):
            require((project / f"{EXIT_PREFIX}{pid}").is_file(), f"fixture editor {pid} did not exit gracefully.")
        if pids:
            (project / ".mcp-smoke-stop").unlink(missing_ok=True)


def run_lifecycle(binary: str, timeout: int, gui: bool, keep_temporary_projects: bool) -> None:
    executable = ProcessRunner.resolve_binary(binary)
    if gui and sys.platform.startswith("linux"):
        require(
            bool(os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")),
            "--gui requires a working DISPLAY/WAYLAND_DISPLAY; use a real display or run under Xvfb.",
        )
    root = Path(tempfile.mkdtemp(prefix="godot-mcp-editor-lifecycle-"))
    hosts: list[HostProcess] = []
    success = False
    print(f"Temporary fixture root: {root}", flush=True)
    try:
        with isolated_editor_environment(root / "environment"):
            runner = ProcessRunner(executable, root, timeout)
            project_a, project_b, project_c = (
                root / name for name in ("host-a", "ordinary-editor", "concurrent-start")
            )
            for path in (project_a, project_b, project_c):
                new_lifecycle_project(path, f"MCP Lifecycle {path.name}")

            print("[1/4] Reusing one Host across discovery, allow-start, tool calls and bridge EOF", flush=True)
            host = HostProcess(runner.host_binary, project_a, "persistent-host", root, headless=not gui)
            hosts.append(host)
            check_host_reuse(runner, host, timeout)
            host.stop(require_graceful=True)

            print("[3/4] Preserving an ordinary editor with MCP_DISABLED", flush=True)
            editor = HostProcess(
                runner.host_binary, project_b, "ordinary-editor", root, enable_mcp=False, headless=not gui
            )
            hosts.append(editor)
            check_ordinary_editor(runner, editor, timeout)
            editor.stop(require_graceful=True)

            if gui:
                print("[4/4] Concurrent allow-start creates one independent GUI Host", flush=True)
                check_concurrent_start(runner, project_c, timeout)
            else:
                print(
                    "[4/4] SKIP: real-window and concurrent detached startup coverage requires --gui and a display. "
                    "The production launcher does not start a headless editor.",
                    flush=True,
                )
            success = True
            print(f"MCP editor lifecycle regression test passed ({'GUI' if gui else 'headless'}).", flush=True)
    finally:
        for host in reversed(hosts):
            try:
                host.stop()
            except Exception as error:
                success = False
                print(f"warning: fixture cleanup failed for {host.label}: {error}", file=sys.stderr)
        if keep_temporary_projects or not success:
            print(f"Temporary projects and logs kept at: {root}", flush=True)
        else:
            remove_tree_with_retry(root)
    require(success, "editor lifecycle fixture cleanup did not complete.")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, help="Godot editor binary built with MCP support.")
    parser.add_argument("--timeout-seconds", type=int, default=45, help="Per-operation timeout (5-300 seconds).")
    parser.add_argument(
        "--gui", action="store_true", help="Use real editor windows and test concurrent detached startup."
    )
    parser.add_argument(
        "--keep-temporary-projects", action="store_true", help="Keep fixtures after a successful run too."
    )
    args = parser.parse_args(argv)
    if not 5 <= args.timeout_seconds <= 300:
        parser.error("--timeout-seconds must be between 5 and 300")
    try:
        run_lifecycle(args.binary, args.timeout_seconds, args.gui, args.keep_temporary_projects)
    except (SmokeFailure, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("MCP editor lifecycle test interrupted.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
