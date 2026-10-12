/**************************************************************************/
/*  mcp_cli_editor_launcher.cpp                                           */
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

#include "mcp_cli_editor_launcher.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#elif defined(UNIX_ENABLED) && !defined(WEB_ENABLED)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#endif

Error MCPOSCLIEditorLauncher::launch(const String &p_project_path, ProcessID &r_pid, String &r_error) {
	r_pid = 0;
	r_error = String();
	const String executable = OS::get_singleton()->get_executable_path();
#ifdef WINDOWS_ENABLED
	BOOL in_job = FALSE;
	if (!IsProcessInJob(GetCurrentProcess(), nullptr, &in_job)) {
		r_error = "LAUNCH_UNAVAILABLE: Cannot verify whether the CLI belongs to a terminating Windows Job.";
		return ERR_UNAVAILABLE;
	}
	// Do not launch a console wrapper or inherit its kill-on-close ownership.
	// A restrictive Job may refuse breakaway; fail rather than bind the editor
	// lifetime to this bridge or change a user's system/process settings.
	DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
	if (in_job) {
		flags |= CREATE_BREAKAWAY_FROM_JOB;
	}
	Char16String command = ("\"" + executable + "\" --editor --path \"" + p_project_path + "\" --mcp").utf16();
	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(nullptr, reinterpret_cast<LPWSTR>(command.ptrw()), nullptr, nullptr, FALSE, flags,
				nullptr, nullptr, &startup, &process)) {
		r_error = vformat("LAUNCH_UNAVAILABLE: Cannot start an independent editor (Windows error %d). Start the MCP editor explicitly and reconnect.", GetLastError());
		return ERR_CANT_FORK;
	}
	r_pid = process.dwProcessId;
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return OK;
#elif defined(UNIX_ENABLED) && !defined(WEB_ENABLED)
	// Prepare everything before fork. The child uses only async-signal-safe
	// calls; startup may already have initialized worker threads.
	const CharString executable_bytes = executable.utf8();
	const CharString project_bytes = p_project_path.utf8();
	char *arguments[] = { const_cast<char *>(executable_bytes.get_data()), const_cast<char *>("--editor"),
		const_cast<char *>("--path"), const_cast<char *>(project_bytes.get_data()), const_cast<char *>("--mcp"), nullptr };
	int errors[2];
	if (pipe(errors) != 0) {
		r_error = "LAUNCH_UNAVAILABLE: Cannot create the editor startup error pipe.";
		return ERR_CANT_CREATE;
	}
	if (fcntl(errors[1], F_SETFD, FD_CLOEXEC) == -1) {
		close(errors[0]);
		close(errors[1]);
		r_error = "LAUNCH_UNAVAILABLE: Cannot isolate editor startup descriptors.";
		return ERR_CANT_CREATE;
	}
	const pid_t child = fork();
	if (child < 0) {
		close(errors[0]);
		close(errors[1]);
		r_error = "LAUNCH_UNAVAILABLE: Cannot fork an independent editor.";
		return ERR_CANT_FORK;
	}
	if (child == 0) {
		close(errors[0]);
		int child_error = 0;
		if (setsid() == -1) {
			child_error = errno;
		}
		const int null_file = open("/dev/null", O_RDWR);
		if (null_file == -1) {
			child_error = errno;
		} else {
			for (int descriptor = 0; descriptor < 3; descriptor++) {
				if (dup2(null_file, descriptor) == -1) {
					child_error = errno;
				}
			}
			if (null_file > 2) {
				close(null_file);
			}
		}
		if (child_error == 0) {
			execv(executable_bytes.get_data(), arguments);
			child_error = errno;
		}
		const ssize_t ignored = write(errors[1], &child_error, sizeof(child_error));
		(void)ignored;
		_exit(127);
	}
	close(errors[1]);
	int child_error = 0;
	ssize_t count;
	do {
		count = read(errors[0], &child_error, sizeof(child_error));
	} while (count < 0 && errno == EINTR);
	close(errors[0]);
	if (count != 0) {
		if (count > 0) {
			waitpid(child, nullptr, 0);
		}
		r_error = vformat("LAUNCH_UNAVAILABLE: Independent editor startup failed (system error %d).", child_error);
		return ERR_CANT_FORK;
	}
	r_pid = child;
	return OK;
#else
	r_error = "LAUNCH_UNAVAILABLE: Independent MCP editor startup is not supported on this platform. Start the editor explicitly and reconnect.";
	return ERR_UNAVAILABLE;
#endif
}

bool MCPOSCLIEditorLauncher::is_running(ProcessID p_pid) const {
#if defined(UNIX_ENABLED) && !defined(WEB_ENABLED)
	int status = 0;
	const pid_t result = waitpid(p_pid, &status, WNOHANG);
	if (result == p_pid) {
		return false;
	}
#endif
	return OS::get_singleton()->is_process_running(p_pid);
}
