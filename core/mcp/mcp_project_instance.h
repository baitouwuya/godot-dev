/**************************************************************************/
/*  mcp_project_instance.h                                                   */
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

#include "core/mcp/mcp_project_lease.h"
#include "core/templates/vector.h"

// Editor presence is separate from the opt-in MCP Host. Register before loading
// editor plugins, scanning/importing resources, or saving any project content.
// Ordinary editors may coexist; managed MCP startup must never create a second
// editor for a project already open in a cooperating editor process.
class MCPProjectInstance {
	MCPProjectIdentity identity;
	MCPDiscoveryRecord presence;
	MCPProjectLease *host_lease = nullptr;
	String presence_file;
	bool registered = false;

public:
	static String get_presence_directory(const MCPProjectIdentity &p_identity);
	static String get_arbitration_directory(const MCPProjectIdentity &p_identity);
	static String get_launch_directory(const MCPProjectIdentity &p_identity);
	static Error list_running(const MCPProjectIdentity &p_identity, Vector<MCPDiscoveryRecord> &r_instances,
			String *r_error = nullptr, const MCPProjectLeaseProcessProbe *p_process_probe = nullptr, uint64_t p_now_unix_ms = 0);

	Error register_editor(const String &p_project_path, bool p_request_mcp, const MCPProjectLeaseHealthProbe &p_health_probe,
			MCPDiscoveryRecord *r_conflicting_owner = nullptr, String *r_error = nullptr,
			const MCPProjectLeaseProcessProbe *p_process_probe = nullptr, uint64_t p_now_unix_ms = 0);
	Error update_host(const MCPDiscoveryRecord &p_record, String *r_error = nullptr);
	void disable_mcp();
	void unregister_editor();

	const MCPProjectIdentity &get_identity() const { return identity; }
	const MCPDiscoveryRecord &get_presence() const { return presence; }
	MCPProjectLease *get_host_lease() const { return host_lease; }
	bool is_registered() const { return registered; }

	MCPProjectInstance() = default;
	MCPProjectInstance(const MCPProjectInstance &) = delete;
	MCPProjectInstance &operator=(const MCPProjectInstance &) = delete;
	~MCPProjectInstance();
};
