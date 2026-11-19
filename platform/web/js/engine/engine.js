/**
 * Projects exported for the Web expose the :js:class:`Engine` class to the JavaScript environment, that allows
 * fine control over the engine's start-up process.
 *
 * This API is built in an asynchronous manner and requires basic understanding
 * of `Promises <https://developer.mozilla.org/en-US/docs/Web/JavaScript/Guide/Using_promises>`__.
 *
 * @module Engine
 * @header Web export JavaScript reference
 */
const Engine = (function () {
	const preloader = new Preloader();

	let loadPromise = null;
	let loadPath = '';
	let initPromise = null;

	/**
	 * @classdesc The ``Engine`` class provides methods for loading and starting exported projects on the Web. For default export
	 * settings, this is already part of the exported HTML page. To understand practical use of the ``Engine`` class,
	 * see :ref:`Custom HTML page for Web export <doc_customizing_html5_shell>`.
	 *
	 * @description Create a new Engine instance with the given configuration.
	 *
	 * @global
	 * @constructor
	 * @param {EngineConfig} initConfig The initial config for this instance.
	 */
	function Engine(initConfig) { // eslint-disable-line no-shadow
		this.config = new InternalConfig(initConfig);
		this.rtenv = null;
		this.renderingBackend = null;
		this.packStream = null;
		this.progress = 0;
		this.progressLabel = '';
		this.rejectStartup = null;
	}

	function reportProgress(engine, ratio, label) {
		const current = Math.max(engine.progress, Math.min(10000, Math.round(ratio * 10000)));
		if (current === engine.progress && label === engine.progressLabel) {
			return;
		}
		engine.progress = current;
		engine.progressLabel = label;
		window['godotStartupStatus'] = { 'ratio': current / 10000, 'label': label };
		if (typeof engine.config.onProgress === 'function') {
			try {
				engine.config.onProgress(current, 10000);
			} catch (error) {
				console.error('Godot startup progress callback failed:', error);
			}
		}
	}

	/**
	 * Load the engine from the specified base path.
	 *
	 * @param {string} basePath Base path of the engine to load.
	 * @param {number=} [size=0] The file size if known.
	 * @returns {Promise} A Promise that resolves once the engine is loaded.
	 *
	 * @function Engine.load
	 */
	Engine.load = function (basePath, size) {
		if (loadPromise == null) {
			loadPath = basePath;
			loadPromise = preloader.loadPromise(`${loadPath}.wasm`, size, true);
		}
		return loadPromise;
	};

	/**
	 * Unload the engine to free memory.
	 *
	 * This method will be called automatically depending on the configuration. See :js:attr:`unloadAfterInit`.
	 *
	 * @function Engine.unload
	 */
	Engine.unload = function () {
		loadPromise = null;
	};

	/**
	 * Safe Engine constructor, creates a new prototype for every new instance to avoid prototype pollution.
	 * @ignore
	 * @constructor
	 */
	function SafeEngine(initConfig) {
		const proto = /** @lends Engine.prototype */ {
			/**
			 * Initialize the engine instance. Optionally, pass the base path to the engine to load it,
			 * if it hasn't been loaded yet. See :js:meth:`Engine.load`.
			 *
			 * @param {string=} basePath Base path of the engine to load.
			 * @return {Promise} A ``Promise`` that resolves once the engine is loaded and initialized.
			 */
			init: function (basePath) {
				if (initPromise) {
					return initPromise;
				}
				if (loadPromise == null) {
					if (!basePath) {
						initPromise = Promise.reject(new Error('A base path must be provided when calling `init` and the engine is not loaded.'));
						return initPromise;
					}
					Engine.load(basePath, this.config.fileSizes[`${basePath}.wasm`]);
				}
				const me = this;
				function doInit(promise) {
					// Care! Promise chaining is bogus with old emscripten versions.
					// This caused a regression with the Mono build (which uses an older emscripten version).
					// Make sure to test that when refactoring.
					return new Promise(function (resolve, reject) {
						function onFailure(error) {
							reject(error);
							if (me.rejectStartup) {
								me.rejectStartup(error);
							}
						}
						promise.then(function (response) {
							const cloned = new Response(response.clone().body, { 'headers': [['content-type', 'application/wasm']] });
							return Godot(me.config.getModuleConfig(loadPath, cloned, onFailure));
						}).then(function (module) {
							return module['initFS'](me.config.persistentPaths).then(function () {
								me.rtenv = module;
								if (me.config.unloadAfterInit) {
									Engine.unload();
								}
								resolve();
							});
						}).catch(onFailure);
					});
				}
				preloader.setProgressFunc(function (loaded, total) {
					// Do not display network progress after initialization stages begin.
					if (me.progress < 5000) {
						reportProgress(me, total > 0 ? Math.min(0.4999, 0.5 * loaded / total) : 0, 'Downloading engine and project...');
					}
				});
				initPromise = doInit(loadPromise);
				return initPromise;
			},

			/**
			 * Load a file so it is available in the instance's file system once it runs. Must be called **before** starting the
			 * instance.
			 *
			 * If not provided, the ``path`` is derived from the URL of the loaded file.
			 *
			 * @param {string|ArrayBuffer} file The file to preload.
			 *
			 * If a ``string`` the file will be loaded from that path.
			 *
			 * If an ``ArrayBuffer`` or a view on one, the buffer will used as the content of the file.
			 *
			 * @param {string=} path Path by which the file will be accessible. Required, if ``file`` is not a string.
			 *
			 * @returns {Promise} A Promise that resolves once the file is loaded.
			 */
			preloadFile: function (file, path) {
				return preloader.preload(file, path, this.config.fileSizes[file]);
			},

			/**
			 * Start the engine instance using the given override configuration (if any).
			 * :js:meth:`startGame <Engine.prototype.startGame>` can be used in typical cases instead.
			 *
			 * This will initialize the instance if it is not initialized. For manual initialization, see :js:meth:`init <Engine.prototype.init>`.
			 * The engine must be loaded beforehand.
			 *
			 * Fails if a canvas cannot be found on the page, or not specified in the configuration.
			 *
			 * @param {EngineConfig} override An optional configuration override.
			 * @return {Promise} Promise that resolves once the engine started.
			 */
			start: function (override) {
				this.config.update(override);
				const me = this;
				return me.init().then(async function () {
					if (!me.rtenv) {
						return Promise.reject(new Error('The engine must be initialized before it can be started'));
					}

					reportProgress(me, 0.5, 'Preparing browser renderer...');
					const rendering = await me.config.negotiateRendering(me.rtenv['godotRenderingDrivers']);
					me.renderingBackend = rendering.state;
					window['godotRenderingBackend'] = rendering.state;
					if (rendering.device) {
						me.rtenv['preinitializedWebGPUDevice'] = rendering.device;
						rendering.state['uncapturedErrors'] = [];
						rendering.device['lost'].then(function (info) {
							rendering.state['deviceLost'] = info['message'] || info['reason'] || 'lost';
						});
						if (typeof rendering.device['addEventListener'] === 'function') {
							rendering.device['addEventListener']('uncapturederror', function (event) {
								const err = event && event['error'];
								const msg = (err && err['message']) ? String(err['message']) : String(err);
								if (rendering.state['uncapturedErrors'].length < 32) {
									rendering.state['uncapturedErrors'].push(msg);
								}
								console.error('[Godot WebGPU]', msg);
							});
						}
					}
					// Preload GDExtension libraries.
					if (me.config.gdextensionLibs.length > 0 && !me.rtenv['loadDynamicLibrary']) {
						return Promise.reject(new Error('GDExtension libraries are not supported by this engine version. '
							+ 'Enable "Extensions Support" for your export preset and/or build your custom template with "dlink_enabled=yes".'));
					}
					return new Promise(function (resolve, reject) {
						function rejectStartup(error) {
							me.rejectStartup = null;
							initPromise = null;
							reject(error);
						}
						function startupExit(code, message) {
							if (me.rejectStartup) {
								const error = new Error(message);
								error['exitCode'] = code;
								rejectStartup(error);
							}
						}
						me.rejectStartup = rejectStartup;
						try {
							const config = me.config.getGodotConfig(function (code) {
								me.rtenv = null;
								startupExit(code, `Godot exited before its first frame (status ${code}).`);
							}, function (ratio, label) {
								reportProgress(me, ratio, label);
							}, function () {
								if (me.rejectStartup) {
									me.rejectStartup = null;
									initPromise = null;
									resolve();
									me.installServiceWorker().catch(function (error) { console.error(error); });
								}
							}, startupExit);
							me.rtenv['initConfig'](config);
							for (const file of preloader.preloadedFiles) {
								me.rtenv['copyToFS'](file.path, file.buffer, file.owned);
							}
							preloader.preloadedFiles.length = 0;
							if (me.packStream) {
								me.rtenv['initPackStream'](me.packStream);
								me.packStream = null;
							}
							me.rtenv['callMain'](rendering.args);
						} catch (error) {
							rejectStartup(error);
						}
					});
				});
			},

			getRenderingBackend: function () {
				return this.renderingBackend;
			},

			/**
			 * Start the game instance using the given configuration override (if any).
			 *
			 * This will initialize the instance if it is not initialized. For manual initialization, see :js:meth:`init <Engine.prototype.init>`.
			 *
			 * This will load the engine if it is not loaded, and preload the main pck.
			 *
			 * This method expects the initial config (or the override) to have both the :js:attr:`executable` and :js:attr:`mainPack`
			 * properties set (normally done by the editor during export).
			 *
			 * @param {EngineConfig} override An optional configuration override.
			 * @return {Promise} Promise that resolves once the game started.
			 */
			startGame: function (override) {
				this.config.update(override);
				// Add main-pack argument.
				const exe = this.config.executable;
				const pack = this.config.mainPack || `${exe}.pck`;
				const me = this;
				const initialized = this.init(exe);
				const loaded = (async function () {
					if (me.config.streamPack && !me.config.args.includes('--main-pack')) {
						me.packStream = await preloader.loadPack(pack, async function () {
							if (!me.config.progressiveTextureMips) {
								return false;
							}
							await initialized;
							return !me.rtenv['godotThreaded'];
						});
					}
					if (!me.packStream) {
						await me.preloadFile(pack, pack);
					}
				})();
				return Promise.all([initialized, loaded]).then(function () {
					me.config.args = ['--main-pack', pack].concat(me.config.args);
					return me.start.apply(me);
				});
			},

			/**
			 * Create a file at the specified ``path`` with the passed as ``buffer`` in the instance's file system.
			 *
			 * @param {string} path The location where the file will be created.
			 * @param {ArrayBuffer} buffer The content of the file.
			 */
			copyToFS: function (path, buffer) {
				if (this.rtenv == null) {
					throw new Error('Engine must be inited before copying files');
				}
				this.rtenv['copyToFS'](path, buffer);
			},

			/**
			 * Request that the current instance quit.
			 *
			 * This is akin the user pressing the close button in the window manager, and will
			 * have no effect if the engine has crashed, or is stuck in a loop.
			 *
			 */
			requestQuit: function () {
				if (this.rtenv) {
					this.rtenv['request_quit']();
				}
			},

			/**
			 * Install the progressive-web app service worker.
			 * @returns {Promise} The service worker registration promise.
			 */
			installServiceWorker: function () {
				if (this.config.serviceWorker && 'serviceWorker' in navigator) {
					try {
						return navigator.serviceWorker.register(this.config.serviceWorker);
					} catch (e) {
						return Promise.reject(e);
					}
				}
				return Promise.resolve();
			},
		};

		Engine.prototype = proto;
		// Closure compiler exported instance methods.
		Engine.prototype['init'] = Engine.prototype.init;
		Engine.prototype['preloadFile'] = Engine.prototype.preloadFile;
		Engine.prototype['start'] = Engine.prototype.start;
		Engine.prototype['startGame'] = Engine.prototype.startGame;
		Engine.prototype['getRenderingBackend'] = Engine.prototype.getRenderingBackend;
		Engine.prototype['copyToFS'] = Engine.prototype.copyToFS;
		Engine.prototype['requestQuit'] = Engine.prototype.requestQuit;
		Engine.prototype['installServiceWorker'] = Engine.prototype.installServiceWorker;
		// Also expose static methods as instance methods
		Engine.prototype['load'] = Engine.load;
		Engine.prototype['unload'] = Engine.unload;
		return new Engine(initConfig);
	}

	// Closure compiler exported static methods.
	SafeEngine['load'] = Engine.load;
	SafeEngine['unload'] = Engine.unload;

	// Feature-detection utilities.
	SafeEngine['isWebGLAvailable'] = Features.isWebGLAvailable;
	SafeEngine['isFetchAvailable'] = Features.isFetchAvailable;
	SafeEngine['isSecureContext'] = Features.isSecureContext;
	SafeEngine['isCrossOriginIsolated'] = Features.isCrossOriginIsolated;
	SafeEngine['isSharedArrayBufferAvailable'] = Features.isSharedArrayBufferAvailable;
	SafeEngine['isAudioWorkletAvailable'] = Features.isAudioWorkletAvailable;
	SafeEngine['getMissingFeatures'] = Features.getMissingFeatures;

	return SafeEngine;
}());
if (typeof window !== 'undefined') {
	window['Engine'] = Engine;
}
