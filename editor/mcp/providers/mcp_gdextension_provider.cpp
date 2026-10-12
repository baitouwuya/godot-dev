/**************************************************************************/
/*  mcp_gdextension_provider.cpp                                          */
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

#include "mcp_gdextension_provider.h"

#include "core/mcp/mcp_tool_registry.h"
#include "core/object/callable_mp.h"
#include "editor/mcp/providers/mcp_gdextension_tool_utils.h"
#include "editor/mcp/providers/mcp_tool_utils.h"

MCPGDExtensionProvider::~MCPGDExtensionProvider() {
	shutdown();
	unregister_tools();
}

Error MCPGDExtensionProvider::register_tools(MCPToolRegistry *p_registry, String *r_error) {
	if (r_error) {
		*r_error = String();
	}
	if (!p_registry) {
		if (r_error) {
			*r_error = "An MCP tool registry is required.";
		}
		return ERR_INVALID_PARAMETER;
	}
	if (tool_registry) {
		if (r_error) {
			*r_error = "GDExtension tools are already registered.";
		}
		return ERR_ALREADY_IN_USE;
	}

	const LocalVector<MCPToolUtils::ToolDescriptor> tools{
		{ "godot.gdextension.build", "Build the selected project GDExtension with a registered godot-cpp SCons profile.",
				MCPGDExtensionToolUtils::build_schema(), MCPToolUtils::TOOL_DESTRUCTIVE, callable_mp(this, &MCPGDExtensionProvider::build), MCPGDExtensionToolUtils::build_output_schema() },
		{ "godot.gdextension.build_status", "Get bounded output, diagnostics, artifact state, and reload state for a GDExtension build job.",
				MCPGDExtensionToolUtils::build_status_schema(), MCPToolUtils::TOOL_READ_ONLY, callable_mp(this, &MCPGDExtensionProvider::build_status), MCPGDExtensionToolUtils::build_status_output_schema() },
		{ "godot.gdextension.build_cancel", "Cancel a running GDExtension build and restore the previous extension library.",
				MCPGDExtensionToolUtils::build_cancel_schema(), MCPToolUtils::TOOL_DESTRUCTIVE, callable_mp(this, &MCPGDExtensionProvider::build_cancel), MCPGDExtensionToolUtils::build_cancel_output_schema() },
	};
	const Error error = MCPToolUtils::register_tools(p_registry, this, tools, r_error);
	if (error != OK) {
		return error;
	}
	tool_registry = p_registry;
	return OK;
}

void MCPGDExtensionProvider::unregister_tools() {
	if (!tool_registry) {
		return;
	}
	tool_registry->unregister_tools_for_owner(this);
	tool_registry = nullptr;
}

Dictionary MCPGDExtensionProvider::build(const Dictionary &p_arguments, const Dictionary &p_context) {
	return build_service.build(p_arguments, p_context);
}

Dictionary MCPGDExtensionProvider::build_status(const Dictionary &p_arguments, const Dictionary &p_context) {
	return build_service.get_status(p_arguments, p_context);
}

Dictionary MCPGDExtensionProvider::build_cancel(const Dictionary &p_arguments, const Dictionary &p_context) {
	return build_service.cancel(p_arguments, p_context);
}
