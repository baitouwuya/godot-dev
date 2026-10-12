/**************************************************************************/
/*  test_mcp_external_change_tracker.cpp                                */
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

#include "editor/file_system/editor_external_change_tracker.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mcp_external_change_tracker);

namespace TestEditorExternalChangeTracker {

using Change = EditorExternalChangeTracker::Change;

TEST_CASE("[MCP][Editor] External-change popup invalidates stale deferred generations") {
	EditorExternalChangeTracker tracker;
	const Vector<Change> first{ { "res://first.gd", 10, "first" } };
	const Vector<Change> second{ { "res://second.gd", 20, "second" } };
	const uint64_t first_generation = tracker.update(first);
	CHECK(tracker.can_show(first_generation));
	const uint64_t second_generation = tracker.update(second);
	CHECK_FALSE(tracker.present(first_generation));
	CHECK(tracker.present(second_generation));
	CHECK(tracker.get_displayed() == second);
	CHECK_FALSE(tracker.present(second_generation));

	tracker.clear();
	CHECK_FALSE(tracker.can_show(second_generation));
	CHECK_FALSE(tracker.present(second_generation));
	CHECK(tracker.get_displayed().is_empty());
}

TEST_CASE("[MCP][Editor] External-change popup never presents an empty scan") {
	EditorExternalChangeTracker tracker;
	CHECK_FALSE(tracker.present(tracker.update({})));
	const uint64_t generation = tracker.update({ { "res://scene.tscn", 10, "scene" } });
	tracker.update({});
	CHECK_FALSE(tracker.present(generation));
	CHECK_FALSE(tracker.present(tracker.update({})));
}

TEST_CASE("[MCP][Editor] External-change confirmations own the displayed file set") {
	EditorExternalChangeTracker tracker;
	const Vector<Change> first{ { "res://first.gd", 10, "first" } };
	const Vector<Change> both{ { "res://first.gd", 10, "first" }, { "res://second.gd", 20, "second" } };
	const uint64_t generation = tracker.update(first);
	REQUIRE(tracker.present(generation));
	CHECK(tracker.is_displayed_current(first));
	CHECK_FALSE(tracker.is_displayed_current(both));
	const uint64_t next_generation = tracker.update(both);
	// An intervening scan must not silently add files to an existing approval.
	CHECK(tracker.get_displayed() == first);
	CHECK_FALSE(tracker.is_displayed_current(both));
	CHECK_FALSE(tracker.is_displayed_current(first));
	REQUIRE(tracker.present(next_generation));
	CHECK(tracker.get_displayed() == both);
	CHECK(tracker.is_displayed_current(both));
}

TEST_CASE("[MCP][Editor] External-change confirmation detects same-second disk replacements") {
	EditorExternalChangeTracker tracker;
	const Vector<Change> first{ { "res://project.godot", 10, "first-content" } };
	const Vector<Change> replaced{ { "res://project.godot", 10, "different-content" } };
	REQUIRE(tracker.present(tracker.update(first)));
	CHECK_FALSE(tracker.is_displayed_current(replaced));
	REQUIRE(tracker.present(tracker.update(replaced)));
	CHECK(tracker.is_displayed_current(replaced));
}

TEST_CASE("[MCP][Editor] Repeated full scans preserve existing displayed conflicts") {
	EditorExternalChangeTracker tracker;
	const Vector<Change> changes{ { "res://dirty.gd", 10, "disk-content" } };
	const uint64_t generation = tracker.update(changes);
	REQUIRE(tracker.present(generation));
	CHECK(tracker.update(changes) == generation);
	CHECK(tracker.is_displayed_current(changes));
	CHECK(tracker.get_displayed() == changes);
	CHECK_FALSE(tracker.can_show(generation));
}

} // namespace TestEditorExternalChangeTracker
