/**************************************************************************/
/*  test_mcp_trace_observer.cpp                                           */
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

#include "core/mcp/mcp_trace_observer.h"
#include "core/templates/vector.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mcp_trace_observer);

namespace TestMCPTraceObserver {

class Observer : public MCPTraceObserver {
public:
	Vector<MCPTraceSpan> started;
	Vector<MCPTraceSpan> finished;
	void on_trace_started(const MCPTraceSpan &p_span) override { started.push_back(p_span); }
	void on_trace_finished(const MCPTraceSpan &p_span) override { finished.push_back(p_span); }
};

TEST_CASE("[MCP] Trace scopes restore nested parents and isolate reentrant sessions") {
	Observer observer;
	{
		MCPTraceScope outer(&observer, MCPTraceSpan::REQUEST, "tools/call", "session-a");
		REQUIRE(observer.started.size() == 1);
		CHECK(observer.finished.is_empty()); // Started is delivered before any work.
		{
			MCPTraceScope nested(&observer, MCPTraceSpan::TOOL, "echo", "session-a");
			CHECK(observer.started[1].parent_id == observer.started[0].id);
			CHECK(observer.started[1].root_id == observer.started[0].id);
		}
		{
			MCPTraceScope another_session(&observer, MCPTraceSpan::REQUEST, "ping", "session-b");
			CHECK(observer.started[2].parent_id == 0);
			CHECK(observer.started[2].root_id == observer.started[2].id);
		}
		MCPTraceScope restored(&observer, MCPTraceSpan::TOOL, "echo", "session-a");
		CHECK(observer.started[3].parent_id == observer.started[0].id);
		restored.finish(MCPTraceSpan::BUSINESS_ERROR, 0, true);
	}
	REQUIRE(observer.started.size() == 4);
	REQUIRE(observer.finished.size() == 4);
	CHECK(observer.finished[2].outcome == MCPTraceSpan::BUSINESS_ERROR);
	CHECK(observer.finished[2].is_error);
	CHECK(observer.finished[3].id == observer.started[0].id);
	{
		MCPTraceScope disabled(nullptr, MCPTraceSpan::REQUEST, "ping", "session-a");
	}
	CHECK(observer.started.size() == 4);
	{
		MCPTraceScope independent(&observer, MCPTraceSpan::REQUEST, "ping", "session-a");
		CHECK(observer.started[4].parent_id == 0);
		CHECK(observer.started[4].id != observer.started[0].id);
	}
}

} // namespace TestMCPTraceObserver
