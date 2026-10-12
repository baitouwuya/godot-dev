/**************************************************************************/
/*  mcp_cli.cpp                                                           */
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

#include "mcp_cli.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/mcp/mcp_project_instance.h"
#include "core/mcp/mcp_protocol.h"
#include "core/os/os.h"
#include "main/mcp_cli_stdio_bridge.h"

#ifdef TOOLS_ENABLED
#include "editor/file_system/editor_paths.h"
#endif

namespace {

void set_error(String *r_error, const String &p_error) {
	if (r_error) {
		*r_error = p_error;
	}
}

} // namespace

MCPCLI::MCPCLI() :
		transport(&default_transport), io(&default_io), launcher(&default_launcher), process_probe(&default_process_probe) {
}

MCPCLI::MCPCLI(const Options &p_options, MCPCLITransport *p_transport, MCPCLIIO *p_io, MCPCLIEditorLauncher *p_launcher, const MCPProjectLeaseProcessProbe *p_process_probe) :
		options(p_options), transport(p_transport ? p_transport : &default_transport), io(p_io ? p_io : &default_io), launcher(p_launcher ? p_launcher : &default_launcher), process_probe(p_process_probe ? p_process_probe : &default_process_probe) {
}

String MCPCLI::get_default_discovery_directory() {
#ifdef TOOLS_ENABLED
	EditorPaths *editor_paths = EditorPaths::get_singleton();
	if (editor_paths && editor_paths->are_paths_valid()) {
		return editor_paths->get_data_dir().path_join("mcp").path_join("discovery");
	}
#endif
	return String();
}

Dictionary MCPCLI::make_public_discovery_output(const MCPDiscoveryRecord &p_record) {
	Dictionary output;
	output["projectId"] = p_record.project_id;
	output["instanceId"] = p_record.instance_id;
	output["pid"] = p_record.pid;
	output["endpoint"] = p_record.endpoint;
	output["protocolVersion"] = p_record.protocol_version;
	return output;
}

Error MCPCLI::_get_project_path_argument(
		const PackedStringArray &p_arguments,
		String &r_project_path,
		String &r_error) {
	r_project_path = String();
	r_error = String();
	options.allow_start = false; // Launch permission never carries across CLI invocations.
	bool mode_seen = false;
	bool wait_seen = false;
	for (int i = 0; i < p_arguments.size(); i++) {
		const String &argument = p_arguments[i];
		if (argument == "--connect-only" || argument == "--allow-start") {
			if (mode_seen) {
				r_error = "Choose exactly one of --connect-only and --allow-start.";
				return ERR_INVALID_PARAMETER;
			}
			mode_seen = true;
			options.allow_start = argument == "--allow-start";
		} else if (argument == "--wait-ms") {
			if (wait_seen || ++i >= p_arguments.size() || !p_arguments[i].is_valid_int() ||
					p_arguments[i].to_int() < 0 || p_arguments[i].to_int() > 120000) {
				r_error = "--wait-ms requires one integer from 0 to 120000.";
				return ERR_INVALID_PARAMETER;
			}
			wait_seen = true;
			options.startup_wait_ms = p_arguments[i].to_int();
		} else if (argument == "--path") {
			if (!r_project_path.is_empty() || ++i >= p_arguments.size() || p_arguments[i].is_empty()) {
				r_error = "MCP CLI commands require one explicit project path.";
				return ERR_INVALID_PARAMETER;
			}
			r_project_path = p_arguments[i];
		} else if (argument == "--restart") {
			r_error = "RESTART_REQUIRES_EDITOR: Restart through the existing editor after reviewing unsaved work. The bridge never closes an editor.";
			return ERR_INVALID_PARAMETER;
		} else if (!argument.is_empty() && !argument.begins_with("--") && r_project_path.is_empty()) {
			r_project_path = argument;
		} else {
			r_error = "MCP CLI commands require one explicit project path and only --connect-only, --allow-start, or --wait-ms options.";
			return ERR_INVALID_PARAMETER;
		}
	}
	if (r_project_path.is_empty() || (options.allow_start && options.startup_wait_ms == 0)) {
		r_error = "MCP CLI commands require one explicit project path; --allow-start also requires a positive --wait-ms.";
		return ERR_INVALID_PARAMETER;
	}
	return OK;
}

Error MCPCLI::_validate_discovery_record(
		const MCPProjectIdentity &p_identity,
		const MCPDiscoveryRecord &p_record,
		String &r_error) const {
	if (!p_record.is_valid(true) || p_record.project_id != p_identity.project_id ||
			p_record.project_path != p_identity.canonical_path) {
		r_error = "MCP discovery record does not match the requested project.";
		return ERR_INVALID_DATA;
	}
	if (p_record.transport != "streamable-http") {
		r_error = "MCP discovery record does not describe a Streamable HTTP Host.";
		return ERR_INVALID_DATA;
	}
	if (!MCPProtocol::is_supported_protocol_version(p_record.protocol_version)) {
		r_error = "MCP discovery record contains an unsupported protocol version.";
		return ERR_INVALID_DATA;
	}
	if (!MCPHTTPCLITransport::is_visible_ascii_header_value(p_record.auth_secret)) {
		r_error = "MCP discovery record does not contain a valid authentication secret.";
		return ERR_INVALID_DATA;
	}
	if (p_record.protocol_version.contains("\r") || p_record.protocol_version.contains("\n") ||
			p_record.auth_secret.contains("\r") || p_record.auth_secret.contains("\n")) {
		r_error = "MCP discovery record contains an invalid header value.";
		return ERR_INVALID_DATA;
	}
	String scheme;
	String host;
	String path;
	String fragment;
	int port = 0;
	Error error = MCPHTTPCLITransport::parse_endpoint(p_record.endpoint, scheme, host, port, path, fragment, r_error);
	if (error != OK) {
		return error;
	}
	if ((host != "127.0.0.1" && host != "localhost" && host != "::1") || path != "/mcp" || !fragment.is_empty()) {
		r_error = "MCP discovery endpoint must be a loopback /mcp URL without a fragment or query.";
		return ERR_INVALID_DATA;
	}
	const int authority_end = p_record.endpoint.find_char('/', p_record.endpoint.find("://") + 3);
	const String authority = authority_end < 0 ? p_record.endpoint : p_record.endpoint.substr(0, authority_end);
	if (authority.contains("@")) {
		r_error = "MCP discovery endpoint must not contain URL credentials.";
		return ERR_INVALID_DATA;
	}
	return OK;
}

Error MCPCLI::resolve_discovery_record(
		const String &p_project_path,
		MCPProjectIdentity &r_identity,
		MCPDiscoveryRecord &r_record,
		String *r_error) const {
	r_identity = MCPProjectIdentity();
	r_record = MCPDiscoveryRecord();
	set_error(r_error, String());

	String error_message;
	Error error = MCPProjectIdentity::create(p_project_path, r_identity, &error_message);
	if (error != OK) {
		set_error(r_error, error_message);
		return error;
	}
	const String discovery_directory = options.discovery_directory.is_empty() ? get_default_discovery_directory() : options.discovery_directory;
	if (discovery_directory.is_empty()) {
		set_error(r_error, "MCP discovery directory is unavailable.");
		return ERR_UNCONFIGURED;
	}
	error = MCPDiscovery::read_record(discovery_directory, r_identity.project_id, r_record, &error_message);
	if (error != OK) {
		set_error(
				r_error,
				error_message.is_empty() ? "No MCP discovery record exists for the requested project." : error_message);
		return error;
	}
	error = _validate_discovery_record(r_identity, r_record, error_message);
	if (error != OK) {
		set_error(r_error, error_message);
	}
	return error;
}

Error MCPCLI::_resolve_ready_host(const String &p_project_path, MCPProjectIdentity &r_identity, MCPDiscoveryRecord &r_record, String &r_error) {
	Error error = MCPProjectIdentity::create(p_project_path, r_identity, &r_error);
	if (error != OK) {
		return error;
	}
	class NoHealth : public MCPProjectLeaseHealthProbe {
	public:
		bool is_healthy(const MCPDiscoveryRecord &) const override { return false; }
	} no_health;
	MCPProjectLease launch_guard(r_identity, MCPProjectInstance::get_launch_directory(r_identity));
	bool launched = false;
	bool observed_starting = false;
	ProcessID launched_pid = 0;
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + options.startup_wait_ms * 1000;
	while (true) {
		MCPProjectIdentity lookup_identity;
		error = resolve_discovery_record(p_project_path, lookup_identity, r_record, &r_error);
		if (error != OK && error != ERR_FILE_NOT_FOUND && error != ERR_DOES_NOT_EXIST) {
			r_error = "DISCOVERY_INVALID: " + r_error;
			return error;
		}
		if (error == OK) {
			const uint64_t now = OS::get_singleton()->get_ticks_usec();
			const uint64_t remaining_ms = options.startup_wait_ms == 0 ? options.request_timeout_ms : (now < deadline ? MAX((deadline - now) / 1000, (uint64_t)1) : 1);
			const Error health_error = transport->check_host(r_record, MIN(options.request_timeout_ms, remaining_ms), r_error);
			if (health_error == OK) {
				return OK;
			}
			if (health_error == ERR_INVALID_DATA) {
				return health_error;
			}
			// Never turn a failed live/stale endpoint into permission to launch.
			// Only a verified absent owner below can permit one explicit start.
			error = health_error;
		}
		Vector<MCPDiscoveryRecord> running;
		String presence_error;
		const Error presence_result = MCPProjectInstance::list_running(r_identity, running, &presence_error, process_probe);
		if (presence_result != OK) {
			r_error = "OWNERSHIP_UNCERTAIN: " + presence_error;
			return presence_result;
		}
		for (const MCPDiscoveryRecord &instance : running) {
			if (instance.transport == "editor") {
				r_error = vformat("MCP_DISABLED: Editor PID %d already has this project open without MCP. Keep it open; enable MCP only through a user-approved restart.", instance.pid);
				return ERR_UNCONFIGURED;
			}
		}
		MCPProjectLease owner_lookup(r_identity);
		MCPDiscoveryRecord owner;
		const Error owner_result = owner_lookup.read_owner(owner);
		const bool live_owner = owner_result == OK && process_probe->is_process_running(owner.pid);
		const bool live_record = r_record.is_valid(false) && process_probe->is_process_running(r_record.pid);
		if (!running.is_empty() || live_owner || live_record) {
			observed_starting = true;
			r_error = "HOST_NOT_READY: The existing project editor is starting, busy, or published in another discovery directory. No editor was relaunched.";
		} else if (!launched && options.allow_start && !observed_starting) {
			// Missing/corrupt owner metadata is not proof of absence. The early
			// registration in the child is the final atomic ownership arbiter.
			if (owner_result != OK && FileAccess::exists(owner_lookup.get_owner_file())) {
				r_error = "OWNERSHIP_UNCERTAIN: The existing project lease metadata is unreadable.";
				return ERR_INVALID_DATA;
			}
			if (!FileAccess::exists(r_identity.canonical_path.path_join("project.godot"))) {
				r_error = "INVALID_PROJECT_ROOT: --path must name the directory containing project.godot.";
				return ERR_DOES_NOT_EXIST;
			}
			if (!launch_guard.is_acquired()) {
				MCPDiscoveryRecord guard_owner = MCPDiscoveryRecord::create(r_identity, "editor-launch", String(), MCPProtocol::PROTOCOL_VERSION_LATEST);
				MCPDiscoveryRecord contender;
				const Error guard_result = launch_guard.acquire(guard_owner, no_health, &contender, &r_error, 0, process_probe);
				if (guard_result == OK) {
					// Re-read after taking the launch lock. Another caller may have
					// finished while this invocation was looking at old metadata.
					continue;
				}
				if (guard_result != ERR_ALREADY_IN_USE) {
					return guard_result;
				}
				observed_starting = true;
				r_error = "HOST_STARTING: Another client is already starting this project. Waiting without launching another editor.";
			} else {
				const Error launch_error = launcher->launch(r_identity.canonical_path, launched_pid, r_error);
				if (launch_error != OK) {
					return launch_error;
				}
				launched = true;
				_diagnose(vformat("ROLE=bridge STARTED_EDITOR pid=%d; editor lifetime is independent of this connection.", launched_pid));
				r_error = "HOST_STARTING: Waiting for the explicitly started editor. It will not be closed or relaunched on timeout.";
			}
		} else if (!launched && !observed_starting) {
			r_error = "HOST_NOT_RUNNING: No reachable MCP editor was found. Connect-only mode never starts an editor; use --allow-start only when starting one is intended.";
			if (options.startup_wait_ms == 0) {
				return ERR_DOES_NOT_EXIST;
			}
			// An explicitly started editor may not have reached its early
			// registration yet. Wait without claiming that no editor exists.
		} else {
			r_error = "HOST_STARTING: The selected startup has not published a reachable Host. No additional editor will be launched.";
		}
		if (launched && !launcher->is_running(launched_pid)) {
			r_error = vformat("LAUNCH_EXITED: Editor PID %d exited before becoming ready. Start it explicitly to inspect startup errors; no automatic relaunch was attempted.", launched_pid);
			return ERR_CANT_CREATE;
		}
		if (OS::get_singleton()->get_ticks_usec() >= deadline) {
			return ERR_TIMEOUT;
		}
		OS::get_singleton()->delay_usec(25000);
	}
}

void MCPCLI::_diagnose(const String &p_message) {
	io->write_stderr_line("MCP CLI: " + p_message);
}

int MCPCLI::run_discover(const String &p_project_path) {
	MCPProjectIdentity identity;
	MCPDiscoveryRecord record;
	String error;
	if (_resolve_ready_host(p_project_path, identity, record, error) != OK) {
		_diagnose(error);
		return EXIT_DISCOVERY_FAILED;
	}
	io->write_stdout_line(JSON::stringify(make_public_discovery_output(record)));
	return EXIT_OK;
}

int MCPCLI::run_stdio_bridge(const String &p_project_path) {
	MCPProjectIdentity identity;
	MCPDiscoveryRecord record;
	String error;
	if (_resolve_ready_host(p_project_path, identity, record, error) != OK) {
		_diagnose(error);
		return EXIT_DISCOVERY_FAILED;
	}

	MCPCLIStdioBridge::Limits limits;
	limits.max_line_bytes = options.max_line_bytes;
	limits.max_response_bytes = options.max_response_bytes;
	limits.request_timeout_ms = options.request_timeout_ms;
	MCPCLIStdioBridge bridge(transport, io, record, limits);
	switch (bridge.run()) {
		case MCPCLIStdioBridge::RESULT_OK:
			return EXIT_OK;
		case MCPCLIStdioBridge::RESULT_TRANSPORT_FAILED:
			return EXIT_TRANSPORT_FAILED;
		case MCPCLIStdioBridge::RESULT_PROTOCOL_FAILED:
			return EXIT_PROTOCOL_FAILED;
	}
	return EXIT_PROTOCOL_FAILED;
}

int MCPCLI::execute_cli_command(const StringName &p_command, const PackedStringArray &p_arguments) {
	String project_path;
	String error;
	if (_get_project_path_argument(p_arguments, project_path, error) != OK) {
		_diagnose(error);
		return EXIT_INVALID_ARGUMENTS;
	}
	if (p_command == COMMAND_DISCOVER) {
		return run_discover(project_path);
	}
	if (p_command == COMMAND_STDIO) {
		return run_stdio_bridge(project_path);
	}
	_diagnose(vformat("unknown command '%s'.", p_command));
	return EXIT_INVALID_ARGUMENTS;
}
