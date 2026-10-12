/**************************************************************************/
/*  mcp_trace_observer.h                                                  */
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

#include "core/string/ustring.h"

// Core dispatch exposes only bounded metadata to this non-owning observer. No
// request IDs, arguments, results, client metadata or error messages cross it.
struct MCPTraceSpan {
	enum Kind {
		REQUEST,
		TOOL,
	} kind = REQUEST;
	enum Outcome {
		SUCCESS,
		BUSINESS_ERROR,
		PROTOCOL_ERROR,
		NOTIFICATION,
	} outcome = SUCCESS;
	uint64_t id = 0;
	uint64_t parent_id = 0;
	uint64_t root_id = 0;
	uint64_t duration_usec = 0;
	String name;
	String session_hash;
	int status = 0;
	bool is_error = false;
};

class MCPTraceObserver {
public:
	virtual void on_trace_started(const MCPTraceSpan &p_span) = 0;
	virtual void on_trace_finished(const MCPTraceSpan &p_span) = 0;
	virtual ~MCPTraceObserver() = default;
};

// Dispatch is synchronous, including nested Registry calls. Thread-local scope
// links avoid a shared mutable "current request" across clients or threads.
// The composition owner must detach the observer before destroying it.
class MCPTraceScope {
	static thread_local MCPTraceScope *current;
	MCPTraceScope *previous = nullptr;
	MCPTraceObserver *observer = nullptr;
	MCPTraceSpan span;
	uint64_t started_usec = 0;

public:
	MCPTraceScope(MCPTraceObserver *p_observer, MCPTraceSpan::Kind p_kind, const String &p_safe_name, const String &p_session_id);
	MCPTraceScope(const MCPTraceScope &) = delete;
	MCPTraceScope &operator=(const MCPTraceScope &) = delete;
	void finish(MCPTraceSpan::Outcome p_outcome, int p_status = 0, bool p_is_error = false);
	~MCPTraceScope();
};
