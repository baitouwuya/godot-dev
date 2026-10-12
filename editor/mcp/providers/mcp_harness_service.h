/**************************************************************************/
/*  mcp_harness_service.h                                                 */
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

#include "editor/mcp/providers/mcp_harness_evidence_collector.h"
#include "editor/mcp/providers/mcp_harness_job_store.h"
#include "editor/mcp/providers/mcp_harness_step_executor.h"

class MCPHarnessService {
public:
	static constexpr int MAX_ACTIVE_JOBS = 8;
	static constexpr int MAX_SESSION_JOBS = 2;
	static constexpr int MAX_TOOL_CALLS_PER_POLL = 1;

	MCPHarnessService();
	MCPHarnessService(const MCPHarnessService &) = delete;
	MCPHarnessService &operator=(const MCPHarnessService &) = delete;

	void set_tool_registry(MCPToolRegistry *p_registry);
	void set_trace_service(MCPTraceService *p_service, const Dictionary &p_project_metadata = Dictionary(), const String &p_root_directory_override = String());

	Dictionary start(const Dictionary &p_arguments, const MCPToolCallContext &p_context);
	Dictionary get_status(const Dictionary &p_arguments, const MCPToolCallContext &p_context) const;
	Dictionary cancel(const Dictionary &p_arguments, const MCPToolCallContext &p_context);
	Dictionary get_report(const Dictionary &p_arguments, const MCPToolCallContext &p_context) const;

	int poll(int p_max_tool_calls = MAX_TOOL_CALLS_PER_POLL);
	void release_session(const String &p_session_id);
	void shutdown();

private:
	MCPToolRegistry *tool_registry = nullptr;
	MCPHarnessJobStore job_store;
	MCPHarnessEvidenceCollector evidence_collector;
	MCPHarnessStepExecutor step_executor;

	static String _session_id(const MCPToolCallContext &p_context);
	MCPHarnessJob *_find_owned_job(const Dictionary &p_arguments, const MCPToolCallContext &p_context, Dictionary &r_error);
	const MCPHarnessJob *_find_owned_job(const Dictionary &p_arguments, const MCPToolCallContext &p_context, Dictionary &r_error) const;
	int _advance_job(MCPHarnessJob &r_job);
};
