/**************************************************************************/
/*  test_mcp_trace_service.cpp                                            */
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

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/mcp/mcp_protocol.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/mcp/providers/mcp_trace_service.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mcp_trace_service);

namespace TestMCPTraceService {

static Array _read_events(const String &p_session_directory) {
	const Dictionary manifest = JSON::parse_string(FileAccess::get_file_as_string(p_session_directory.path_join("manifest.json")));
	const Array files = manifest.get("eventFiles", Array());
	Array events;
	for (const Variant &file_value : files) {
		const PackedStringArray lines = FileAccess::get_file_as_string(p_session_directory.path_join(String(file_value))).split("\n", false);
		for (const String &line : lines) {
			if (!line.strip_edges().is_empty()) {
				events.push_back(JSON::parse_string(line));
			}
		}
	}
	return events;
}

class TraceEnvironment {
	String enabled = OS::get_singleton()->get_environment("GODOT_MCP_TRACE");
	String directory = OS::get_singleton()->get_environment("GODOT_MCP_TRACE_DIR");
	bool had_enabled = OS::get_singleton()->has_environment("GODOT_MCP_TRACE");
	bool had_directory = OS::get_singleton()->has_environment("GODOT_MCP_TRACE_DIR");

public:
	~TraceEnvironment() {
		if (had_enabled) {
			OS::get_singleton()->set_environment("GODOT_MCP_TRACE", enabled);
		} else {
			OS::get_singleton()->unset_environment("GODOT_MCP_TRACE");
		}
		if (had_directory) {
			OS::get_singleton()->set_environment("GODOT_MCP_TRACE_DIR", directory);
		} else {
			OS::get_singleton()->unset_environment("GODOT_MCP_TRACE_DIR");
		}
	}
};

class TraceProvider : public Object {
public:
	MCPToolRegistry *registry = nullptr;
	int calls = 0;

	Dictionary echo(const Dictionary &p_arguments, const Dictionary &p_context) {
		calls++;
		if (p_arguments.get("nested", false)) {
			MCPToolCallContext context;
			context.session = p_context.get("session", Dictionary());
			registry->call_tool("trace.echo", Dictionary(), context);
		}
		return Dictionary{ { "structuredContent", p_arguments }, { "content", Array{ Dictionary{ { "type", "text" }, { "text", "result-content-secret" } } } }, { "isError", p_arguments.get("fail", false) } };
	}

	int invalid_result(const Dictionary &p_arguments, const Dictionary &p_context) {
		return 42;
	}
};

static Dictionary _request(const String &p_method, const Dictionary &p_params = Dictionary()) {
	return Dictionary{ { "jsonrpc", "2.0" }, { "method", p_method }, { "id", "raw-request-id-secret" }, { "params", p_params } };
}

static void _initialize(MCPProtocol &p_protocol, const Dictionary &p_session) {
	const Dictionary params{ { "protocolVersion", MCPProtocol::PROTOCOL_VERSION_2025_03_26 }, { "capabilities", Dictionary() }, { "clientInfo", Dictionary{ { "name", "raw-client-secret" }, { "version", "1" } } } };
	p_protocol.dispatch(_request("initialize", params), p_session);
	p_protocol.dispatch(Dictionary{ { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } }, p_session);
}

TEST_CASE("[MCP] Ordinary trace is opt-in, payload-free and correlated through nested dispatch") {
	TraceEnvironment environment;
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_trace_service", false, &error);
	REQUIRE(error == OK);
	const String root = temporary_directory->get_current_dir().path_join("traces");
	OS::get_singleton()->set_environment("GODOT_MCP_TRACE_DIR", root);
	MCPToolRegistry registry;
	MCPTraceService service;
	REQUIRE(service.register_tools(&registry) == OK);
	MCPProtocol protocol(&registry);
	const Dictionary session{ { "sessionId", "raw-session-secret" } };
	for (const char *disabled : { "", "0", "off", "true" }) {
		OS::get_singleton()->set_environment("GODOT_MCP_TRACE", disabled);
		service.configure_ordinary(Dictionary());
		protocol.dispatch(_request("ping"), session);
		CHECK_FALSE(service.is_started());
		CHECK(registry.get_trace_observer() == nullptr);
		CHECK_FALSE(DirAccess::dir_exists_absolute(root));
	}

	OS::get_singleton()->set_environment("GODOT_MCP_TRACE", "1");
	service.configure_ordinary(Dictionary{ { "projectId", "project-id-secret" }, { "projectPath", "C:/private/project-secret" }, { "arbitrary", "manifest-secret" } });
	REQUIRE(service.is_started());
	REQUIRE(registry.get_trace_observer() == &service);
	const String session_directory = service.get_session_directory();
	// Reconfiguration and explicit Harness starts cannot replace an active writer.
	REQUIRE(service.start(Dictionary(), "editor-mcp-harness", root) == OK);
	CHECK(service.get_session_directory() == session_directory);

	TraceProvider *provider = memnew(TraceProvider);
	provider->registry = &registry;
	REQUIRE(registry.register_tool(Dictionary{ { "name", "trace.echo" } }, callable_mp(provider, &TraceProvider::echo), provider) == OK);
	REQUIRE(registry.register_tool(Dictionary{ { "name", "trace.invalid" } }, callable_mp(provider, &TraceProvider::invalid_result), provider) == OK);
	_initialize(protocol, session);
	Dictionary arguments{ { "opaque", "argument-secret" }, { "name", "credential-name" }, { "value", "name-value-secret" }, { "nested", true } };
	protocol.dispatch(_request("tools/call", Dictionary{ { "name", "trace.echo" }, { "arguments", arguments } }), session);
	protocol.dispatch(_request("tools/call", Dictionary{ { "name", "trace.echo" }, { "arguments", Dictionary{ { "fail", true } } } }), session);
	protocol.dispatch(_request("tools/call", Dictionary{ { "name", "trace.invalid" } }), session);
	protocol.dispatch(_request("tools/call", Dictionary{ { "name", "unknown-tool-secret" } }), session);
	protocol.dispatch(_request("unknown-method-secret"), session);
	protocol.dispatch(_request("tools/call", Dictionary{ { "arguments", "invalid-params-secret" } }), session);
	protocol.dispatch(42, session);
	ERR_PRINT_OFF;
	protocol.dispatch_json("{raw-parse-secret", session);
	ERR_PRINT_ON;
	// A nested JSON-RPC array remains invalid and must never execute its tool.
	const int previous_calls = provider->calls;
	const Array nested{ Array{ _request("tools/call", Dictionary{ { "name", "trace.echo" } }) } };
	const Array batch_response = protocol.dispatch(nested, session).response;
	REQUIRE(batch_response.size() == 1);
	CHECK(int(Dictionary(Dictionary(batch_response[0])["error"])["code"]) == MCPProtocol::INVALID_REQUEST);
	CHECK(provider->calls == previous_calls);
	MCPProtocol other_protocol(&registry);
	const Dictionary other_session{ { "sessionId", "other-session-secret" } };
	_initialize(other_protocol, other_session);
	other_protocol.dispatch(_request("tools/call", Dictionary{ { "name", "trace.echo" } }), other_session);
	service.unregister_tools();
	service.shutdown();
	service.stop();
	CHECK_FALSE(service.is_started());
	CHECK(registry.get_trace_observer() == nullptr);
	registry.unregister_tools_for_owner(provider);
	memdelete(provider);

	const String manifest_text = FileAccess::get_file_as_string(session_directory.path_join("manifest.json"));
	const Dictionary manifest = JSON::parse_string(manifest_text);
	CHECK_FALSE(String(manifest.get("endedAtUtc", String())).is_empty());
	CHECK(int(manifest["maxEventFiles"]) == 4);
	const Array events = _read_events(session_directory);
	REQUIRE(events.size() > 20);
	Dictionary started;
	int business_errors = 0;
	int protocol_errors = 0;
	bool nested_tool = false;
	bool saw_other_session = false;
	String serialized = manifest_text;
	for (const Variant &value : events) {
		const Dictionary event = value;
		serialized += JSON::stringify(event);
		const Dictionary data = event["data"];
		const String id = event["correlationId"];
		const String session_hash = data["sessionHash"];
		CHECK((session_hash == String("raw-session-secret").sha256_text().left(16) || session_hash == String("other-session-secret").sha256_text().left(16)));
		saw_other_session |= session_hash == String("other-session-secret").sha256_text().left(16);
		if (event["event"] == "started") {
			CHECK_FALSE(started.has(id));
			started[id] = event;
			const String parent = data["parentId"];
			if (!parent.is_empty()) {
				REQUIRE(started.has(parent));
				const Dictionary parent_event = started[parent];
				CHECK(Dictionary(parent_event["data"])["sessionHash"] == session_hash);
				CHECK(Dictionary(parent_event["data"])["rootId"] == data["rootId"]);
				nested_tool |= event["category"] == "tool" && parent_event["category"] == "tool";
			} else {
				CHECK(data["rootId"] == id);
			}
		} else {
			REQUIRE(started.has(id));
			CHECK(int64_t(data["durationUsec"]) >= 0);
			if (data["outcome"] == "business_error") {
				business_errors++;
				CHECK(bool(data["isError"]));
				CHECK(event["severity"] == "error");
			}
			if (data["outcome"] == "protocol_error") {
				protocol_errors++;
				CHECK(bool(data["isError"]));
			}
			started.erase(id);
		}
	}
	CHECK(started.is_empty());
	CHECK(nested_tool);
	CHECK(saw_other_session);
	CHECK(business_errors == 2); // Registry and enclosing protocol response.
	CHECK(protocol_errors >= 7);
	CHECK_FALSE(serialized.contains("secret"));
	CHECK_FALSE(serialized.contains("credential-name"));
	CHECK_FALSE(serialized.contains("C:/private"));
}

TEST_CASE("[MCP] Explicit Harness trace does not enable ordinary request tracing") {
	TraceEnvironment environment;
	OS::get_singleton()->set_environment("GODOT_MCP_TRACE", "0");
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_harness_trace_only", false, &error);
	REQUIRE(error == OK);
	MCPToolRegistry registry;
	MCPTraceService service;
	REQUIRE(service.register_tools(&registry) == OK);
	service.configure_ordinary(Dictionary());
	REQUIRE(service.start(Dictionary{ { "projectPath", "C:/private/project.godot" }, { "authToken", "hidden-token" } }, "editor-mcp-harness", temporary_directory->get_current_dir()) == OK);
	MCPProtocol protocol(&registry);
	protocol.dispatch(_request("ping"));
	const String directory = service.get_session_directory();
	const Dictionary manifest = JSON::parse_string(FileAccess::get_file_as_string(directory.path_join("manifest.json")));
	CHECK(int(manifest["maxEventFiles"]) == 0);
	CHECK(Dictionary(manifest["project"])["projectPath"] == "project.godot");
	CHECK_FALSE(JSON::stringify(manifest).contains("hidden-token"));
	CHECK(_read_events(directory).is_empty());
	REQUIRE(service.record_job_event("job", "started", "info", Dictionary()) == OK);
	CHECK(_read_events(directory).size() == 1);
	service.unregister_tools();
	REQUIRE(service.register_tools(&registry) == OK);
	CHECK(registry.get_trace_observer() == nullptr);
	service.unregister_tools();
}

TEST_CASE("[MCP] Ordinary trace initialization failure is nonfatal and leaves no observer") {
	TraceEnvironment environment;
	OS::get_singleton()->set_environment("GODOT_MCP_TRACE", "1");
	OS::get_singleton()->set_environment("GODOT_MCP_TRACE_DIR", "relative-path-secret");
	MCPToolRegistry registry;
	MCPTraceService service;
	REQUIRE(service.register_tools(&registry) == OK);
	service.configure_ordinary(Dictionary());
	CHECK_FALSE(service.is_started());
	CHECK(registry.get_trace_observer() == nullptr);
	CHECK_FALSE(String(service.get_safe_reference().get("error", String())).is_empty());
	MCPProtocol protocol(&registry);
	CHECK(Dictionary(protocol.dispatch(_request("ping")).response).has("result"));
	service.unregister_tools();
}

TEST_CASE("[MCP] Ordinary trace write failure is nonfatal and stops repeated writes") {
	TraceEnvironment environment;
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_trace_write_failure", false, &error);
	REQUIRE(error == OK);
	OS::get_singleton()->set_environment("GODOT_MCP_TRACE", "1");
	OS::get_singleton()->set_environment("GODOT_MCP_TRACE_DIR", temporary_directory->get_current_dir());
	MCPToolRegistry registry;
	MCPTraceService service;
	REQUIRE(service.register_tools(&registry) == OK);
	service.configure_ordinary(Dictionary());
	REQUIRE(service.is_started());
	const String manifest = service.get_session_directory().path_join("manifest.json");
	REQUIRE(DirAccess::remove_absolute(manifest) == OK);
	REQUIRE(DirAccess::make_dir_absolute(manifest) == OK);
	MCPProtocol protocol(&registry);
	CHECK(Dictionary(protocol.dispatch(_request("ping")).response).has("result"));
	CHECK_FALSE(service.is_started());
	CHECK_FALSE(String(service.get_safe_reference().get("error", String())).is_empty());
	CHECK(Dictionary(protocol.dispatch(_request("ping")).response).has("result"));
	REQUIRE(DirAccess::remove_absolute(manifest) == OK);
	CHECK(registry.get_trace_observer() == nullptr);
	REQUIRE(service.start(Dictionary(), "editor-mcp-harness", temporary_directory->get_current_dir()) == OK);
	const String harness_directory = service.get_session_directory();
	CHECK(Dictionary(protocol.dispatch(_request("ping")).response).has("result"));
	CHECK(_read_events(harness_directory).is_empty());
	service.unregister_tools();
	CHECK(registry.get_trace_observer() == nullptr);
}

TEST_CASE("[MCP] Trace service bounds structured fields and summarizes binary payloads") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_trace_bounds", false, &error);
	REQUIRE(error == OK);
	MCPTraceService service;
	REQUIRE(service.start(Dictionary(), "editor", temporary_directory->get_current_dir()) == OK);

	Dictionary large_data;
	const String chunk = String("x").repeat(MCPTraceService::MAX_STRING_CHARACTERS);
	for (int i = 0; i < 40; i++) {
		large_data[vformat("field%02d", i)] = chunk;
	}
	PackedByteArray bytes;
	bytes.resize(256);
	for (int i = 0; i < bytes.size(); i++) {
		bytes.set(i, uint8_t(i));
	}
	Dictionary payloads;
	payloads["capture"] = bytes;
	REQUIRE(service.record_job_event("job-bounded", "sample", "info", large_data, payloads) == OK);
	CHECK(service.record_job_event("job-bounded", "bad", "fatal", Dictionary()) == ERR_INVALID_PARAMETER);
	const String session_directory = service.get_session_directory();
	service.stop();

	const Array events = _read_events(session_directory);
	REQUIRE(events.size() == 1);
	const Dictionary entry_data = Dictionary(events[0]).get("data", Dictionary());
	const Dictionary bounded_data = entry_data.get("data", Dictionary());
	CHECK(bool(bounded_data.get("_truncated", false)));
	CHECK(int64_t(bounded_data.get("originalBytes", 0)) > int64_t(MCPTraceService::MAX_STRUCTURED_FIELD_BYTES));
	CHECK_FALSE(String(bounded_data.get("sha256", String())).is_empty());
	const Dictionary bounded_payloads = entry_data.get("payloads", Dictionary());
	const Dictionary capture = bounded_payloads.get("capture", Dictionary());
	CHECK(capture.get("type", String()) == "bytes");
	CHECK(int(capture.get("bytes", 0)) == 256);
	CHECK(String(capture.get("sha256", String())).length() == 64);
	CHECK_FALSE(JSON::stringify(events[0]).contains(String("x").repeat(MCPTraceService::MAX_STRING_CHARACTERS + 1)));
}

} // namespace TestMCPTraceService
