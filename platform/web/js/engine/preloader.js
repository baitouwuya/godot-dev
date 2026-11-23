const Preloader = /** @constructor */ function () { // eslint-disable-line no-unused-vars
	const STREAM_MIN_BYTES = 128 * 1024;
	const HEAD_BYTES = 512;
	const TAIL_BYTES = 96 * 1024;
	const COALESCE_GAP = 64 * 1024;
	const BOOT_CHUNK_BYTES = 8 * 1024 * 1024;
	const MAX_DIRECTORY_BYTES = 64 * 1024 * 1024;
	const MAX_IMAGE_PIXELS = 16384 * 16384;
	const DOWNLOAD_CONCURRENCY = 4;

	function readU64(view, offset) {
		const n = view.getUint32(offset, true) + 0x100000000 * view.getUint32(offset + 4, true);
		if (!Number.isSafeInteger(n)) {
			throw new Error('PCK offset exceeds the exact integer range');
		}
		return n;
	}

	function simplifyResourcePath(path) {
		const parts = [];
		for (const item of path.replace(/\\/g, '/').replace(/^res:\/\//, '').split('/')) {
			if (item === '..') {
				if (!parts.length) {
					throw new Error('PCK entry escapes its resource root');
				}
				parts.pop();
			} else if (item && item !== '.') {
				parts.push(item);
			}
		}
		return parts.join('/');
	}

	function parseDirectory(bytes, fileBase, directoryStart, directoryEnd, total, progressive) {
		const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
		const count = view.getUint32(0, true);
		const decoder = new TextDecoder('utf-8', { 'fatal': true, 'ignoreBOM': true });
		const paths = new Set();
		const candidates = [];
		const ranges = [];
		let pos = 4;
		let firstData = total;
		if (count > Math.floor((bytes.length - 4) / 41)) {
			throw new Error('Invalid PCK entry count');
		}
		for (let i = 0; i < count; ++i) {
			const pathLength = view.getUint32(pos, true);
			pos += 4;
			if (!pathLength || pathLength > bytes.length - pos - 36) {
				throw new Error('Invalid PCK entry path length');
			}
			const name = decoder.decode(bytes.subarray(pos, pos + pathLength)).replace(/\0+$/, '');
			const path = simplifyResourcePath(name);
			pos += pathLength;
			const offset = fileBase + readU64(view, pos);
			const size = readU64(view, pos + 8);
			const flags = view.getUint32(pos + 32, true);
			const md5 = bytes.subarray(pos + 16, pos + 32);
			pos += 36;
			if (!path || name.includes('\0') || flags & ~2) {
				throw new Error('PCK contains an encrypted, delta, or unsupported entry');
			}
			if (progressive && paths.has(path)) {
				throw new Error('PCK has overlapping path definitions');
			}
			paths.add(path);
			if (flags & 2) { // PACK_FILE_REMOVAL, resolved by the native PCK parser.
				continue;
			}
			if (!Number.isSafeInteger(offset) || offset < 64 || offset > total || size > total - offset
				|| (size > 0 && offset < directoryEnd && offset + size > directoryStart)) {
				throw new Error('PCK entry is outside its data region');
			}
			if (size > 0) {
				firstData = Math.min(firstData, offset);
				if (progressive) {
					ranges.push([offset, offset + size]);
				}
			}
			if (progressive && size > STREAM_MIN_BYTES && size - HEAD_BYTES - TAIL_BYTES > COALESCE_GAP && /\.(ctex|dds)$/i.test(path)
				&& md5.some(function (byte) { return byte !== 0; })) {
				candidates.push({ 'path': `res://${path}`, 'offset': offset, 'size': size, 'headEnd': HEAD_BYTES, 'tailStart': size - TAIL_BYTES, 'md5': md5 });
			}
		}
		if (directoryStart < firstData && directoryStart + pos > firstData) {
			throw new Error('PCK directory overlaps file data');
		}
		if (candidates.length) {
			// Check candidates against every entry, including aliases with different extensions.
			ranges.sort(function (a, b) { return a[0] - b[0]; });
			for (let i = 1; i < ranges.length; ++i) {
				if (ranges[i - 1][1] > ranges[i][0]) {
					return []; // Alias entries can be served safely from the fully resident pack.
				}
			}
		}
		return candidates;
	}

	// Indexed by serialized Godot Image::Format. Positive values are bytes per pixel;
	// negative values are bytes per 4x4 block for BC/ETC formats.
	const CTEX_LAYOUTS = [1, 2, 1, 2, 3, 4, 2, 2, 4, 8, 12, 16, 2, 4, 6, 8, 4,
		-8, -16, -16, -8, -16, -16, -16, -16, -8, -8, -8, -16, -16, -8, -16, -8, -16, -16];
	const DDS_BC = { 'DXT1': -8, 'DXT3': -16, 'DXT5': -16, 'ATI1': -8, 'ATI2': -16, 'BC4U': -8, 'BC5U': -16 };
	const DDS_DXGI = {
		2: 16, 10: 8, 11: 8, 12: 8, 16: 8, 24: 4, 28: 4, 29: 4, 34: 4, 35: 4, 36: 4,
		41: 4, 49: 2, 54: 2, 56: 2, 57: 2, 61: 1, 65: 1, 67: 4,
		71: -8, 72: -8, 74: -16, 75: -16, 77: -16, 78: -16, 80: -8, 83: -16,
		87: 4, 95: -16, 96: -16, 98: -16, 99: -16,
	};

	function mipSize(width, height, layout) {
		return layout > 0 ? width * height * layout : Math.ceil(width / 4) * Math.ceil(height / 4) * -layout;
	}

	function hasCompleteTail(width, height, levels, layout, payload, entry) {
		if (!layout || !width || !height || width > 65535 || height > 65535 || width * height > MAX_IMAGE_PIXELS
			|| levels < 2 || levels !== 1 + Math.floor(Math.log2(Math.max(width, height)))) {
			return false;
		}
		let pos = payload;
		let foundTail = false;
		for (let i = 0; i < levels; ++i) {
			if (!foundTail && pos >= entry['tailStart']) {
				// A standalone compressed base level must be block-aligned.
				if (layout < 0 && (width % 4 || height % 4)) {
					return false;
				}
				foundTail = true;
			}
			pos += mipSize(width, height, layout);
			width = Math.max(1, width >>> 1);
			height = Math.max(1, height >>> 1);
		}
		return foundTail && pos === entry['size'];
	}

	function isStreamable(bytes, entry) {
		const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
		const u32 = function (offset) { return view.getUint32(offset, true); };
		if (/\.ctex$/i.test(entry['path'])) {
			return u32(0) === 0x32545347 && u32(4) <= 1 && u32(36) === 0
				&& hasCompleteTail(view.getUint16(40, true), view.getUint16(42, true), u32(44) + 1, CTEX_LAYOUTS[u32(48)], 52, entry);
		}
		const flags = u32(8);
		if (u32(0) !== 0x20534444 || u32(4) !== 124 || !(flags & 0x20000)
			|| (flags & 0x800000) || u32(24) > 1 || (u32(112) & 0x20FE00) || u32(76) !== 32 || !(u32(80) & 4)) {
			return false;
		}
		const fourcc = String.fromCharCode(...bytes.subarray(84, 88));
		let layout = DDS_BC[fourcc];
		let payload = 128;
		if (fourcc === 'DX10') {
			if (u32(132) !== 3 || (u32(136) & 4) || u32(140) !== 1) {
				return false;
			}
			layout = DDS_DXGI[u32(128)];
			payload = 148;
		}
		const width = u32(16);
		const height = u32(12);
		const pitch = u32(20);
		if (layout < 0 && (width % 4 || height % 4 || pitch !== ((flags & 0x80000) ? mipSize(width, height, layout) : 0))) {
			return false;
		}
		if (layout > 0 && ((flags & 0x80000) || pitch !== ((flags & 8) ? width * layout : 0))) {
			return false;
		}
		return hasCompleteTail(width, height, u32(28), layout, payload, entry);
	}

	function parseRange(response, start, end) {
		const header = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(response.headers.get('Content-Range') || '');
		const encoding = response.headers.get('Content-Encoding');
		if (response.status !== 206 || !header || Number(header[1]) !== start || Number(header[2]) !== end - 1
			|| !Number.isSafeInteger(Number(header[3])) || Number(header[3]) < end || (encoding && encoding !== 'identity')) {
			throw new Error(`Expected an identity HTTP 206 for bytes ${start}-${end - 1}`);
		}
		return Number(header[3]);
	}

	async function readRangeBody(response, size, progress) {
		const bytes = new Uint8Array(size);
		const reader = response.body.getReader();
		let written = 0;
		try {
			while (true) {
				const item = await reader.read();
				if (item.done) {
					break;
				}
				const chunk = item.value;
				if (chunk.length > size - written) {
					await reader.cancel();
					throw new Error('PCK range response is longer than advertised');
				}
				bytes.set(chunk, written);
				written += chunk.length;
				progress(chunk.length);
			}
		} finally {
			reader.releaseLock();
		}
		if (written !== size) {
			throw new Error('Truncated PCK range response');
		}
		return bytes;
	}

	function addChunks(chunks, start, end) {
		while (start < end) {
			const next = Math.min(start + BOOT_CHUNK_BYTES, end);
			chunks.push([start, next]);
			start = next;
		}
	}

	function packChunks(total, slabs, candidates) {
		const skipped = slabs.map(function (slab) { return [slab['start'], slab['start'] + slab['bytes'].length]; });
		for (const entry of candidates) {
			skipped.push([entry['offset'] + entry['headEnd'], entry['offset'] + entry['tailStart']]);
		}
		// Coalesce everything between large texture holes, including alignment padding.
		skipped.sort(function (a, b) { return a[0] - b[0]; });
		const chunks = [];
		let pos = 0;
		for (const [start, end] of skipped) {
			addChunks(chunks, pos, start);
			pos = Math.max(pos, end);
		}
		addChunks(chunks, pos, total);
		return chunks;
	}

	function readHeader(slabs, offset) {
		const bytes = new Uint8Array(HEAD_BYTES);
		let written = 0;
		for (const slab of slabs) {
			const pos = offset + written - slab['start'];
			if (pos >= 0 && pos < slab['bytes'].length) {
				const chunk = slab['bytes'].subarray(pos, pos + HEAD_BYTES - written);
				bytes.set(chunk, written);
				written += chunk.length;
				if (written === HEAD_BYTES) {
					return bytes;
				}
			}
		}
		throw new Error('Missing a planned PCK texture header');
	}

	function getTrackedResponse(response, load_status) {
		const reader = response.body.getReader();
		return new Response(new ReadableStream({
			pull: function (controller) {
				return reader.read().then(function (result) {
					if (load_status.done) {
						return; // The stream was canceled while a read was outstanding.
					}
					if (result.done) {
						load_status.done = true;
						load_status.total = load_status.loaded;
						controller.close();
					} else {
						load_status.loaded += result.value.length;
						controller.enqueue(result.value);
					}
				}, function (error) {
					load_status.done = true;
					throw error;
				});
			},
			cancel: function (reason) {
				load_status.done = true;
				return reader.cancel(reason);
			},
		}), { headers: response.headers });
	}

	function loadFetch(file, tracker, fileSize, raw) {
		const status = tracker[file] = {
			total: fileSize || 0,
			loaded: 0,
			done: false,
		};
		return fetch(file).then(function (response) {
			if (!response.ok) {
				return Promise.reject(new Error(`Failed loading file '${file}'`));
			}
			if (!status.total && !response.headers.get('Content-Encoding')) {
				status.total = Number(response.headers.get('Content-Length')) || 0;
			}
			const tr = getTrackedResponse(response, status);
			if (raw) {
				return Promise.resolve(tr);
			}
			return tr.arrayBuffer();
		}).catch(function (error) {
			status.done = true;
			throw error;
		});
	}

	function retry(func, attempts = 1) {
		function onerror(err) {
			if (attempts <= 1) {
				return Promise.reject(err);
			}
			return new Promise(function (resolve, reject) {
				setTimeout(function () {
					retry(func, attempts - 1).then(resolve).catch(reject);
				}, 1000);
			});
		}
		return func().catch(onerror);
	}

	const DOWNLOAD_ATTEMPTS_MAX = 4;
	const loadingFiles = Object.create(null);
	const lastProgress = { loaded: -1, total: -1 };
	let progressFunc = null;
	let progressFrame = 0;

	const animateProgress = function () {
		progressFrame = 0;
		let loaded = 0;
		let total = 0;
		let totalIsValid = true;
		let progressIsFinal = true;
		let knownLoaded = 0;
		let knownTotal = 0;

		Object.keys(loadingFiles).forEach(function (file) {
			const stat = loadingFiles[file];
			if (!stat.done) {
				progressIsFinal = false;
			}
			if (stat.total > 0) {
				knownLoaded += stat.loaded;
				knownTotal += stat.total;
			}
			if (!totalIsValid || stat.total === 0) {
				totalIsValid = false;
				total = 0;
			} else {
				total += stat.total;
			}
			loaded += stat.loaded;
		});
		if (!totalIsValid && knownTotal > 0) {
			loaded = knownLoaded;
			total = knownTotal;
		}
		if (loaded !== lastProgress.loaded || total !== lastProgress.total) {
			lastProgress.loaded = loaded;
			lastProgress.total = total;
			if (typeof progressFunc === 'function') {
				progressFunc(loaded, total);
			}
		}
		if (!progressIsFinal) {
			scheduleProgress();
		}
	};

	function scheduleProgress() {
		if (!progressFrame) {
			progressFrame = requestAnimationFrame(animateProgress);
		}
	}

	this.setProgressFunc = function (callback) {
		progressFunc = callback;
		lastProgress.loaded = -1;
		scheduleProgress();
	};

	this.getFileStatus = function (file) {
		return loadingFiles[file] || null;
	};

	this.loadPromise = function (file, fileSize, raw = false) {
		scheduleProgress();
		return retry(loadFetch.bind(null, file, loadingFiles, fileSize, raw), DOWNLOAD_ATTEMPTS_MAX);
	};

	this.loadPack = async function (path, progressive) {
		const status = { total: 0, loaded: 0, done: false, streamed: true };
		loadingFiles[path] = status;
		scheduleProgress();
		const controller = new AbortController();
		const progress = function (count) { status.loaded += count; };
		const slabs = [];
		try {
			const probe = await fetch(path, { 'headers': { 'Range': 'bytes=0-63' }, 'signal': controller.signal });
			if (probe.status !== 206) {
				return null;
			}
			const total = parseRange(probe, 0, 64);
			const etag = probe.headers.get('ETag');
			const url = probe.url || path;
			if (!etag || !/^"[^"\r\n]*"$/.test(etag)) {
				throw new Error('Range streaming requires a strong ETag');
			}
			async function fetchRange(start, end, onBytes, signal) {
				const response = await fetch(url, {
					'headers': { 'Range': `bytes=${start}-${end - 1}`, 'If-Range': etag },
					'signal': signal,
					'redirect': 'error',
				});
				try {
					if (parseRange(response, start, end) !== total || response.headers.get('ETag') !== etag) {
						throw new Error('PCK representation changed during Range download');
					}
				} catch (error) {
					if (response.body) {
						await response.body.cancel();
					}
					throw error;
				}
				return readRangeBody(response, end - start, onBytes);
			}
			const header = await readRangeBody(probe, 64, progress);
			const view = new DataView(header.buffer);
			const version = view.getUint32(4, true);
			const flags = view.getUint32(20, true);
			if (view.getUint32(0, true) !== 0x43504447 || version < 2 || version > 4 || (flags & ~2)) {
				throw new Error('Unrecognized, encrypted, or sparse PCK header');
			}
			const base = readU64(view, 24);
			const dir = version === 2 ? 96 : readU64(view, 32);
			const dirEnd = version === 2 ? base : total;
			const fetchStart = version === 2 ? 64 : dir;
			if (dir < 64 || dir > dirEnd - 4 || dirEnd > total || dirEnd - fetchStart > MAX_DIRECTORY_BYTES) {
				throw new Error('PCK directory bounds are not streamable');
			}
			slabs.push({ 'start': 0, 'bytes': header });
			const directory = await fetchRange(fetchStart, dirEnd, progress, controller.signal);
			slabs.push({ 'start': fetchStart, 'bytes': directory });
			const candidates = parseDirectory(directory.subarray(dir - fetchStart), base, dir, dirEnd, total, await progressive());
			const boot = packChunks(total, slabs, candidates);
			status.total = status.loaded;
			async function download(chunks) {
				for (const [start, end] of chunks) {
					status.total += end - start;
				}
				let next = 0;
				async function downloadSlabs() {
					while (next < chunks.length) {
						const [start, end] = chunks[next++];
						const bytes = await fetchRange(start, end, progress, controller.signal);
						slabs.push({ 'start': start, 'bytes': bytes });
					}
				}
				await Promise.all(Array.from({ 'length': Math.min(DOWNLOAD_CONCURRENCY, chunks.length) }, downloadSlabs));
				slabs.sort(function (a, b) { return a['start'] - b['start']; });
			}
			await download(boot);
			const entries = [];
			const missing = [];
			for (const entry of candidates) {
				if (isStreamable(readHeader(slabs, entry['offset']), entry)) {
					entries.push(entry);
				} else {
					addChunks(missing, entry['offset'] + entry['headEnd'], entry['offset'] + entry['tailStart']);
				}
			}
			await download(missing);
			return { 'path': path, 'size': total, 'slabs': slabs, 'entries': entries, 'fetchRange': fetchRange };
		} catch (error) {
			console.warn(`Range streaming unavailable for '${path}': ${error.message}. Loading the complete pack.`);
			return null;
		} finally {
			controller.abort();
			status.done = true;
		}
	};

	this.preloadedFiles = [];
	this.preload = function (pathOrBuffer, destPath, fileSize) {
		let buffer = null;
		if (typeof pathOrBuffer === 'string') {
			const me = this;
			return this.loadPromise(pathOrBuffer, fileSize).then(function (buf) {
				me.preloadedFiles.push({
					path: destPath || pathOrBuffer,
					buffer: buf,
					owned: true,
				});
				return Promise.resolve();
			});
		} else if (pathOrBuffer instanceof ArrayBuffer) {
			buffer = new Uint8Array(pathOrBuffer);
		} else if (ArrayBuffer.isView(pathOrBuffer)) {
			buffer = new Uint8Array(pathOrBuffer.buffer, pathOrBuffer.byteOffset, pathOrBuffer.byteLength);
		}
		if (buffer) {
			this.preloadedFiles.push({
				path: destPath,
				buffer: buffer,
				owned: false,
			});
			return Promise.resolve();
		}
		return Promise.reject(new Error('Invalid object for preloading'));
	};
};
