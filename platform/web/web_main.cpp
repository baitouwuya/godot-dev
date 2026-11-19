/**************************************************************************/
/*  web_main.cpp                                                          */
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

#include "display_server_web.h"
#include "godot_js.h"
#include "os_web.h"
#include "web_pack_stream.h"

#include "core/config/engine.h"
#include "core/io/resource_loader.h"
#include "core/os/main_loop.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "main/main.h"

#ifdef TOOLS_ENABLED
#include "core/io/file_access.h"
#include "editor/web_tools_editor_plugin.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h" // SceneTree only forward declares it.
#endif

#include <emscripten/emscripten.h>

#include <cstdlib>

static OS_Web *os = nullptr;
#ifndef PROXY_TO_PTHREAD_ENABLED
static uint64_t target_ticks = 0;
#endif

static bool main_started = false;
static bool shutdown_complete = false;
enum class StartupPhase { SETUP2, START, FIRST_FRAME };
static StartupPhase startup_phase = StartupPhase::SETUP2;

void exit_callback() {
	if (!shutdown_complete) {
		return; // Still waiting.
	}
	if (main_started) {
		WebPackStream::clear_upgrades();
		Main::cleanup();
		main_started = false;
	}
	int exit_code = OS_Web::get_singleton()->get_exit_code();
	delete os; // We used a normal new, so it needs a normal delete.
	os = nullptr;
	godot_cleanup_profiler();
	emscripten_force_exit(exit_code); // Exit runtime.
}

void cleanup_after_sync() {
	shutdown_complete = true;
}

static void begin_shutdown() {
	emscripten_cancel_main_loop();
	emscripten_set_main_loop(exit_callback, -1, false);
	godot_js_os_finish_async(cleanup_after_sync);
}

static void abort_startup(int p_exit_code, const char *p_message) {
	os->set_exit_code(p_exit_code);
	godot_js_os_startup_exit(p_exit_code, p_message);
	begin_shutdown();
}

void main_loop_callback() {
#ifndef PROXY_TO_PTHREAD_ENABLED
	uint64_t current_ticks = os->get_ticks_usec();
#endif

	bool force_draw = DisplayServerWeb::get_singleton()->check_size_force_redraw();
	if (force_draw) {
		Main::force_redraw();
#ifndef PROXY_TO_PTHREAD_ENABLED
	} else if (current_ticks < target_ticks) {
		return; // Skip frame.
#endif
	}

#ifndef PROXY_TO_PTHREAD_ENABLED
	int max_fps = Engine::get_singleton()->get_max_fps();
	if (max_fps > 0) {
		if (current_ticks - target_ticks > 1000000) {
			// When the window loses focus, we stop getting updates and accumulate delay.
			// For this reason, if the difference is too big, we reset target ticks to the current ticks.
			target_ticks = current_ticks;
		}
		target_ticks += (uint64_t)(1000000 / max_fps);
	}
#endif

	if (os->main_loop_iterate()) {
		begin_shutdown();
	}
}

void print_web_header() {
	// Emscripten.
	char *emscripten_version_char = godot_js_emscripten_get_version();
	String emscripten_version = vformat("Emscripten %s", emscripten_version_char);
	// `free()` is used here because it's not memory that was allocated by Godot.
	free(emscripten_version_char);

	// Build features.
	String thread_support = OS::get_singleton()->has_feature("threads")
			? "multi-threaded"
			: "single-threaded";
	String extensions_support = OS::get_singleton()->has_feature("web_extensions")
			? "GDExtension support"
			: "no GDExtension support";

	Vector<String> build_configuration = { emscripten_version, thread_support, extensions_support };
	print_line(vformat("Build configuration: %s.", String(", ").join(build_configuration)));
}

static void startup_loop_callback() {
	switch (startup_phase) {
		case StartupPhase::SETUP2: {
			if (Main::setup2() != OK) {
				abort_startup(EXIT_FAILURE, "Godot failed to initialize servers.");
				return;
			}
			main_started = true;
			print_web_header();
			ResourceLoader::set_abort_on_missing_resources(false);
			startup_phase = StartupPhase::START;
			godot_js_os_report_progress(0.76f, "Loading scene and resident textures...");
			return;
		}
		case StartupPhase::START: {
			const int result = Main::start();
			if (result != EXIT_SUCCESS || !os->get_main_loop()) {
				abort_startup(result, result == EXIT_SUCCESS ? "Godot exited before starting a scene." : "Godot failed to start the scene.");
				return;
			}
			os->get_main_loop()->initialize();
#ifdef TOOLS_ENABLED
			if (Engine::get_singleton()->is_project_manager_hint() && FileAccess::exists("/tmp/preload.zip")) {
				PackedStringArray ps;
				ps.push_back("/tmp/preload.zip");
				SceneTree::get_singleton()->get_root()->emit_signal(SNAME("files_dropped"), ps);
			}
#endif
			startup_phase = StartupPhase::FIRST_FRAME;
			godot_js_os_report_progress(0.93f, "Preparing first frame pipelines...");
			return;
		}
		case StartupPhase::FIRST_FRAME: {
			DisplayServerWeb::get_singleton()->check_size_force_redraw();
			Main::force_redraw();
			const uint64_t before = Engine::get_singleton()->get_process_frames();
			const bool quit = os->main_loop_iterate();
			if (quit && os->get_exit_code() != EXIT_SUCCESS) {
				abort_startup(os->get_exit_code(), "Godot failed during its first frame.");
				return;
			}
			if (Engine::get_singleton()->get_process_frames() == before) {
				if (quit) {
					abort_startup(EXIT_FAILURE, "Godot failed before the first frame iteration.");
				}
				return; // ERR_BUSY does not run Main::iteration().
			}
			godot_js_os_report_progress(1.0f, "Ready");
			godot_js_os_first_frame_ready(!quit);
			if (quit) {
				begin_shutdown();
			} else {
				emscripten_cancel_main_loop();
				emscripten_set_main_loop(main_loop_callback, -1, false);
			}
			return;
		}
	}
}

/// When calling main, it is assumed FS is setup and synced.
extern EMSCRIPTEN_KEEPALIVE int godot_web_main(int argc, char *argv[]) {
	godot_init_profiler();

	os = new OS_Web();

#ifdef TOOLS_ENABLED
	WebToolsEditorPlugin::initialize();
#endif

	// We must override main when testing is enabled
	TEST_MAIN_OVERRIDE

	const Error err = Main::setup(argv[0], argc - 1, &argv[1], false);
	if (err != OK) {
		const int result = err == ERR_HELP ? EXIT_SUCCESS : EXIT_FAILURE;
		abort_startup(result, err == ERR_HELP ? "Godot exited after printing help or version information." : "Godot failed to set up the project.");
		return result;
	}

	WebPackStream::mount_partial_files();
	godot_js_os_report_progress(0.56f, "Initializing renderer and shaders...");
	emscripten_set_main_loop(startup_loop_callback, -1, false);
	return EXIT_SUCCESS;
}
