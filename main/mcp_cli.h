/**************************************************************************/
/*  mcp_cli.h                                                             */
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

#pragma once

#include "core/mcp/mcp_discovery.h"
#include "core/mcp/mcp_project_lease.h"
#include "core/variant/dictionary.h"
#include "main/mcp_cli_command_registry.h"
#include "main/mcp_cli_editor_launcher.h"
#include "main/mcp_cli_http_transport.h"
#include "main/mcp_cli_stdio_io.h"

class MCPCLI final {
public:
	enum ExitCode {
		EXIT_OK = 0,
		EXIT_INVALID_ARGUMENTS = 2,
		EXIT_DISCOVERY_FAILED = 3,
		EXIT_TRANSPORT_FAILED = 4,
		EXIT_PROTOCOL_FAILED = 5,
	};

	struct Options {
		String discovery_directory;
		uint64_t max_line_bytes = 1024 * 1024;
		uint64_t max_response_bytes = 4 * 1024 * 1024;
		uint64_t request_timeout_ms = 10000;
		uint64_t startup_wait_ms = 10000;
		bool allow_start = false;
	};

	static constexpr const char *COMMAND_DISCOVER = MCP_CLI_COMMAND_DISCOVER;
	static constexpr const char *COMMAND_STDIO = MCP_CLI_COMMAND_STDIO;

private:
	Options options;
	MCPHTTPCLITransport default_transport;
	MCPOSCLIIO default_io;
	MCPOSCLIEditorLauncher default_launcher;
	MCPOSProjectLeaseProcessProbe default_process_probe;
	MCPCLITransport *transport = nullptr;
	MCPCLIIO *io = nullptr;
	MCPCLIEditorLauncher *launcher = nullptr;
	const MCPProjectLeaseProcessProbe *process_probe = nullptr;

	Error _get_project_path_argument(const PackedStringArray &p_arguments, String &r_project_path, String &r_error);
	Error _resolve_ready_host(const String &p_project_path, MCPProjectIdentity &r_identity, MCPDiscoveryRecord &r_record, String &r_error);
	Error _validate_discovery_record(
			const MCPProjectIdentity &p_identity,
			const MCPDiscoveryRecord &p_record,
			String &r_error) const;
	void _diagnose(const String &p_message);

public:
	MCPCLI();
	MCPCLI(const Options &p_options, MCPCLITransport *p_transport = nullptr, MCPCLIIO *p_io = nullptr, MCPCLIEditorLauncher *p_launcher = nullptr, const MCPProjectLeaseProcessProbe *p_process_probe = nullptr);
	MCPCLI(const MCPCLI &) = delete;
	MCPCLI &operator=(const MCPCLI &) = delete;
	MCPCLI(MCPCLI &&) = delete;
	MCPCLI &operator=(MCPCLI &&) = delete;

	static String get_default_discovery_directory();
	static Dictionary make_public_discovery_output(const MCPDiscoveryRecord &p_record);

	Error resolve_discovery_record(
			const String &p_project_path,
			MCPProjectIdentity &r_identity,
			MCPDiscoveryRecord &r_record,
			String *r_error = nullptr) const;
	int run_discover(const String &p_project_path);
	int run_stdio_bridge(const String &p_project_path);
	int execute_cli_command(const StringName &p_command, const PackedStringArray &p_arguments);
};
