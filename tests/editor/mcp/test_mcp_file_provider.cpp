/**************************************************************************/
/*  test_mcp_file_provider.cpp                                            */
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
#include "core/mcp/mcp_tool_registry.h"
#include "core/os/os.h"
#include "editor/mcp/providers/mcp_file_provider.h"
#include "editor/mcp/providers/mcp_file_tool_utils.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mcp_file_provider);

namespace TestMCPFileProvider {

class ProjectFileCleanup {
	String absolute_path;

public:
	explicit ProjectFileCleanup(const String &p_absolute_path) :
			absolute_path(p_absolute_path) {
		remove();
	}

	~ProjectFileCleanup() {
		remove();
	}

	void remove() {
		if (!absolute_path.is_empty() && FileAccess::exists(absolute_path)) {
			DirAccess::remove_absolute(absolute_path);
		}
	}
};

static MCPFileProvider::ManagedDocument _find_test_document(const String &p_resource_path) {
	if (p_resource_path == "res://open.gd") {
		return MCPFileProvider::DOCUMENT_SCRIPT;
	}
	if (p_resource_path == "res://open.txt") {
		return MCPFileProvider::DOCUMENT_TEXT;
	}
	if (p_resource_path == "res://open.tscn") {
		return MCPFileProvider::DOCUMENT_SCENE;
	}
	return MCPFileProvider::DOCUMENT_NONE;
}

TEST_CASE("[MCP][Provider] File create is UTF-8 safe and overwrite-aware") {
	REQUIRE(OS::get_singleton() != nullptr);
	Error temp_error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_file_provider", false, &temp_error);
	REQUIRE(temp_error == OK);
	REQUIRE(temporary_directory->make_dir("project") == OK);
	const String project_root = temporary_directory->get_current_dir().path_join("project");

	const String resource_path = "res://.godot/mcp_tests/file_provider_" +
			String::num_int64(OS::get_singleton()->get_process_id()) + ".txt";
	const String absolute_path = project_root.path_join(resource_path.trim_prefix("res://"));
	ProjectFileCleanup cleanup(absolute_path);

	MCPToolRegistry registry;
	MCPFileProvider *provider = memnew(MCPFileProvider(project_root));
	REQUIRE(provider->register_tools(&registry) == OK);
	CHECK(registry.has_tool("godot.file.create"));
	const Array definitions = registry.get_tool_definitions();
	REQUIRE(definitions.size() == 1);
	const Dictionary input_schema = Dictionary(definitions[0]).get("inputSchema", Dictionary());
	const Dictionary input_properties = input_schema.get("properties", Dictionary());
	CHECK(PackedStringArray(input_schema.get("required", PackedStringArray())).has("path"));
	CHECK(PackedStringArray(input_schema.get("required", PackedStringArray())).has("content"));
	CHECK_FALSE(bool(Dictionary(input_properties.get("overwrite", Dictionary())).get("default", true)));
	const Dictionary output_schema = Dictionary(definitions[0]).get("outputSchema", Dictionary());
	CHECK(Dictionary(output_schema.get("properties", Dictionary())).has("bytesWritten"));
	CHECK(PackedStringArray(output_schema.get("required", PackedStringArray())).has("overwritten"));
	CHECK_FALSE(bool(output_schema.get("additionalProperties", true)));

	const String first_content = "hello " + String::chr(0x4E16) + String::chr(0x754C);
	Dictionary arguments;
	arguments["path"] = resource_path;
	arguments["content"] = first_content;

	MCPToolCallContext context;
	MCPToolRegistry::CallResult call_result = registry.call_tool("godot.file.create", arguments, context);
	REQUIRE(call_result.status == MCPToolRegistry::CALL_OK);
	CHECK_FALSE(bool(call_result.result.get("isError", false)));

	const Dictionary created = call_result.result.get("structuredContent", Dictionary());
	CHECK(created.get("path", String()) == resource_path);
	CHECK(int(created.get("bytesWritten", 0)) == first_content.utf8().length());
	CHECK_FALSE(bool(created.get("overwritten", true)));

	Error read_error = OK;
	CHECK(FileAccess::get_file_as_string(absolute_path, &read_error) == first_content);
	CHECK(read_error == OK);

	call_result = registry.call_tool("godot.file.create", arguments, context);
	REQUIRE(call_result.status == MCPToolRegistry::CALL_OK);
	CHECK(bool(call_result.result.get("isError", false)));
	const Dictionary exists_error =
			Dictionary(call_result.result.get("structuredContent", Dictionary())).get("error", Dictionary());
	CHECK(exists_error.get("code", String()) == "FILE_EXISTS");

	const String replacement_content = "replacement";
	arguments["content"] = replacement_content;
	arguments["overwrite"] = true;
	call_result = registry.call_tool("godot.file.create", arguments, context);
	REQUIRE(call_result.status == MCPToolRegistry::CALL_OK);
	CHECK_FALSE(bool(call_result.result.get("isError", false)));
	CHECK(bool(Dictionary(call_result.result.get("structuredContent", Dictionary())).get("overwritten", false)));
	CHECK(FileAccess::get_file_as_string(absolute_path) == replacement_content);
	provider->unregister_tools();
	CHECK(registry.call_tool("godot.file.create", arguments, context).status == MCPToolRegistry::CALL_TOOL_NOT_FOUND);
	memdelete(provider);
}

TEST_CASE("[MCP][Provider] File create contract parses arguments consistently") {
	MCPFileToolUtils::CreateOptions options;
	String error_message;
	Dictionary arguments;
	arguments["path"] = "res://notes.txt";
	arguments["content"] = "content";
	CHECK(MCPFileToolUtils::parse_create_options(arguments, options, error_message));
	CHECK(options.path == "res://notes.txt");
	CHECK(options.content == "content");
	CHECK_FALSE(options.overwrite);
	CHECK(error_message.is_empty());

	arguments["overwrite"] = true;
	CHECK(MCPFileToolUtils::parse_create_options(arguments, options, error_message));
	CHECK(options.overwrite);
	arguments["overwrite"] = 1;
	CHECK_FALSE(MCPFileToolUtils::parse_create_options(arguments, options, error_message));
	CHECK(error_message.contains("overwrite"));
	arguments.erase("overwrite");
	arguments["content"] = false;
	CHECK_FALSE(MCPFileToolUtils::parse_create_options(arguments, options, error_message));
	arguments.erase("content");
	CHECK_FALSE(MCPFileToolUtils::parse_create_options(arguments, options, error_message));
}

TEST_CASE("[MCP][Provider] File overwrite skips byte-identical UTF-8 content") {
	Error temp_error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_file_identical", false, &temp_error);
	REQUIRE(temp_error == OK);
	REQUIRE(temporary_directory.is_valid());
	const String project_root = temporary_directory->get_current_dir();
	const String absolute_path = project_root.path_join("same.txt");
	MCPFileProvider *provider = memnew(MCPFileProvider(project_root));

	const String contents[] = { "", "hello " + String::chr(0x4E16) + "\r\n", String::chr(0xFEFF) + "hello\n" };
	for (const String &content : contents) {
		Dictionary arguments;
		arguments["path"] = "res://same.txt";
		arguments["content"] = content;
		arguments["overwrite"] = true;
		REQUIRE_FALSE(bool(provider->create_file(arguments, Dictionary()).get("isError", false)));
		const uint64_t modified_time = FileAccess::get_modified_time(absolute_path);

		const Dictionary result = provider->create_file(arguments, Dictionary());
		REQUIRE_FALSE(bool(result.get("isError", false)));
		const Dictionary structured = result.get("structuredContent", Dictionary());
		CHECK(int(structured.get("bytesWritten", -1)) == 0);
		CHECK_FALSE(bool(structured.get("overwritten", true)));
		CHECK(FileAccess::get_modified_time(absolute_path) == modified_time);
	}

	// A BOM or CRLF difference is a real byte change, even if decoded text is similar.
	Dictionary arguments;
	arguments["path"] = "res://same.txt";
	arguments["content"] = "hello\n";
	arguments["overwrite"] = true;
	Dictionary result = provider->create_file(arguments, Dictionary());
	REQUIRE_FALSE(bool(result.get("isError", false)));
	Dictionary structured = result.get("structuredContent", Dictionary());
	CHECK(int(structured.get("bytesWritten", 0)) == 6);
	CHECK(bool(structured.get("overwritten", false)));
	arguments["content"] = "hello\r\n";
	result = provider->create_file(arguments, Dictionary());
	REQUIRE_FALSE(bool(result.get("isError", false)));
	structured = result.get("structuredContent", Dictionary());
	CHECK(int(structured.get("bytesWritten", 0)) == 7);
	CHECK(bool(structured.get("overwritten", false)));
	CHECK(FileAccess::get_file_as_bytes(absolute_path) == String("hello\r\n").to_utf8_buffer());
	memdelete(provider);
}

TEST_CASE("[MCP][Provider] File create protects open documents before identical-content checks") {
	Error temp_error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_file_managed", false, &temp_error);
	REQUIRE(temp_error == OK);
	REQUIRE(temporary_directory.is_valid());
	const String project_root = temporary_directory->get_current_dir();
	MCPFileProvider *provider = memnew(MCPFileProvider(project_root, _find_test_document));
	const String paths[] = { "res://open.gd", "res://open.txt", "res://open.tscn" };
	const String guidance[] = { "godot.script.save", "native Script editor", "godot.scene.save" };
	for (int i = 0; i < 3; i++) {
		const String absolute_path = project_root.path_join(paths[i].trim_prefix("res://"));
		Ref<FileAccess> file = FileAccess::open(absolute_path, FileAccess::WRITE);
		REQUIRE(file.is_valid());
		REQUIRE(file->store_string("disk content"));
		file.unref();
		const uint64_t modified_time = FileAccess::get_modified_time(absolute_path);
		Dictionary arguments;
		arguments["path"] = paths[i];
		arguments["overwrite"] = true;
		const String replacements[] = { "different content", "disk content" };
		for (const String &replacement : replacements) {
			arguments["content"] = replacement;
			const Dictionary result = provider->create_file(arguments, Dictionary());
			REQUIRE(bool(result.get("isError", false)));
			const Dictionary error = Dictionary(result.get("structuredContent", Dictionary())).get("error", Dictionary());
			CHECK(error.get("code", String()) == "FILE_MANAGED");
			CHECK(String(error.get("message", String())).contains(guidance[i]));
			CHECK(FileAccess::get_file_as_string(absolute_path) == "disk content");
			CHECK(FileAccess::get_modified_time(absolute_path) == modified_time);
		}

		// A deleted disk file can still have a dirty or externally changed editor buffer.
		REQUIRE(DirAccess::remove_absolute(absolute_path) == OK);
		arguments["overwrite"] = false;
		const Dictionary result = provider->create_file(arguments, Dictionary());
		CHECK(bool(result.get("isError", false)));
		CHECK_FALSE(FileAccess::exists(absolute_path));
	}

	Dictionary arguments;
	arguments["path"] = "res://closed.txt";
	arguments["content"] = "new file";
	CHECK_FALSE(bool(provider->create_file(arguments, Dictionary()).get("isError", false)));
	CHECK(FileAccess::get_file_as_string(project_root.path_join("closed.txt")) == "new file");
	memdelete(provider);
}

TEST_CASE("[MCP][Provider] File create rejects traversal and malformed arguments before writing") {
	MCPToolRegistry registry;
	MCPFileProvider *provider = memnew(MCPFileProvider);
	REQUIRE(provider->register_tools(&registry) == OK);

	MCPToolCallContext context;

	Dictionary arguments;
	arguments["path"] = "res://../outside.txt";
	arguments["content"] = "blocked";
	MCPToolRegistry::CallResult call_result = registry.call_tool("godot.file.create", arguments, context);
	REQUIRE(call_result.status == MCPToolRegistry::CALL_OK);
	CHECK(bool(call_result.result.get("isError", false)));

	arguments["path"] = "res://safe.txt";
	arguments["extra"] = true;
	call_result = registry.call_tool("godot.file.create", arguments, context);
	REQUIRE(call_result.status == MCPToolRegistry::CALL_OK);
	CHECK(bool(call_result.result.get("isError", false)));
	const Dictionary malformed_error = Dictionary(call_result.result.get("structuredContent", Dictionary())).get("error", Dictionary());
	CHECK(malformed_error.get("code", String()) == "INVALID_ARGUMENTS");
	CHECK(Dictionary(malformed_error.get("details", Dictionary())).get("keyword", String()) == "additionalProperties");

	provider->unregister_tools();
	memdelete(provider);
}

TEST_CASE("[MCP][Provider] File create keeps project.godot under project tool control") {
	REQUIRE(OS::get_singleton() != nullptr);
	Error temp_error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_file_provider_project_file", false, &temp_error);
	REQUIRE(temp_error == OK);
	REQUIRE(temporary_directory->make_dir("project") == OK);
	const String project_root = temporary_directory->get_current_dir().path_join("project");
	const String project_file = project_root.path_join("project.godot");

	MCPToolRegistry registry;
	MCPFileProvider *provider = memnew(MCPFileProvider(project_root));
	REQUIRE(provider->register_tools(&registry) == OK);

	MCPToolCallContext context;
	Dictionary arguments;
	arguments["path"] = "res://project.godot";
	arguments["content"] = "[application]\nconfig/name=\"blocked\"\n";

	MCPToolRegistry::CallResult call_result = registry.call_tool("godot.file.create", arguments, context);
	REQUIRE(call_result.status == MCPToolRegistry::CALL_OK);
	CHECK(bool(call_result.result.get("isError", false)));
	Dictionary error = Dictionary(call_result.result.get("structuredContent", Dictionary())).get("error", Dictionary());
	CHECK(error.get("code", String()) == "PROJECT_FILE_MANAGED");
	CHECK(String(error.get("message", String())).contains("godot.project.apply"));
	CHECK(String(error.get("message", String())).contains("dedicated project configuration tools"));
	CHECK_FALSE(FileAccess::exists(project_file));

	const String original_content = "[application]\nconfig/name=\"original\"\n";
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(project_file, FileAccess::WRITE, &open_error);
	REQUIRE(open_error == OK);
	REQUIRE(file.is_valid());
	REQUIRE(file->store_string(original_content));
	file->flush();
	file.unref();

	arguments["content"] = "[application]\nconfig/name=\"replacement\"\n";
	arguments["overwrite"] = true;
	call_result = registry.call_tool("godot.file.create", arguments, context);
	REQUIRE(call_result.status == MCPToolRegistry::CALL_OK);
	CHECK(bool(call_result.result.get("isError", false)));
	error = Dictionary(call_result.result.get("structuredContent", Dictionary())).get("error", Dictionary());
	CHECK(error.get("code", String()) == "PROJECT_FILE_MANAGED");
	CHECK(FileAccess::get_file_as_string(project_file) == original_content);

	provider->unregister_tools();
	memdelete(provider);
}

TEST_CASE("[MCP][Provider] File create cannot overwrite project.godot through a directory alias") {
	Error temp_error = OK;
	Ref<DirAccess> temporary_directory = DirAccess::create_temp("mcp_file_project_alias", false, &temp_error);
	REQUIRE(temp_error == OK);
	REQUIRE(temporary_directory.is_valid());
	REQUIRE(temporary_directory->make_dir("project") == OK);
	const String project_root = temporary_directory->get_current_dir().path_join("project");
	const String alias_path = project_root.path_join("alias");
	if (temporary_directory->create_link(project_root, alias_path) != OK) {
		return;
	}
	const String project_file = project_root.path_join("project.godot");
	Ref<FileAccess> file = FileAccess::open(project_file, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	REQUIRE(file->store_string("original project settings"));
	file.unref();

	MCPFileProvider *provider = memnew(MCPFileProvider(project_root));
	Dictionary arguments;
	arguments["path"] = "res://alias/project.godot";
	arguments["content"] = "replacement";
	arguments["overwrite"] = true;
	const Dictionary result = provider->create_file(arguments, Dictionary());
	CHECK(bool(result.get("isError", false)));
	const Dictionary error = Dictionary(result.get("structuredContent", Dictionary())).get("error", Dictionary());
	CHECK(error.get("code", String()) == "PROJECT_FILE_MANAGED");
	CHECK(FileAccess::get_file_as_string(project_file) == "original project settings");
	memdelete(provider);
	CHECK(DirAccess::remove_absolute(alias_path) == OK);
}

} // namespace TestMCPFileProvider
