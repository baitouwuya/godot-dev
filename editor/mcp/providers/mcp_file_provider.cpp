/**************************************************************************/
/*  mcp_file_provider.cpp                                                 */
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

#include "mcp_file_provider.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/mcp/mcp_project_identity.h"
#include "core/mcp/mcp_tool_registry.h"
#include "core/object/callable_mp.h"
#include "core/object/script_language.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/mcp/providers/mcp_file_tool_utils.h"
#include "editor/mcp/providers/mcp_path_utils.h"
#include "editor/mcp/providers/mcp_tool_utils.h"
#include "editor/script/script_editor_base.h"
#include "editor/script/script_editor_plugin.h"

namespace {

static void _set_error(String *r_error, const String &p_message) {
	if (r_error) {
		*r_error = p_message;
	}
}

static String _file_path_for_comparison(const String &p_path) {
	if (p_path.is_empty()) {
		return String();
	}
	String path = p_path;
	if (ProjectSettings::get_singleton()) {
		path = ProjectSettings::get_singleton()->globalize_path(path);
	}
	path = path.replace_char('\\', '/').simplify_path();
	// Resolve directory aliases even when the document's backing file was deleted.
	String canonical_directory;
	if (MCPProjectIdentity::canonicalize_project_path(path.get_base_dir(), canonical_directory) == OK) {
		path = canonical_directory.path_join(path.get_file());
	}
#ifdef WINDOWS_ENABLED
	path = path.to_lower();
#endif
	return path;
}

static MCPFileProvider::ManagedDocument _find_managed_document(const String &p_resource_path) {
	const String target_path = _file_path_for_comparison(p_resource_path);
	if (EditorNode::get_singleton()) {
		const EditorData &editor_data = EditorNode::get_editor_data();
		for (int i = 0; i < editor_data.get_edited_scene_count(); i++) {
			if (_file_path_for_comparison(editor_data.get_scene_path(i)) == target_path) {
				return MCPFileProvider::DOCUMENT_SCENE;
			}
		}
	}
	ScriptEditor *script_editor = ScriptEditor::get_singleton();
	if (script_editor) {
		const Array open_editors = script_editor->call("get_open_script_editors");
		for (int i = 0; i < open_editors.size(); i++) {
			ScriptEditorBase *editor = Object::cast_to<ScriptEditorBase>(open_editors[i]);
			if (!editor) {
				continue;
			}
			const Ref<Resource> resource = editor->get_edited_resource();
			// Resource::get_path() may be cleared when the file is deleted, while
			// the editor retains its original path and unsaved document contents.
			const String path = editor->edited_file_data.path.is_empty() && resource.is_valid() ? resource->get_path() : editor->edited_file_data.path;
			if (_file_path_for_comparison(path) == target_path) {
				const Ref<Script> script = resource;
				return script.is_valid() ? MCPFileProvider::DOCUMENT_SCRIPT : MCPFileProvider::DOCUMENT_TEXT;
			}
		}
	}
	return MCPFileProvider::DOCUMENT_NONE;
}

static bool _has_same_content(const String &p_absolute_path, const String &p_content) {
	Ref<FileAccess> file = FileAccess::open(p_absolute_path, FileAccess::READ);
	const CharString content = p_content.utf8();
	if (file.is_null() || file->get_length() != uint64_t(content.length())) {
		return false;
	}
	// Compare the actual bytes, without normalizing newlines or stripping a UTF-8 BOM.
	const Vector<uint8_t> bytes = file->get_buffer(content.length());
	return file->get_error() == OK && bytes.size() == content.length() &&
			(bytes.is_empty() || memcmp(bytes.ptr(), content.ptr(), bytes.size()) == 0);
}

} // namespace

MCPFileProvider::MCPFileProvider(const String &p_project_root, ManagedDocumentLookup p_document_lookup) :
		project_root(p_project_root),
		document_lookup(p_document_lookup ? p_document_lookup : (p_project_root.is_empty() ? _find_managed_document : nullptr)) {
}

MCPFileProvider::~MCPFileProvider() {
	unregister_tools();
}

Error MCPFileProvider::register_tools(MCPToolRegistry *p_registry, String *r_error) {
	if (r_error) {
		*r_error = String();
	}
	if (!p_registry) {
		_set_error(r_error, "An MCP tool registry is required.");
		return ERR_INVALID_PARAMETER;
	}
	if (tool_registry) {
		_set_error(r_error, "File tools are already registered.");
		return ERR_ALREADY_IN_USE;
	}

	const LocalVector<MCPToolUtils::ToolDescriptor> tools{
		{ "godot.file.create", "Create a UTF-8 text file inside the current Godot project.",
				MCPFileToolUtils::create_schema(), MCPToolUtils::TOOL_DESTRUCTIVE, callable_mp(this, &MCPFileProvider::create_file), MCPFileToolUtils::create_output_schema() },
	};
	const Error err = MCPToolUtils::register_tools(p_registry, this, tools, r_error);
	if (err != OK) {
		return err;
	}

	tool_registry = p_registry;
	return OK;
}

void MCPFileProvider::unregister_tools() {
	if (!tool_registry) {
		return;
	}
	tool_registry->unregister_tools_for_owner(this);
	tool_registry = nullptr;
}

Dictionary MCPFileProvider::create_file(const Dictionary &p_arguments, const Dictionary &) {
	MCPFileToolUtils::CreateOptions options;
	String argument_error;
	if (!MCPFileToolUtils::parse_create_options(p_arguments, options, argument_error)) {
		return MCPToolUtils::make_error_result("INVALID_ARGUMENTS", argument_error);
	}

	String resource_path;
	String absolute_path;
	String path_error;
	const Error path_result = project_root.is_empty() ? MCPPathUtils::resolve_project_file_path(options.path, resource_path, absolute_path, &path_error) : MCPPathUtils::resolve_project_file_path_for_root(options.path, project_root, resource_path, absolute_path, &path_error);
	if (path_result != OK) {
		return MCPToolUtils::make_error_result("INVALID_PATH", path_error);
	}
	const String project_file = project_root.is_empty() ? String("res://project.godot") : project_root.path_join("project.godot");
	if (_file_path_for_comparison(absolute_path) == _file_path_for_comparison(project_file)) {
		return MCPToolUtils::make_error_result("PROJECT_FILE_MANAGED", "Use godot.project.apply or dedicated project configuration tools to update project.godot.");
	}

	const bool existed = FileAccess::exists(absolute_path);
	if (existed && !options.overwrite) {
		return MCPToolUtils::make_error_result("FILE_EXISTS", "File already exists: " + resource_path);
	}

	// Check even if the disk file was removed or its contents already match: an open
	// document may still contain unsaved edits or have an unresolved external change.
	const ManagedDocument managed_document = document_lookup ? document_lookup(resource_path) : DOCUMENT_NONE;
	if (managed_document != DOCUMENT_NONE) {
		String guidance;
		switch (managed_document) {
			case DOCUMENT_SCRIPT:
				guidance = "Use godot.script.edit and godot.script.save, or the native Script editor for other script languages.";
				break;
			case DOCUMENT_SCENE:
				guidance = "Use scene editing tools and godot.scene.save.";
				break;
			default:
				guidance = "Save or resolve the open text document in the native Script editor before closing it and retrying.";
				break;
		}
		return MCPToolUtils::make_error_result("FILE_MANAGED", "File is open in the editor: " + resource_path + ". " + guidance);
	}
	if (existed && _has_same_content(absolute_path, options.content)) {
		Dictionary result;
		result["path"] = resource_path;
		result["bytesWritten"] = 0;
		result["overwritten"] = false;
		return MCPToolUtils::make_success_result(result);
	}

	const String parent_directory = absolute_path.get_base_dir();
	if (!DirAccess::dir_exists_absolute(parent_directory)) {
		const Error directory_error = DirAccess::make_dir_recursive_absolute(parent_directory);
		if (directory_error != OK) {
			return MCPToolUtils::make_error_result(
					"DIRECTORY_CREATE_FAILED", "Could not create parent directory for: " + resource_path);
		}
	}

	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(absolute_path, FileAccess::WRITE, &open_error);
	if (file.is_null() || open_error != OK) {
		return MCPToolUtils::make_error_result("FILE_OPEN_FAILED", "Could not open file for writing: " + resource_path);
	}

	if (!file->store_string(options.content)) {
		return MCPToolUtils::make_error_result("FILE_WRITE_FAILED", "Could not write file: " + resource_path);
	}
	file->flush();
	file.unref();
	if (project_root.is_empty() && EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan_changes();
	}

	Dictionary result;
	result["path"] = resource_path;
	result["bytesWritten"] = options.content.utf8().length();
	result["overwritten"] = existed;
	return MCPToolUtils::make_success_result(result);
}
