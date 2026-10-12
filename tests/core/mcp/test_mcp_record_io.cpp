/**************************************************************************/
/*  test_mcp_record_io.cpp                                                */
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

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/mcp/mcp_record_io.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "tests/test_macros.h"

#ifdef WINDOWS_ENABLED
#include <windows.h>
#endif

TEST_FORCE_LINK(test_mcp_record_io);

namespace TestMCPRecordIO {

#ifdef WINDOWS_ENABLED
class ScopedReadHandle {
	HANDLE handle = INVALID_HANDLE_VALUE;

public:
	explicit ScopedReadHandle(const String &p_path, DWORD p_sharing) {
		const Char16String path = p_path.utf16();
		handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.get_data()), GENERIC_READ,
				p_sharing, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	}
	~ScopedReadHandle() {
		if (handle != INVALID_HANDLE_VALUE) {
			CloseHandle(handle);
		}
	}
	HANDLE get() const { return handle; }
};

TEST_CASE("[MCP][RecordIO] Windows denied replacement preserves the previous record") {
	Error error = OK;
	Ref<DirAccess> temporary = DirAccess::create_temp("mcp_record_deny_delete", false, &error);
	REQUIRE(error == OK);
	const String records = temporary->get_current_dir().path_join("records");
	const String path = records.path_join("record.json");
	const String previous = R"({"revision":1,"value":"original"})";
	const String replacement = R"({"revision":2,"value":"replacement"})";
	REQUIRE(MCPRecordIO::write_atomic(path, previous) == OK);
	{
		// This reader deliberately denies FILE_SHARE_DELETE, as external readers may.
		ScopedReadHandle reader(path, FILE_SHARE_READ | FILE_SHARE_WRITE);
		REQUIRE(reader.get() != INVALID_HANDLE_VALUE);
		String diagnostic;
		CHECK(MCPRecordIO::write_atomic(path, replacement, &diagnostic) == ERR_BUSY);
		CHECK_FALSE(diagnostic.is_empty());
		String text;
		REQUIRE(MCPRecordIO::read(path, 1024, text) == OK);
		CHECK(text == previous);
		Ref<DirAccess> entries = DirAccess::open(records);
		REQUIRE(entries.is_valid());
		CHECK(entries->get_files().size() == 1); // Failed refresh did not leave a temporary file.
	}
	// A later refresh succeeds after the conflicting reader closes.
	REQUIRE(MCPRecordIO::write_atomic(path, replacement) == OK);
	String text;
	REQUIRE(MCPRecordIO::read(path, 1024, text) == OK);
	CHECK(text == replacement);
}

TEST_CASE("[MCP][RecordIO] Windows share-delete readers keep a complete snapshot during replacement") {
	Error error = OK;
	Ref<DirAccess> temporary = DirAccess::create_temp("mcp_record_share_delete", false, &error);
	REQUIRE(error == OK);
	const String path = temporary->get_current_dir().path_join("records/record.json");
	const String previous = R"({"revision":1,"value":"original"})";
	const String replacement = R"({"revision":2,"value":"replacement"})";
	REQUIRE(MCPRecordIO::write_atomic(path, previous) == OK);
	ScopedReadHandle reader(path, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
	REQUIRE(reader.get() != INVALID_HANDLE_VALUE);
	REQUIRE(MCPRecordIO::write_atomic(path, replacement) == OK);
	String text;
	REQUIRE(MCPRecordIO::read(path, 1024, text) == OK);
	CHECK(text == replacement);

	// The pre-existing handle still refers to the complete old file, never a mixture.
	char snapshot[1024];
	DWORD count = 0;
	REQUIRE(ReadFile(reader.get(), snapshot, sizeof(snapshot), &count, nullptr));
	CHECK(String::utf8(snapshot, count) == previous);
}
#endif

#if defined(UNIX_ENABLED) && defined(THREADS_ENABLED)
struct ReadRace {
	String path;
	String first;
	String second;
	SafeFlag running;
	SafeFlag failed;
	SafeNumeric<uint32_t> reads;
};

static void read_during_replacement(void *p_userdata) {
	ReadRace &race = *static_cast<ReadRace *>(p_userdata);
	while (race.running.is_set()) {
		String text;
		if (MCPRecordIO::read(race.path, 32768, text) != OK || (text != race.first && text != race.second)) {
			race.failed.set();
			return;
		}
		race.reads.increment();
		OS::get_singleton()->delay_usec(100);
	}
}

TEST_CASE("[MCP][RecordIO] Concurrent Unix readers see complete owner-only record versions") {
	Error error = OK;
	Ref<DirAccess> temporary = DirAccess::create_temp("mcp_record_atomic_read", false, &error);
	REQUIRE(error == OK);
	const String records = temporary->get_current_dir().path_join("records");
	ReadRace race;
	race.path = records.path_join("record.json");
	race.first = String("A").repeat(16384);
	race.second = String("B").repeat(16384);
	REQUIRE(MCPRecordIO::write_atomic(race.path, race.first) == OK);
	race.running.set();
	Thread reader;
	REQUIRE(reader.start(read_during_replacement, &race) != Thread::UNASSIGNED_ID);
	// Avoid fatal assertions while the worker owns references to this stack frame.
	for (int i = 0; i < 20; i++) {
		CHECK(MCPRecordIO::write_atomic(race.path, i % 2 == 0 ? race.second : race.first) == OK);
		OS::get_singleton()->delay_usec(1000);
	}
	race.running.clear();
	reader.wait_to_finish();
	CHECK_FALSE(race.failed.is_set());
	CHECK(race.reads.get() > 0);
	CHECK(FileAccess::get_unix_permissions(race.path) == 0600);
	CHECK(FileAccess::get_unix_permissions(records) == 0700);
	Ref<DirAccess> entries = DirAccess::open(records);
	REQUIRE(entries.is_valid());
	CHECK(entries->get_files().size() == 1);
}
#endif

} // namespace TestMCPRecordIO
