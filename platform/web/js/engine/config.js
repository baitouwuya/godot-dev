/**
 * An object used to configure the Engine instance based on godot export options, and to override those in custom HTML
 * templates if needed.
 *
 * @header Engine configuration
 * @summary The Engine configuration object. This is just a typedef, create it like a regular object, e.g.:
 *
 * ``const MyConfig = { executable: 'godot', unloadAfterInit: false }``
 *
 * @typedef {Object} EngineConfig
 */
const EngineConfig = {}; // eslint-disable-line no-unused-vars

/**
 * @struct
 * @constructor
 * @ignore
 */
const InternalConfig = function (initConfig) { // eslint-disable-line no-unused-vars
	const cfg = /** @lends {InternalConfig.prototype} */ {
		/** @type {string} */
		renderingMethod: 'gl_compatibility',
		/** @type {string} */
		renderingDriver: '',
		/**
		 * Whether to unload the engine automatically after the instance is initialized.
		 *
		 * @memberof EngineConfig
		 * @default
		 * @type {boolean}
		 */
		unloadAfterInit: true,
		/**
		 * The HTML DOM Canvas object to use.
		 *
		 * By default, the first canvas element in the document will be used is none is specified.
		 *
		 * @memberof EngineConfig
		 * @default
		 * @type {?HTMLCanvasElement}
		 */
		canvas: null,
		/**
		 * The name of the WASM file without the extension. (Set by Godot Editor export process).
		 *
		 * @memberof EngineConfig
		 * @default
		 * @type {string}
		 */
		executable: '',
		/**
		 * An alternative name for the game pck to load. The executable name is used otherwise.
		 *
		 * @memberof EngineConfig
		 * @default
		 * @type {?string}
		 */
		mainPack: null,
		/** @type {boolean} Use Range requests for a compatible main pack. */
		streamPack: true,
		/**
		 * Opt in to partial texture residency on single-threaded exports.
		 * Leave this off if the application reads original texture bytes synchronously or applies texture delta packs at runtime.
		 * @type {boolean}
		 */
		progressiveTextureMips: false,
		/**
		 * Specify a language code to select the proper localization for the game.
		 *
		 * The browser locale will be used if none is specified. See complete list of
		 * :ref:`supported locales <doc_locales>`.
		 *
		 * @memberof EngineConfig
		 * @type {?string}
		 * @default
		 */
		locale: null,
		/**
		 * The canvas resize policy determines how the canvas should be resized by Godot.
		 *
		 * ``0`` means Godot won't do any resizing. This is useful if you want to control the canvas size from
		 * javascript code in your template.
		 *
		 * ``1`` means Godot will resize the canvas on start, and when changing window size via engine functions.
		 *
		 * ``2`` means Godot will adapt the canvas size to match the whole browser window.
		 *
		 * @memberof EngineConfig
		 * @type {number}
		 * @default
		 */
		canvasResizePolicy: 2,
		/**
		 * The arguments to be passed as command line arguments on startup.
		 *
		 * See :ref:`command line tutorial <doc_command_line_tutorial>`.
		 *
		 * **Note**: :js:meth:`startGame <Engine.prototype.startGame>` will always add the ``--main-pack`` argument.
		 *
		 * @memberof EngineConfig
		 * @type {Array<string>}
		 * @default
		 */
		args: [],
		/**
		 * When enabled, the game canvas will automatically grab the focus when the engine starts.
		 *
		 * @memberof EngineConfig
		 * @type {boolean}
		 * @default
		 */
		focusCanvas: true,
		/**
		 * When enabled, this will turn on experimental virtual keyboard support on mobile.
		 *
		 * @memberof EngineConfig
		 * @type {boolean}
		 * @default
		 */
		experimentalVK: false,
		/**
		 * The progressive web app service worker to install.
		 * @memberof EngineConfig
		 * @default
		 * @type {string}
		 */
		serviceWorker: '',
		/**
		 * @ignore
		 * @type {Array.<string>}
		 */
		persistentPaths: ['/userfs'],
		/**
		 * @ignore
		 * @type {boolean}
		 */
		persistentDrops: false,
		/**
		 * @ignore
		 * @type {Array.<string>}
		 */
		gdextensionLibs: [],
		/**
		 * @ignore
		 * @type {Array.<string>}
		 */
		fileSizes: [],
		/**
		 * @ignore
		 * @type {number}
		 */
		emscriptenPoolSize: 8,
		/**
		 * @ignore
		 * @type {number}
		 */
		godotPoolSize: 4,
		/**
		 * A callback function for handling Godot's ``OS.execute`` calls.
		 *
		 * This is for example used in the Web Editor template to switch between project manager and editor, and for running the game.
		 *
		 * @callback EngineConfig.onExecute
		 * @param {string} path The path that Godot's wants executed.
		 * @param {Array.<string>} args The arguments of the "command" to execute.
		 */
		/**
		 * @ignore
		 * @type {?function(string, Array.<string>)}
		 */
		onExecute: null,
		/**
		 * A callback function for being notified when the Godot instance quits.
		 *
		 * **Note**: This function will not be called if the engine crashes or become unresponsive.
		 *
		 * @callback EngineConfig.onExit
		 * @param {number} status_code The status code returned by Godot on exit.
		 */
		/**
		 * @ignore
		 * @type {?function(number)}
		 */
		onExit: null,
		/**
		 * A callback function for displaying startup progress.
		 *
		 * The function is called once per frame while downloading files, so the usage of ``requestAnimationFrame()``
		 * is not necessary.
		 *
		 * The first half tracks downloads with known totals. The second half reports engine initialization through the first iteration.
		 *
		 * @callback EngineConfig.onProgress
		 * @param {number} current Startup progress in normalized units.
		 * @param {number} total Total number of progress units.
		 */
		/**
		 * @ignore
		 * @type {?function(number, number)}
		 */
		onProgress: null,
		/**
		 * A callback function for handling the standard output stream. This method should usually only be used in debug pages.
		 *
		 * By default, ``console.log()`` is used.
		 *
		 * @callback EngineConfig.onPrint
		 * @param {...*} [var_args] A variadic number of arguments to be printed.
		 */
		/**
		 * @ignore
		 * @type {?function(...*)}
		 */
		onPrint: function () {
			console.log.apply(console, Array.from(arguments)); // eslint-disable-line no-console
		},
		/**
		 * A callback function for handling the standard error stream. This method should usually only be used in debug pages.
		 *
		 * By default, ``console.error()`` is used.
		 *
		 * @callback EngineConfig.onPrintError
		 * @param {...*} [var_args] A variadic number of arguments to be printed as errors.
		*/
		/**
		 * @ignore
		 * @type {?function(...*)}
		 */
		onPrintError: function (var_args) {
			console.error.apply(console, Array.from(arguments)); // eslint-disable-line no-console
		},
	};

	/**
	 * @ignore
	 * @struct
	 * @constructor
	 * @param {EngineConfig} opts
	 */
	function Config(opts) {
		this.update(opts);
	}

	Config.prototype = cfg;

	/**
	 * @ignore
	 * @param {EngineConfig} opts
	 */
	Config.prototype.update = function (opts) {
		const config = opts || {};
		// NOTE: We must explicitly pass the default, accessing it via
		// the key will fail due to closure compiler renames.
		function parse(key, def) {
			if (typeof (config[key]) === 'undefined') {
				return def;
			}
			return config[key];
		}
		// Module config
		this.unloadAfterInit = parse('unloadAfterInit', this.unloadAfterInit);
		this.onPrintError = parse('onPrintError', this.onPrintError);
		this.onPrint = parse('onPrint', this.onPrint);
		this.onProgress = parse('onProgress', this.onProgress);

		// Godot config
		this.canvas = parse('canvas', this.canvas);
		this.renderingMethod = parse('renderingMethod', this.renderingMethod);
		this.renderingDriver = parse('renderingDriver', this.renderingDriver);
		this.executable = parse('executable', this.executable);
		this.mainPack = parse('mainPack', this.mainPack);
		this.streamPack = parse('streamPack', this.streamPack);
		this.progressiveTextureMips = parse('progressiveTextureMips', this.progressiveTextureMips);
		this.locale = parse('locale', this.locale);
		this.canvasResizePolicy = parse('canvasResizePolicy', this.canvasResizePolicy);
		this.persistentPaths = parse('persistentPaths', this.persistentPaths);
		this.persistentDrops = parse('persistentDrops', this.persistentDrops);
		this.experimentalVK = parse('experimentalVK', this.experimentalVK);
		this.focusCanvas = parse('focusCanvas', this.focusCanvas);
		this.serviceWorker = parse('serviceWorker', this.serviceWorker);
		this.gdextensionLibs = parse('gdextensionLibs', this.gdextensionLibs);
		this.fileSizes = parse('fileSizes', this.fileSizes);
		this.emscriptenPoolSize = parse('emscriptenPoolSize', this.emscriptenPoolSize);
		this.godotPoolSize = parse('godotPoolSize', this.godotPoolSize);
		this.args = parse('args', this.args);
		this.onExecute = parse('onExecute', this.onExecute);
		this.onExit = parse('onExit', this.onExit);
	};

	// Minimums for the built-in corpus, including the Mobile lightmap/uber
	// variants. Per-pipeline validation still checks plans with user materials.
	const rdLimits = {
		// Forward+ also has the compositor's 48-sampled-texture floor. Its
		// octmap filter needs six storage textures (SSAO downsample needs five).
		'forward_plus': { 'maxSampledTexturesPerShaderStage': 48, 'maxSamplersPerShaderStage': 8, 'maxStorageBuffersPerShaderStage': 10, 'maxStorageTexturesPerShaderStage': 6, 'maxUniformBuffersPerShaderStage': 6 },
		'mobile': { 'maxSampledTexturesPerShaderStage': 29, 'maxSamplersPerShaderStage': 7, 'maxStorageBuffersPerShaderStage': 9, 'maxStorageTexturesPerShaderStage': 4, 'maxUniformBuffersPerShaderStage': 3 },
	};
	const requestedLimits = [
		'maxBindGroups', 'maxBindGroupsPlusVertexBuffers', 'maxBindingsPerBindGroup',
		'maxDynamicUniformBuffersPerPipelineLayout', 'maxDynamicStorageBuffersPerPipelineLayout',
		'maxSampledTexturesPerShaderStage', 'maxSamplersPerShaderStage', 'maxStorageBuffersPerShaderStage',
		'maxStorageTexturesPerShaderStage', 'maxUniformBuffersPerShaderStage', 'maxStorageBufferBindingSize',
		'maxUniformBufferBindingSize', 'maxBufferSize', 'maxComputeWorkgroupStorageSize', 'maxComputeInvocationsPerWorkgroup',
		'maxComputeWorkgroupSizeX', 'maxComputeWorkgroupSizeY', 'maxComputeWorkgroupSizeZ', 'maxComputeWorkgroupsPerDimension',
		'maxColorAttachments', 'maxColorAttachmentBytesPerSample', 'maxInterStageShaderVariables',
		'maxTextureDimension1D', 'maxTextureDimension2D', 'maxTextureDimension3D', 'maxTextureArrayLayers',
		'maxVertexBuffers', 'maxVertexAttributes', 'maxVertexBufferArrayStride',
		'minUniformBufferOffsetAlignment', 'minStorageBufferOffsetAlignment',
	];
	const optionalFeatures = [
		'timestamp-query', 'float32-filterable', 'float32-blendable', 'rg11b10ufloat-renderable',
		'depth32float-stencil8', 'depth-clip-control', 'clip-distances', 'dual-source-blending', 'subgroups',
		'texture-compression-bc', 'texture-compression-etc2', 'texture-compression-astc',
		'indirect-first-instance', 'shader-f16', 'bgra8unorm-storage',
		'texture-formats-tier1', 'texture-formats-tier2',
	];

	/** @ignore */
	Config.prototype.negotiateRendering = async function (templateDrivers) {
		const drivers = templateDrivers || ['opengl3']; // Old custom templates expose only WebGL2.
		let method = this.renderingMethod;
		let driver = this.renderingDriver;
		let methodArg = false;
		let driverArg = false;
		const args = [];
		for (let i = 0; i < this.args.length; i++) {
			const arg = this.args[i];
			if (arg === '--' || arg === '++') {
				args.push(...this.args.slice(i));
				break;
			}
			if (arg === '--rendering-method' && i + 1 < this.args.length) {
				method = this.args[++i];
				methodArg = true;
			} else if (arg === '--rendering-driver' && i + 1 < this.args.length) {
				driver = this.args[++i];
				driverArg = true;
			} else {
				args.push(arg);
			}
		}
		if (methodArg && !driverArg) {
			driver = ''; // A CLI method overrides the exported method/driver pair.
		} else if (driverArg && !methodArg) {
			method = driver === 'webgpu' ? 'forward_plus' : 'gl_compatibility';
		}
		if (driver === 'dummy' || method === 'dummy' || args.includes('--headless')) {
			return { args: this.args, state: { 'requestedMethod': method, 'method': 'dummy', 'driver': 'dummy', 'reason': '' }, device: null };
		}
		function selected(chosenMethod, chosenDriver, reason, device) {
			return {
				args: ['--rendering-driver', chosenDriver, '--rendering-method', chosenMethod, ...args],
				state: { 'requestedMethod': method, 'method': chosenMethod, 'driver': chosenDriver, 'reason': reason },
				device: device,
			};
		}
		if ((driver === 'opengl3' || method === 'gl_compatibility') && drivers.includes('opengl3')) {
			return selected('gl_compatibility', 'opengl3', '', null);
		}
		let reason = 'This template does not include WebGPU.';
		const gpu = navigator['gpu'];
		if (drivers.includes('webgpu') && gpu) {
			try {
				const adapter = await gpu['requestAdapter']();
				if (!adapter) {
					throw new Error('No WebGPU adapter is available.');
				}
				const limits = {};
				for (const name of requestedLimits) {
					if (Number.isSafeInteger(adapter['limits'][name])) {
						limits[name] = adapter['limits'][name];
					}
				}
				const features = optionalFeatures.filter((feature) => adapter['features']['has'](feature));
				const device = await adapter['requestDevice']({ 'requiredLimits': limits, 'requiredFeatures': features });
				const supported = (candidate) => device['limits']['maxBindGroups'] >= 4
					&& Object.entries(rdLimits[candidate]).every(([name, count]) => device['limits'][name] >= count);
				if (method === 'forward_plus' && supported('forward_plus') && device['features']['has']('depth-clip-control')
						&& gpu['wgslLanguageFeatures'] && gpu['wgslLanguageFeatures']['has']('readonly_and_readwrite_storage_textures')) {
					return selected('forward_plus', 'webgpu', '', device);
				}
				if (supported('mobile')) {
					return selected('mobile', 'webgpu', method === 'mobile' ? '' : 'Forward+ requirements were not met.', device);
				}
				device['destroy']();
				reason = 'WebGPU device limits do not cover the built-in Mobile shader variants.';
			} catch (error) {
				reason = `WebGPU device negotiation failed: ${error.message}`;
			}
		} else if (drivers.includes('webgpu')) {
			reason = 'WebGPU is not available in this browser context.';
		}
		if (drivers.includes('opengl3') && Features.isWebGLAvailable(2)) {
			return selected('gl_compatibility', 'opengl3', reason, null);
		}
		throw new Error(`${reason} No WebGL2 fallback is available.`);
	};

	/**
	 * @ignore
	 * @param {string} loadPath
	 * @param {Response} response
	 * @param {function(!Error)} onFailure
	 */
	Config.prototype.getModuleConfig = function (loadPath, response, onFailure) {
		let r = response;
		const gdext = this.gdextensionLibs;
		return {
			'print': this.onPrint,
			'printErr': this.onPrintError,
			'thisProgram': this.executable,
			'noExitRuntime': false,
			'onAbort': function (reason) { onFailure(new Error(`Godot runtime aborted: ${reason}`)); },
			'dynamicLibraries': [`${loadPath}.side.wasm`].concat(this.gdextensionLibs),
			'emscriptenPoolSize': this.emscriptenPoolSize,
			'instantiateWasm': function (imports, onSuccess) {
				function done(result) {
					onSuccess(result['instance'], result['module']);
				}
				const wasm = typeof WebAssembly.instantiateStreaming === 'function'
					? WebAssembly.instantiateStreaming(r, imports)
					: r.arrayBuffer().then(function (buffer) {
						return WebAssembly.instantiate(buffer, imports);
					});
				wasm.then(done).catch(onFailure);
				r = null;
				return {};
			},
			'locateFile': function (path) {
				if (!path.startsWith('godot.')) {
					return path;
				} else if (path.endsWith('.audio.worklet.js')) {
					return `${loadPath}.audio.worklet.js`;
				} else if (path.endsWith('.audio.position.worklet.js')) {
					return `${loadPath}.audio.position.worklet.js`;
				} else if (path.endsWith('.js')) {
					return `${loadPath}.js`;
				} else if (path in gdext) {
					return path;
				} else if (path.endsWith('.side.wasm')) {
					return `${loadPath}.side.wasm`;
				} else if (path.endsWith('.wasm')) {
					return `${loadPath}.wasm`;
				}
				return path;
			},
		};
	};

	/**
	 * @ignore
	 * @param {function(number)} cleanup
	 * @param {function(number, string)} onProgress
	 * @param {function()} onReady
	 * @param {function(number, string)} onStartupExit
	 */
	Config.prototype.getGodotConfig = function (cleanup, onProgress, onReady, onStartupExit) {
		// Try to find a canvas
		if (!(this.canvas instanceof HTMLCanvasElement)) {
			const nodes = document.getElementsByTagName('canvas');
			if (nodes.length && nodes[0] instanceof HTMLCanvasElement) {
				const first = nodes[0];
				this.canvas = /** @type {!HTMLCanvasElement} */ (first);
			}
			if (!this.canvas) {
				throw new Error('No canvas found in page');
			}
		}
		// Canvas can grab focus on click, or key events won't work.
		if (this.canvas.tabIndex < 0) {
			this.canvas.tabIndex = 0;
		}

		// Browser locale, or custom one if defined.
		let locale = this.locale;
		if (!locale) {
			locale = navigator.languages ? navigator.languages[0] : navigator.language;
			locale = locale.split('.')[0];
		}
		locale = locale.replace('-', '_');
		const onExit = this.onExit;

		// Godot configuration.
		return {
			'canvas': this.canvas,
			'canvasResizePolicy': this.canvasResizePolicy,
			'locale': locale,
			'persistentDrops': this.persistentDrops,
			'virtualKeyboard': this.experimentalVK,
			'godotPoolSize': this.godotPoolSize,
			'focusCanvas': this.focusCanvas,
			'onExecute': this.onExecute,
			'onStartupProgress': onProgress,
			'onFirstFrameReady': onReady,
			'onStartupExit': onStartupExit,
			'onExit': function (p_code) {
				cleanup(p_code); // We always need to call the cleanup callback to free memory.
				if (typeof (onExit) === 'function') {
					onExit(p_code);
				}
			},
		};
	};
	return new Config(initConfig);
};
