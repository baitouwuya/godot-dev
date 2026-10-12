/**************************************************************************/
/*  mcp_trace_observer.cpp                                                */
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

#include "mcp_trace_observer.h"

#include "core/os/os.h"
#include "core/templates/safe_refcount.h"

thread_local MCPTraceScope *MCPTraceScope::current = nullptr;

MCPTraceScope::MCPTraceScope(MCPTraceObserver *p_observer, MCPTraceSpan::Kind p_kind, const String &p_safe_name, const String &p_session_id) :
		observer(p_observer) {
	if (!observer) {
		return;
	}
	static SafeNumeric<uint64_t> next_id;
	span.id = next_id.increment();
	span.root_id = span.id;
	span.kind = p_kind;
	span.name = p_safe_name.left(256);
	span.session_hash = p_session_id.is_empty() ? String() : p_session_id.sha256_text().left(16);
	previous = current;
	if (previous && previous->observer == observer && previous->span.session_hash == span.session_hash) {
		span.parent_id = previous->span.id;
		span.root_id = previous->span.root_id;
	}
	started_usec = OS::get_singleton()->get_ticks_usec();
	current = this;
	observer->on_trace_started(span);
}

void MCPTraceScope::finish(MCPTraceSpan::Outcome p_outcome, int p_status, bool p_is_error) {
	span.outcome = p_outcome;
	span.status = p_status;
	span.is_error = p_is_error;
}

MCPTraceScope::~MCPTraceScope() {
	if (!observer) {
		return;
	}
	current = previous;
	span.duration_usec = OS::get_singleton()->get_ticks_usec() - started_usec;
	observer->on_trace_finished(span);
}
