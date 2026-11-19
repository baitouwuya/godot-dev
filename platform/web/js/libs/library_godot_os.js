/**************************************************************************/
/*  library_godot_os.js                                                   */
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

const IDHandler = {
	$IDHandler: {
		_last_id: 0,
		_references: {},

		get: function (p_id) {
			return IDHandler._references[p_id];
		},

		add: function (p_data) {
			const id = ++IDHandler._last_id;
			IDHandler._references[id] = p_data;
			return id;
		},

		remove: function (p_id) {
			delete IDHandler._references[p_id];
		},
	},
};

autoAddDeps(IDHandler, '$IDHandler');
mergeInto(LibraryManager.library, IDHandler);

const GodotConfig = {
	$GodotConfig__postset: 'Module["initConfig"] = GodotConfig.init_config;',
	$GodotConfig__deps: ['$GodotRuntime'],
	$GodotConfig: {
		canvas: null,
		locale: 'en',
		canvas_resize_policy: 2, // Adaptive
		virtual_keyboard: false,
		persistent_drops: false,
		godot_pool_size: 4,
		on_execute: null,
		on_exit: null,
		on_startup_progress: null,
		on_first_frame_ready: null,
		on_startup_exit: null,

		init_config: function (p_opts) {
			GodotConfig.canvas_resize_policy = p_opts['canvasResizePolicy'];
			GodotConfig.canvas = p_opts['canvas'];
			GodotConfig.locale = p_opts['locale'] || GodotConfig.locale;
			GodotConfig.virtual_keyboard = p_opts['virtualKeyboard'];
			GodotConfig.persistent_drops = !!p_opts['persistentDrops'];
			GodotConfig.godot_pool_size = p_opts['godotPoolSize'];
			GodotConfig.on_execute = p_opts['onExecute'];
			GodotConfig.on_exit = p_opts['onExit'];
			GodotConfig.on_startup_progress = p_opts['onStartupProgress'];
			GodotConfig.on_first_frame_ready = p_opts['onFirstFrameReady'];
			GodotConfig.on_startup_exit = p_opts['onStartupExit'];
			if (p_opts['focusCanvas']) {
				GodotConfig.canvas.focus();
			}
		},

		locate_file: function (file) {
			return Module['locateFile'](file);
		},
		clear: function () {
			GodotConfig.canvas = null;
			GodotConfig.locale = 'en';
			GodotConfig.canvas_resize_policy = 2;
			GodotConfig.virtual_keyboard = false;
			GodotConfig.persistent_drops = false;
			GodotConfig.on_execute = null;
			GodotConfig.on_exit = null;
			GodotConfig.on_startup_progress = null;
			GodotConfig.on_first_frame_ready = null;
			GodotConfig.on_startup_exit = null;
		},
	},

	godot_js_config_canvas_id_get__proxy: 'sync',
	godot_js_config_canvas_id_get__sig: 'vii',
	godot_js_config_canvas_id_get: function (p_ptr, p_ptr_max) {
		GodotRuntime.stringToHeap(`#${GodotConfig.canvas.id}`, p_ptr, p_ptr_max);
	},

	godot_js_config_locale_get__proxy: 'sync',
	godot_js_config_locale_get__sig: 'vii',
	godot_js_config_locale_get: function (p_ptr, p_ptr_max) {
		GodotRuntime.stringToHeap(GodotConfig.locale, p_ptr, p_ptr_max);
	},
};

autoAddDeps(GodotConfig, '$GodotConfig');
mergeInto(LibraryManager.library, GodotConfig);

const GodotFS = {
	$GodotFS__deps: ['$FS', '$IDBFS', '$GodotRuntime'],
	$GodotFS__postset: [
		'Module["initFS"] = GodotFS.init;',
		'Module["copyToFS"] = GodotFS.copy_to_fs;',
	].join(''),
	$GodotFS: {
		// ERRNO_CODES works every odd version of emscripten, but this will break too eventually.
		ENOENT: 44,
		_idbfs: false,
		_syncing: false,
		_mount_points: [],

		is_persistent: function () {
			return GodotFS._idbfs ? 1 : 0;
		},

		// Initialize godot file system, setting up persistent paths.
		// Returns a promise that resolves when the FS is ready.
		// We keep track of mount_points, so that we can properly close the IDBFS
		// since emscripten is not doing it by itself. (emscripten GH#12516).
		init: function (persistentPaths) {
			GodotFS._idbfs = false;
			if (!Array.isArray(persistentPaths)) {
				return Promise.reject(new Error('Persistent paths must be an array'));
			}
			if (!persistentPaths.length) {
				return Promise.resolve();
			}
			GodotFS._mount_points = persistentPaths.slice();

			function createRecursive(dir) {
				try {
					FS.stat(dir);
				} catch (e) {
					if (e.errno !== GodotFS.ENOENT) {
						// Let mkdirTree throw in case, we cannot trust the above check.
						GodotRuntime.error(e);
					}
					FS.mkdirTree(dir);
				}
			}

			GodotFS._mount_points.forEach(function (path) {
				createRecursive(path);
				FS.mount(IDBFS, {}, path);
			});
			return new Promise(function (resolve, reject) {
				FS.syncfs(true, function (err) {
					if (err) {
						GodotFS._mount_points = [];
						GodotFS._idbfs = false;
						GodotRuntime.print(`IndexedDB not available: ${err.message}`);
					} else {
						GodotFS._idbfs = true;
					}
					resolve(err);
				});
			});
		},

		// Deinit godot file system, making sure to unmount file systems, and close IDBFS(s).
		deinit: function () {
			GodotFS._mount_points.forEach(function (path) {
				try {
					FS.unmount(path);
				} catch (e) {
					GodotRuntime.print('Already unmounted', e);
				}
				if (GodotFS._idbfs && IDBFS.dbs[path]) {
					IDBFS.dbs[path].close();
					delete IDBFS.dbs[path];
				}
			});
			GodotFS._mount_points = [];
			GodotFS._idbfs = false;
			GodotFS._syncing = false;
		},

		sync: function () {
			if (GodotFS._syncing) {
				GodotRuntime.error('Already syncing!');
				return Promise.resolve();
			}
			GodotFS._syncing = true;
			return new Promise(function (resolve, reject) {
				FS.syncfs(false, function (error) {
					if (error) {
						GodotRuntime.error(`Failed to save IDB file system: ${error.message}`);
					}
					GodotFS._syncing = false;
					resolve(error);
				});
			});
		},

		// Copies a buffer to the internal file system. Creating directories recursively.
		copy_to_fs: function (path, buffer, canOwn) {
			if (!path.startsWith('/')) {
				path = `${FS.cwd()}/${path}`;
			}
			const idx = path.lastIndexOf('/');
			const dir = path.slice(0, idx) || '/';
			try {
				FS.stat(dir);
			} catch (e) {
				if (e.errno !== GodotFS.ENOENT) {
					throw e;
				}
				FS.mkdirTree(dir);
			}
			const bytes = ArrayBuffer.isView(buffer)
				? new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength)
				: new Uint8Array(buffer);
			if (!canOwn) {
				FS.writeFile(path, bytes);
				return;
			}
			try {
				FS.unlink(path);
			} catch (e) {
				if (e.errno !== GodotFS.ENOENT) {
					throw e;
				}
			}
			FS.createDataFile(dir, path.slice(idx + 1), bytes, true, true, true);
		},
	},
};
mergeInto(LibraryManager.library, GodotFS);

const GodotPackStream = {
	$GodotPackStream__deps: ['$GodotRuntime'],
	$GodotPackStream__postset: 'Module["initPackStream"] = GodotPackStream.init;',
	$GodotPackStream: {
		pack: null,
		path: null,
		controller: null,
		started: false,
		entries: [],
		queued: [],
		ready: [],
		slots: 0,
		slot_bytes: 0,
		publish_frame: 0,
		stats: null,

		init: function (pack) {
			GodotPackStream.pack = pack;
			GodotPackStream.controller = new AbortController();
			GodotPackStream.stats = { 'total': 0, 'completed': 0, 'failed': 0, 'bytesLoaded': 0, 'bytesTotal': 0, 'active': false };
			const encoder = new TextEncoder();
			GodotPackStream.path = encoder.encode(pack['path']);
			GodotPackStream.entries = pack['entries'].map(function (metadata) {
				return { data: metadata, path: encoder.encode(metadata['path']), state: 'idle', middle: null };
			});
			GodotPackStream.publish();
		},

		publish: function () {
			if (!GodotPackStream.publish_frame) {
				GodotPackStream.publish_frame = requestAnimationFrame(function () {
					GodotPackStream.publish_frame = 0;
					const state = { ...GodotPackStream.stats };
					state['active'] = GodotPackStream.started && state['total'] > state['completed'] + state['failed'];
					window['godotStreamState'] = state;
					if (typeof window['onGodotStreamProgress'] === 'function') {
						window['onGodotStreamProgress'](state);
					}
				});
			}
		},

		start: function () {
			if (GodotPackStream.pack) {
				GodotPackStream.started = true;
				GodotPackStream.pump();
			}
		},

		stop: function () {
			if (GodotPackStream.controller) {
				GodotPackStream.controller.abort();
				GodotPackStream.controller = null;
				GodotPackStream.started = false;
				GodotPackStream.pack = null;
				GodotPackStream.path = null;
				GodotPackStream.entries.length = 0;
				GodotPackStream.queued.length = 0;
				GodotPackStream.ready.length = 0;
				GodotPackStream.slots = 0;
				GodotPackStream.slot_bytes = 0;
				GodotPackStream.publish();
			}
		},

		// First slab whose starting byte is greater than the requested offset.
		upper_bound: function (slabs, offset) {
			let lo = 0;
			let hi = slabs.length;
			while (lo < hi) {
				const mid = Math.floor((lo + hi) / 2);
				if (slabs[mid]['start'] <= offset) {
					lo = mid + 1;
				} else {
					hi = mid;
				}
			}
			return lo;
		},

		read: function (offset, dst, length) {
			const slabs = GodotPackStream.pack['slabs'];
			const first = GodotPackStream.upper_bound(slabs, offset) - 1;
			const end = offset + length;
			let pos = offset;
			let last = first;
			// Validate the entire interval before writing into WASM.
			while (pos < end) {
				const slab = slabs[last++];
				if (!slab || slab['start'] > pos || slab['start'] + slab['bytes'].length <= pos) {
					return 0;
				}
				pos = slab['start'] + slab['bytes'].length;
			}
			pos = offset;
			for (let i = first; i < last; ++i) {
				const slab = slabs[i];
				const bytes = slab['bytes'].subarray(pos - slab['start'], end - slab['start']);
				HEAPU8.set(bytes, dst + pos - offset);
				pos += bytes.length;
			}
			return 1;
		},

		request_full: function (index) {
			const entry = GodotPackStream.entries[index];
			if (entry.state === 'idle') {
				entry.state = 'queued';
				GodotPackStream.queued.push(index);
				GodotPackStream.stats['total'] += 1;
				GodotPackStream.stats['bytesTotal'] += entry.data['tailStart'] - entry.data['headEnd'];
				GodotPackStream.publish();
				GodotPackStream.pump();
			}
		},

		pump: function () {
			if (!GodotPackStream.started) {
				return;
			}
			const pack = GodotPackStream.pack;
			const controller = GodotPackStream.controller;
			while (GodotPackStream.slots < 4 && GodotPackStream.queued.length) {
				const index = GodotPackStream.queued[0];
				const entry = GodotPackStream.entries[index];
				const length = entry.data['tailStart'] - entry.data['headEnd'];
				if (GodotPackStream.slots && GodotPackStream.slot_bytes + length > 64 * 1024 * 1024) {
					return;
				}
				GodotPackStream.queued.shift();
				GodotPackStream.slots += 1; // A ready result holds its slot until C++ consumes it.
				GodotPackStream.slot_bytes += length;
				entry.state = 'fetching';
				const start = entry.data['offset'] + entry.data['headEnd'];
				pack['fetchRange'](start, start + length, function (count) {
					if (!controller.signal.aborted) {
						GodotPackStream.stats['bytesLoaded'] += count;
						GodotPackStream.publish();
					}
				}, controller.signal).then(function (bytes) {
					if (!controller.signal.aborted) {
						const slabs = pack['slabs'];
						entry.middle = { 'start': start, 'bytes': bytes };
						slabs.splice(GodotPackStream.upper_bound(slabs, start), 0, entry.middle);
						entry.state = 'ready';
						GodotPackStream.ready.push(index);
						GodotPackStream.publish();
					}
				}, function (error) {
					if (!controller.signal.aborted) {
						console.error(`Failed to stream texture '${entry.data['path']}': ${error.message}`);
						entry.state = 'ready';
						GodotPackStream.stats['failed'] += 1;
						GodotPackStream.ready.push(index);
						GodotPackStream.publish();
					}
				});
			}
		},

		evict: function (index) {
			const entry = GodotPackStream.entries[index];
			if (entry.middle) {
				const slabs = GodotPackStream.pack['slabs'];
				slabs.splice(GodotPackStream.upper_bound(slabs, entry.middle['start']) - 1, 1);
				entry.middle = null;
				GodotPackStream.stats['completed'] += 1;
				GodotPackStream.publish();
			}
			entry.state = 'idle';
			GodotPackStream.slots -= 1;
			GodotPackStream.slot_bytes -= entry.data['tailStart'] - entry.data['headEnd'];
			GodotPackStream.pump();
		},
	},

	godot_js_pck_get_size__proxy: 'sync',
	godot_js_pck_get_size__sig: 'd',
	godot_js_pck_get_size: function () {
		return GodotPackStream.pack ? GodotPackStream.pack['size'] : 0;
	},

	godot_js_pck_path_size__proxy: 'sync',
	godot_js_pck_path_size__sig: 'i',
	godot_js_pck_path_size: function () {
		return GodotPackStream.path.length + 1;
	},

	godot_js_pck_get_path__proxy: 'sync',
	godot_js_pck_get_path__sig: 'vii',
	godot_js_pck_get_path: function (pathPtr, pathSize) {
		GodotRuntime.stringToHeap(GodotPackStream.pack['path'], pathPtr, pathSize);
	},

	godot_js_pck_read__proxy: 'sync',
	godot_js_pck_read__sig: 'idii',
	godot_js_pck_read: function (offset, dst, length) {
		return GodotPackStream.read(offset, dst >>> 0, length >>> 0);
	},

	godot_js_pck_entry_path_size__proxy: 'sync',
	godot_js_pck_entry_path_size__sig: 'ii',
	godot_js_pck_entry_path_size: function (index) {
		return index < GodotPackStream.entries.length ? GodotPackStream.entries[index].path.length + 1 : 0;
	},

	godot_js_pck_get_entry__proxy: 'sync',
	godot_js_pck_get_entry__sig: 'viiiii',
	godot_js_pck_get_entry: function (index, pathPtr, pathSize, rangesPtr, md5Ptr) {
		const entry = GodotPackStream.entries[index];
		HEAPU8.set(entry.path, pathPtr >>> 0);
		HEAPU8[(pathPtr >>> 0) + pathSize - 1] = 0;
		const ranges = rangesPtr >>> 3;
		HEAPF64[ranges] = entry.data['offset'];
		HEAPF64[ranges + 1] = entry.data['size'];
		HEAPF64[ranges + 2] = entry.data['headEnd'];
		HEAPF64[ranges + 3] = entry.data['tailStart'];
		HEAPU8.set(entry.data['md5'], md5Ptr >>> 0);
	},

	godot_js_pck_request_full__proxy: 'sync',
	godot_js_pck_request_full__sig: 'vi',
	godot_js_pck_request_full: function (index) {
		GodotPackStream.request_full(index);
	},

	godot_js_pck_poll_ready__proxy: 'sync',
	godot_js_pck_poll_ready__sig: 'ii',
	godot_js_pck_poll_ready: function (successPtr) {
		if (!GodotPackStream.ready.length) {
			return -1;
		}
		const index = GodotPackStream.ready.shift();
		HEAP32[successPtr >>> 2] = GodotPackStream.entries[index].middle ? 1 : 0;
		return index;
	},

	godot_js_pck_evict_middle__proxy: 'sync',
	godot_js_pck_evict_middle__sig: 'vi',
	godot_js_pck_evict_middle: function (index) {
		GodotPackStream.evict(index);
	},
};

autoAddDeps(GodotPackStream, '$GodotPackStream');
mergeInto(LibraryManager.library, GodotPackStream);

const GodotOS = {
	$GodotOS__deps: ['$GodotRuntime', '$GodotConfig', '$GodotFS', '$GodotPackStream'],
	$GodotOS__postset: [
		'Module["request_quit"] = function() { GodotOS.request_quit() };',
		'Module["onExit"] = GodotOS.cleanup;',
		'GodotOS._fs_sync_promise = Promise.resolve();',
	].join(''),
	$GodotOS: {
		request_quit: function () {},
		_async_cbs: [],
		_fs_sync_promise: null,

		atexit: function (p_promise_cb) {
			GodotOS._async_cbs.push(p_promise_cb);
		},

		cleanup: function (exit_code) {
			const cb = GodotConfig.on_exit;
			GodotPackStream.stop();
			GodotFS.deinit();
			GodotConfig.clear();
			if (cb) {
				cb(exit_code);
			}
		},

		finish_async: function (callback) {
			GodotOS._fs_sync_promise.then(function (err) {
				const promises = [];
				GodotOS._async_cbs.forEach(function (cb) {
					promises.push(new Promise(cb));
				});
				return Promise.all(promises);
			}).then(function () {
				return GodotFS.sync(); // Final FS sync.
			}).then(function (err) {
				// Always deferred.
				setTimeout(function () {
					callback();
				}, 0);
			});
		},
	},

	godot_js_os_report_progress__proxy: 'sync',
	godot_js_os_report_progress__sig: 'vfi',
	godot_js_os_report_progress: function (ratio, label) {
		if (GodotConfig.on_startup_progress) {
			GodotConfig.on_startup_progress(ratio, GodotRuntime.parseString(label));
		}
	},

	godot_js_os_first_frame_ready__proxy: 'sync',
	godot_js_os_first_frame_ready__sig: 'vi',
	godot_js_os_first_frame_ready: function (keepRunning) {
		if (GodotConfig.on_first_frame_ready) {
			GodotConfig.on_first_frame_ready();
		}
		if (keepRunning) {
			GodotPackStream.start();
		}
	},

	godot_js_os_startup_exit__proxy: 'sync',
	godot_js_os_startup_exit__sig: 'vii',
	godot_js_os_startup_exit: function (exitCode, message) {
		if (GodotConfig.on_startup_exit) {
			GodotConfig.on_startup_exit(exitCode, GodotRuntime.parseString(message));
		}
	},

	godot_js_os_finish_async__proxy: 'sync',
	godot_js_os_finish_async__sig: 'vi',
	godot_js_os_finish_async: function (p_callback) {
		const func = GodotRuntime.get_func(p_callback);
		GodotOS.finish_async(func);
	},

	godot_js_os_request_quit_cb__proxy: 'sync',
	godot_js_os_request_quit_cb__sig: 'vi',
	godot_js_os_request_quit_cb: function (p_callback) {
		GodotOS.request_quit = GodotRuntime.get_func(p_callback);
	},

	godot_js_os_fs_is_persistent__proxy: 'sync',
	godot_js_os_fs_is_persistent__sig: 'i',
	godot_js_os_fs_is_persistent: function () {
		return GodotFS.is_persistent();
	},

	godot_js_os_fs_sync__proxy: 'sync',
	godot_js_os_fs_sync__sig: 'vi',
	godot_js_os_fs_sync: function (callback) {
		const func = GodotRuntime.get_func(callback);
		GodotOS._fs_sync_promise = GodotFS.sync();
		GodotOS._fs_sync_promise.then(function (err) {
			func();
		});
	},

	godot_js_os_has_feature__proxy: 'sync',
	godot_js_os_has_feature__sig: 'ii',
	godot_js_os_has_feature: function (p_ftr) {
		const ftr = GodotRuntime.parseString(p_ftr);
		const ua = navigator.userAgent;
		if (ftr === 'web_macos') {
			return (ua.indexOf('Mac') !== -1) ? 1 : 0;
		}
		if (ftr === 'web_windows') {
			return (ua.indexOf('Windows') !== -1) ? 1 : 0;
		}
		if (ftr === 'web_android') {
			return (ua.indexOf('Android') !== -1) ? 1 : 0;
		}
		if (ftr === 'web_ios') {
			return ((ua.indexOf('iPhone') !== -1) || (ua.indexOf('iPad') !== -1) || (ua.indexOf('iPod') !== -1)) ? 1 : 0;
		}
		if (ftr === 'web_linuxbsd') {
			return ((ua.indexOf('CrOS') !== -1) || (ua.indexOf('BSD') !== -1) || (ua.indexOf('Linux') !== -1) || (ua.indexOf('X11') !== -1)) ? 1 : 0;
		}
		return 0;
	},

	godot_js_os_execute__proxy: 'sync',
	godot_js_os_execute__sig: 'ii',
	godot_js_os_execute: function (p_json) {
		const json_args = GodotRuntime.parseString(p_json);
		const args = JSON.parse(json_args);
		if (GodotConfig.on_execute) {
			GodotConfig.on_execute(args);
			return 0;
		}
		return 1;
	},

	godot_js_os_shell_open__proxy: 'sync',
	godot_js_os_shell_open__sig: 'vi',
	godot_js_os_shell_open: function (p_uri) {
		window.open(GodotRuntime.parseString(p_uri), '_blank');
	},

	godot_js_os_hw_concurrency_get__proxy: 'sync',
	godot_js_os_hw_concurrency_get__sig: 'i',
	godot_js_os_hw_concurrency_get: function () {
		// TODO Godot core needs fixing to avoid spawning too many threads (> 24).
		const concurrency = navigator.hardwareConcurrency || 1;
		return concurrency < 2 ? concurrency : 2;
	},

	godot_js_os_thread_pool_size_get__proxy: 'sync',
	godot_js_os_thread_pool_size_get__sig: 'i',
	godot_js_os_thread_pool_size_get: function () {
		if (typeof PThread === 'undefined') {
			// Threads aren't supported, so default to `1`.
			return 1;
		}

		return GodotConfig.godot_pool_size;
	},

	godot_js_os_download_buffer__proxy: 'sync',
	godot_js_os_download_buffer__sig: 'viiii',
	godot_js_os_download_buffer: function (p_ptr, p_size, p_name, p_mime) {
		const buf = GodotRuntime.heapSlice(HEAP8, p_ptr, p_size);
		const name = GodotRuntime.parseString(p_name);
		const mime = GodotRuntime.parseString(p_mime);
		const blob = new Blob([buf], { type: mime });
		const url = window.URL.createObjectURL(blob);
		const a = document.createElement('a');
		a.href = url;
		a.download = name;
		a.style.display = 'none';
		document.body.appendChild(a);
		a.click();
		a.remove();
		window.URL.revokeObjectURL(url);
	},
};

autoAddDeps(GodotOS, '$GodotOS');
mergeInto(LibraryManager.library, GodotOS);

/*
 * Godot event listeners.
 * Keeps track of registered event listeners so it can remove them on shutdown.
 */
const GodotEventListeners = {
	$GodotEventListeners__deps: ['$GodotOS'],
	$GodotEventListeners__postset: 'GodotOS.atexit(function(resolve, reject) { GodotEventListeners.clear(); resolve(); });',
	$GodotEventListeners: {
		handlers: [],

		has: function (target, event, method, capture) {
			return GodotEventListeners.handlers.findIndex(function (e) {
				return e.target === target && e.event === event && e.method === method && e.capture === capture;
			}) !== -1;
		},

		add: function (target, event, method, capture) {
			if (GodotEventListeners.has(target, event, method, capture)) {
				return;
			}
			function Handler(p_target, p_event, p_method, p_capture) {
				this.target = p_target;
				this.event = p_event;
				this.method = p_method;
				this.capture = p_capture;
			}
			GodotEventListeners.handlers.push(new Handler(target, event, method, capture));
			target.addEventListener(event, method, capture);
		},

		clear: function () {
			GodotEventListeners.handlers.forEach(function (h) {
				h.target.removeEventListener(h.event, h.method, h.capture);
			});
			GodotEventListeners.handlers.length = 0;
		},
	},
};
mergeInto(LibraryManager.library, GodotEventListeners);

const GodotPWA = {

	$GodotPWA__deps: ['$GodotRuntime', '$GodotEventListeners'],
	$GodotPWA: {
		hasUpdate: false,

		updateState: function (cb, reg) {
			if (!reg) {
				return;
			}
			if (!reg.active) {
				return;
			}
			if (reg.waiting) {
				GodotPWA.hasUpdate = true;
				cb();
			}
			GodotEventListeners.add(reg, 'updatefound', function () {
				const installing = reg.installing;
				GodotEventListeners.add(installing, 'statechange', function () {
					if (installing.state === 'installed') {
						GodotPWA.hasUpdate = true;
						cb();
					}
				});
			});
		},
	},

	godot_js_pwa_cb__proxy: 'sync',
	godot_js_pwa_cb__sig: 'vi',
	godot_js_pwa_cb: function (p_update_cb) {
		if ('serviceWorker' in navigator) {
			try {
				const cb = GodotRuntime.get_func(p_update_cb);
				navigator.serviceWorker.getRegistration().then(GodotPWA.updateState.bind(null, cb));
			} catch (e) {
				GodotRuntime.error('Failed to assign PWA callback', e);
			}
		}
	},

	godot_js_pwa_update__proxy: 'sync',
	godot_js_pwa_update__sig: 'i',
	godot_js_pwa_update: function () {
		if ('serviceWorker' in navigator && GodotPWA.hasUpdate) {
			try {
				navigator.serviceWorker.getRegistration().then(function (reg) {
					if (!reg || !reg.waiting) {
						return;
					}
					reg.waiting.postMessage('update');
				});
			} catch (e) {
				GodotRuntime.error(e);
				return 1;
			}
			return 0;
		}
		return 1;
	},
};

autoAddDeps(GodotPWA, '$GodotPWA');
mergeInto(LibraryManager.library, GodotPWA);
