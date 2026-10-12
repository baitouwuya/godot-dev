/**************************************************************************/
/*  mcp_gdextension_profile_registry.cpp                                  */
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

#include "mcp_gdextension_profile_registry.h"

namespace MCPGDExtensionProfileRegistry {

PackedStringArray names() {
	return PackedStringArray{ "debug", "release" };
}

bool resolve(const String &p_name, Profile &r_profile, String *r_error) {
	r_profile = Profile();
	if (p_name == "debug") {
		r_profile.name = p_name;
		r_profile.scons_target = "template_debug";
		return true;
	}
	if (p_name == "release") {
		r_profile.name = p_name;
		r_profile.scons_target = "template_release";
		r_profile.release = true;
		return true;
	}
	if (r_error) {
		*r_error = "profile must be debug or release; allowed values: " + String(", ").join(names());
	}
	return false;
}

String current_platform() {
#if defined(MACOS_ENABLED)
	return "macos";
#elif defined(WINDOWS_ENABLED)
	return "windows";
#elif defined(LINUXBSD_ENABLED)
	return "linux";
#elif defined(ANDROID_ENABLED)
	return "android";
#elif defined(IOS_ENABLED)
	return "ios";
#else
	return String();
#endif
}

} // namespace MCPGDExtensionProfileRegistry
