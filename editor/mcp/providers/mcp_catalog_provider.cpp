/**************************************************************************/
/*  mcp_catalog_provider.cpp                                              */
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

#include "mcp_catalog_provider.h"

#include "core/mcp/mcp_tool_registry.h"
#include "core/object/callable_mp.h"
#include "editor/mcp/providers/mcp_catalog_tool_utils.h"
#include "editor/mcp/providers/mcp_tool_utils.h"

MCPCatalogProvider::~MCPCatalogProvider() {
	unregister_tools();
}

Error MCPCatalogProvider::register_tools(MCPToolRegistry *p_registry, String *r_error) {
	if (r_error) {
		*r_error = String();
	}
	if (!p_registry || tool_registry) {
		if (r_error) {
			*r_error = tool_registry ? "Catalog tools are already registered." : "An MCP tool registry is required.";
		}
		return tool_registry ? ERR_ALREADY_IN_USE : ERR_INVALID_PARAMETER;
	}
	const LocalVector<MCPToolUtils::ToolDescriptor> tools{
		{ "godot.catalog.search", "Find available Godot tools by name prefix or description. Returns bounded summaries; use godot.catalog.describe for exact schemas.",
				MCPCatalogToolUtils::search_schema(), MCPToolUtils::TOOL_READ_ONLY, callable_mp(this, &MCPCatalogProvider::search), MCPCatalogToolUtils::search_output_schema() },
		{ "godot.catalog.describe", "Read one complete current tool definition, then invoke its canonical name using tools/call.",
				MCPCatalogToolUtils::describe_schema(), MCPToolUtils::TOOL_READ_ONLY, callable_mp(this, &MCPCatalogProvider::describe), MCPCatalogToolUtils::describe_output_schema() },
	};
	const Error error = MCPToolUtils::register_tools(p_registry, this, tools, r_error);
	if (error != OK) {
		return error;
	}
	tool_registry = p_registry;
	return OK;
}

void MCPCatalogProvider::unregister_tools() {
	if (tool_registry) {
		tool_registry->unregister_tools_for_owner(this);
		tool_registry = nullptr;
	}
}

Dictionary MCPCatalogProvider::search(const Dictionary &p_arguments, const Dictionary &) {
	if (!tool_registry) {
		return MCPToolUtils::make_error_result("CATALOG_UNAVAILABLE", "Catalog tools are not registered.");
	}
	MCPCatalogToolUtils::SearchOptions options;
	String error;
	if (!MCPCatalogToolUtils::parse_search_options(p_arguments, options, error)) {
		return MCPToolUtils::make_error_result("INVALID_ARGUMENTS", error);
	}
	return MCPToolUtils::make_success_result(MCPCatalogToolUtils::search(*tool_registry, options));
}

Dictionary MCPCatalogProvider::describe(const Dictionary &p_arguments, const Dictionary &) {
	if (!tool_registry) {
		return MCPToolUtils::make_error_result("CATALOG_UNAVAILABLE", "Catalog tools are not registered.");
	}
	String name;
	String error;
	if (!MCPCatalogToolUtils::parse_tool_name(p_arguments, name, error)) {
		return MCPToolUtils::make_error_result("INVALID_ARGUMENTS", error);
	}
	const Dictionary definition = tool_registry->get_tool_definition(name);
	if (definition.is_empty()) {
		return MCPToolUtils::make_error_result("TOOL_NOT_FOUND", "No callable tool is registered with that name.");
	}
	Dictionary result;
	result["tool"] = definition;
	return MCPToolUtils::make_success_result(result);
}
