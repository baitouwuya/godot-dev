/**************************************************************************/
/*  test_editor_help.cpp                                                  */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_editor_help);

#ifdef TOOLS_ENABLED

#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/message_queue.h"
#include "editor/doc/editor_help.h"
#include "editor/file_system/editor_file_system.h"

struct EditorHelpTestAccess {
	const bool previous_stopped = EditorHelp::doc_generation_stopped;
	const int previous_generation_count = EditorHelp::doc_generation_count;

	EditorHelpTestAccess() {
		EditorHelp::doc_generation_stopped = false;
		EditorHelp::doc = memnew(DocTools);
	}

	~EditorHelpTestAccess() {
		EditorHelp::cleanup_doc();
		// A test must not leave the process-wide service shut down for later tests.
		// Production shutdown remains terminal; only this fixture restores the state.
		EditorHelp::doc_generation_stopped = previous_stopped;
		EditorHelp::doc_generation_count = previous_generation_count;
	}

	static Callable extension_callback() {
		return callable_mp_static(&EditorHelp::_gen_extensions_docs);
	}

	static void queue_from_worker(void *p_userdata) {
		static_cast<Callable *>(p_userdata)->call_deferred();
	}

	static void start_worker(Callable &p_callback) {
#ifdef THREADS_ENABLED
		EditorHelp::worker_thread.start(queue_from_worker, &p_callback);
#else
		queue_from_worker(&p_callback);
#endif
	}

	static bool has_threads() {
		return EditorHelp::worker_thread.is_started() || EditorHelp::loader_thread.is_started();
	}

	static DocTools *get_doc() {
		return EditorHelp::doc;
	}

	bool generation_unchanged() const {
		return EditorHelp::doc_generation_count == previous_generation_count;
	}
};

namespace TestEditorHelp {

class TestableEditorFileSystem : public EditorFileSystem {
public:
	void release_directory_tree() {
		// Exercise the real filesystem teardown without sending an invalid exit
		// notification to Node, since this fixture was never added to a SceneTree.
		EditorFileSystem::_notification(NOTIFICATION_EXIT_TREE);
	}
};

class ScopedEditorFileSystem {
	ResourceLoaderImport previous_import = ResourceLoader::import;
	ResourceFormatImporterLoadOnStartup previous_load_on_startup = ResourceImporter::load_on_startup;
	TestableEditorFileSystem *filesystem = memnew(TestableEditorFileSystem);

public:
	~ScopedEditorFileSystem() {
		memdelete(filesystem);
		ResourceLoader::import = previous_import;
		ResourceImporter::load_on_startup = previous_load_on_startup;
	}

	TestableEditorFileSystem *get() const {
		return filesystem;
	}
};

TEST_CASE("[EditorHelp] Deferred documentation callbacks cannot restart work after cleanup") {
	REQUIRE(EditorFileSystem::get_singleton() == nullptr);
	REQUIRE(EditorHelpTestAccess::get_doc() == nullptr);
	REQUIRE_FALSE(EditorHelpTestAccess::has_threads());
	ScopedEditorFileSystem filesystem;
	EditorHelpTestAccess fixture;
	REQUIRE(filesystem.get()->get_filesystem() != nullptr);
	Callable callback;

	SUBCASE("Native cache miss retries generation") {
		callback = callable_mp_static(&EditorHelp::generate_doc).bind(false, false);
	}
	SUBCASE("Native cache hit generates extension docs") {
		callback = EditorHelpTestAccess::extension_callback();
	}
	SUBCASE("Native docs finish and load the script cache") {
		callback = callable_mp_static(&EditorHelp::load_script_doc_cache);
	}
	SUBCASE("Corrupted script cache schedules regeneration") {
		callback = callable_mp_static(&EditorHelp::regenerate_script_doc_cache);
	}

	REQUIRE(callback.is_valid());
	// Queue from a worker, then join it through the actual shutdown path, without
	// flushing the main queue until the documentation has been deleted.
	EditorHelpTestAccess::start_worker(callback);
	EditorHelp::cleanup_doc();
	CHECK(EditorHelpTestAccess::get_doc() == nullptr);
	CHECK_FALSE(EditorHelpTestAccess::has_threads());

	MessageQueue::get_singleton()->flush();
	CHECK(EditorHelpTestAccess::get_doc() == nullptr);
	CHECK_FALSE(EditorHelpTestAccess::has_threads());
	CHECK(fixture.generation_unchanged());
}

TEST_CASE("[EditorHelp] Script cache callbacks tolerate an unavailable editor filesystem") {
	REQUIRE(EditorFileSystem::get_singleton() == nullptr);
	REQUIRE(EditorHelpTestAccess::get_doc() == nullptr);
	REQUIRE_FALSE(EditorHelpTestAccess::has_threads());
	EditorHelpTestAccess fixture;
	DocTools *doc = EditorHelpTestAccess::get_doc();

	// The editor filesystem can disappear before EditorHelp::cleanup_doc runs.
	callable_mp_static(&EditorHelp::load_script_doc_cache).call_deferred();
	callable_mp_static(&EditorHelp::regenerate_script_doc_cache).call_deferred();
	MessageQueue::get_singleton()->flush();

	CHECK(EditorHelpTestAccess::get_doc() == doc);
	CHECK_FALSE(EditorHelpTestAccess::has_threads());
	CHECK(fixture.generation_unchanged());
}

TEST_CASE("[EditorHelp] Script cache callbacks tolerate a filesystem that has exited the tree") {
	REQUIRE(EditorFileSystem::get_singleton() == nullptr);
	REQUIRE(EditorHelpTestAccess::get_doc() == nullptr);
	REQUIRE_FALSE(EditorHelpTestAccess::has_threads());
	ScopedEditorFileSystem filesystem;
	EditorHelpTestAccess fixture;
	DocTools *doc = EditorHelpTestAccess::get_doc();

	// EXIT_TREE releases the directory before the singleton itself is destroyed.
	filesystem.get()->release_directory_tree();
	REQUIRE(EditorFileSystem::get_singleton() == filesystem.get());
	REQUIRE(filesystem.get()->get_filesystem() == nullptr);

	callable_mp_static(&EditorHelp::load_script_doc_cache).call_deferred();
	callable_mp_static(&EditorHelp::regenerate_script_doc_cache).call_deferred();
	MessageQueue::get_singleton()->flush();

	CHECK(EditorHelpTestAccess::get_doc() == doc);
	CHECK_FALSE(EditorHelpTestAccess::has_threads());
	CHECK(fixture.generation_unchanged());
}

TEST_CASE("[EditorHelp] Documentation cleanup is idempotent") {
	REQUIRE(EditorHelpTestAccess::get_doc() == nullptr);
	REQUIRE_FALSE(EditorHelpTestAccess::has_threads());
	EditorHelpTestAccess fixture;

	EditorHelp::cleanup_doc();
	EditorHelp::cleanup_doc();

	CHECK(EditorHelpTestAccess::get_doc() == nullptr);
	CHECK_FALSE(EditorHelpTestAccess::has_threads());
	CHECK(fixture.generation_unchanged());
}

} // namespace TestEditorHelp

#endif // TOOLS_ENABLED
