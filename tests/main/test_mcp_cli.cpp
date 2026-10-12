/**************************************************************************/
/*  test_mcp_cli.cpp                                                      */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mcp_cli);

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/mcp/mcp_project_instance.h"
#include "core/mcp/mcp_protocol.h"
#include "main/mcp_cli.h"
#include "main/mcp_cli_runtime.h"
#include "main/mcp_cli_stdio_bridge.h"

namespace TestMCPCLI {

PackedByteArray bytes_from_string(const String &p_text) {
	const CharString utf8 = p_text.utf8();
	PackedByteArray bytes;
	bytes.resize(utf8.length());
	if (utf8.length() > 0) {
		memcpy(bytes.ptrw(), utf8.get_data(), utf8.length());
	}
	return bytes;
}

String find_request_header(const Vector<String> &p_headers, const String &p_name) {
	for (const String &header : p_headers) {
		const int separator = header.find_char(':');
		if (separator > 0 && header.substr(0, separator).strip_edges().to_lower() == p_name.to_lower()) {
			return header.substr(separator + 1).strip_edges();
		}
	}
	return String();
}

class MemoryCLIIO final : public MCPCLIIO {
public:
	ReadStatus terminal_read_status = READ_EOF;
	Vector<String> input_lines;
	Vector<String> stdout_lines;
	Vector<String> stderr_lines;
	int next_input = 0;

	ReadStatus read_line(String &r_line, uint64_t p_max_line_bytes) override {
		if (next_input >= input_lines.size()) {
			return terminal_read_status;
		}
		r_line = input_lines[next_input++];
		return (uint64_t)r_line.utf8().length() > p_max_line_bytes ? READ_TOO_LARGE : READ_LINE;
	}

	void write_stdout_line(const String &p_line) override {
		stdout_lines.push_back(p_line);
	}

	void write_stderr_line(const String &p_line) override {
		stderr_lines.push_back(p_line);
	}
};

class QueueCLITransport final : public MCPCLITransport {
public:
	struct Request {
		MCPCLITransport::RequestMethod method = MCPCLITransport::REQUEST_POST;
		String endpoint;
		Vector<String> headers;
		String body;
		uint64_t timeout_ms = 0;
		uint64_t max_response_bytes = 0;
	};

	Vector<Request> requests;
	Vector<MCPCLIHTTPResponse> responses;
	int next_response = 0;
	int fail_request_index = -1;
	Error request_failure = ERR_UNAVAILABLE;
	Error host_check_result = OK;
	int host_check_count = 0;

	Error check_host(const MCPDiscoveryRecord &, uint64_t, String &r_error) override {
		host_check_count++;
		r_error = host_check_result == OK ? String() : String("test host is unavailable");
		return host_check_result;
	}

	Error request(
			MCPCLITransport::RequestMethod p_method,
			const String &p_endpoint,
			const Vector<String> &p_headers,
			const String &p_body,
			uint64_t p_timeout_ms,
			uint64_t p_max_response_bytes,
			MCPCLIHTTPResponse &r_response,
			String &r_error) override {
		Request request;
		request.method = p_method;
		request.endpoint = p_endpoint;
		request.headers = p_headers;
		request.body = p_body;
		request.timeout_ms = p_timeout_ms;
		request.max_response_bytes = p_max_response_bytes;
		requests.push_back(request);
		if (requests.size() - 1 == fail_request_index) {
			r_error = "Simulated request failure.";
			return request_failure;
		}
		if (next_response >= responses.size()) {
			r_error = "No queued response.";
			return ERR_UNAVAILABLE;
		}
		r_response = responses[next_response++];
		return OK;
	}
};

MCPCLIHTTPResponse make_response(int p_status, const String &p_body = String(), const String &p_session_id = String()) {
	MCPCLIHTTPResponse response;
	response.status_code = p_status;
	response.body = bytes_from_string(p_body);
	if (!p_session_id.is_empty()) {
		response.headers["Mcp-Session-Id"] = p_session_id;
	}
	return response;
}

void create_project_and_record(
		const Ref<DirAccess> &p_temporary_directory,
		const String &p_endpoint,
		MCPProjectIdentity &r_identity,
		MCPDiscoveryRecord &r_record,
		String &r_project_path,
		String &r_discovery_directory) {
	REQUIRE(p_temporary_directory->make_dir("project") == OK);
	r_project_path = p_temporary_directory->get_current_dir().path_join("project");
	r_discovery_directory = p_temporary_directory->get_current_dir().path_join("discovery");
	REQUIRE(MCPProjectIdentity::create(r_project_path, r_identity) == OK);
	r_record = MCPDiscoveryRecord::create(r_identity, "streamable-http", p_endpoint, "2025-06-18", "private-token", 1000);
}

TEST_CASE("[MCP][CLI] HTTP transport validates MCP endpoint and header primitives") {
	String scheme;
	String host;
	String path;
	String fragment;
	String error;
	int port = 0;

	CHECK(MCPHTTPCLITransport::parse_endpoint("http://[::1]/mcp", scheme, host, port, path, fragment, error) == OK);
	CHECK(scheme == "http://");
	CHECK(host == "::1");
	CHECK(port == 80);
	CHECK(path == "/mcp");
	CHECK(fragment.is_empty());

	CHECK(MCPHTTPCLITransport::parse_endpoint("file:///tmp/mcp", scheme, host, port, path, fragment, error) == ERR_INVALID_DATA);
	CHECK_FALSE(error.is_empty());
	CHECK(MCPHTTPCLITransport::is_visible_ascii_header_value("session-123"));
	CHECK_FALSE(MCPHTTPCLITransport::is_visible_ascii_header_value(String()));
	CHECK_FALSE(MCPHTTPCLITransport::is_visible_ascii_header_value("unsafe\nheader"));
}

TEST_CASE("[MCP][CLI] Stdio bridge classifies transport failures without a discovery lookup") {
	MemoryCLIIO io;
	io.input_lines.push_back(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{}})");
	QueueCLITransport transport;

	MCPDiscoveryRecord record;
	record.endpoint = "http://127.0.0.1:35001/mcp";
	record.protocol_version = MCPProtocol::PROTOCOL_VERSION_LATEST;
	record.auth_secret = "private-token";
	MCPCLIStdioBridge::Limits limits;
	limits.max_line_bytes = 1024;
	limits.max_response_bytes = 2048;
	limits.request_timeout_ms = 50;

	MCPCLIStdioBridge bridge(&transport, &io, record, limits);
	CHECK(bridge.run() == MCPCLIStdioBridge::RESULT_TRANSPORT_FAILED);
	CHECK(io.stdout_lines.is_empty());
	REQUIRE(io.stderr_lines.size() == 1);
	CHECK(io.stderr_lines[0].contains("No queued response"));
	REQUIRE(transport.requests.size() == 1);
	CHECK(transport.requests[0].timeout_ms == 50);
	CHECK(transport.requests[0].max_response_bytes == 2048);
}

TEST_CASE("[MCP][CLI] Stdio read errors are not treated as successful EOF") {
	MemoryCLIIO io;
	io.terminal_read_status = MCPCLIIO::READ_ERROR;
	QueueCLITransport transport;
	MCPDiscoveryRecord record;
	MCPCLIStdioBridge bridge(&transport, &io, record, MCPCLIStdioBridge::Limits());
	CHECK(bridge.run() == MCPCLIStdioBridge::RESULT_TRANSPORT_FAILED);
	CHECK(transport.requests.is_empty());
	CHECK(io.stdout_lines.is_empty());
	REQUIRE(io.stderr_lines.size() == 1);
	CHECK(io.stderr_lines[0] == "MCP CLI: stdin could not be read.");
}

TEST_CASE("[MCP][CLI] A timed out mutation is never replayed and only its session is closed") {
	MemoryCLIIO io;
	io.input_lines.push_back(
			R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})");
	const String mutation = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"godot.node.create","arguments":{"type":"Node","name":"Once"}}})";
	io.input_lines.push_back(mutation);
	QueueCLITransport transport;
	transport.responses.push_back(make_response(
			200,
			R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18"}})",
			"session-timeout"));
	transport.responses.push_back(make_response(204));
	transport.fail_request_index = 1;
	transport.request_failure = ERR_TIMEOUT;
	MCPDiscoveryRecord record;
	record.endpoint = "http://127.0.0.1:35001/mcp";
	record.protocol_version = MCPProtocol::PROTOCOL_VERSION_LATEST;
	record.auth_secret = "private-token";
	MCPCLIStdioBridge bridge(&transport, &io, record, MCPCLIStdioBridge::Limits());
	CHECK(bridge.run() == MCPCLIStdioBridge::RESULT_TRANSPORT_FAILED);
	REQUIRE(transport.requests.size() == 3);
	CHECK(transport.requests[0].method == MCPCLITransport::REQUEST_POST);
	CHECK(transport.requests[1].method == MCPCLITransport::REQUEST_POST);
	CHECK(transport.requests[1].body == mutation);
	CHECK(transport.requests[2].method == MCPCLITransport::REQUEST_DELETE);
	CHECK(transport.requests[2].body.is_empty());
	CHECK(find_request_header(transport.requests[2].headers, "mcp-session-id") == "session-timeout");
	CHECK(io.stdout_lines.size() == 1);
	REQUIRE(io.stderr_lines.size() == 1);
	CHECK_FALSE(io.stderr_lines[0].contains(record.auth_secret));
}

TEST_CASE("[MCP][CLI] Discover routes through the command registry without exposing secrets") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_cli_discover", false, &error);
	REQUIRE(error == OK);

	MCPProjectIdentity identity;
	MCPDiscoveryRecord record;
	String project_path;
	String discovery_directory;
	create_project_and_record(
			temporary_directory,
			"http://127.0.0.1:35101/mcp",
			identity,
			record,
			project_path,
			discovery_directory);
	REQUIRE(MCPDiscovery::write_record(discovery_directory, record, true) == OK);

	MemoryCLIIO io;
	QueueCLITransport transport;
	MCPCLI::Options options;
	options.discovery_directory = discovery_directory;
	MCPCLI cli(options, &transport, &io);
	MCPCLIRuntime runtime(&cli);
	REQUIRE(runtime.select_command(MCPCLI::COMMAND_DISCOVER) == OK);

	PackedStringArray arguments;
	arguments.push_back(project_path);
	int exit_code = -1;
	REQUIRE(runtime.invoke_selected(arguments, exit_code) == OK);
	CHECK(exit_code == MCPCLI::EXIT_OK);
	REQUIRE(io.stdout_lines.size() == 1);
	CHECK(io.stderr_lines.is_empty());
	CHECK(transport.requests.is_empty());

	const Variant parsed_output = JSON::parse_string(io.stdout_lines[0]);
	REQUIRE(parsed_output.get_type() == Variant::DICTIONARY);
	const Dictionary output = parsed_output;
	CHECK(output.get("projectId", String()) == identity.project_id);
	CHECK(output.get("instanceId", String()) == identity.instance_id);
	CHECK(output.get("endpoint", String()) == record.endpoint);
	CHECK(output.get("protocolVersion", String()) == record.protocol_version);
	CHECK(output.has("pid"));
	CHECK_FALSE(output.has("authSecret"));
	CHECK_FALSE(output.has("projectPath"));
}

TEST_CASE("[MCP][CLI] Unknown command arguments are preserved and rejected") {
	MemoryCLIIO io;
	QueueCLITransport transport;
	MCPCLI::Options options;
	MCPCLI cli(options, &transport, &io);
	MCPCLIRuntime runtime(&cli);
	REQUIRE(runtime.select_command(MCPCLI::COMMAND_DISCOVER) == OK);
	REQUIRE(runtime.append_raw_argument("--unknown-option") == OK);

	PackedStringArray arguments;
	arguments.push_back("C:/project");
	int exit_code = -1;
	REQUIRE(runtime.invoke_selected(arguments, exit_code) == OK);
	CHECK(exit_code == MCPCLI::EXIT_INVALID_ARGUMENTS);
	CHECK(io.stdout_lines.is_empty());
	REQUIRE(io.stderr_lines.size() == 1);
	CHECK(io.stderr_lines[0].contains("require one explicit project path"));
	CHECK(transport.requests.is_empty());
}

TEST_CASE("[MCP][CLI] Stdio bridge keeps stdout JSON-RPC-only and carries the HTTP session") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_cli_stdio", false, &error);
	REQUIRE(error == OK);

	MCPProjectIdentity identity;
	MCPDiscoveryRecord record;
	String project_path;
	String discovery_directory;
	create_project_and_record(
			temporary_directory,
			"http://127.0.0.1:35201/mcp",
			identity,
			record,
			project_path,
			discovery_directory);
	REQUIRE(MCPDiscovery::write_record(discovery_directory, record, true) == OK);

	MemoryCLIIO io;
	io.input_lines.push_back(
			R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26",)"
			R"("capabilities":{},"clientInfo":{"name":"test","version":"1"}}})");
	io.input_lines.push_back(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
	io.input_lines.push_back(R"({ "jsonrpc": "2.0", "id": 9007199254740993, "method": "tools/list", "params": {} })");

	QueueCLITransport transport;
	transport.responses.push_back(make_response(
			200,
			"{\n  \"jsonrpc\": \"2.0\",\n  \"id\": 1,\n  \"result\": {\"protocolVersion\": \"2025-03-26\"}\n}",
			"session-abc"));
	transport.responses.push_back(make_response(202));
	transport.responses.push_back(make_response(
			200,
			"{\n  \"jsonrpc\": \"2.0\",\n  \"id\": 9007199254740993,\n  \"result\": {\"processId\": 29700, \"message\": \"space \\\\t and \\\\n stay\"}\n}"));
	transport.responses.push_back(make_response(204));

	MCPCLI::Options options;
	options.discovery_directory = discovery_directory;
	MCPCLI cli(options, &transport, &io);
	CHECK(cli.run_stdio_bridge(project_path) == MCPCLI::EXIT_OK);
	CHECK(io.stderr_lines.is_empty());
	REQUIRE(transport.requests.size() == 4);
	REQUIRE(io.stdout_lines.size() == 2);

	for (int i = 0; i < 3; i++) {
		CHECK(transport.requests[i].method == MCPCLITransport::REQUEST_POST);
		CHECK(transport.requests[i].endpoint == record.endpoint);
		CHECK(find_request_header(transport.requests[i].headers, "authorization") == "Bearer private-token");
		CHECK(find_request_header(transport.requests[i].headers, "mcp-protocol-version") ==
				(i == 0 ? record.protocol_version : MCPProtocol::PROTOCOL_VERSION_2025_03_26));
		CHECK(find_request_header(transport.requests[i].headers, "content-type") == "application/json");
		CHECK(find_request_header(transport.requests[i].headers, "accept") ==
				"application/json, text/event-stream");
	}
	CHECK(find_request_header(transport.requests[0].headers, "mcp-session-id").is_empty());
	CHECK(find_request_header(transport.requests[1].headers, "mcp-session-id") == "session-abc");
	CHECK(find_request_header(transport.requests[2].headers, "mcp-session-id") == "session-abc");
	CHECK(transport.requests[3].method == MCPCLITransport::REQUEST_DELETE);
	CHECK(find_request_header(transport.requests[3].headers, "authorization") == "Bearer private-token");
	CHECK(find_request_header(transport.requests[3].headers, "mcp-protocol-version") == MCPProtocol::PROTOCOL_VERSION_2025_03_26);
	CHECK(find_request_header(transport.requests[3].headers, "mcp-session-id") == "session-abc");
	CHECK(transport.requests[2].body.contains("9007199254740993"));
	CHECK_FALSE(transport.requests[2].body.contains("9007199254740992"));

	for (const String &stdout_line : io.stdout_lines) {
		CHECK_FALSE(stdout_line.contains("\n"));
		CHECK_FALSE(stdout_line.contains("\r"));
		const Variant response = JSON::parse_string(stdout_line);
		REQUIRE(response.get_type() == Variant::DICTIONARY);
		CHECK(Dictionary(response).get("jsonrpc", String()) == "2.0");
	}
	CHECK(io.stdout_lines[1].contains("\"id\":9007199254740993"));
	CHECK(io.stdout_lines[1].contains("\"processId\":29700"));
	CHECK_FALSE(io.stdout_lines[1].contains("29700.0"));
	CHECK(io.stdout_lines[1].contains("space \\\\t and \\\\n stay"));
}

TEST_CASE("[MCP][CLI] Stdio bridge rejects unsafe HTTP session identifiers") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_cli_session", false, &error);
	REQUIRE(error == OK);

	MCPProjectIdentity identity;
	MCPDiscoveryRecord record;
	String project_path;
	String discovery_directory;
	create_project_and_record(
			temporary_directory,
			"http://127.0.0.1:35301/mcp",
			identity,
			record,
			project_path,
			discovery_directory);
	REQUIRE(MCPDiscovery::write_record(discovery_directory, record, true) == OK);

	MemoryCLIIO io;
	io.input_lines.push_back(
			R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18",)"
			R"("capabilities":{},"clientInfo":{"name":"test","version":"1"}}})");
	QueueCLITransport transport;
	transport.responses.push_back(make_response(
			200,
			R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18"}})",
			"unsafe session"));

	MCPCLI::Options options;
	options.discovery_directory = discovery_directory;
	MCPCLI cli(options, &transport, &io);
	CHECK(cli.run_stdio_bridge(project_path) == MCPCLI::EXIT_PROTOCOL_FAILED);
	CHECK(io.stdout_lines.is_empty());
	CHECK_FALSE(io.stderr_lines.is_empty());
}

TEST_CASE("[MCP][CLI] Stdio bridge cleans up a session after a protocol error") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_cli_cleanup", false, &error);
	REQUIRE(error == OK);

	MCPProjectIdentity identity;
	MCPDiscoveryRecord record;
	String project_path;
	String discovery_directory;
	create_project_and_record(
			temporary_directory,
			"http://127.0.0.1:35302/mcp",
			identity,
			record,
			project_path,
			discovery_directory);
	REQUIRE(MCPDiscovery::write_record(discovery_directory, record, true) == OK);

	MemoryCLIIO io;
	io.input_lines.push_back(
			R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})");
	io.input_lines.push_back("not-json");
	QueueCLITransport transport;
	transport.responses.push_back(make_response(
			200,
			R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18"}})",
			"session-cleanup"));
	transport.responses.push_back(make_response(204));

	MCPCLI::Options options;
	options.discovery_directory = discovery_directory;
	MCPCLI cli(options, &transport, &io);
	CHECK(cli.run_stdio_bridge(project_path) == MCPCLI::EXIT_PROTOCOL_FAILED);
	REQUIRE(transport.requests.size() == 2);
	CHECK(transport.requests[0].method == MCPCLITransport::REQUEST_POST);
	CHECK(transport.requests[1].method == MCPCLITransport::REQUEST_DELETE);
	CHECK(find_request_header(transport.requests[1].headers, "mcp-session-id") == "session-cleanup");
	CHECK(io.stdout_lines.size() == 1);
	CHECK_FALSE(io.stderr_lines.is_empty());
}

TEST_CASE("[MCP][CLI] Missing and invalid discovery records only report diagnostics") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_cli_bad_discovery", false, &error);
	REQUIRE(error == OK);

	MCPProjectIdentity identity;
	MCPDiscoveryRecord record;
	String project_path;
	String discovery_directory;
	create_project_and_record(
			temporary_directory,
			"http://example.com/mcp",
			identity,
			record,
			project_path,
			discovery_directory);

	SUBCASE("Missing discovery record") {
		MemoryCLIIO io;
		QueueCLITransport transport;
		MCPCLI::Options options;
		options.discovery_directory = discovery_directory;
		options.startup_wait_ms = 0;
		MCPCLI cli(options, &transport, &io);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(io.stdout_lines.is_empty());
		CHECK_FALSE(io.stderr_lines.is_empty());
		CHECK(transport.requests.is_empty());
	}

	SUBCASE("Non-loopback endpoint is rejected before bearer transport") {
		REQUIRE(MCPDiscovery::write_record(discovery_directory, record, true) == OK);
		MemoryCLIIO io;
		QueueCLITransport transport;
		MCPCLI::Options options;
		options.discovery_directory = discovery_directory;
		options.startup_wait_ms = 0;
		MCPCLI cli(options, &transport, &io);
		CHECK(cli.run_stdio_bridge(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(io.stdout_lines.is_empty());
		CHECK_FALSE(io.stderr_lines.is_empty());
		CHECK(transport.requests.is_empty());
	}

	SUBCASE("Unknown discovery transport is rejected before output") {
		record.endpoint = "http://127.0.0.1:35401/mcp";
		record.transport = "custom";
		REQUIRE(MCPDiscovery::write_record(discovery_directory, record, true) == OK);
		MemoryCLIIO io;
		QueueCLITransport transport;
		MCPCLI::Options options;
		options.discovery_directory = discovery_directory;
		options.startup_wait_ms = 0;
		MCPCLI cli(options, &transport, &io);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(io.stdout_lines.is_empty());
		CHECK_FALSE(io.stderr_lines.is_empty());
		CHECK(transport.requests.is_empty());
	}
}

class LiveProcessProbe final : public MCPProjectLeaseProcessProbe {
public:
	bool is_process_running(ProcessID) const override { return true; }
};

class DeadProcessProbe final : public MCPProjectLeaseProcessProbe {
public:
	bool is_process_running(ProcessID) const override { return false; }
};

class RecordingLauncher final : public MCPCLIEditorLauncher {
public:
	int calls = 0;
	Error result = ERR_UNAVAILABLE;
	String discovery_directory;
	String block_candidate_path;
	MCPDiscoveryRecord record;

	bool is_running(ProcessID) const override { return true; }

	Error launch(const String &, ProcessID &r_pid, String &r_error) override {
		calls++;
		if (result != OK) {
			r_error = "LAUNCH_UNAVAILABLE: test launch failed";
			return result;
		}
		r_pid = record.pid;
		const Error publish_error = MCPDiscovery::write_record(discovery_directory, record, true, &r_error);
		if (publish_error == OK && !block_candidate_path.is_empty()) {
			return DirAccess::make_dir_absolute(block_candidate_path);
		}
		return publish_error;
	}
};

class NoEditorHealth final : public MCPProjectLeaseHealthProbe {
public:
	bool is_healthy(const MCPDiscoveryRecord &) const override { return false; }
};

TEST_CASE("[MCP][CLI] Attach-first lifecycle never launches for a healthy, busy or ordinary editor") {
	Error error = OK;
	Ref<DirAccess> root = DirAccess::create_temp("mcp_cli_attach", false, &error);
	REQUIRE(error == OK);
	MCPProjectIdentity identity;
	MCPDiscoveryRecord record;
	String project_path;
	String discovery_directory;
	create_project_and_record(root, "http://127.0.0.1:35900/mcp", identity, record, project_path, discovery_directory);
	MCPCLI::Options options;
	options.discovery_directory = discovery_directory;
	options.startup_wait_ms = 0;
	options.allow_start = true;
	RecordingLauncher launcher;
	LiveProcessProbe process_probe;
	MemoryCLIIO io;
	QueueCLITransport transport;

	SUBCASE("Healthy Host is attached") {
		REQUIRE(MCPDiscovery::write_record(discovery_directory, record, true) == OK);
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_OK);
		CHECK(transport.host_check_count == 1);
		CHECK(launcher.calls == 0);
	}
	SUBCASE("Live but unresponsive Host is never replaced") {
		REQUIRE(MCPDiscovery::write_record(discovery_directory, record, true) == OK);
		transport.host_check_result = ERR_TIMEOUT;
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(launcher.calls == 0);
		REQUIRE(io.stderr_lines.size() == 1);
		CHECK(io.stderr_lines[0].contains("HOST_NOT_READY"));
	}
	SUBCASE("Ordinary editor is retained with MCP disabled") {
		NoEditorHealth health;
		MCPProjectInstance instance;
		REQUIRE(instance.register_editor(project_path, false, health) == OK);
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(launcher.calls == 0);
		CHECK(instance.is_registered());
		REQUIRE(io.stderr_lines.size() == 1);
		CHECK(io.stderr_lines[0].contains("MCP_DISABLED"));
	}
}

TEST_CASE("[MCP][CLI] Explicit start launches once and concurrent callers do not launch competitors") {
	Error error = OK;
	Ref<DirAccess> root = DirAccess::create_temp("mcp_cli_launch", false, &error);
	REQUIRE(error == OK);
	MCPProjectIdentity identity;
	MCPDiscoveryRecord record;
	String project_path;
	String discovery_directory;
	create_project_and_record(root, "http://127.0.0.1:35901/mcp", identity, record, project_path, discovery_directory);
	Ref<FileAccess> project = FileAccess::open(project_path.path_join("project.godot"), FileAccess::WRITE);
	REQUIRE(project.is_valid());
	project->store_string("config_version=5\n");
	project->close();
	MCPCLI::Options options;
	options.discovery_directory = discovery_directory;
	options.startup_wait_ms = 100;
	options.allow_start = true;
	RecordingLauncher launcher;
	LiveProcessProbe process_probe;
	launcher.result = OK;
	launcher.record = record;
	launcher.discovery_directory = discovery_directory;
	MemoryCLIIO io;
	QueueCLITransport transport;

	SUBCASE("Explicit absent-project startup happens once") {
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_OK);
		CHECK(launcher.calls == 1);
		CHECK(transport.host_check_count == 1);
		CHECK_FALSE(FileAccess::exists(MCPProjectInstance::get_launch_directory(identity).path_join("candidate.json")));
	}
	SUBCASE("Connect-only never launches") {
		options.allow_start = false;
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(launcher.calls == 0);
	}
	SUBCASE("Another client's launch reservation is awaited without spawning") {
		NoEditorHealth health;
		MCPProjectLease guard(identity, MCPProjectInstance::get_launch_directory(identity));
		REQUIRE(guard.acquire(record, health) == OK);
		options.startup_wait_ms = 1;
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(launcher.calls == 0);
		CHECK(guard.is_acquired());
	}
	SUBCASE("A live candidate survives its launcher exiting and prevents another spawn") {
		MCPDiscoveryRecord candidate = record;
		candidate.transport = "editor-launch";
		candidate.endpoint = String();
		const String candidate_path = MCPProjectInstance::get_launch_directory(identity).path_join("candidate.json");
		REQUIRE(MCPDiscovery::write_record_file(candidate_path, candidate, false) == OK);
		options.startup_wait_ms = 1;
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(launcher.calls == 0);
		MCPDiscoveryRecord retained;
		REQUIRE(MCPDiscovery::read_record_file(candidate_path, retained) == OK);
		CHECK(retained.instance_id == candidate.instance_id);
		CHECK(retained.pid == candidate.pid);
	}
	SUBCASE("Corrupt candidate metadata never authorizes a launch") {
		const String candidate_path = MCPProjectInstance::get_launch_directory(identity).path_join("candidate.json");
		REQUIRE(MCPDiscovery::write_record_file(candidate_path, record, false) == OK);
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(launcher.calls == 0);
		REQUIRE(io.stderr_lines.size() == 1);
		CHECK(io.stderr_lines[0].contains("OWNERSHIP_UNCERTAIN"));
	}
	SUBCASE("A dead launcher with no recorded candidate is ambiguous and fails closed") {
		NoEditorHealth health;
		MCPProjectLease abandoned(identity, MCPProjectInstance::get_launch_directory(identity));
		REQUIRE(abandoned.acquire(record, health) == OK);
		abandoned.abandon();
		DeadProcessProbe dead_process;
		MCPCLI cli(options, &transport, &io, &launcher, &dead_process);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(launcher.calls == 0);
		CHECK(FileAccess::exists(abandoned.get_owner_file()));
		REQUIRE(io.stderr_lines.size() == 1);
		CHECK(io.stderr_lines[0].contains("OWNERSHIP_UNCERTAIN"));
	}
	SUBCASE("Candidate publication failure preserves evidence and never retries or kills") {
		launcher.block_candidate_path = MCPProjectInstance::get_launch_directory(identity).path_join("candidate.json");
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(launcher.calls == 1);
		MCPProjectLease retained_guard(identity, MCPProjectInstance::get_launch_directory(identity));
		CHECK(FileAccess::exists(retained_guard.get_owner_file()));
		REQUIRE(io.stderr_lines.size() == 1);
		CHECK(io.stderr_lines[0].contains("OWNERSHIP_UNCERTAIN"));
	}
	SUBCASE("Failed launch is not retried") {
		launcher.result = ERR_CANT_FORK;
		MCPCLI cli(options, &transport, &io, &launcher, &process_probe);
		CHECK(cli.run_discover(project_path) == MCPCLI::EXIT_DISCOVERY_FAILED);
		CHECK(launcher.calls == 1);
	}
}

TEST_CASE("[MCP][CLI] Lifecycle flags are closed and cannot restart an editor") {
	MemoryCLIIO io;
	QueueCLITransport transport;
	RecordingLauncher launcher;
	LiveProcessProbe process_probe;
	MCPCLI cli(MCPCLI::Options(), &transport, &io, &launcher, &process_probe);
	const Vector<PackedStringArray> invalid{
		{ "/project", "--allow-start", "--connect-only" },
		{ "/project", "--allow-start", "--allow-start" },
		{ "/project", "--restart" },
		{ "/project", "--wait-ms", "-1" },
		{ "/project", "--wait-ms", "120001" },
		{ "/project", "--wait-ms", "abc" },
		{ "/project", "--allow-start", "--wait-ms", "0" },
		{ "/project", "--wait-ms" },
	};
	for (const PackedStringArray &arguments : invalid) {
		CHECK(cli.execute_cli_command(MCPCLI::COMMAND_STDIO, arguments) == MCPCLI::EXIT_INVALID_ARGUMENTS);
	}
	CHECK(launcher.calls == 0);
	CHECK(transport.requests.is_empty());
}

TEST_CASE("[MCP][CLI] Health verification rejects a stale endpoint with another instance") {
	QueueCLITransport transport;
	MCPDiscoveryRecord record;
	record.project_id = "requested-project";
	record.instance_id = "requested-instance";
	record.endpoint = "http://127.0.0.1:35902/mcp";
	transport.responses.push_back(make_response(200, R"({"ok":true,"projectId":"requested-project","instanceId":"another-instance"})"));
	String error;
	CHECK(transport.MCPCLITransport::check_host(record, 100, error) == ERR_INVALID_DATA);
	CHECK(error.contains("HOST_IDENTITY_MISMATCH"));
	REQUIRE(transport.requests.size() == 1);
	CHECK(transport.requests[0].method == MCPCLITransport::REQUEST_GET);
	CHECK(transport.requests[0].endpoint == "http://127.0.0.1:35902/health");
	CHECK(find_request_header(transport.requests[0].headers, "authorization").is_empty());
}

} // namespace TestMCPCLI
