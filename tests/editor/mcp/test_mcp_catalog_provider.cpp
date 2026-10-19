/**************************************************************************/
/*  test_mcp_catalog_provider.cpp                                         */
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

#include "core/mcp/mcp_tool_registry.h"
#include "core/object/callable_mp.h"
#include "editor/mcp/providers/mcp_catalog_provider.h"
#include "editor/mcp/providers/mcp_tool_utils.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mcp_catalog_provider);

namespace TestMCPCatalogProvider {

class ToolProvider : public Object {
public:
	int call_count = 0;

	Dictionary call(const Dictionary &, const Dictionary &) {
		call_count++;
		return MCPToolUtils::make_success_result(Dictionary());
	}
};

static Dictionary _definition(const String &p_name, const String &p_description = "Inspect a test node.") {
	return MCPToolUtils::make_tool_definition(p_name, p_description, MCPToolUtils::make_object_schema(), MCPToolUtils::TOOL_READ_ONLY, MCPToolUtils::make_object_schema());
}

static Dictionary _success(MCPToolRegistry &p_registry, const String &p_name, const Dictionary &p_arguments = Dictionary()) {
	const MCPToolRegistry::CallResult result = p_registry.call_tool(p_name, p_arguments, MCPToolCallContext());
	INFO(result.message);
	INFO(result.result);
	REQUIRE(result.status == MCPToolRegistry::CALL_OK);
	REQUIRE_FALSE(bool(result.result.get("isError", false)));
	return result.result.get("structuredContent", Dictionary());
}

static String _error_code(const Dictionary &p_result) {
	const Dictionary content = p_result.get("structuredContent", Dictionary());
	return Dictionary(content.get("error", Dictionary())).get("code", String());
}

TEST_CASE("[MCP][Catalog] Search filters metadata and describe preserves exact definitions") {
	MCPToolRegistry registry;
	ToolProvider target;
	MCPCatalogProvider catalog;
	const String long_description = String("x").repeat(300) + " needle";
	const Dictionary definition = _definition("godot.node.test", long_description);
	REQUIRE(registry.register_tool(definition, callable_mp(&target, &ToolProvider::call), &target) == OK);
	REQUIRE(registry.register_tool(_definition("godot.runtime.node.test"), callable_mp(&target, &ToolProvider::call), &target) == OK);
	REQUIRE(catalog.register_tools(&registry) == OK);

	Dictionary arguments;
	arguments["prefix"] = "godot.node.";
	arguments["query"] = "NEEDLE";
	const Dictionary search = _success(registry, "godot.catalog.search", arguments);
	CHECK(int(search["total"]) == 1);
	CHECK(int(search["nextOffset"]) == -1);
	const Array matches = search["tools"];
	REQUIRE(matches.size() == 1);
	const Dictionary match = matches[0];
	CHECK(String(match["name"]) == "godot.node.test");
	CHECK(String(match["description"]).length() == 256);
	CHECK_FALSE(match.has("inputSchema"));
	CHECK_FALSE(match.has("outputSchema"));
	CHECK(Dictionary(match["annotations"]) == Dictionary(definition["annotations"]));

	arguments["prefix"] = "GODOT.node.";
	CHECK(int(_success(registry, "godot.catalog.search", arguments)["total"]) == 0);
	arguments["prefix"] = "godot.runtime.";
	arguments["query"] = "NODE.TEST";
	CHECK(int(_success(registry, "godot.catalog.search", arguments)["total"]) == 1);

	Dictionary name;
	name["name"] = "godot.node.test";
	Dictionary described = _success(registry, "godot.catalog.describe", name)["tool"];
	CHECK(described == definition);
	CHECK(String(described["description"]) == long_description);
	Dictionary input_schema = described["inputSchema"];
	input_schema["additionalProperties"] = true;
	CHECK(Dictionary(_success(registry, "godot.catalog.describe", name)["tool"]) == definition);
	CHECK(target.call_count == 0);
	_success(registry, "godot.node.test");
	CHECK(target.call_count == 1);

	name["name"] = "godot.catalog.describe";
	CHECK(Dictionary(_success(registry, "godot.catalog.describe", name)["tool"]) == registry.get_tool_definition("godot.catalog.describe"));
	registry.unregister_tools_for_owner(&target);
}

TEST_CASE("[MCP][Catalog] Empty search exhaustively pages the live registry") {
	MCPToolRegistry registry;
	ToolProvider target;
	MCPCatalogProvider catalog;
	for (int i = 0; i < 53; i++) {
		REQUIRE(registry.register_tool(_definition("godot.test.tool_" + itos(i)), callable_mp(&target, &ToolProvider::call), &target) == OK);
	}
	REQUIRE(catalog.register_tools(&registry) == OK);
	const PackedStringArray expected = registry.get_tool_names();
	const Dictionary first = _success(registry, "godot.catalog.search");
	CHECK(Array(first["tools"]).size() == 20);
	CHECK(int(first["total"]) == expected.size());
	CHECK(int(first["nextOffset"]) == 20);

	PackedStringArray observed;
	Dictionary arguments;
	arguments["limit"] = 50;
	int offset = 0;
	for (int page = 0; page < 2; page++) {
		arguments["offset"] = offset;
		const Dictionary result = _success(registry, "godot.catalog.search", arguments);
		const Array matches = result["tools"];
		for (const Variant &match_value : matches) {
			const Dictionary match = match_value;
			observed.push_back(match["name"]);
		}
		offset = result["nextOffset"];
	}
	CHECK(offset == -1);
	CHECK(observed == expected);
	arguments["offset"] = INT32_MAX;
	const Dictionary exhausted = _success(registry, "godot.catalog.search", arguments);
	CHECK(Array(exhausted["tools"]).is_empty());
	CHECK(int(exhausted["nextOffset"]) == -1);
	CHECK(registry.unregister_tool("godot.test.tool_0"));
	arguments["offset"] = 0;
	const Dictionary refreshed = _success(registry, "godot.catalog.search", arguments);
	CHECK(int(refreshed["total"]) == expected.size() - 1);
	CHECK(String(Dictionary(Array(refreshed["tools"])[0])["name"]) == "godot.test.tool_1");
	registry.unregister_tools_for_owner(&target);
}

TEST_CASE("[MCP][Catalog] Strict schemas reject malformed and out-of-range arguments") {
	MCPToolRegistry registry;
	MCPCatalogProvider catalog;
	REQUIRE(catalog.register_tools(&registry) == OK);
	for (const String &name : PackedStringArray{ "godot.catalog.search", "godot.catalog.describe" }) {
		const Dictionary definition = registry.get_tool_definition(name);
		CHECK_FALSE(bool(Dictionary(definition["inputSchema"])["additionalProperties"]));
		CHECK(String(Dictionary(definition["outputSchema"])["type"]) == "object");
		const Dictionary annotations = definition["annotations"];
		CHECK(bool(annotations["readOnlyHint"]));
		CHECK(bool(annotations["idempotentHint"]));
		CHECK_FALSE(bool(annotations["destructiveHint"]));
		CHECK_FALSE(bool(annotations["openWorldHint"]));
	}
	const Array invalid_values{ -1, 0, 51, 1.5, true, "20" };
	for (const Variant &value : invalid_values) {
		Dictionary arguments;
		arguments["limit"] = value;
		const MCPToolRegistry::CallResult result = registry.call_tool("godot.catalog.search", arguments, MCPToolCallContext());
		CHECK(result.status == MCPToolRegistry::CALL_OK);
		CHECK(_error_code(result.result) == "INVALID_ARGUMENTS");
	}
	for (const String &field : PackedStringArray{ "query", "prefix", "offset", "unexpected" }) {
		Dictionary arguments;
		arguments[field] = field == "offset" ? Variant(-1) : Variant(String("x").repeat(129));
		const MCPToolRegistry::CallResult result = registry.call_tool("godot.catalog.search", arguments, MCPToolCallContext());
		CHECK(result.status == MCPToolRegistry::CALL_OK);
		CHECK(_error_code(result.result) == "INVALID_ARGUMENTS");
	}
	for (const Variant &value : Array{ Variant(int64_t(INT32_MAX) + 1), Variant(0.5), Variant(true) }) {
		Dictionary arguments;
		arguments["offset"] = value;
		CHECK(_error_code(registry.call_tool("godot.catalog.search", arguments, MCPToolCallContext()).result) == "INVALID_ARGUMENTS");
	}
	Dictionary wrong_type;
	wrong_type["query"] = 42;
	CHECK(_error_code(registry.call_tool("godot.catalog.search", wrong_type, MCPToolCallContext()).result) == "INVALID_ARGUMENTS");
	CHECK(_error_code(registry.call_tool("godot.catalog.describe", Dictionary(), MCPToolCallContext()).result) == "INVALID_ARGUMENTS");
	for (const Variant &value : Array{ Variant(), Variant(""), Variant(42) }) {
		Dictionary arguments;
		arguments["name"] = value;
		CHECK(_error_code(registry.call_tool("godot.catalog.describe", arguments, MCPToolCallContext()).result) == "INVALID_ARGUMENTS");
	}
	Dictionary missing;
	missing["name"] = "godot.missing.tool";
	const MCPToolRegistry::CallResult result = registry.call_tool("godot.catalog.describe", missing, MCPToolCallContext());
	CHECK(result.status == MCPToolRegistry::CALL_OK);
	CHECK(_error_code(result.result) == "TOOL_NOT_FOUND");
	CHECK(registry.call_tool("godot.missing.tool", Dictionary(), MCPToolCallContext()).status == MCPToolRegistry::CALL_TOOL_NOT_FOUND);
	missing["unexpected"] = true;
	CHECK(_error_code(registry.call_tool("godot.catalog.describe", missing, MCPToolCallContext()).result) == "INVALID_ARGUMENTS");
}

TEST_CASE("[MCP][Catalog] Registration rollback and cleanup preserve unrelated tools") {
	MCPToolRegistry registry;
	ToolProvider target;
	MCPCatalogProvider *catalog = memnew(MCPCatalogProvider);
	CHECK(catalog->register_tools(nullptr) == ERR_INVALID_PARAMETER);
	CHECK(_error_code(catalog->search(Dictionary(), Dictionary())) == "CATALOG_UNAVAILABLE");
	CHECK(_error_code(catalog->describe(Dictionary(), Dictionary())) == "CATALOG_UNAVAILABLE");
	REQUIRE(registry.register_tool(_definition("godot.catalog.describe"), callable_mp(&target, &ToolProvider::call), &target) == OK);
	CHECK(catalog->register_tools(&registry) == ERR_ALREADY_EXISTS);
	CHECK_FALSE(registry.has_tool("godot.catalog.search"));
	CHECK(registry.has_tool("godot.catalog.describe"));
	CHECK(registry.unregister_tool("godot.catalog.describe"));
	REQUIRE(catalog->register_tools(&registry) == OK);
	CHECK(catalog->register_tools(&registry) == ERR_ALREADY_IN_USE);

	ToolProvider *expired = memnew(ToolProvider);
	REQUIRE(registry.register_tool(_definition("godot.test.expired"), callable_mp(expired, &ToolProvider::call), expired) == OK);
	memdelete(expired);
	Dictionary name;
	name["name"] = "godot.test.expired";
	CHECK(_error_code(registry.call_tool("godot.catalog.describe", name, MCPToolCallContext()).result) == "TOOL_NOT_FOUND");
	CHECK(int(_success(registry, "godot.catalog.search")["total"]) == 2);

	REQUIRE(registry.register_tool(_definition("godot.test.retained"), callable_mp(&target, &ToolProvider::call), &target) == OK);
	catalog->on_session_removed("test-session");
	catalog->shutdown();
	CHECK(registry.has_tool("godot.catalog.search"));
	catalog->unregister_tools();
	catalog->unregister_tools();
	CHECK(registry.has_tool("godot.test.retained"));
	CHECK_FALSE(registry.has_tool("godot.catalog.search"));
	REQUIRE(catalog->register_tools(&registry) == OK);
	memdelete(catalog);
	CHECK(registry.has_tool("godot.test.retained"));
	CHECK_FALSE(registry.has_tool("godot.catalog.describe"));
	registry.unregister_tools_for_owner(&target);
}

} // namespace TestMCPCatalogProvider
