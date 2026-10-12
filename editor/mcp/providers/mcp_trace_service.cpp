/**************************************************************************/
/*  mcp_trace_service.cpp                                                 */
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

#include "mcp_trace_service.h"

#include "core/crypto/crypto_core.h"
#include "core/io/json.h"
#include "core/mcp/mcp_tool_registry.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

MCPTraceService::~MCPTraceService() {
	unregister_tools();
	stop();
}

void MCPTraceService::_set_error(String *r_error, const String &p_message) {
	if (r_error) {
		*r_error = p_message;
	}
}

bool MCPTraceService::_is_secret_key(const String &p_key) {
	const String key = p_key.to_lower().replace("-", "_");
	return key.contains("auth") || key.contains("token") || key.contains("secret") || key.contains("password");
}

bool MCPTraceService::_is_session_key(const String &p_key) {
	const String key = p_key.to_lower().replace("-", "_");
	return key == "session" || key == "session_id" || key == "sessionid";
}

bool MCPTraceService::_is_path_key(const String &p_key) {
	const String key = p_key.to_lower();
	return key.contains("path") || key.contains("file") || key == "cwd" || key.contains("binary") || key.contains("executable");
}

String MCPTraceService::_session_hash(const String &p_session_id) {
	return p_session_id.is_empty() ? String() : p_session_id.sha256_text().left(16);
}

String MCPTraceService::_sanitize_path(const String &p_path) {
	const String normalized = p_path.replace_char('\\', '/').simplify_path();
	return normalized.begins_with("res://") ? normalized : normalized.get_file();
}

String MCPTraceService::_bounded_text(const String &p_value, int p_max_characters) {
	return p_value.length() <= p_max_characters ? p_value : p_value.left(p_max_characters) + "<truncated>";
}

Dictionary MCPTraceService::_sanitize_dictionary(const Dictionary &p_value, int p_depth) {
	Dictionary result;
	const Array keys = p_value.keys();
	const int count = MIN(keys.size(), MAX_CONTAINER_ENTRIES);
	for (int i = 0; i < count; i++) {
		const Variant &key_value = keys[i];
		if (key_value.get_type() != Variant::STRING && key_value.get_type() != Variant::STRING_NAME) {
			continue;
		}
		const String key = _bounded_text(String(key_value), 256);
		if (_is_secret_key(key)) {
			result[key] = "<redacted>";
		} else if (_is_session_key(key)) {
			result[key] = _session_hash(String(p_value[key_value]));
		} else {
			result[key] = _sanitize_variant(p_value[key_value], key, p_depth + 1);
		}
	}
	if (keys.size() > MAX_CONTAINER_ENTRIES) {
		result["_truncated"] = true;
		result["_originalEntries"] = keys.size();
	}
	return result;
}

Variant MCPTraceService::_sanitize_variant(const Variant &p_value, const String &p_key_hint, int p_depth) {
	if (p_depth > MAX_DEPTH) {
		Dictionary omitted;
		omitted["_truncated"] = true;
		omitted["reason"] = "maxDepth";
		return omitted;
	}
	switch (p_value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
			return p_value;
		case Variant::STRING:
		case Variant::STRING_NAME: {
			const String text = p_value;
			if (_is_path_key(p_key_hint)) {
				return _sanitize_path(text);
			}
			return _bounded_text(text, MAX_STRING_CHARACTERS);
		}
		case Variant::DICTIONARY:
			return _sanitize_dictionary(p_value, p_depth);
		case Variant::ARRAY: {
			const Array source = p_value;
			Array result;
			const int count = MIN(source.size(), MAX_CONTAINER_ENTRIES);
			for (int i = 0; i < count; i++) {
				result.push_back(_sanitize_variant(source[i], p_key_hint, p_depth + 1));
			}
			if (source.size() > MAX_CONTAINER_ENTRIES) {
				Dictionary marker;
				marker["_truncated"] = true;
				marker["_originalEntries"] = source.size();
				result.push_back(marker);
			}
			return result;
		}
		case Variant::PACKED_BYTE_ARRAY: {
			const PackedByteArray bytes = p_value;
			uint8_t hash[32] = {};
			CryptoCore::sha256(bytes.ptr(), bytes.size(), hash);
			Dictionary descriptor;
			descriptor["type"] = "bytes";
			descriptor["bytes"] = bytes.size();
			descriptor["sha256"] = String::hex_encode_buffer(hash, 32);
			return descriptor;
		}
		default: {
			Dictionary omitted;
			omitted["type"] = Variant::get_type_name(p_value.get_type());
			omitted["omitted"] = true;
			return omitted;
		}
	}
}

Dictionary MCPTraceService::_bounded_dictionary(const Dictionary &p_value) {
	Dictionary sanitized = _sanitize_dictionary(p_value);
	const String serialized = JSON::stringify(sanitized);
	const uint64_t bytes = serialized.to_utf8_buffer().size();
	if (bytes <= MAX_STRUCTURED_FIELD_BYTES) {
		return sanitized;
	}
	Dictionary summary;
	summary["_truncated"] = true;
	summary["originalBytes"] = int64_t(bytes);
	summary["sha256"] = serialized.sha256_text();
	return summary;
}

Error MCPTraceService::start(const Dictionary &p_project_metadata, const String &p_process_mode, const String &p_root_directory_override, String *r_error) {
	_set_error(r_error, String());
	MutexLock lock(mutex);
	if (started) {
		return OK;
	}
	last_error = String();
	StructuredTraceWriter::Options options;
	options.root_directory_override = p_root_directory_override;
	const bool harness_only = p_process_mode == "editor-mcp-harness";
	options.max_event_files = harness_only ? 0 : 4;
	Dictionary process;
	process["pid"] = int64_t(OS::get_singleton()->get_process_id());
	process["mode"] = p_process_mode == "editor-mcp-harness" ? "editor-mcp-harness" : "editor-mcp";
	// Metadata supplied by projects must not become a content-logging back door.
	Dictionary project;
	for (const char *key : { "projectId", "instanceId" }) {
		const Variant value = p_project_metadata.get(key, Variant());
		if (value.get_type() == Variant::STRING) {
			project[String(key) + "Hash"] = String(value).sha256_text().left(16);
		}
	}
	if (harness_only) {
		// Explicit Harness evidence keeps its existing metadata/retention policy.
		project = _bounded_dictionary(p_project_metadata);
		process["binaryPath"] = OS::get_singleton()->get_executable_path().get_file();
		process["cwd"] = OS::get_singleton()->get_cwd().get_file();
	}
	const Error error = writer.open(project, process, options, r_error);
	started = error == OK;
	return error;
}

Error MCPTraceService::_record(const String &p_category, const String &p_event, const String &p_severity, const String &p_correlation_id, const Dictionary &p_data, const Dictionary &p_error, String *r_error) {
	_set_error(r_error, String());
	MutexLock lock(mutex);
	if (!started) {
		return OK;
	}
	return writer.record(p_category, _bounded_text(p_event, 256), p_severity, _bounded_text(p_correlation_id, 256), _bounded_dictionary(p_data), Dictionary(), _bounded_dictionary(p_error), r_error);
}

Error MCPTraceService::register_tools(MCPToolRegistry *p_registry, String *r_error) {
	_set_error(r_error, String());
	if (!p_registry || tool_registry) {
		_set_error(r_error, "Trace service requires an unbound Tool Registry.");
		return ERR_INVALID_PARAMETER;
	}
	tool_registry = p_registry;
	return OK;
}

void MCPTraceService::unregister_tools() {
	if (tool_registry) {
		if (tool_registry->get_trace_observer() == this) {
			tool_registry->set_trace_observer(nullptr);
		}
		tool_registry = nullptr;
	}
	stop();
}

void MCPTraceService::configure_ordinary(const Dictionary &p_project_metadata) {
	if (!tool_registry || OS::get_singleton()->get_environment("GODOT_MCP_TRACE") != "1") {
		return;
	}
	String error;
	if (start(p_project_metadata, "editor-mcp", OS::get_singleton()->get_environment("GODOT_MCP_TRACE_DIR"), &error) != OK) {
		_report_failure(error);
		return;
	}
	tool_registry->set_trace_observer(this);
	print_error("MCP_TRACE_ENABLED: metadata-only trace " + writer.get_session_id());
}

void MCPTraceService::_report_failure(const String &p_error) {
	MutexLock lock(mutex);
	if (!last_error.is_empty()) {
		return;
	}
	last_error = p_error.is_empty() ? String("Unable to write MCP trace.") : p_error;
	if (tool_registry && tool_registry->get_trace_observer() == this) {
		tool_registry->set_trace_observer(nullptr);
	}
	// Only fixed writer diagnostics reach stderr. Never print request content.
	print_error("MCP_TRACE_FAILED: " + last_error);
	writer.close();
	started = false;
}

void MCPTraceService::_record_span(const MCPTraceSpan &p_span, bool p_finished) {
	Dictionary data;
	data["name"] = p_span.name;
	data["sessionHash"] = p_span.session_hash;
	data["parentId"] = p_span.parent_id ? String::num_uint64(p_span.parent_id) : String();
	data["rootId"] = String::num_uint64(p_span.root_id);
	if (p_finished) {
		static const char *outcomes[] = { "success", "business_error", "protocol_error", "notification" };
		data["durationUsec"] = int64_t(p_span.duration_usec);
		data["outcome"] = outcomes[p_span.outcome];
		data["status"] = p_span.status;
		data["isError"] = p_span.is_error;
	}
	String error;
	const Error result = _record(p_span.kind == MCPTraceSpan::REQUEST ? "mcp" : "tool", p_finished ? "finished" : "started", p_finished && p_span.is_error ? "error" : "info", String::num_uint64(p_span.id), data, Dictionary(), &error);
	if (result != OK) {
		_report_failure(error);
	}
}

void MCPTraceService::on_trace_started(const MCPTraceSpan &p_span) {
	_record_span(p_span, false);
}

void MCPTraceService::on_trace_finished(const MCPTraceSpan &p_span) {
	_record_span(p_span, true);
}

Error MCPTraceService::record_job_event(const String &p_job_id, const String &p_event, const String &p_severity, const Dictionary &p_data, const Dictionary &p_payloads, const Dictionary &p_error, String *r_error) {
	if (!is_started()) {
		_set_error(r_error, String());
		return OK;
	}
	Dictionary data;
	data["jobId"] = _bounded_text(p_job_id, 256);
	data["data"] = _bounded_dictionary(p_data);
	data["payloads"] = _bounded_dictionary(p_payloads);
	return _record("job", p_event, p_severity, p_job_id, data, p_error, r_error);
}

void MCPTraceService::stop() {
	MutexLock lock(mutex);
	if (tool_registry && tool_registry->get_trace_observer() == this) {
		tool_registry->set_trace_observer(nullptr);
	}
	if (!started) {
		return;
	}
	String error;
	if (writer.close(&error) != OK && last_error.is_empty()) {
		last_error = error;
		print_error("MCP_TRACE_FAILED: " + error);
	}
	started = false;
}

bool MCPTraceService::is_started() const {
	MutexLock lock(mutex);
	return started;
}

String MCPTraceService::get_session_directory() const {
	MutexLock lock(mutex);
	return writer.get_session_directory();
}

Dictionary MCPTraceService::get_safe_reference(const String &p_correlation_id) const {
	MutexLock lock(mutex);
	Dictionary reference;
	reference["available"] = writer.is_open();
	if (!last_error.is_empty()) {
		reference["error"] = last_error;
	}
	const String writer_session_id = writer.get_session_id();
	reference["traceId"] = writer_session_id.is_empty() ? String("active") : writer_session_id;
	reference["manifest"] = "manifest.json";
	if (!p_correlation_id.is_empty()) {
		reference["correlationId"] = _bounded_text(p_correlation_id, 256);
	}
	return reference;
}
