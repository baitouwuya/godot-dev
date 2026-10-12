/**************************************************************************/
/*  mcp_editor_plugin.cpp                                                 */
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

#include "mcp_editor_plugin.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_paths.h"
#include "scene/gui/dialogs.h"
#include "scene/main/scene_tree.h"
#include "servers/display/display_server.h"

void MCPEditorPlugin::configure(bool p_requested, int p_port, MCPProjectInstance *p_instance) {
	requested = p_requested;
	requested_port = p_port;
	editor_instance = p_instance;
}

bool MCPEditorPlugin::_is_headless() const {
	return !DisplayServer::get_singleton() || DisplayServer::get_singleton()->get_name() == "headless";
}

Error MCPEditorPlugin::_register_tools(String &r_error) {
	const Error prepare_error = builtin_features.prepare_for_registration(r_error);
	if (prepare_error != OK) {
		return prepare_error;
	}
	return builtin_features.register_tools(&tool_registry, &r_error);
}

void MCPEditorPlugin::_unregister_tools() {
	builtin_features.unregister_tools();
}

void MCPEditorPlugin::on_mcp_session_removed(const String &p_session_id) {
	builtin_features.on_session_removed(p_session_id);
}

Error MCPEditorPlugin::_publish_discovery(String &r_error) {
	const uint64_t now_ms = MCPDiscovery::get_current_unix_time_ms();
	discovery_record.heartbeat_at_unix_ms = now_ms;
	Error error = project_lease->refresh(discovery_record, &r_error, now_ms);
	if (error != OK) {
		return error;
	}
	return MCPDiscovery::write_record(discovery_directory, discovery_record, true, &r_error);
}

Error MCPEditorPlugin::_start_mcp(String &r_error) {
	r_error = String();
	if (!editor_instance || !editor_instance->is_registered() || !editor_instance->get_host_lease() ||
			!editor_instance->get_host_lease()->is_acquired()) {
		r_error = "MCP editor ownership must be reserved before editor initialization.";
		return ERR_UNCONFIGURED;
	}
	project_identity = editor_instance->get_identity();
	project_lease = editor_instance->get_host_lease();
	if (MCPProjectIdentity::generate_instance_id(auth_secret, &r_error) != OK) {
		return ERR_CANT_CREATE;
	}
	discovery_directory = EditorPaths::get_singleton()->get_data_dir().path_join("mcp/discovery");
	discovery_record = MCPDiscoveryRecord::create(project_identity, "streamable-http", String(), MCPProtocol::PROTOCOL_VERSION_LATEST, auth_secret);
	Error error;

	error = _register_tools(r_error);
	if (error != OK) {
		project_lease->release();
		return error;
	}

	MCPHost::Config host_config;
	host_config.project_identity = project_identity;
	host_config.bearer_token = auth_secret;
	host_config.port = requested_port;
	host_config.server_version = VERSION_FULL_CONFIG;
	host_config.session_observer = this;
	error = host.start(host_config, &tool_registry);
	if (error != OK) {
		_unregister_tools();
		project_lease->release();
		r_error = "Unable to start the MCP Streamable HTTP Host.";
		return error;
	}
	builtin_features.configure_host(host.get_project_metadata());

	discovery_record.endpoint = host.get_endpoint();
	error = editor_instance->update_host(discovery_record, &r_error);
	if (error == OK) {
		error = _publish_discovery(r_error);
	}
	if (error != OK) {
		host.stop();
		_unregister_tools();
		project_lease->release();
		return error;
	}

	builtin_features.clear_debug_events();
	error = builtin_features.start_debug_capture();
	if (error != OK) {
		MCPDiscovery::remove_record(discovery_directory, project_identity.project_id, project_identity.instance_id);
		host.stop();
		_unregister_tools();
		project_lease->release();
		r_error = "Unable to start MCP debug event capture.";
		return error;
	}
	add_debugger_plugin(builtin_features.get_runtime_input_debugger_plugin());
	runtime_input_debugger_registered = true;
	add_debugger_plugin(builtin_features.get_runtime_observation_debugger_plugin());
	runtime_observation_debugger_registered = true;

	error = project_heartbeat.start(project_lease, discovery_directory, discovery_record);
	if (error != OK) {
		r_error = "Unable to start the MCP project heartbeat worker.";
		_stop_mcp();
		return error;
	}
	print_line(vformat("Godot MCP Host started for project %s at %s with %d tools",
			project_identity.project_id, host.get_endpoint(), tool_registry.get_tool_names().size()));
	return OK;
}

void MCPEditorPlugin::_stop_mcp() {
	project_heartbeat.stop();
	if (host.is_running()) {
		host.stop();
	}
	_unregister_tools();
	builtin_features.shutdown();
	if (runtime_observation_debugger_registered) {
		remove_debugger_plugin(builtin_features.get_runtime_observation_debugger_plugin());
		runtime_observation_debugger_registered = false;
	}
	builtin_features.clear_runtime_requests();
	if (runtime_input_debugger_registered) {
		remove_debugger_plugin(builtin_features.get_runtime_input_debugger_plugin());
		runtime_input_debugger_registered = false;
	}
	builtin_features.stop_debug_capture();
	if (!discovery_directory.is_empty() && project_identity.is_valid()) {
		MCPDiscovery::remove_record(discovery_directory, project_identity.project_id, project_identity.instance_id);
	}
	project_lease = nullptr; // Borrowed from the process-lifetime owner.
	if (editor_instance) {
		editor_instance->disable_mcp();
	}
	discovery_record = MCPDiscoveryRecord();
	project_identity = MCPProjectIdentity();
	discovery_directory = String();
	auth_secret = String();
}

void MCPEditorPlugin::_handle_start_error(Error p_error, const String &p_message) {
	last_start_error = p_message.is_empty() ? error_names[p_error] : p_message;
	if (_is_headless()) {
		_exit_with_error("Unable to start Godot MCP Host: " + last_start_error);
		return;
	}
	// Initialization has already reached the user's editor. Service startup
	// failure must not discard buffers or close the GUI they are working in.
	startup_state = STARTUP_DISABLED;
	_stop_mcp();
	set_process_internal(false);
	ERR_PRINT("MCP is unavailable; the editor remains open: " + last_start_error);
	EditorNode::get_singleton()->show_warning(TTR("MCP could not start. Your editor and unsaved changes remain open.") + "\n\n" + last_start_error);
}

void MCPEditorPlugin::_exit_with_error(const String &p_message) {
	startup_state = STARTUP_FAILED;
	ERR_PRINT(p_message);
	_stop_mcp();
	if (get_tree()) {
		get_tree()->quit(EXIT_FAILURE);
	} else {
		OS::get_singleton()->set_exit_code(EXIT_FAILURE);
	}
}

void MCPEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!requested || startup_state == STARTUP_DISABLED || startup_state == STARTUP_FAILED || startup_state == STARTUP_WAITING_FOR_CONFLICT) {
				return;
			}
			if (startup_state == STARTUP_PENDING) {
				if (!EditorNode::get_singleton()->is_editor_ready()) {
					return;
				}
				String error;
				const Error start_error = _start_mcp(error);
				if (start_error != OK) {
					_handle_start_error(start_error, error);
					return;
				}
				startup_state = STARTUP_RUNNING;
			}
			host.poll();
			builtin_features.process();
			MCPProjectHeartbeat::Status heartbeat_status;
			String heartbeat_message;
			if (project_heartbeat.poll_status_change(heartbeat_status, heartbeat_message)) {
				if (heartbeat_status == MCPProjectHeartbeat::STATUS_OWNERSHIP_LOST) {
					// Preserve the user's editor and unsaved buffers. Only the MCP
					// service loses authority when another instance owns the lease.
					WARN_PRINT("MCP stopped after losing its project lease. The editor remains open: " + heartbeat_message);
					startup_state = STARTUP_DISABLED;
					_stop_mcp();
					set_process_internal(false);
					if (!_is_headless() && EditorNode::get_singleton()) {
						EditorNode::get_singleton()->show_warning(TTR("MCP lost ownership of this project and has stopped. Your editor and unsaved changes remain open."));
					}
				} else if (heartbeat_status == MCPProjectHeartbeat::STATUS_RETRYING) {
					WARN_PRINT("MCP discovery publishing is temporarily unavailable; retrying without closing the editor: " + heartbeat_message);
				} else {
					print_verbose(heartbeat_message);
				}
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			_stop_mcp();
		} break;
	}
}

MCPEditorPlugin::MCPEditorPlugin() {
	singleton = this;
	ERR_FAIL_COND(builtin_features.initialize() != OK);
	set_process_internal(requested);
}

MCPEditorPlugin::~MCPEditorPlugin() {
	_stop_mcp();
	if (singleton == this) {
		singleton = nullptr;
	}
}
