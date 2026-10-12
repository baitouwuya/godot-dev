/**************************************************************************/
/*  mcp_project_instance.cpp                                                 */
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

#include "mcp_project_instance.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/mcp/mcp_protocol.h"
#include "core/os/os.h"

namespace {
void set_error(String *r_error, const String &p_error) {
	if (r_error) {
		*r_error = p_error;
	}
}
} // namespace

String MCPProjectInstance::get_presence_directory(const MCPProjectIdentity &p_identity) {
	return MCPProjectLease::get_default_project_state_directory(p_identity).path_join("mcp_instances");
}

String MCPProjectInstance::get_arbitration_directory(const MCPProjectIdentity &p_identity) {
	return get_presence_directory(p_identity).path_join("registration");
}

String MCPProjectInstance::get_launch_directory(const MCPProjectIdentity &p_identity) {
	return get_presence_directory(p_identity).path_join("launch");
}

Error MCPProjectInstance::list_running(const MCPProjectIdentity &p_identity, Vector<MCPDiscoveryRecord> &r_instances,
		String *r_error, const MCPProjectLeaseProcessProbe *p_process_probe, uint64_t p_now_unix_ms) {
	r_instances.clear();
	set_error(r_error, String());
	ERR_FAIL_COND_V(!p_identity.is_valid(), ERR_INVALID_PARAMETER);
	const String directory = get_presence_directory(p_identity);
	if (!DirAccess::dir_exists_absolute(directory)) {
		return OK;
	}
	Error error = OK;
	Ref<DirAccess> access = DirAccess::open(directory, &error);
	if (access.is_null()) {
		set_error(r_error, "Cannot inspect existing editor registrations.");
		return error;
	}
	MCPOSProjectLeaseProcessProbe os_probe;
	const MCPProjectLeaseProcessProbe &probe = p_process_probe ? *p_process_probe : os_probe;
	const uint64_t now = p_now_unix_ms ? p_now_unix_ms : MCPDiscovery::get_current_unix_time_ms();
	access->list_dir_begin();
	for (String name = access->get_next(); !name.is_empty(); name = access->get_next()) {
		if (access->current_is_dir() || !name.ends_with(".json")) {
			continue;
		}
		MCPDiscoveryRecord owner;
		error = MCPDiscovery::read_record_file(directory.path_join(name), owner, r_error);
		if (error != OK) {
			// Never interpret an unreadable registration as permission to launch.
			return error;
		}
		if (owner.project_id != p_identity.project_id || owner.project_path != p_identity.canonical_path ||
				!owner.is_valid(false) || name != owner.instance_id + ".json") {
			set_error(r_error, "Editor registration does not match this project identity.");
			return ERR_INVALID_DATA;
		}
		const bool starting = now < owner.started_at_unix_ms || now - owner.started_at_unix_ms < 5000;
		if (probe.is_process_running(owner.pid) || starting) {
			r_instances.push_back(owner);
		}
	}
	return OK;
}

Error MCPProjectInstance::register_editor(const String &p_project_path, bool p_request_mcp, const MCPProjectLeaseHealthProbe &p_health_probe,
		MCPDiscoveryRecord *r_conflicting_owner, String *r_error,
		const MCPProjectLeaseProcessProbe *p_process_probe, uint64_t p_now_unix_ms) {
	set_error(r_error, String());
	ERR_FAIL_COND_V(registered || host_lease, ERR_ALREADY_IN_USE);
	Error error = MCPProjectIdentity::create(p_project_path, identity, r_error);
	if (error != OK) {
		return error;
	}
	presence = MCPDiscoveryRecord::create(identity, p_request_mcp ? "streamable-http" : "editor", String(),
			MCPProtocol::PROTOCOL_VERSION_LATEST, String(), p_now_unix_ms);
	MCPProjectLease arbitration(identity, get_arbitration_directory(identity));
	// This lock covers both the presence check and publication. All cooperating
	// ordinary and managed editors participate, so startup cannot race a GUI.
	MCPDiscoveryRecord registration_owner = presence;
	registration_owner.transport = "editor-registration";
	error = arbitration.acquire(registration_owner, p_health_probe, r_conflicting_owner, r_error, p_now_unix_ms, p_process_probe);
	if (error != OK) {
		return error;
	}
	if (p_request_mcp) {
		Vector<MCPDiscoveryRecord> running;
		error = list_running(identity, running, r_error, p_process_probe, p_now_unix_ms);
		if (error != OK) {
			return error;
		}
		if (!running.is_empty()) {
			if (r_conflicting_owner) {
				*r_conflicting_owner = running[0];
			}
			set_error(r_error, running[0].transport == "editor" ?
					"MCP_DISABLED: An existing editor has not enabled MCP. Keep it open; enable MCP through an explicit user-approved restart." :
					"ALREADY_RUNNING: An existing editor owns or is starting this project. Connect to it instead of launching another editor.");
			return ERR_ALREADY_IN_USE;
		}
		// Retain the original Host lease as well, including compatibility with a
		// Host started by an older build that has no editor-presence record.
		host_lease = memnew(MCPProjectLease(identity));
		error = host_lease->acquire(presence, p_health_probe, r_conflicting_owner, r_error, p_now_unix_ms, p_process_probe);
		if (error != OK) {
			memdelete(host_lease);
			host_lease = nullptr;
			return error;
		}
	}
	presence_file = get_presence_directory(identity).path_join(identity.instance_id + ".json");
	error = MCPDiscovery::write_record_file(presence_file, presence, false, r_error);
	if (error != OK) {
		unregister_editor();
		return error;
	}
	registered = true;
	return OK;
}

Error MCPProjectInstance::update_host(const MCPDiscoveryRecord &p_record, String *r_error) {
	ERR_FAIL_COND_V(!registered || !host_lease || !host_lease->is_acquired(), ERR_UNCONFIGURED);
	ERR_FAIL_COND_V(p_record.instance_id != identity.instance_id || p_record.project_id != identity.project_id, ERR_INVALID_PARAMETER);
	MCPDiscoveryRecord updated = p_record;
	updated.auth_secret = String();
	const Error error = MCPDiscovery::write_record_file(presence_file, updated, false, r_error);
	if (error == OK) {
		presence = updated;
	}
	return error;
}

void MCPProjectInstance::disable_mcp() {
	if (host_lease) {
		host_lease->release();
		memdelete(host_lease);
		host_lease = nullptr;
	}
	if (registered && presence.transport != "editor") {
		presence.transport = "editor";
		presence.endpoint = String();
		presence.auth_secret = String();
		MCPDiscovery::write_record_file(presence_file, presence, false);
	}
}

void MCPProjectInstance::unregister_editor() {
	if (host_lease) {
		memdelete(host_lease);
		host_lease = nullptr;
	}
	if (registered) {
		// The filename is unique to this instance; never erase another editor's
		// registration or a shared directory during process cleanup.
		DirAccess::remove_absolute(presence_file);
	}
	registered = false;
	presence_file = String();
}

MCPProjectInstance::~MCPProjectInstance() {
	unregister_editor();
}
