/**************************************************************************/
/*  test_mcp_project_instance.cpp                                         */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mcp_project_instance);

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/mcp/mcp_project_instance.h"

namespace TestMCPProjectInstance {

class NoHealth : public MCPProjectLeaseHealthProbe {
public:
	bool is_healthy(const MCPDiscoveryRecord &) const override { return false; }
};

class DeadProcess : public MCPProjectLeaseProcessProbe {
public:
	bool is_process_running(ProcessID) const override { return false; }
};

TEST_CASE("[MCP][ProjectInstance] Ordinary editors register without opting into MCP") {
	Error error = OK;
	Ref<DirAccess> root = DirAccess::create_temp("mcp_editor_presence", false, &error);
	REQUIRE(error == OK);
	NoHealth health;
	MCPProjectInstance first;
	MCPProjectInstance second;
	REQUIRE(first.register_editor(root->get_current_dir(), false, health) == OK);
	REQUIRE(second.register_editor(root->get_current_dir(), false, health) == OK);
	CHECK(first.get_host_lease() == nullptr);
	CHECK(second.get_host_lease() == nullptr);
	Vector<MCPDiscoveryRecord> running;
	REQUIRE(MCPProjectInstance::list_running(first.get_identity(), running) == OK);
	CHECK(running.size() == 2);
	for (const MCPDiscoveryRecord &record : running) {
		CHECK(record.transport == "editor");
		CHECK(record.auth_secret.is_empty());
		CHECK(record.endpoint.is_empty());
	}
	first.unregister_editor();
	REQUIRE(MCPProjectInstance::list_running(second.get_identity(), running) == OK);
	CHECK(running.size() == 1);
}

TEST_CASE("[MCP][ProjectInstance] Managed startup cannot replace an ordinary editor") {
	Error error = OK;
	Ref<DirAccess> root = DirAccess::create_temp("mcp_editor_disabled", false, &error);
	REQUIRE(error == OK);
	NoHealth health;
	MCPProjectInstance ordinary;
	MCPProjectInstance managed;
	REQUIRE(ordinary.register_editor(root->get_current_dir(), false, health) == OK);
	MCPDiscoveryRecord conflict;
	String message;
	CHECK(managed.register_editor(root->get_current_dir(), true, health, &conflict, &message) == ERR_ALREADY_IN_USE);
	CHECK(message.begins_with("MCP_DISABLED:"));
	CHECK(conflict.instance_id == ordinary.get_identity().instance_id);
	CHECK_FALSE(managed.is_registered());
	CHECK(managed.get_host_lease() == nullptr);
	CHECK(ordinary.is_registered());
}

TEST_CASE("[MCP][ProjectInstance] Managed startup reserves the Host before editor initialization") {
	Error error = OK;
	Ref<DirAccess> root = DirAccess::create_temp("mcp_editor_managed", false, &error);
	REQUIRE(error == OK);
	NoHealth health;
	MCPProjectInstance first;
	MCPProjectInstance second;
	REQUIRE(first.register_editor(root->get_current_dir(), true, health) == OK);
	REQUIRE(first.get_host_lease() != nullptr);
	CHECK(first.get_host_lease()->is_acquired());
	MCPDiscoveryRecord owner;
	REQUIRE(first.get_host_lease()->read_owner(owner) == OK);
	CHECK(owner.instance_id == first.get_identity().instance_id);
	CHECK(owner.endpoint.is_empty());
	MCPDiscoveryRecord conflict;
	String message;
	CHECK(second.register_editor(root->get_current_dir(), true, health, &conflict, &message) == ERR_ALREADY_IN_USE);
	CHECK(message.begins_with("ALREADY_RUNNING:"));
	CHECK(conflict.instance_id == owner.instance_id);
	CHECK_FALSE(second.is_registered());
	first.disable_mcp();
	CHECK(first.get_host_lease() == nullptr);
	CHECK(first.is_registered());
	CHECK(second.register_editor(root->get_current_dir(), true, health, &conflict, &message) == ERR_ALREADY_IN_USE);
	CHECK(message.begins_with("MCP_DISABLED:"));
	first.unregister_editor();
	REQUIRE(second.register_editor(root->get_current_dir(), true, health) == OK);
}

TEST_CASE("[MCP][ProjectInstance] Startup presence never includes bearer credentials") {
	Error error = OK;
	Ref<DirAccess> root = DirAccess::create_temp("mcp_editor_secret", false, &error);
	REQUIRE(error == OK);
	NoHealth health;
	MCPProjectInstance instance;
	REQUIRE(instance.register_editor(root->get_current_dir(), true, health) == OK);
	MCPDiscoveryRecord record = instance.get_presence();
	record.endpoint = "http://127.0.0.1:45001/mcp";
	record.auth_secret = "must-not-appear-in-presence";
	REQUIRE(instance.update_host(record) == OK);
	Vector<MCPDiscoveryRecord> running;
	REQUIRE(MCPProjectInstance::list_running(instance.get_identity(), running) == OK);
	REQUIRE(running.size() == 1);
	CHECK(running[0].auth_secret.is_empty());
	CHECK(running[0].endpoint == record.endpoint);
}

TEST_CASE("[MCP][ProjectInstance] Dead presence is ignored but unreadable metadata fails closed") {
	Error error = OK;
	Ref<DirAccess> root = DirAccess::create_temp("mcp_editor_stale", false, &error);
	REQUIRE(error == OK);
	NoHealth health;
	MCPProjectInstance ordinary;
	REQUIRE(ordinary.register_editor(root->get_current_dir(), false, health, nullptr, nullptr, nullptr, 1000) == OK);
	DeadProcess dead;
	Vector<MCPDiscoveryRecord> running;
	REQUIRE(MCPProjectInstance::list_running(ordinary.get_identity(), running, nullptr, &dead, 10000) == OK);
	CHECK(running.is_empty());
	const String record_file = MCPProjectInstance::get_presence_directory(ordinary.get_identity()).path_join(ordinary.get_identity().instance_id + ".json");
	Ref<FileAccess> file = FileAccess::open(record_file, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	file->store_string("invalid");
	file->close();
	MCPProjectInstance managed;
	ERR_PRINT_OFF;
	CHECK(managed.register_editor(root->get_current_dir(), true, health, nullptr, nullptr, &dead, 10000) != OK);
	ERR_PRINT_ON;
	CHECK_FALSE(managed.is_registered());
}

} // namespace TestMCPProjectInstance
