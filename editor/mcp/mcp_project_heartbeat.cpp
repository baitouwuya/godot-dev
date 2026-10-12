/**************************************************************************/
/*  mcp_project_heartbeat.cpp                                             */
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

#include "mcp_project_heartbeat.h"

#include "core/mcp/mcp_project_lease.h"
#include "core/os/os.h"

void MCPProjectHeartbeat::_thread_main(void *p_userdata) {
	static_cast<MCPProjectHeartbeat *>(p_userdata)->_run();
}

void MCPProjectHeartbeat::_set_status(Status p_status, const String &p_message) {
	MutexLock lock(state_mutex);
	if (status != p_status) {
		status_changed = true;
	}
	status = p_status;
	status_message = p_message;
}

void MCPProjectHeartbeat::_run() {
	Thread::set_name("MCP Heartbeat");
	uint64_t retry_interval_usec = interval_usec;
	uint64_t next_refresh_usec = OS::get_singleton()->get_ticks_usec() + interval_usec;
	while (running.is_set()) {
		const uint64_t now_usec = OS::get_singleton()->get_ticks_usec();
		if (now_usec < next_refresh_usec) {
			OS::get_singleton()->delay_usec(MIN(STOP_POLL_INTERVAL_USEC, next_refresh_usec - now_usec));
			continue;
		}

		MCPDiscoveryRecord updated_record = record;
		updated_record.heartbeat_at_unix_ms = MCPDiscovery::get_current_unix_time_ms();
		String error;
		Error refresh_error = lease->refresh(updated_record, &error, updated_record.heartbeat_at_unix_ms);
		if (refresh_error == ERR_UNAUTHORIZED || !lease->is_acquired()) {
			_set_status(STATUS_OWNERSHIP_LOST, error.is_empty() ? String(error_names[refresh_error]) : error);
			running.clear();
			return;
		}
		if (refresh_error == OK) {
			refresh_error = MCPDiscovery::write_record(discovery_directory, updated_record, true, &error);
		}
		if (refresh_error != OK) {
			// Discovery is an address cache, not the lease itself. A reader, disk
			// error, or temporary publishing failure must not quit the editor.
			_set_status(STATUS_RETRYING, error.is_empty() ? String(error_names[refresh_error]) : error);
			retry_interval_usec = MIN(retry_interval_usec * 2, uint64_t(30 * 1000 * 1000));
			next_refresh_usec = OS::get_singleton()->get_ticks_usec() + retry_interval_usec;
			continue;
		}
		record = updated_record;
		retry_interval_usec = interval_usec;
		_set_status(STATUS_HEALTHY, "MCP discovery publishing recovered.");
		next_refresh_usec = OS::get_singleton()->get_ticks_usec() + interval_usec;
	}
}

Error MCPProjectHeartbeat::start(MCPProjectLease *p_lease, const String &p_discovery_directory, const MCPDiscoveryRecord &p_record, uint64_t p_interval_usec) {
	ERR_FAIL_COND_V(thread.is_started() || running.is_set(), ERR_ALREADY_IN_USE);
	ERR_FAIL_NULL_V(p_lease, ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(!p_lease->is_acquired() || p_discovery_directory.is_empty() || !p_record.is_valid(true) || p_interval_usec == 0, ERR_INVALID_PARAMETER);

	lease = p_lease;
	discovery_directory = p_discovery_directory;
	record = p_record;
	interval_usec = p_interval_usec;
	{
		MutexLock lock(state_mutex);
		status_message = String();
		status = STATUS_HEALTHY;
		status_changed = false;
	}
	running.set();
	const Thread::ID thread_id = thread.start(_thread_main, this);
	if (thread_id == Thread::UNASSIGNED_ID) {
		running.clear();
		lease = nullptr;
		discovery_directory = String();
		record = MCPDiscoveryRecord();
		return ERR_CANT_CREATE;
	}
	return OK;
}

void MCPProjectHeartbeat::stop() {
	running.clear();
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	lease = nullptr;
	discovery_directory = String();
	record = MCPDiscoveryRecord();
}

bool MCPProjectHeartbeat::poll_status_change(Status &r_status, String &r_message) {
	MutexLock lock(state_mutex);
	if (!status_changed) {
		return false;
	}
	r_status = status;
	r_message = status_message;
	status_changed = false;
	return true;
}

MCPProjectHeartbeat::Status MCPProjectHeartbeat::get_status() const {
	MutexLock lock(state_mutex);
	return status;
}

MCPProjectHeartbeat::~MCPProjectHeartbeat() {
	stop();
}
