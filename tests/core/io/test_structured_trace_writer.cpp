/**************************************************************************/
/*  test_structured_trace_writer.cpp                                      */
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
#include "core/io/structured_trace_writer.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_structured_trace_writer);

namespace TestStructuredTraceWriter {

static void _write_file(const String &p_path, const String &p_text) {
	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
	REQUIRE(error == OK);
	REQUIRE(file.is_valid());
	file->store_string(p_text);
	file->close();
}

static Dictionary _project_metadata() {
	Dictionary metadata;
	metadata["projectId"] = "project-id";
	metadata["path"] = "C:/project";
	return metadata;
}

static Dictionary _process_metadata() {
	Dictionary metadata;
	metadata["pid"] = 42;
	metadata["mode"] = "editor";
	return metadata;
}

TEST_CASE("[StructuredTraceWriter] Session manifest, rolling JSONL, and payload storage are structured") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("structured_trace", false, &error);
	REQUIRE(error == OK);
	REQUIRE(temporary_directory.is_valid());
	StructuredTraceWriter::Options options;
	options.root_directory_override = temporary_directory->get_current_dir();
	options.max_event_file_bytes = 1;
	options.inline_payload_max_bytes = 4;
	StructuredTraceWriter writer;
	CHECK_FALSE(writer.is_open());
	REQUIRE(writer.open(_project_metadata(), _process_metadata(), options) == OK);
	CHECK(writer.is_open());

	StructuredTraceWriter::PayloadDescriptor small;
	REQUIRE(writer.make_text_payload("small", "text/plain", "tiny", small) == OK);
	CHECK(small.inline_value == Variant("tiny"));
	CHECK(small.ref_path.is_empty());
	CHECK(small.sha256 == String("tiny").sha256_text());
	StructuredTraceWriter::PayloadDescriptor large;
	const String large_text = "payload body";
	REQUIRE(writer.make_text_payload("large payload", "text/markdown", large_text, large) == OK);
	CHECK(large.inline_value.get_type() == Variant::NIL);
	CHECK(large.ref_path.begins_with("payloads/"));
	CHECK(large.ref_path.ends_with(".md"));
	CHECK(large.sha256 == large_text.sha256_text());
	CHECK(FileAccess::get_file_as_string(writer.get_session_directory().path_join(large.ref_path)) == large_text);

	Dictionary payloads;
	payloads["small"] = small.to_dictionary();
	payloads["large"] = large.to_dictionary();
	Dictionary data;
	data["value"] = 1;
	REQUIRE(writer.record("runtime", "started", "info", "corr-1", data, payloads) == OK);
	Dictionary trace_error;
	trace_error["code"] = "FAILED";
	REQUIRE(writer.record("runtime", "failed", "error", "corr-1", Dictionary(), Dictionary(), trace_error) == OK);
	const String session_directory = writer.get_session_directory();
	const String session_id = writer.get_session_id();
	writer.close();
	CHECK_FALSE(writer.is_open());

	const Variant manifest_value = JSON::parse_string(FileAccess::get_file_as_string(session_directory.path_join("manifest.json")));
	REQUIRE(manifest_value.get_type() == Variant::DICTIONARY);
	const Dictionary manifest = manifest_value;
	CHECK(manifest.get("sessionId", String()) == session_id);
	CHECK_FALSE(String(manifest.get("startedAtUtc", String())).is_empty());
	CHECK_FALSE(String(manifest.get("endedAtUtc", String())).is_empty());
	CHECK(Dictionary(manifest.get("project", Dictionary())).get("projectId", String()) == "project-id");
	CHECK(Dictionary(manifest.get("process", Dictionary())).get("mode", String()) == "editor");
	const Array categories = manifest.get("categories", Array());
	REQUIRE(categories.size() == 1);
	CHECK(categories[0] == "runtime");
	const Array event_files = manifest.get("eventFiles", Array());
	REQUIRE(event_files.size() == 2);
	const String first_line = FileAccess::get_file_as_string(session_directory.path_join(String(event_files[0]))).strip_edges();
	const Dictionary first_event = JSON::parse_string(first_line);
	CHECK(int64_t(first_event.get("seq", 0)) == 1);
	CHECK(first_event.get("category", String()) == "runtime");
	CHECK(first_event.get("event", String()) == "started");
	CHECK(Dictionary(first_event.get("payloads", Dictionary())).size() == 2);
}

TEST_CASE("[StructuredTraceWriter] Live events remain visible with editor safe-save enabled") {
	struct RestoreBackupSave {
		bool enabled = FileAccess::is_backup_save_enabled();
		~RestoreBackupSave() { FileAccess::set_backup_save(enabled); }
	} restore_backup_save;
	FileAccess::set_backup_save(true);
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("structured_trace_live", false, &error);
	REQUIRE(error == OK);
	StructuredTraceWriter::Options options;
	options.root_directory_override = temporary_directory->get_current_dir();
	StructuredTraceWriter writer;
	REQUIRE(writer.open(Dictionary(), Dictionary(), options) == OK);
	REQUIRE(writer.record("mcp", "started", "info", "1", Dictionary()) == OK);
	const String path = writer.get_session_directory().path_join("events-0001.jsonl");
	REQUIRE(FileAccess::exists(path));
	const Dictionary event = JSON::parse_string(FileAccess::get_file_as_string(path).strip_edges());
	CHECK(event.get("event", String()) == "started");
	REQUIRE(writer.record("mcp", "finished", "info", "1", Dictionary()) == OK);
	const PackedStringArray lines = FileAccess::get_file_as_string(path).split("\n", false);
	REQUIRE(lines.size() == 2);
	CHECK(Dictionary(JSON::parse_string(lines[0])).get("event", String()) == "started");
	CHECK(Dictionary(JSON::parse_string(lines[1])).get("event", String()) == "finished");
	CHECK(writer.is_open());
	CHECK(FileAccess::is_backup_save_enabled());
	CHECK(writer.close() == OK);
}

TEST_CASE("[StructuredTraceWriter] Existing file payloads are referenced and never copied") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("structured_trace_ref", false, &error);
	REQUIRE(error == OK);
	const String root = temporary_directory->get_current_dir();
	const String source = root.path_join("capture.bin");
	_write_file(source, "binary-like-content");
	StructuredTraceWriter writer;
	StructuredTraceWriter::PayloadDescriptor descriptor;
	CHECK(writer.reference_existing_file("application/octet-stream", source, descriptor) == OK);
	CHECK(descriptor.external);
	CHECK(descriptor.ref_path == source);
	CHECK(descriptor.bytes == int64_t(FileAccess::get_size(source)));
	CHECK(descriptor.sha256 == FileAccess::get_sha256(source));
	CHECK(writer.reference_existing_file("application/octet-stream", root.path_join("missing.bin"), descriptor) == ERR_FILE_NOT_FOUND);
	CHECK(writer.reference_existing_file("application/octet-stream", root, descriptor) == ERR_FILE_NOT_FOUND);
}

TEST_CASE("[StructuredTraceWriter] Retention only removes managed sessions and reserves room for the new session") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("structured_trace_retention", false, &error);
	REQUIRE(error == OK);
	const String root = temporary_directory->get_current_dir();
	for (int i = 0; i < 3; i++) {
		const String old_session = root.path_join(vformat("trace-old-%d", i));
		REQUIRE(DirAccess::make_dir_recursive_absolute(old_session) == OK);
		_write_file(old_session.path_join("manifest.json"), "{}");
	}
	const String unrelated = root.path_join("unrelated-data");
	REQUIRE(DirAccess::make_dir_recursive_absolute(unrelated) == OK);
	_write_file(unrelated.path_join("keep.txt"), "keep");

	StructuredTraceWriter::Options options;
	options.root_directory_override = root;
	options.max_sessions = 2;
	options.max_session_age_days = 0;
	StructuredTraceWriter writer;
	REQUIRE(writer.open(_project_metadata(), _process_metadata(), options) == OK);
	writer.close();
	int managed_count = 0;
	for (const String &name : DirAccess::get_directories_at(root)) {
		if (name.begins_with("trace-") && FileAccess::exists(root.path_join(name).path_join("manifest.json"))) {
			managed_count++;
		}
	}
	CHECK(managed_count == 2);
	CHECK(FileAccess::exists(unrelated.path_join("keep.txt")));
}

TEST_CASE("[StructuredTraceWriter] Opt-in active retention bounds files and preserves sequence order") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("structured_trace_window", false, &error);
	REQUIRE(error == OK);
	StructuredTraceWriter::Options options;
	options.root_directory_override = temporary_directory->get_current_dir();
	options.max_event_file_bytes = 1024;
	options.max_event_files = 2;
	StructuredTraceWriter writer;
	REQUIRE(writer.open(Dictionary(), Dictionary(), options) == OK);
	Dictionary data{ { "padding", String("x").repeat(400) } };
	for (int i = 0; i < 20; i++) {
		REQUIRE(writer.record("mcp", "finished", "info", "1", data) == OK);
	}
	const String directory = writer.get_session_directory();
	writer.close();
	const Dictionary manifest = JSON::parse_string(FileAccess::get_file_as_string(directory.path_join("manifest.json")));
	const Array files = manifest["eventFiles"];
	REQUIRE(files.size() == 2);
	uint64_t total_bytes = 0;
	int64_t sequence = 0;
	int event_file_count = 0;
	for (const String &file : DirAccess::get_files_at(directory)) {
		if (file.ends_with(".jsonl")) {
			event_file_count++;
			CHECK(files.has(file));
		}
	}
	CHECK(event_file_count == 2);
	for (const Variant &file : files) {
		total_bytes += FileAccess::get_size(directory.path_join(file));
		for (const String &line : FileAccess::get_file_as_string(directory.path_join(file)).split("\n", false)) {
			const Dictionary event = JSON::parse_string(line);
			CHECK(int64_t(event["seq"]) > sequence);
			sequence = event["seq"];
		}
	}
	CHECK(sequence == 20);
	CHECK(total_bytes <= 2048);
	REQUIRE(writer.open(Dictionary(), Dictionary(), options) == OK);
	CHECK(writer.record("mcp", "too_large", "info", "1", Dictionary{ { "padding", String("x").repeat(2048) } }) == ERR_OUT_OF_MEMORY);
	writer.close();
	options.max_event_files = -1;
	CHECK(writer.open(Dictionary(), Dictionary(), options) == ERR_INVALID_PARAMETER);
}

TEST_CASE("[StructuredTraceWriter] Rotation failure closes the writer before another record") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("structured_trace_rotate_failure", false, &error);
	REQUIRE(error == OK);
	StructuredTraceWriter::Options options;
	options.root_directory_override = temporary_directory->get_current_dir();
	options.max_event_file_bytes = 1024;
	options.max_event_files = 2;
	StructuredTraceWriter writer;
	REQUIRE(writer.open(Dictionary(), Dictionary(), options) == OK);
	Dictionary data{ { "padding", String("x").repeat(400) } };
	REQUIRE(writer.record("mcp", "started", "info", "1", data) == OK);
	REQUIRE(writer.record("mcp", "finished", "info", "1", data) == OK);
	const String oldest = writer.get_session_directory().path_join("events-0001.jsonl");
	REQUIRE(DirAccess::remove_absolute(oldest) == OK);
	REQUIRE(DirAccess::make_dir_absolute(oldest) == OK);
	_write_file(oldest.path_join("obstruction"), "keep");
	String diagnostic;
	CHECK(writer.record("mcp", "started", "info", "2", data, Dictionary(), Dictionary(), &diagnostic) != OK);
	CHECK_FALSE(diagnostic.is_empty());
	CHECK_FALSE(writer.is_open());
	CHECK(writer.record("mcp", "retry", "info", "2", Dictionary()) == ERR_UNCONFIGURED);
}

TEST_CASE("[StructuredTraceWriter] Manifest write failures are reported to callers") {
	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("structured_trace_failure", false, &error);
	REQUIRE(error == OK);
	StructuredTraceWriter::Options options;
	options.root_directory_override = temporary_directory->get_current_dir();
	StructuredTraceWriter writer;
	REQUIRE(writer.open(Dictionary(), Dictionary(), options) == OK);
	const String manifest = writer.get_session_directory().path_join("manifest.json");
	REQUIRE(DirAccess::remove_absolute(manifest) == OK);
	REQUIRE(DirAccess::make_dir_absolute(manifest) == OK);
	String diagnostic;
	CHECK(writer.record("mcp", "started", "info", "1", Dictionary(), Dictionary(), Dictionary(), &diagnostic) != OK);
	CHECK_FALSE(diagnostic.is_empty());
	CHECK(writer.record("mcp", "retry", "info", "1", Dictionary()) == ERR_UNCONFIGURED);
	REQUIRE(DirAccess::remove_absolute(manifest) == OK);
	writer.close();
}

TEST_CASE("[StructuredTraceWriter] Lifecycle and bounds reject invalid input") {
	StructuredTraceWriter writer;
	CHECK(writer.record("runtime", "event", "info", String(), Dictionary()) == ERR_UNCONFIGURED);
	StructuredTraceWriter::PayloadDescriptor payload;
	CHECK(writer.make_text_payload("name", "text/plain", "text", payload) == ERR_UNCONFIGURED);
	StructuredTraceWriter::Options options;
	options.max_sessions = StructuredTraceWriter::MAX_SESSIONS + 1;
	CHECK(writer.open(_project_metadata(), _process_metadata(), options) == ERR_INVALID_PARAMETER);

	Error error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("structured_trace_validation", false, &error);
	REQUIRE(error == OK);
	options = StructuredTraceWriter::Options();
	options.root_directory_override = temporary_directory->get_current_dir();
	REQUIRE(writer.open(_project_metadata(), _process_metadata(), options) == OK);
	CHECK(writer.record("runtime", "event", "fatal", String(), Dictionary()) == ERR_INVALID_PARAMETER);
	CHECK(writer.record(String(), "event", "info", String(), Dictionary()) == ERR_INVALID_PARAMETER);
	writer.close();
}

} // namespace TestStructuredTraceWriter
