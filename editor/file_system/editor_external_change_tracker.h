/**************************************************************************/
/*  editor_external_change_tracker.h                                      */
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
#include "core/templates/vector.h"

// Owns the immutable file set shown by an external-change confirmation. A newer
// scan invalidates queued popups and confirmations without changing that set.
class EditorExternalChangeTracker {
public:
	struct Change {
		String path;
		uint64_t modified_time = 0;
		String content_hash;

		bool operator==(const Change &p_other) const {
			return path == p_other.path && modified_time == p_other.modified_time && content_hash == p_other.content_hash;
		}

		bool operator!=(const Change &p_other) const { return !(*this == p_other); }
	};

private:
	Vector<Change> pending;
	Vector<Change> displayed;
	uint64_t generation = 0;
	uint64_t displayed_generation = 0;

public:
	uint64_t update(const Vector<Change> &p_changes) {
		if (pending != p_changes) {
			pending = p_changes;
			generation++;
		}
		return generation;
	}

	bool can_show(uint64_t p_generation) const {
		return p_generation == generation && !pending.is_empty() && displayed_generation != generation;
	}

	bool present(uint64_t p_generation) {
		if (!can_show(p_generation)) {
			return false;
		}
		displayed = pending;
		displayed_generation = generation;
		return true;
	}

	bool is_displayed_current(const Vector<Change> &p_changes) const {
		return !displayed.is_empty() && displayed_generation == generation && displayed == p_changes;
	}

	const Vector<Change> &get_displayed() const { return displayed; }
	const Vector<Change> &get_pending() const { return pending; }

	void clear() {
		generation++;
		pending.clear();
		displayed.clear();
		displayed_generation = 0;
	}
};
