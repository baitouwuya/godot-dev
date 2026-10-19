/**************************************************************************/
/*  mcp_catalog_tool_utils.cpp                                            */
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

#include "mcp_catalog_tool_utils.h"

#include "core/mcp/mcp_json_schema_validator.h"
#include "core/mcp/mcp_tool_registry.h"
#include "editor/mcp/providers/mcp_tool_utils.h"

namespace MCPCatalogToolUtils {

static constexpr int MAX_SEARCH_RESULTS = 50;
static constexpr int MAX_FILTER_LENGTH = 128;
static constexpr int MAX_DESCRIPTION_LENGTH = 256;

static Dictionary _integer_schema(const String &p_description, int p_minimum) {
	Dictionary schema = MCPToolUtils::make_property_schema("integer", p_description);
	schema["minimum"] = p_minimum;
	return schema;
}

static Dictionary _open_object_schema() {
	return MCPToolUtils::make_object_schema(Dictionary(), PackedStringArray(), true);
}

static bool _validate_arguments(const Dictionary &p_arguments, const Dictionary &p_schema, String &r_error) {
	MCPJSONSchemaValidator::ValidationError error;
	const bool valid = MCPJSONSchemaValidator::validate(p_arguments, p_schema, error);
	r_error = valid ? String() : error.message;
	return valid;
}

Dictionary search_schema() {
	Dictionary properties;
	Dictionary query = MCPToolUtils::make_property_schema("string", "Case-insensitive substring of a tool name or description. Empty browses all tools.");
	query["maxLength"] = MAX_FILTER_LENGTH;
	query["default"] = "";
	properties["query"] = query;
	Dictionary prefix = MCPToolUtils::make_property_schema("string", "Case-sensitive tool-name prefix, for example godot.runtime. Empty searches all families.");
	prefix["maxLength"] = MAX_FILTER_LENGTH;
	prefix["default"] = "";
	properties["prefix"] = prefix;
	Dictionary offset = _integer_schema("Zero-based match offset. Pages reflect the live registry; restart enumeration if registrations change.", 0);
	offset["maximum"] = INT32_MAX;
	offset["default"] = 0;
	properties["offset"] = offset;
	Dictionary limit = _integer_schema("Maximum summaries returned. Use describe to fetch one complete tool schema.", 1);
	limit["maximum"] = MAX_SEARCH_RESULTS;
	limit["default"] = SearchOptions().limit;
	properties["limit"] = limit;
	return MCPToolUtils::make_object_schema(properties);
}

Dictionary describe_schema() {
	Dictionary properties;
	Dictionary name = MCPToolUtils::make_property_schema("string", "Exact registered tool name. Call the returned tool by its canonical name using tools/call.");
	name["minLength"] = 1;
	properties["name"] = name;
	return MCPToolUtils::make_object_schema(properties, PackedStringArray{ "name" });
}

Dictionary search_output_schema() {
	Dictionary summary_properties;
	summary_properties["name"] = MCPToolUtils::make_property_schema("string", "Canonical tool name.");
	Dictionary description = MCPToolUtils::make_property_schema("string", "Description preview; describe returns the complete definition.");
	description["maxLength"] = MAX_DESCRIPTION_LENGTH;
	summary_properties["description"] = description;
	summary_properties["annotations"] = _open_object_schema();
	Dictionary summaries = MCPToolUtils::make_property_schema("array", "Matching summaries in registration order, without input or output schemas.");
	summaries["items"] = MCPToolUtils::make_object_schema(summary_properties, PackedStringArray{ "name", "description", "annotations" });
	summaries["maxItems"] = MAX_SEARCH_RESULTS;
	Dictionary properties;
	properties["tools"] = summaries;
	properties["total"] = _integer_schema("Total matches before pagination.", 0);
	properties["nextOffset"] = _integer_schema("Next match offset, or -1 when exhausted.", -1);
	return MCPToolUtils::make_object_schema(properties, PackedStringArray{ "tools", "total", "nextOffset" });
}

Dictionary describe_output_schema() {
	Dictionary definition_properties;
	definition_properties["name"] = MCPToolUtils::make_property_schema("string", "Canonical tool name.");
	definition_properties["description"] = MCPToolUtils::make_property_schema("string", "Complete tool description.");
	definition_properties["inputSchema"] = _open_object_schema();
	definition_properties["outputSchema"] = _open_object_schema();
	definition_properties["annotations"] = _open_object_schema();
	Dictionary properties;
	properties["tool"] = MCPToolUtils::make_object_schema(definition_properties, PackedStringArray{ "name", "description", "inputSchema" }, true);
	return MCPToolUtils::make_object_schema(properties, PackedStringArray{ "tool" });
}

bool parse_search_options(const Dictionary &p_arguments, SearchOptions &r_options, String &r_error) {
	if (!_validate_arguments(p_arguments, search_schema(), r_error)) {
		return false;
	}
	r_options.query = p_arguments.get("query", String());
	r_options.prefix = p_arguments.get("prefix", String());
	r_options.offset = int(p_arguments.get("offset", 0));
	r_options.limit = int(p_arguments.get("limit", SearchOptions().limit));
	return true;
}

bool parse_tool_name(const Dictionary &p_arguments, String &r_name, String &r_error) {
	if (!_validate_arguments(p_arguments, describe_schema(), r_error)) {
		return false;
	}
	r_name = p_arguments["name"];
	return true;
}

Dictionary search(const MCPToolRegistry &p_registry, const SearchOptions &p_options) {
	Array matches;
	int total = 0;
	const Array summaries = p_registry.get_tool_summaries();
	for (const Variant &summary_value : summaries) {
		const Dictionary summary = summary_value;
		const String name = summary["name"];
		const String description = summary["description"];
		if (!name.begins_with(p_options.prefix) ||
				(!p_options.query.is_empty() && !name.containsn(p_options.query) && !description.containsn(p_options.query))) {
			continue;
		}
		if (total >= p_options.offset && matches.size() < p_options.limit) {
			Dictionary match = summary.duplicate(true);
			match["description"] = description.left(MAX_DESCRIPTION_LENGTH);
			matches.push_back(match);
		}
		total++;
	}
	Dictionary result;
	result["tools"] = matches;
	result["total"] = total;
	result["nextOffset"] = p_options.offset + matches.size() < total ? p_options.offset + matches.size() : -1;
	return result;
}

} // namespace MCPCatalogToolUtils
