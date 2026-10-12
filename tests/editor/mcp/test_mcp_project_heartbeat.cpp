/**************************************************************************/
/*  test_mcp_project_heartbeat.cpp                                        */
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

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/mcp/mcp_project_lease.h"
#include "core/os/os.h"
#include "editor/mcp/mcp_project_heartbeat.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mcp_project_heartbeat);

namespace TestMCPProjectHeartbeat {

class UnusedHealthProbe : public MCPProjectLeaseHealthProbe {
public:
	bool is_healthy(const MCPDiscoveryRecord &) const override { return false; }
};

static bool wait_for_status(MCPProjectHeartbeat &p_heartbeat, MCPProjectHeartbeat::Status p_status) {
	const uint64_t deadline = OS::get_singleton()->get_ticks_usec() + 2 * 1000 * 1000;
	while (OS::get_singleton()->get_ticks_usec() < deadline) {
		if (p_heartbeat.get_status() == p_status) {
			return true;
		}
		OS::get_singleton()->delay_usec(1000);
	}
	return false;
}

TEST_CASE("[MCP][Heartbeat] Publishing failures retry and recover without surrendering ownership") {
	Error error = OK;
	Ref<DirAccess> temporary = DirAccess::create_temp("mcp_heartbeat_retry", false, &error);
	REQUIRE(error == OK);
	REQUIRE(temporary->make_dir("project") == OK);
	MCPProjectIdentity identity;
	REQUIRE(MCPProjectIdentity::create(temporary->get_current_dir().path_join("project"), identity) == OK);
	const MCPDiscoveryRecord record = MCPDiscoveryRecord::create(identity, "streamable-http", "http://127.0.0.1:32123/mcp", "2025-06-18", "private-token");
	MCPProjectLease lease(identity, temporary->get_current_dir().path_join("lease"));
	UnusedHealthProbe probe;
	REQUIRE(lease.acquire(record, probe) == OK);
	const String discovery = temporary->get_current_dir().path_join("records");
	Ref<FileAccess> blocker = FileAccess::open(discovery, FileAccess::WRITE);
	REQUIRE(blocker.is_valid());
	blocker.unref();

	MCPProjectHeartbeat heartbeat;
	REQUIRE(heartbeat.start(&lease, discovery, record, 1000) == OK);
	CHECK(wait_for_status(heartbeat, MCPProjectHeartbeat::STATUS_RETRYING));
	CHECK(heartbeat.is_running());
	REQUIRE(DirAccess::remove_absolute(discovery) == OK);
	CHECK(wait_for_status(heartbeat, MCPProjectHeartbeat::STATUS_HEALTHY));
	heartbeat.stop();
	CHECK(lease.is_acquired());
	MCPDiscoveryRecord refreshed;
	REQUIRE(MCPDiscovery::read_record(discovery, identity.project_id, refreshed) == OK);
	CHECK(refreshed.instance_id == identity.instance_id);
	CHECK(lease.release() == OK);
}

TEST_CASE("[MCP][Heartbeat] Confirmed replacement owner stops the heartbeat") {
	Error error = OK;
	Ref<DirAccess> temporary = DirAccess::create_temp("mcp_heartbeat_owner", false, &error);
	REQUIRE(error == OK);
	REQUIRE(temporary->make_dir("project") == OK);
	MCPProjectIdentity identity;
	MCPProjectIdentity replacement_identity;
	const String project = temporary->get_current_dir().path_join("project");
	REQUIRE(MCPProjectIdentity::create(project, identity) == OK);
	REQUIRE(MCPProjectIdentity::create(project, replacement_identity) == OK);
	const MCPDiscoveryRecord record = MCPDiscoveryRecord::create(identity, "streamable-http", "http://127.0.0.1:32123/mcp", "2025-06-18");
	const MCPDiscoveryRecord replacement = MCPDiscoveryRecord::create(replacement_identity, "streamable-http", "http://127.0.0.1:32124/mcp", "2025-06-18");
	MCPProjectLease lease(identity, temporary->get_current_dir().path_join("lease"));
	UnusedHealthProbe probe;
	REQUIRE(lease.acquire(record, probe) == OK);
	REQUIRE(MCPDiscovery::write_record_file(lease.get_owner_file(), replacement) == OK);
	MCPProjectHeartbeat heartbeat;
	REQUIRE(heartbeat.start(&lease, temporary->get_current_dir().path_join("records"), record, 1000) == OK);
	CHECK(wait_for_status(heartbeat, MCPProjectHeartbeat::STATUS_OWNERSHIP_LOST));
	heartbeat.stop();
	CHECK_FALSE(lease.is_acquired());
	MCPDiscoveryRecord current;
	REQUIRE(lease.read_owner(current) == OK);
	CHECK(current.instance_id == replacement_identity.instance_id);
}

} // namespace TestMCPProjectHeartbeat
