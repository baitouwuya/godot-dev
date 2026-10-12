/**************************************************************************/
/*  mcp_project_revision.cpp                                              */
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

#include "mcp_project_revision.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/templates/vector.h"
#include "editor/mcp/providers/mcp_variant_codec.h"

namespace MCPProjectRevision {

String compute(ProjectSettings *p_settings) {
	if (!p_settings) {
		return String();
	}

	// Include both persisted bytes and canonical in-memory storage values so
	// save=false mutations and external project.godot edits invalidate a token.
	String canonical;
	const String project_path = p_settings->get_resource_path().path_join("project.godot");
	canonical += "path=" + p_settings->get_resource_path() + "\n";
	canonical += "disk=" + (FileAccess::exists(project_path) ? FileAccess::get_sha256(project_path) : String()) + "\n";

	List<PropertyInfo> properties;
	p_settings->get_property_list(&properties, true);
	Vector<String> names;
	for (const PropertyInfo &property : properties) {
		if (!(property.usage & PROPERTY_USAGE_STORAGE) || !p_settings->has_setting(property.name)) {
			continue;
		}
		names.push_back(property.name);
	}
	names.sort();

	String previous_name;
	for (const String &name : names) {
		if (name == previous_name) {
			continue;
		}
		previous_name = name;

		const Variant value = p_settings->get_setting(name);
		Variant encoded_value;
		String encoding_error;
		String value_text;
		if (MCPVariantCodec::encode(value, encoded_value, &encoding_error) == OK) {
			value_text = JSON::stringify(encoded_value, "", true, true);
		} else {
			value_text = "unencodable:" + Variant::get_type_name(value.get_type()) + ":" + value.stringify();
		}
		canonical += itos(name.length()) + ":" + name + "|" + itos(p_settings->get_order(name)) + "|" + value_text + "\n";
	}

	return canonical.sha256_text();
}

} // namespace MCPProjectRevision
