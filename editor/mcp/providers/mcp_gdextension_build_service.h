/**************************************************************************/
/*  mcp_gdextension_build_service.h                                       */
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

#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

class MCPGDExtensionBuildService {
	struct BuildRecord {
		String id;
		String session_id;
		String extension_path;
		String artifact_path;
		String artifact_absolute_path;
		String backup_path;
		String profile;
		String reload_policy;
		String state;
		String reload_status;
		String project_revision;
		String artifact_sha256;
		String failure_code;
		String failure_message;
		String stdout_tail;
		String stderr_tail;
		Array diagnostics;
		ProcessID process_id = 0;
		Ref<FileAccess> stdout_pipe;
		Ref<FileAccess> stderr_pipe;
		uint64_t started_usec = 0;
		uint64_t finished_usec = 0;
		uint64_t started_unix_ms = 0;
		uint64_t finished_unix_ms = 0;
		uint64_t timeout_usec = 0;
		int exit_code = -1;
		bool had_artifact = false;
		bool rollback_complete = true;
		bool runtime_was_playing = false;
	};

	HashMap<String, BuildRecord> jobs;
	Vector<String> job_order;
	uint64_t next_job_id = 1;
	bool shutting_down = false;

	BuildRecord *_resolve_job(const Dictionary &p_arguments, const Dictionary &p_context, Dictionary &r_error);
	Dictionary _result(const BuildRecord &p_job) const;
	void _drain_output(BuildRecord &r_job);
	void _finish_process(BuildRecord &r_job);
	void _rollback(BuildRecord &r_job);
	void _cleanup_backup(BuildRecord &r_job);

public:
	Dictionary build(const Dictionary &p_arguments, const Dictionary &p_context);
	Dictionary get_status(const Dictionary &p_arguments, const Dictionary &p_context);
	Dictionary cancel(const Dictionary &p_arguments, const Dictionary &p_context);

	void process();
	void release_session(const String &p_session_id);
	void shutdown();

	~MCPGDExtensionBuildService();
};
