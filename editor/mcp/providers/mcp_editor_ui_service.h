/**************************************************************************/
/*  mcp_editor_ui_service.h                                               */
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

#include "editor/mcp/providers/mcp_editor_ui_action_executor.h"
#include "editor/mcp/providers/mcp_editor_ui_snapshot_builder.h"
#include "editor/mcp/providers/mcp_editor_ui_snapshot_store.h"
#include "editor/mcp/providers/mcp_editor_ui_target_validator.h"

class MCPEditorUIService {
public:
	using SnapshotOptions = MCPEditorUISnapshotOptions;

	Dictionary capture(const String &p_session_id, const SnapshotOptions &p_options);
	Error perform(const String &p_session_id, const String &p_snapshot_id, const String &p_target_id, const String &p_action, const Dictionary &p_arguments, Dictionary &r_result, String &r_error_code, String &r_error_message);
	void release_session(const String &p_session_id);
	void clear();

private:
	MCPEditorUISnapshotBuilder snapshot_builder;
	MCPEditorUISnapshotStore snapshot_store;
	MCPEditorUITargetValidator target_validator;
	MCPEditorUIActionExecutor action_executor;
};
