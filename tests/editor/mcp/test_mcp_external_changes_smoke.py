#!/usr/bin/env python3
"""Exercise native change dialogs and MCP save guards in an isolated editor.

Use --gui under a display server (for example xvfb-run) to exercise real windows.
The default headless mode still creates the actual editor/dialog objects.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from mcp_smoke_test_utils import (
    ProcessRunner,
    SmokeFailure,
    invoke_stdio,
    parse_discovery,
    require,
    tool_call,
    wait_until,
)
from test_mcp_cli_smoke import structured_result, tool_error_code, write_text

PLUGIN = """@tool
extends EditorPlugin

var last_id := 0
var busy := false

func _enter_tree() -> void:
	set_process(true)

func _dialogs(node: Node, result: Array) -> void:
	if node is ConfirmationDialog and node.title == "Files have been modified outside Godot" and node.visible:
		var rows: Array = []
		for tree in node.find_children("*", "Tree", true, false):
			var root = tree.get_root()
			var row = root.get_first_child() if root else null
			while row:
				rows.append(row.get_text(0))
				row = row.get_next()
		result.append({"node": node, "rows": rows, "scope": "script" if node.get_parent() == EditorInterface.get_script_editor() else "scene"})
	for child in node.get_children():
		_dialogs(child, result)

func _process(_delta: float) -> void:
	if FileAccess.file_exists("res://.stop"):
		get_tree().quit()
		return
	if busy or not FileAccess.file_exists("res://.command.json"):
		return
	var command = JSON.parse_string(FileAccess.get_file_as_string("res://.command.json"))
	if not command is Dictionary or int(command.get("id", 0)) <= last_id:
		return
	last_id = int(command.id)
	busy = true
	_execute(command)

func _execute(command: Dictionary) -> void:
	var error := ""
	match command.action:
		"scan":
			EditorInterface.get_script_editor().reload_open_files()
		"focus":
			get_tree().root.propagate_notification(MainLoop.NOTIFICATION_APPLICATION_FOCUS_IN)
		"focus_then_save":
			get_tree().root.propagate_notification(MainLoop.NOTIFICATION_APPLICATION_FOCUS_IN)
			ProjectSettings.save()
			get_tree().root.propagate_notification(MainLoop.NOTIFICATION_APPLICATION_FOCUS_IN)
		"open_scene":
			EditorInterface.open_scene_from_path(command.path)
		"confirm", "resave", "cancel":
			var dialogs: Array = []
			_dialogs(get_tree().root, dialogs)
			var selected = null
			for dialog in dialogs:
				if dialog.scope == command.scope:
					selected = dialog.node
			if selected == null:
				error = "No matching visible dialog"
			elif command.action == "confirm":
				selected.get_ok_button().emit_signal("pressed")
			elif command.action == "resave":
				selected.emit_signal("custom_action", "resave")
			else:
				selected.get_cancel_button().emit_signal("pressed")
	await get_tree().process_frame
	await get_tree().process_frame
	var dialogs: Array = []
	_dialogs(get_tree().root, dialogs)
	for dialog in dialogs:
		dialog.erase("node")
	var response = FileAccess.open("res://.response.json", FileAccess.WRITE)
	response.store_string(JSON.stringify({"id": command.id, "dialogs": dialogs, "error": error}))
	response.close()
	busy = false
"""


def run(binary: str, timeout: int, gui: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="godot-external-changes-") as temp:
        root = Path(temp)
        project = root / "project"
        # Keep editor caches outside the user's profile; behavior overrides live in the project.
        for key, directory in (
            ("HOME", "home"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_DATA_HOME", "data"),
            ("XDG_CACHE_HOME", "cache"),
            ("APPDATA", "config"),
            ("LOCALAPPDATA", "data"),
        ):
            (root / directory).mkdir(exist_ok=True)
            os.environ[key] = str(root / directory)
        os.environ["LANG"] = "en_US.UTF-8"
        write_text(
            project / "project.godot",
            """config_version=5
[application]
config/name="External Change Regression"
[rendering]
renderer/rendering_method="gl_compatibility"
[editor_overrides]
text_editor/behavior/files/auto_reload_scripts_on_external_change=false
interface/editor/behavior/save_on_focus_loss=false
interface/editor/behavior/import_resources_when_unfocused=false
[editor_plugins]
enabled=PackedStringArray("res://addons/probe/plugin.cfg")
""",
        )
        write_text(
            project / "addons/probe/plugin.cfg",
            """[plugin]
name="External Change Probe"
description="Temporary native-dialog regression driver"
author="Godot"
version="1.0"
script="plugin.gd"
""",
        )
        write_text(project / "addons/probe/plugin.gd", PLUGIN)
        initial = "extends Node\nvar value = 1\n"
        for name in ("a.gd", "b.gd", "c.gd", "closed.gd"):
            write_text(project / name, initial)
        first_scene = '[gd_scene format=3]\n\n[node name="First" type="Node"]\n'
        second_scene = '[gd_scene format=3]\n\n[node name="Second" type="Node"]\n'
        write_text(project / "first.tscn", first_scene)
        write_text(project / "second.tscn", second_scene)
        runner = ProcessRunner(ProcessRunner.resolve_binary(binary), root, timeout)
        args = [str(runner.host_binary), "--editor", "--path", str(project), "--mcp", "--mcp-port", "0"]
        if not gui:
            args.append("--headless")
        stdout = (root / "stdout.log").open("wb")
        stderr = (root / "stderr.log").open("wb")
        process = subprocess.Popen(args, cwd=root, stdout=stdout, stderr=stderr)
        command_id = 0
        try:

            def alive() -> None:
                require(process.poll() is None, "editor exited: " + (root / "stderr.log").read_text(errors="replace"))

            def discovery_attempt():
                alive()
                result = runner.invoke(["--mcp-discover", "--path", str(project)])
                return parse_discovery(result, "external changes") if result.returncode == 0 else None

            discovery = wait_until(timeout, discovery_attempt, "MCP discovery")

            def call(name: str, arguments: dict, error: str | None = None) -> dict:
                alive()
                result = invoke_stdio(
                    runner, project, discovery["protocolVersion"], [tool_call(2, name, arguments)], name
                )
                if error:
                    require(tool_error_code(result, 2, name) == error, f"{name} did not return {error}")
                    return {}
                return structured_result(result, 2, name)

            def command(action: str, **kwargs) -> list:
                nonlocal command_id
                command_id += 1
                data = {"id": command_id, "action": action, **kwargs}
                pending = project / ".command.pending"
                pending.write_text(json.dumps(data), encoding="utf-8")
                pending.replace(project / ".command.json")

                def response_attempt():
                    alive()
                    try:
                        response = json.loads((project / ".response.json").read_text())
                    except (FileNotFoundError, json.JSONDecodeError):
                        return None
                    return response if response.get("id") == command_id else None

                response = wait_until(timeout, response_attempt, action)
                require(not response["error"], response["error"])
                return response["dialogs"]

            def rows(dialogs: list, scope: str) -> set[str]:
                matching = [dialog for dialog in dialogs if dialog["scope"] == scope]
                require(len(matching) <= 1, f"duplicate {scope} dialogs: {matching}")
                return set(matching[0]["rows"]) if matching else set()

            def state(name: str) -> dict:
                return call("godot.script.open", {"path": "res://" + name})

            def edit(name: str, value: int) -> str:
                text = f"extends Node\nvar value = {value}\n"
                call(
                    "godot.script.edit",
                    {"path": "res://" + name, "text": text, "expected_sha256": state(name)["sha256"]},
                )
                return text

            def external_write(name: str, text: str) -> None:
                target = project / name
                timestamp = int(target.stat().st_mtime) + 2
                write_text(target, text)
                os.utime(target, (timestamp, timestamp))

            def assert_buffer(name: str, text: str, dirty: bool) -> None:
                snapshot = state(name)
                require(
                    snapshot["sha256"] == hashlib.sha256(text.encode()).hexdigest(),
                    f"{name} buffer was changed unexpectedly",
                )
                require(snapshot["unsaved"] == dirty, f"{name} dirty state was changed unexpectedly")

            print("Checking native saves and managed-write guards", flush=True)
            command("open_scene", path="res://first.tscn")
            state("a.gd")
            state("b.gd")
            edit("a.gd", 2)
            call("godot.script.save", {"path": "res://a.gd"})
            call("godot.scene.save", {})
            call("godot.project.set_setting", {"name": "application/config/description", "value": "native save"})
            call("godot.project.save", {})
            require(not command("focus"), "native saves generated an external-change dialog")
            for path in ("a.gd", "first.tscn"):
                call("godot.resource.save", {"path": "res://" + path}, "RESOURCE_SAVE_MANAGED")
                call(
                    "godot.file.create",
                    {"path": "res://" + path, "content": "must not write", "overwrite": True},
                    "FILE_MANAGED",
                )
            original_stat = (project / "closed.gd").stat().st_mtime_ns
            result = call("godot.file.create", {"path": "res://closed.gd", "content": initial, "overwrite": True})
            require(result["bytesWritten"] == 0, "identical content was rewritten")
            require(
                (project / "closed.gd").stat().st_mtime_ns == original_stat, "identical content changed the timestamp"
            )

            print("Checking filtered scans and exact resave scope", flush=True)
            dirty_a = edit("a.gd", 10)
            dirty_b = edit("b.gd", 20)
            external_write("a.gd", "extends Node\nvar value = 11\n")
            require(rows(command("scan"), "script") == {"res://a.gd"}, "missing script conflict")
            state("c.gd")
            require(
                rows(command("snapshot"), "script") == {"res://a.gd"}, "filtered open erased another tab's conflict"
            )
            command("resave", scope="script")
            require((project / "a.gd").read_text() == dirty_a, "resave did not save the displayed script")
            require((project / "b.gd").read_text() == initial, "resave wrote an undisplayed dirty script")
            assert_buffer("b.gd", dirty_b, True)

            print("Checking stale confirmations preserve dirty buffers", flush=True)
            dirty_a = edit("a.gd", 30)
            disk_a = "extends Node\nvar value = 31\n"
            disk_b = "extends Node\nvar value = 21\n"
            external_write("a.gd", disk_a)
            require(rows(command("scan"), "script") == {"res://a.gd"}, "wrong initial approval set")
            external_write("b.gd", disk_b)
            require(
                rows(command("confirm", scope="script"), "script") == {"res://a.gd", "res://b.gd"},
                "stale confirmation did not require a fresh approval",
            )
            assert_buffer("a.gd", dirty_a, True)
            assert_buffer("b.gd", dirty_b, True)
            require(not rows(command("confirm", scope="script"), "script"), "confirmed reload left a stale dialog")
            assert_buffer("a.gd", disk_a, False)
            assert_buffer("b.gd", disk_b, False)

            print("Checking deferred popup cancellation and scene action scope", flush=True)
            external_write("project.godot", (project / "project.godot").read_text() + "\n; external timestamp\n")
            require(not command("focus_then_save"), "resolved project change left an empty deferred popup")
            command("open_scene", path="res://second.tscn")
            external_write("first.tscn", first_scene.replace("First", "External"))
            require(rows(command("focus"), "scene") == {"res://first.tscn"}, "wrong scene approval set")
            command("resave", scope="scene")
            require(
                'name="First"' in (project / "first.tscn").read_text(),
                "resave used the active scene instead of the displayed scene",
            )
            require('name="Second"' in (project / "second.tscn").read_text(), "resave changed the undisplayed scene")
            call("godot.project.set_setting", {"name": "application/config/description", "value": "unsaved setting"})
            external_write("first.tscn", first_scene.replace("First", "Reloaded"))
            require(rows(command("focus"), "scene") == {"res://first.tscn"}, "unexpected project conflict")
            command("confirm", scope="scene")
            setting = call("godot.project.get_setting", {"name": "application/config/description"})
            require(
                setting["value"] == "unsaved setting", "scene-only confirmation reloaded unrelated project settings"
            )
            print(
                "External-change integration smoke passed" + (" with GUI windows" if gui else " headlessly"), flush=True
            )
        except Exception:
            print((root / "stdout.log").read_text(errors="replace"), file=sys.stderr)
            print((root / "stderr.log").read_text(errors="replace"), file=sys.stderr)
            raise
        finally:
            (project / ".stop").write_text("stop")
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            stdout.close()
            stderr.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--timeout-seconds", type=int, default=45)
    parser.add_argument("--gui", action="store_true")
    args = parser.parse_args()
    try:
        run(args.binary, args.timeout_seconds, args.gui)
    except (SmokeFailure, OSError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
