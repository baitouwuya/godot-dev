/**************************************************************************/
/*  mcp_resource_provider.h                                               */
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

#include "core/object/object.h"
#include "core/variant/dictionary.h"
#include "editor/mcp/mcp_editor_feature.h"
#include "editor/mcp/providers/mcp_resource_edit_service.h"
#include "editor/mcp/providers/mcp_resource_import_service.h"

class MCPToolRegistry;

class MCPResourceProvider : public Object, public MCPEditorFeature {
public:
	using ImportFunction = MCPResourceImportService::ImportFunction;
	using ScanFunction = MCPResourceImportService::ScanFunction;

	explicit MCPResourceProvider(const String &p_project_root = String(), ImportFunction p_import_function = nullptr, ScanFunction p_scan_function = nullptr);
	~MCPResourceProvider();

	Error register_tools(MCPToolRegistry *p_registry, String *r_error = nullptr) override;
	void unregister_tools() override;

	Dictionary import_options(const Dictionary &p_arguments, const Dictionary &p_context);
	Dictionary import_resource(const Dictionary &p_arguments, const Dictionary &p_context);
	Dictionary get_properties(const Dictionary &p_arguments, const Dictionary &p_context);
	Dictionary set_property(const Dictionary &p_arguments, const Dictionary &p_context);
	Dictionary save(const Dictionary &p_arguments, const Dictionary &p_context);

private:
	MCPToolRegistry *tool_registry = nullptr;
	MCPResourceEditService edit_service;
	MCPResourceImportService import_service;
};
