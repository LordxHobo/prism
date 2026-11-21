// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The chrome.* / browser.* API for an extension's own pages: its background
// page (or service worker, which Prism runs as a page), its toolbar popup
// and its options page.
//
// Prism adds this function expression, followed by "(config)", to every
// page once per enabled extension. It only does something on that
// extension's own pages (files in its folder); Prism checks the same thing
// on its side before answering, so other pages can't use it.
(function (config) {
  'use strict';

  // ---- Is this one of the extension's pages? -------------------------------
  const normalize = (url) => {
    let text = String(url);
    try { text = decodeURI(text); } catch (error) { /* keep it as it is */ }
    return text.replace(/\\/g, '/').toLowerCase();
  };
  if (window.top !== window) return;
  if (!normalize(location.href).startsWith(normalize(config.base))) return;
  const handler = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.prismExtensionPage;
  if (!handler) return;

  const base = config.base;
  const isBackground = /\/_prism_background\.html$/i.test(location.pathname);

  // Extensions from the Chrome Web Store are written for Chrome, and those
  // from Firefox Add-ons for Firefox. In their own pages Prism says it is that
  // browser: with Prism's Safari user agent, extensions like Bitwarden switch
  // to their Safari code, which expects Safari's native app.
  (function presentAs(family) {
    const ua = family === 'firefox'
      ? 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:145.0) Gecko/20100101 Firefox/145.0'
      : 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/142.0.0.0 Safari/537.36';
    const define = (name, value) => {
      try { Object.defineProperty(navigator, name, { get: () => value, configurable: true }); } catch (error) { /* keep WebKit's */ }
    };
    define('userAgent', ua);
    define('appVersion', ua.replace(/^Mozilla\//, ''));
    define('vendor', family === 'firefox' ? '' : 'Google Inc.');
    if (family !== 'firefox') {
      const brands = [{ brand: 'Chromium', version: '142' }, { brand: 'Not_A Brand', version: '24' }];
      define('userAgentData', Object.freeze({
        brands, mobile: false, platform: 'Windows',
        getHighEntropyValues: () => Promise.resolve({ brands, mobile: false, platform: 'Windows', platformVersion: '15.0.0', architecture: 'x86', bitness: '64', uaFullVersion: '142.0.0.0' }),
        toJSON: () => ({ brands, mobile: false, platform: 'Windows' }),
      }));
    }
  })(config.family);
  const manifest = config.manifest || {};

  // ---- The log Prism keeps for the Extensions page ----------------------------------
  // Console lines and a trace of the extension's chrome.* calls: the first
  // use of each, calls that fail, and calls still unanswered after 5 s.
  // Sent in batches; a busy logger loses lines rather than slowing the page.
  let consoleLines = [];
  let consoleTimer = 0;
  function pushLine(level, text) {
    try {
      if (consoleLines.length >= 200) return;
      consoleLines.push({ level, text: String(text).slice(0, 2000), time: Date.now() });
      if (!consoleTimer) consoleTimer = setTimeout(flushConsole, 700);
    } catch (error) { /* never let logging break the page */ }
  }
  function flushConsole() {
    consoleTimer = 0;
    const lines = consoleLines;
    consoleLines = [];
    if (lines.length) call('console', { lines }).catch(() => {});
  }
  const QUIET_OPS = new Set(['poll', 'console', 'log', 'respond', 'hello', 'popup.resize']);
  const seenOps = new Set();
  const failures = new Map();
  const waiting = new Map();
  let nextCall = 1;
  function describeCall(op, data) {
    if (!data) return op;
    const message = data.message;
    const kind = message && typeof message === 'object' ? (message.command || message.type || message.name || message.action || message.method) : null;
    if (kind && typeof kind !== 'object') return op + ' "' + String(kind).slice(0, 80) + '"';
    if (data.name) return op + ' "' + String(data.name).slice(0, 80) + '"';
    if (Array.isArray(data.keys)) return op + ' ' + (data.area || '') + ' [' + data.keys.slice(0, 3).join(', ') + (data.keys.length > 3 ? ', …' : '') + ']';
    if (data.area) return op + ' ' + data.area;
    if (data.url) return op + ' ' + String(data.url).slice(0, 120);
    return op;
  }
  setInterval(() => {
    const now = Date.now();
    for (const entry of waiting.values()) {
      if (!entry.flagged && now - entry.started > 5000) {
        entry.flagged = true;
        pushLine('trace', entry.what + ': still waiting for an answer after 5 s');
      }
    }
  }, 2500);
  if (typeof WebAssembly === 'undefined') pushLine('trace', 'WebAssembly isn\'t available in this page');

  // ---- requestIdleCallback ------------------------------------------------------------
  // WebKit doesn't have it (Safari never shipped it), but extensions written
  // for Chrome and Firefox call it, e.g. AdNauseam whenever a tab changes.
  if (typeof window.requestIdleCallback !== 'function') {
    const requestIdleCallback = function requestIdleCallback(callback) {
      if (typeof callback !== 'function') throw new TypeError('requestIdleCallback: callback is not a function');
      return setTimeout(() => {
        const start = performance.now();
        callback(Object.freeze({ didTimeout: false, timeRemaining: () => Math.max(0, 50 - (performance.now() - start)) }));
      }, 1);
    };
    const cancelIdleCallback = function cancelIdleCallback(handle) { clearTimeout(handle); };
    Object.defineProperty(window, 'requestIdleCallback', { value: requestIdleCallback, writable: true, configurable: true });
    Object.defineProperty(window, 'cancelIdleCallback', { value: cancelIdleCallback, writable: true, configurable: true });
  }

  // ---- Talking to Prism -----------------------------------------------------------
  function call(op, data) {
    const message = Object.assign({}, data || {}, { op });
    let text;
    try {
      text = JSON.stringify(message);
    } catch (error) {
      return Promise.reject(error);
    }
    const traced = !QUIET_OPS.has(op);
    const id = nextCall++;
    let what = '';
    if (traced) {
      what = describeCall(op, data);
      if (!seenOps.has(op)) {
        seenOps.add(op);
        pushLine('trace', 'first ' + what);
      }
      waiting.set(id, { what, started: Date.now(), flagged: false });
    }
    const settled = () => {
      const entry = waiting.get(id);
      if (entry && entry.flagged) pushLine('trace', entry.what + ': answered after ' + Math.round((Date.now() - entry.started) / 1000) + ' s');
      waiting.delete(id);
    };
    return handler.postMessage(text).then((reply) => {
      settled();
      const result = JSON.parse(reply);
      if (result && result.ok) return result.result;
      if (op !== 'hello' && op !== 'poll' && result && typeof result.error === 'string' && result.error.includes('Say hello first') && typeof reconnect === 'function') reconnect();
      const message = (result && result.error) || 'Unknown error';
      if (traced) {
        const key = what + '\n' + message;
        const count = (failures.get(key) || 0) + 1;
        failures.set(key, count);
        if (count <= 3) pushLine('trace', what + ' failed: ' + message + (count === 3 ? ' (not logged again)' : ''));
      }
      throw new Error(message);
    }, (error) => {
      settled();
      throw error;
    });
  }

  // ---- API helpers -----------------------------------------------------------------
  let lastError;

  // Promises (browser.*, MV3) and a trailing callback (chrome.*, with
  // chrome.runtime.lastError set on failure) both work.
  function withCallback(fn) {
    return function (...args) {
      const callback = typeof args[args.length - 1] === 'function' ? args.pop() : null;
      let promise;
      try {
        promise = Promise.resolve(fn(...args));
      } catch (error) {
        promise = Promise.reject(error);
      }
      if (!callback) return promise;
      promise.then((value) => {
        try { callback(value); } catch (error) { console.error(error); }
      }, (error) => {
        lastError = { message: String((error && error.message) || error) };
        try { callback(); } catch (callbackError) { console.error(callbackError); }
        lastError = undefined;
      });
      return undefined;
    };
  }

  const events = Object.create(null);

  function makeEvent(name) {
    const listeners = [];
    const source = {
      listeners,
      event: {
        addListener(listener) { if (typeof listener === 'function' && !listeners.includes(listener)) listeners.push(listener); },
        removeListener(listener) { const i = listeners.indexOf(listener); if (i >= 0) listeners.splice(i, 1); },
        hasListener(listener) { return listeners.includes(listener); },
        hasListeners() { return listeners.length > 0; },
      },
      fire(...args) {
        for (const listener of listeners.slice()) {
          try { listener(...args); } catch (error) { console.error(error); }
        }
      },
    };
    if (name) events[name] = source;
    return source;
  }

  // Events Prism doesn't send (yet): listening to them is harmless.
  const quiet = () => makeEvent().event;
  const resolved = (value) => withCallback(() => value);
  const notYet = (what) => withCallback(() => Promise.reject(new Error(what + " isn't supported in Prism yet.")));

  // ---- Errors: shown on Prism's Extensions page ----------------------------------------------
  // WebKit's error.stack leaves out the message itself: put both in.
  const describe = (value) => {
    if (value instanceof Error || (value && typeof value === 'object' && 'message' in value && 'stack' in value)) {
      const head = (value.name || 'Error') + ': ' + value.message;
      if (!value.stack) return head;
      return String(value.stack).startsWith(head) ? String(value.stack) : head + '\n' + value.stack;
    }
    if (value && typeof value === 'object') {
      try { return JSON.stringify(value); } catch (error) { return String(value); }
    }
    return String(value);
  };
  const report = (message, source) => {
    try {
      call('log', { message: String(message).slice(0, 2000), source: String(source || location.pathname).slice(0, 300) }).catch(() => {});
    } catch (error) { /* never let reporting break the page */ }
  };
  window.addEventListener('error', (event) => {
    const text = event.error ? describe(event.error) : (event.message || 'Error');
    report(text, (event.filename || '').replace(base, '/') + (event.lineno ? ':' + event.lineno : ''));
  });
  window.addEventListener('unhandledrejection', (event) => {
    report('Unhandled promise rejection: ' + describe(event.reason), location.pathname);
  });
  const consoleError = console.error.bind(console);
  console.error = (...args) => {
    consoleError(...args);
    report(args.map(describe).join(' '), location.pathname);
    keepLine('error', args);
  };

  // The console goes into the same log (see pushLine).
  function keepLine(level, args) {
    try { pushLine(level, args.map(describe).join(' ')); } catch (error) { /* never let logging break the page */ }
  }
  for (const level of ['log', 'info', 'warn', 'debug']) {
    const original = console[level].bind(console);
    console[level] = (...args) => {
      original(...args);
      keepLine(level, args);
    };
  }

  // ---- Messages, ports and events from Prism -----------------------------------------
  let context = { ctx: 0, kind: 'unknown', tabId: -1, windowId: -1 };
  const ports = new Map();
  const onMessage = makeEvent();
  const onConnect = makeEvent();

  const ready = call('hello', { url: location.href }).then((info) => {
    context = info;
    pollLoop();
    if (info.kind === 'popup') watchPopupSize();
    return info;
  });
  ready.catch((error) => console.error('[Prism] extension page:', error));

  function pollLoop() {
    call('poll').then((list) => {
      if (Array.isArray(list)) for (const event of list) handleEvent(event);
      pollLoop();
    }, (error) => {
      if (lostConnection(error)) reconnect();
      else setTimeout(pollLoop, 2000);
    });
  }

  // Prism dropped this page's connection (it went quiet for a long time):
  // say hello again and register what Prism forgot with it. Open ports are
  // gone, as when Chrome restarts an extension's worker.
  function lostConnection(error) {
    return String((error && error.message) || error).includes('Say hello first');
  }
  let reconnecting = false;
  function reconnect() {
    if (reconnecting) return;
    reconnecting = true;
    call('hello', { url: location.href }).then((info) => {
      context = info;
      reconnecting = false;
      for (const entry of webRequestListeners.values()) {
        if (entry.options) call('webRequest.addListener', entry.options).catch(() => {});
      }
      pollLoop();
    }, () => {
      reconnecting = false;
      setTimeout(reconnect, 2000);
    });
  }

  // What arrives, in the log: the first few of each kind.
  const arrivals = new Map();
  function traceArrival(key, text) {
    const count = (arrivals.get(key) || 0) + 1;
    arrivals.set(key, count);
    if (count <= 3) pushLine('trace', text);
  }
  const actionOf = (message) => message && typeof message === 'object'
    ? (message.command || message.action || message.type || message.name || '') : '';

  function handleEvent(event) {
    if (!event) return;
    switch (event.type) {
    case 'message':
      traceArrival('message ' + actionOf(event.message), 'received message "' + String(actionOf(event.message)).slice(0, 80) + '"');
      dispatchMessage(event);
      break;
    case 'connect': {
      const sender = event.sender || {};
      traceArrival('connect ' + event.name, 'received connect "' + event.name + '" from ' + (sender.url || '?') + ' (origin ' + (sender.origin || 'none')
        + (sender.frameId !== undefined ? ', frame ' + sender.frameId : '') + '; ' + onConnect.listeners.length + ' onConnect listeners)');
      onConnect.fire(makePort(event.port, event.name, event.sender).port);
      break;
    }
    case 'portMessage': {
      const entry = ports.get(event.port);
      traceArrival('portMessage ' + event.port + actionOf(event.message), 'received port message "' + String(actionOf(event.message)).slice(0, 80) + '" on port ' + event.port
        + (entry ? ' (' + entry.onMessage.listeners.length + ' listeners)' : ' (port unknown here)'));
      if (entry) entry.onMessage.fire(event.message, entry.port);
      break;
    }
    case 'portDisconnect': {
      const entry = ports.get(event.port);
      if (entry) entry.close();
      break;
    }
    case 'storageChanged':
      if (storageAreaEvents[event.area]) storageAreaEvents[event.area].fire(event.changes);
      storageChanged.fire(event.changes, event.area);
      break;
    case 'event': {
      const target = events[event.name];
      if (target) target.fire(...(event.args || []));
      break;
    }
    case 'webRequest':
      handleWebRequest(event);
      break;
    }
  }

  function dispatchMessage(event) {
    let responded = false;
    let willRespond = false;
    const respond = (handled, response) => {
      if (responded) return;
      responded = true;
      const data = { id: event.id, handled };
      if (handled) data.response = response;
      call('respond', data).catch((error) => console.error(error));
    };
    const sendResponse = (response) => respond(true, response);
    for (const listener of onMessage.listeners.slice()) {
      let result;
      try {
        result = listener(event.message, event.sender, sendResponse);
      } catch (error) {
        console.error(error);
        continue;
      }
      if (result === true) willRespond = true;
      else if (result && typeof result.then === 'function') {
        willRespond = true;
        result.then(sendResponse, (error) => { console.error(error); respond(false); });
      }
    }
    if (!willRespond && !responded) respond(false);
  }

  function makePort(id, name, sender) {
    const portMessage = makeEvent();
    const portDisconnect = makeEvent();
    let open = true;
    let portId = id;
    const queued = [];
    const port = {
      name: name || '',
      sender: sender || undefined,
      onMessage: portMessage.event,
      onDisconnect: portDisconnect.event,
      postMessage(message) {
        if (!open) throw new Error('Attempting to use a disconnected port object');
        if (portId === null) queued.push(message);
        else call('port.post', { port: portId, message }).catch(() => {});
      },
      disconnect() {
        if (!open) return;
        open = false;
        if (portId !== null) {
          ports.delete(portId);
          call('port.disconnect', { port: portId }).catch(() => {});
        }
      },
    };
    const entry = {
      port,
      onMessage: portMessage,
      close() {
        if (!open) return;
        open = false;
        ports.delete(portId);
        portDisconnect.fire(port);
      },
      setId(newId) {
        portId = newId;
        ports.set(newId, entry);
        if (!open) {
          call('port.disconnect', { port: newId }).catch(() => {});
          return;
        }
        for (const message of queued.splice(0)) call('port.post', { port: newId, message }).catch(() => {});
      },
    };
    if (id !== null) entry.setId(id);
    return entry;
  }

  function connect(data) {
    const entry = makePort(null, data.name);
    ready.then(() => call('port.connect', data)).then((result) => {
      entry.setId(result.port);
      if (result.disconnected) {
        lastError = { message: 'Could not establish connection. Receiving end does not exist.' };
        entry.close();
        lastError = undefined;
      }
    }, () => entry.close());
    return entry.port;
  }

  function messageArguments(args) {
    if (args.length === 1) return { message: args[0] };
    if (args.length >= 3) return { id: args[0], message: args[1] };
    const optionsLike = args[1] && typeof args[1] === 'object' && 'includeTlsChannelId' in args[1];
    if ((typeof args[0] === 'string' || args[0] === null || args[0] === undefined) && !optionsLike) return { id: args[0], message: args[1] };
    return { message: args[0] };
  }

  const responseOf = (result) => (result && 'response' in result ? result.response : undefined);

  // ---- runtime -----------------------------------------------------------------------
  const getURL = (path) => {
    const text = String(path || '');
    if (/^[a-z-]+:/i.test(text)) return text;
    return base + text.replace(/^\//, '');
  };

  const runtime = {
    id: config.id,
    getURL,
    getManifest: () => JSON.parse(JSON.stringify(manifest)),
    sendMessage: withCallback((...args) => {
      const target = messageArguments(args);
      if (target.id !== undefined && target.id !== null && target.id !== config.id) {
        return Promise.reject(new Error("Messaging other extensions isn't supported in Prism yet."));
      }
      return ready.then(() => call('runtime.sendMessage', { message: target.message })).then(responseOf);
    }),
    connect: (...args) => {
      const info = args.find((arg) => arg && typeof arg === 'object') || {};
      return connect({ name: info.name || '' });
    },
    onMessage: onMessage.event,
    onConnect: onConnect.event,
    onMessageExternal: quiet(),
    onConnectExternal: quiet(),
    onInstalled: makeEvent('runtime.onInstalled').event,
    onStartup: makeEvent('runtime.onStartup').event,
    onSuspend: quiet(),
    onSuspendCanceled: quiet(),
    onUpdateAvailable: quiet(),
    onRestartRequired: quiet(),
    openOptionsPage: withCallback(() => call('runtime.openOptionsPage')),
    reload: () => { call('runtime.reload').catch(() => {}); },
    setUninstallURL: resolved(undefined),
    requestUpdateCheck: resolved({ status: 'no_update' }),
    getPlatformInfo: resolved({ os: 'win', arch: 'x86-64', nacl_arch: 'x86-64' }),
    getBrowserInfo: resolved({ name: 'Prism', vendor: 'Prism', version: '0.9.12', buildID: '0' }),
    getContexts: withCallback((filter) => call('runtime.getContexts').then((contexts) => {
      const f = filter || {};
      const has = (list, value) => !Array.isArray(list) || list.includes(value);
      return contexts.filter((c) => has(f.contextTypes, c.contextType) && has(f.contextIds, c.contextId) && has(f.tabIds, c.tabId)
        && has(f.windowIds, c.windowId) && has(f.documentIds, c.documentId) && has(f.frameIds, c.frameId)
        && has(f.documentUrls, c.documentUrl) && has(f.documentOrigins, c.documentOrigin) && (f.incognito === undefined || f.incognito === c.incognito));
    })),
    getBackgroundPage: notYet('Reaching the background page directly'),
    getPackageDirectoryEntry: notYet('getPackageDirectoryEntry'),
    OnInstalledReason: { INSTALL: 'install', UPDATE: 'update', CHROME_UPDATE: 'chrome_update', SHARED_MODULE_UPDATE: 'shared_module_update' },
    ContextType: { TAB: 'TAB', POPUP: 'POPUP', BACKGROUND: 'BACKGROUND', OFFSCREEN_DOCUMENT: 'OFFSCREEN_DOCUMENT', SIDE_PANEL: 'SIDE_PANEL', DEVELOPER_TOOLS: 'DEVELOPER_TOOLS' },
    PlatformOs: { WIN: 'win', MAC: 'mac', LINUX: 'linux', ANDROID: 'android', CROS: 'cros', OPENBSD: 'openbsd' },
  };
  Object.defineProperty(runtime, 'lastError', { get: () => lastError, enumerable: true });

  // ---- storage --------------------------------------------------------------------------
  const storageChanged = makeEvent();
  const storageAreaEvents = Object.create(null);

  function makeStorageArea(area) {
    const areaChanged = makeEvent();
    storageAreaEvents[area] = areaChanged;
    const fireChanged = (changes) => {
      if (!changes || !Object.keys(changes).length) return;
      areaChanged.fire(changes);
      storageChanged.fire(changes, area);
    };
    const normalizeKeys = (keys) => {
      if (keys === null || keys === undefined) return { keys: null, defaults: null };
      if (typeof keys === 'string') return { keys: [keys], defaults: null };
      if (Array.isArray(keys)) return { keys: keys.map(String), defaults: null };
      if (typeof keys === 'object') return { keys: Object.keys(keys), defaults: keys };
      throw new TypeError('Invalid storage keys');
    };
    return {
      get: withCallback((keys) => {
        const normalized = normalizeKeys(keys);
        return call('storage.get', { area, keys: normalized.keys }).then((result) => Object.assign({}, normalized.defaults || {}, result || {}));
      }),
      set: withCallback((items) => {
        if (!items || typeof items !== 'object') return Promise.reject(new TypeError('Invalid storage items'));
        return call('storage.set', { area, items }).then((result) => { fireChanged(result && result.changes); });
      }),
      remove: withCallback((keys) => {
        const list = typeof keys === 'string' ? [keys] : Array.isArray(keys) ? keys.map(String) : [];
        return call('storage.remove', { area, keys: list }).then((result) => { fireChanged(result && result.changes); });
      }),
      clear: withCallback(() => call('storage.clear', { area }).then((result) => { fireChanged(result && result.changes); })),
      getBytesInUse: withCallback(() => call('storage.bytes', { area })),
      setAccessLevel: resolved(undefined),
      onChanged: areaChanged.event,
      QUOTA_BYTES: area === 'sync' ? 102400 : 10485760,
      ...(area === 'sync' ? { QUOTA_BYTES_PER_ITEM: 8192, MAX_ITEMS: 512, MAX_WRITE_OPERATIONS_PER_HOUR: 1800, MAX_WRITE_OPERATIONS_PER_MINUTE: 120, MAX_SUSTAINED_WRITE_OPERATIONS_PER_MINUTE: 1000000 } : {}),
    };
  }

  const storage = {
    local: makeStorageArea('local'),
    sync: makeStorageArea('sync'),
    session: makeStorageArea('session'),
    managed: {
      get: withCallback((keys) => (keys && typeof keys === 'object' && !Array.isArray(keys) ? Object.assign({}, keys) : {})),
      getBytesInUse: resolved(0),
      onChanged: quiet(),
    },
    onChanged: storageChanged.event,
  };

  // ---- i18n -----------------------------------------------------------------------------
  const messages = Object.create(null);
  for (const key of Object.keys(config.messages || {})) messages[key.toLowerCase()] = config.messages[key];

  function getMessage(name, substitutions) {
    name = String(name || '').toLowerCase();
    if (name === '@@extension_id') return config.id;
    if (name === '@@ui_locale') return config.uiLanguage.replace('-', '_');
    if (name === '@@bidi_dir') return 'ltr';
    if (name === '@@bidi_reversed_dir') return 'rtl';
    if (name === '@@bidi_start_edge') return 'left';
    if (name === '@@bidi_end_edge') return 'right';
    const entry = messages[name];
    if (!entry || typeof entry.message !== 'string') return '';
    const subs = substitutions === undefined || substitutions === null ? [] : Array.isArray(substitutions) ? substitutions : [substitutions];
    const placeholders = Object.create(null);
    for (const key of Object.keys(entry.placeholders || {})) placeholders[key.toLowerCase()] = String(entry.placeholders[key].content || '');
    const fillNumbers = (text) => text.replace(/\$(\d)/g, (match, digit) => {
      const value = subs[Number(digit) - 1];
      return value === undefined ? '' : String(value);
    });
    let text = entry.message.replace(/\$([a-z0-9_@]+)\$/gi, (match, key) => {
      const content = placeholders[key.toLowerCase()];
      return content === undefined ? match : fillNumbers(content);
    });
    text = fillNumbers(text);
    return text.replace(/\$\$/g, '$');
  }

  const i18n = {
    getMessage,
    getUILanguage: () => config.uiLanguage,
    getAcceptLanguages: resolved([config.uiLanguage]),
    detectLanguage: resolved({ isReliable: false, languages: [] }),
  };

  // ---- tabs and windows --------------------------------------------------------------------
  const tabIdArgument = (args) => (typeof args[0] === 'number' ? args.shift() : undefined);

  const tabs = {
    TAB_ID_NONE: -1,
    query: withCallback((query) => call('tabs.query', { query: query || {} })),
    get: withCallback((tabId) => call('tabs.get', { tabId })),
    getCurrent: withCallback(() => call('tabs.getCurrent').then((tab) => tab || undefined)),
    create: withCallback((properties) => call('tabs.create', { properties: properties || {} })),
    duplicate: withCallback((tabId) => call('tabs.get', { tabId }).then((tab) => call('tabs.create', { properties: { url: tab.url, index: tab.index + 1, windowId: tab.windowId } }))),
    update: withCallback((...args) => {
      const tabId = tabIdArgument(args);
      return call('tabs.update', { tabId, properties: args[0] || {} });
    }),
    highlight: withCallback((info) => {
      const index = Array.isArray(info.tabs) ? info.tabs[0] : info.tabs;
      return call('tabs.query', { query: { windowId: info.windowId === undefined ? -2 : info.windowId, index } })
        .then((list) => (list[0] ? call('tabs.update', { tabId: list[0].id, properties: { active: true } }) : null))
        .then(() => call('windows.getCurrent', { populate: true }));
    }),
    move: withCallback((tabIds) => call('tabs.query', { query: {} }).then((list) => list.filter((tab) => [].concat(tabIds).includes(tab.id)))),
    remove: withCallback((tabIds) => call('tabs.remove', { tabIds: [].concat(tabIds) })),
    reload: withCallback((...args) => {
      const tabId = tabIdArgument(args);
      return call('tabs.reload', { tabId, bypassCache: !!(args[0] && args[0].bypassCache) });
    }),
    goBack: withCallback((tabId) => call('tabs.goBack', { tabId })),
    goForward: withCallback((tabId) => call('tabs.goForward', { tabId })),
    sendMessage: withCallback((tabId, message, options) => ready
      .then(() => call('tabs.sendMessage', { tabId, message, frameId: options && typeof options.frameId === 'number' ? options.frameId : undefined }))
      .then(responseOf)),
    connect: (tabId, info) => connect({ tabId, name: (info && info.name) || '', frameId: info && typeof info.frameId === 'number' ? info.frameId : undefined }),
    executeScript: withCallback((...args) => {
      const tabId = tabIdArgument(args);
      const details = Object.assign({}, args[0] || {});
      if (details.file) details.files = [details.file];
      return call('tabs.executeScript', { tabId, details });
    }),
    insertCSS: withCallback((...args) => {
      const tabId = tabIdArgument(args);
      const details = Object.assign({}, args[0] || {});
      if (details.file) details.files = [details.file];
      return call('tabs.insertCSS', { tabId, details });
    }),
    removeCSS: withCallback((...args) => {
      const tabId = tabIdArgument(args);
      const details = Object.assign({}, args[0] || {});
      if (details.file) details.files = [details.file];
      return call('tabs.removeCSS', { tabId, details });
    }),
    captureVisibleTab: notYet('Screenshots of tabs'),
    detectLanguage: resolved('und'),
    getZoom: resolved(1),
    setZoom: resolved(undefined),
    getZoomSettings: resolved({ mode: 'automatic', scope: 'per-origin', defaultZoomFactor: 1 }),
    discard: withCallback((tabId) => call('tabs.get', { tabId })),
    group: notYet('Tab groups'),
    ungroup: resolved(undefined),
    onCreated: makeEvent('tabs.onCreated').event,
    onUpdated: makeEvent('tabs.onUpdated').event,
    onRemoved: makeEvent('tabs.onRemoved').event,
    onActivated: makeEvent('tabs.onActivated').event,
    onHighlighted: quiet(),
    onMoved: quiet(),
    onReplaced: quiet(),
    onAttached: quiet(),
    onDetached: quiet(),
    onZoomChange: quiet(),
  };
  // Older names.
  tabs.getSelected = withCallback((windowId) => call('tabs.query', { query: { active: true, windowId: windowId === undefined ? -2 : windowId } }).then((list) => list[0]));
  tabs.onSelectionChanged = tabs.onActivated;
  tabs.onActiveChanged = tabs.onActivated;

  const windowsApi = {
    WINDOW_ID_NONE: -1,
    WINDOW_ID_CURRENT: -2,
    get: withCallback((windowId, info) => call('windows.get', { windowId, populate: !!(info && info.populate) })),
    getCurrent: withCallback((info) => call('windows.getCurrent', { populate: !!(info && info.populate) })),
    getLastFocused: withCallback((info) => call('windows.getLastFocused', { populate: !!(info && info.populate) })),
    getAll: withCallback((info) => call('windows.getAll', { populate: !!(info && info.populate) })),
    create: withCallback((data) => call('windows.create', { data: data || {} })),
    update: withCallback((windowId, info) => call('windows.update', { windowId, info: info || {} })),
    remove: withCallback((windowId) => call('windows.remove', { windowId })),
    onCreated: makeEvent('windows.onCreated').event,
    onRemoved: makeEvent('windows.onRemoved').event,
    onFocusChanged: makeEvent('windows.onFocusChanged').event,
    onBoundsChanged: quiet(),
  };

  // ---- Toolbar button (action / browserAction / pageAction) ---------------------------------
  const onClicked = makeEvent('action.onClicked');

  function colorString(color) {
    if (Array.isArray(color)) return 'rgba(' + color[0] + ',' + color[1] + ',' + color[2] + ',' + ((color[3] === undefined ? 255 : color[3]) / 255) + ')';
    return String(color);
  }

  function colorArray(text) {
    const probe = document.createElement('canvas').getContext('2d');
    probe.fillStyle = text;
    const value = probe.fillStyle;
    if (/^#[0-9a-f]{6}$/i.test(value)) return [parseInt(value.slice(1, 3), 16), parseInt(value.slice(3, 5), 16), parseInt(value.slice(5, 7), 16), 255];
    const parts = /rgba?\(([^)]+)\)/.exec(value);
    if (!parts) return [0, 0, 0, 255];
    const numbers = parts[1].split(',').map((part) => parseFloat(part));
    return [numbers[0], numbers[1], numbers[2], Math.round((numbers[3] === undefined ? 1 : numbers[3]) * 255)];
  }

  // setIcon({imageData}) gets turned into a picture Prism can show.
  function iconFrom(details) {
    let source = details.imageData !== undefined ? details.imageData : details.path;
    if (source && typeof source === 'object' && !(source instanceof ImageData)) {
      const sizes = Object.keys(source).map(Number).sort((a, b) => Math.abs(a - 32) - Math.abs(b - 32));
      source = source[sizes[0]];
    }
    if (typeof ImageData !== 'undefined' && source instanceof ImageData) {
      const canvas = document.createElement('canvas');
      canvas.width = source.width;
      canvas.height = source.height;
      canvas.getContext('2d').putImageData(source, 0, 0);
      return canvas.toDataURL('image/png');
    }
    if (typeof source !== 'string') return null;
    if (/^(data:|[a-z-]+:\/\/)/i.test(source)) return source;
    // Relative paths start at the extension's folder (like Chrome).
    return new URL(source.replace(/^\//, ''), base).href;
  }

  const setAction = (property, value, tabId) => call('action.set', { property, value, tabId: typeof tabId === 'number' ? tabId : undefined });
  const getAction = (property, details) => call('action.get', { property, tabId: details && typeof details.tabId === 'number' ? details.tabId : undefined });

  const action = {
    setTitle: withCallback((details) => setAction('title', details.title === null ? null : String(details.title), details.tabId)),
    getTitle: withCallback((details) => getAction('title', details)),
    setIcon: withCallback((details) => {
      const icon = iconFrom(details || {});
      return icon === null ? Promise.reject(new Error('Invalid icon')) : setAction('icon', icon, details.tabId);
    }),
    setPopup: withCallback((details) => setAction('popup', details.popup === null ? null : String(details.popup), details.tabId)),
    getPopup: withCallback((details) => getAction('popup', details).then((popup) => (popup ? getURL(popup) : ''))),
    setBadgeText: withCallback((details) => setAction('badgeText', details.text === null || details.text === undefined ? null : String(details.text), details.tabId)),
    getBadgeText: withCallback((details) => getAction('badgeText', details)),
    setBadgeBackgroundColor: withCallback((details) => setAction('badgeColor', colorString(details.color), details.tabId)),
    getBadgeBackgroundColor: withCallback((details) => getAction('badgeColor', details).then(colorArray)),
    setBadgeTextColor: withCallback((details) => setAction('badgeTextColor', colorString(details.color), details.tabId)),
    getBadgeTextColor: withCallback((details) => getAction('badgeTextColor', details).then(colorArray)),
    enable: withCallback((tabId) => setAction('enabled', '1', tabId)),
    disable: withCallback((tabId) => setAction('enabled', '0', tabId)),
    isEnabled: withCallback((tabId) => call('action.get', { property: 'enabled', tabId })),
    openPopup: withCallback(() => call('action.openPopup')),
    getUserSettings: resolved({ isOnToolbar: true }),
    onClicked: onClicked.event,
    onUserSettingsChanged: quiet(),
  };
  const pageAction = Object.assign({}, action, {
    show: withCallback((tabId) => setAction('enabled', '1', tabId)),
    hide: withCallback((tabId) => setAction('enabled', '0', tabId)),
  });

  // ---- scripting (MV3) ---------------------------------------------------------------------
  const scripting = {
    executeScript: withCallback((details) => {
      const data = { target: details.target || {}, args: details.args || [], world: details.world };
      if (typeof details.func === 'function' || typeof details.function === 'function') data.func = String(details.func || details.function);
      else data.files = details.files || [];
      return call('scripting.executeScript', { details: data });
    }),
    insertCSS: withCallback((details) => call('scripting.insertCSS', { details })),
    removeCSS: withCallback((details) => call('scripting.removeCSS', { details })),
    registerContentScripts: withCallback(() => {
      console.warn('[Prism] scripting.registerContentScripts isn\'t supported yet; only manifest content scripts run.');
    }),
    getRegisteredContentScripts: resolved([]),
    unregisterContentScripts: resolved(undefined),
    updateContentScripts: resolved(undefined),
    ExecutionWorld: { ISOLATED: 'ISOLATED', MAIN: 'MAIN' },
  };

  // ---- alarms -------------------------------------------------------------------------------
  const alarms = {
    create: withCallback((...args) => {
      const name = typeof args[0] === 'string' ? args.shift() : '';
      return call('alarms.create', { name, info: args[0] || {} });
    }),
    get: withCallback((name) => call('alarms.get', { name: name || '' }).then((alarm) => alarm || undefined)),
    getAll: withCallback(() => call('alarms.getAll')),
    clear: withCallback((name) => call('alarms.clear', { name: name || '' })),
    clearAll: withCallback(() => call('alarms.clearAll')),
    onAlarm: makeEvent('alarms.onAlarm').event,
  };

  // ---- permissions ------------------------------------------------------------------------------
  const permissions = {
    contains: withCallback((items) => call('permissions.contains', { permissions: items || {} })),
    getAll: withCallback(() => call('permissions.getAll')),
    request: withCallback((items) => call('permissions.request', { permissions: items || {} })),
    remove: withCallback((items) => call('permissions.remove', { permissions: items || {} })),
    onAdded: quiet(),
    onRemoved: quiet(),
  };

  // ---- declarativeNetRequest (the parts ad blockers' popups use) -------------------------------
  const dynamicRules = [];
  const sessionRules = [];
  const updateRules = (list) => withCallback((options) => {
    const remove = new Set((options && options.removeRuleIds) || []);
    for (let i = list.length - 1; i >= 0; i--) if (remove.has(list[i].id)) list.splice(i, 1);
    list.push(...((options && options.addRules) || []));
  });
  const declarativeNetRequest = {
    getEnabledRulesets: withCallback(() => call('dnr.getEnabledRulesets')),
    updateEnabledRulesets: withCallback((options) => call('dnr.updateEnabledRulesets', { options: options || {} })),
    getAvailableStaticRuleCount: resolved(30000),
    getDynamicRules: withCallback(() => dynamicRules.slice()),
    updateDynamicRules: updateRules(dynamicRules),
    getSessionRules: withCallback(() => sessionRules.slice()),
    updateSessionRules: updateRules(sessionRules),
    getDisabledRuleIds: resolved([]),
    updateStaticRules: resolved(undefined),
    isRegexSupported: resolved({ isSupported: true }),
    setExtensionActionOptions: resolved(undefined),
    getMatchedRules: resolved({ rulesInfo: [] }),
    testMatchOutcome: notYet('declarativeNetRequest.testMatchOutcome'),
    onRuleMatchedDebug: quiet(),
    MAX_NUMBER_OF_DYNAMIC_RULES: 30000,
    MAX_NUMBER_OF_SESSION_RULES: 5000,
    MAX_NUMBER_OF_ENABLED_STATIC_RULESETS: 50,
    MAX_NUMBER_OF_STATIC_RULESETS: 100,
    MAX_NUMBER_OF_REGEX_RULES: 1000,
    GUARANTEED_MINIMUM_STATIC_RULES: 30000,
    DYNAMIC_RULESET_ID: '_dynamic',
    SESSION_RULESET_ID: '_session',
  };

  // ---- Smaller APIs -------------------------------------------------------------------------------
  // Right-click menu items (contextMenus / menus): Prism adds them to the
  // page's menu and to the toolbar button's menu.
  const menuClickHandlers = new Map(); // id -> onclick (Manifest V2)
  let menuCounter = 0;
  const menuClicked = makeEvent('menus.onClicked');
  menuClicked.event.addListener((info, tab) => {
    const handler = menuClickHandlers.get(info && info.menuItemId);
    if (handler) handler(info, tab);
  });
  const menuKeys = ['title', 'type', 'contexts', 'documentUrlPatterns', 'targetUrlPatterns', 'enabled', 'visible', 'checked', 'parentId'];
  function menuProps(properties) {
    const props = {};
    for (const key of menuKeys) if (properties && properties[key] !== undefined) props[key] = properties[key];
    return props;
  }
  const menus = {
    // Returns the item's id at once, like Chrome (numbers when none is given).
    create(properties, callback) {
      const props = menuProps(properties);
      const id = properties && properties.id !== undefined ? properties.id : ++menuCounter;
      props.id = id;
      if (properties && typeof properties.onclick === 'function') menuClickHandlers.set(id, properties.onclick);
      ready.then(() => call('menus.create', { props })).then(() => {
        if (typeof callback === 'function') callback();
      }, (error) => {
        if (typeof callback === 'function') {
          lastError = { message: String((error && error.message) || error) };
          try { callback(); } catch (callbackError) { console.error(callbackError); }
          lastError = undefined;
        } else {
          console.error(error);
        }
      });
      return id;
    },
    update: withCallback((id, properties) => {
      if (properties && typeof properties.onclick === 'function') menuClickHandlers.set(id, properties.onclick);
      return ready.then(() => call('menus.update', { id, props: menuProps(properties) }));
    }),
    remove: withCallback((id) => {
      menuClickHandlers.delete(id);
      return ready.then(() => call('menus.remove', { id }));
    }),
    removeAll: withCallback(() => {
      menuClickHandlers.clear();
      return ready.then(() => call('menus.removeAll'));
    }),
    refresh: resolved(undefined),
    onClicked: menuClicked.event,
    onShown: quiet(),
    onHidden: quiet(),
    ACTION_MENU_TOP_LEVEL_LIMIT: 6,
    ContextType: {
      ALL: 'all', PAGE: 'page', FRAME: 'frame', SELECTION: 'selection', LINK: 'link', EDITABLE: 'editable', IMAGE: 'image',
      VIDEO: 'video', AUDIO: 'audio', LAUNCHER: 'launcher', BROWSER_ACTION: 'browser_action', PAGE_ACTION: 'page_action', ACTION: 'action',
    },
    ItemType: { NORMAL: 'normal', CHECKBOX: 'checkbox', RADIO: 'radio', SEPARATOR: 'separator' },
  };

  // Keyboard shortcuts from the manifest's "commands" (Prism sends onCommand).
  const commands = {
    getAll: withCallback(() => ready.then(() => call('commands.getAll'))),
    onCommand: makeEvent('commands.onCommand').event,
    onChanged: quiet(),
  };

  let notificationCounter = 0;
  const notifications = {
    create: withCallback((...args) => {
      const id = typeof args[0] === 'string' ? args.shift() : 'prism-' + (++notificationCounter);
      const options = args[0] || {};
      console.info('[Prism] notification (not shown yet):', options.title, options.message);
      return id;
    }),
    update: resolved(false),
    clear: resolved(true),
    getAll: resolved({}),
    getPermissionLevel: resolved('granted'),
    onClicked: quiet(),
    onClosed: quiet(),
    onButtonClicked: quiet(),
  };

  const extension = {
    getURL,
    inIncognitoContext: false,
    getViews: () => [],
    getBackgroundPage: () => null,
    isAllowedIncognitoAccess: resolved(false),
    isAllowedFileSchemeAccess: resolved(false),
    setUpdateUrlData: () => {},
    sendMessage: runtime.sendMessage,
    onMessage: onMessage.event,
    onRequest: quiet(),
  };

  const self = { id: config.id, name: config.name, shortName: manifest.short_name || config.name, version: manifest.version, enabled: true,
    description: manifest.description || '', type: 'extension', installType: 'normal', mayDisable: true, offlineEnabled: false,
    permissions: manifest.permissions || [], hostPermissions: manifest.host_permissions || [] };
  const management = {
    getSelf: resolved(self),
    getAll: resolved([self]),
    get: withCallback((id) => (id === config.id ? self : Promise.reject(new Error('Not found')))),
    onInstalled: quiet(),
    onUninstalled: quiet(),
    onEnabled: quiet(),
    onDisabled: quiet(),
  };

  // webRequest: Prism sends onBeforeRequest (blocking listeners can cancel
  // or redirect), onCompleted and onErrorOccurred for the requests pages
  // make. Header events aren't sent (Prism can't see or change headers):
  // listening to them is harmless.
  const webRequestListeners = new Map(); // id -> { name, callback, blocking }
  let nextWebRequestListener = 1;
  function webRequestEvent(name, sent) {
    const ids = new Map(); // callback -> id
    return {
      addListener(callback, filter, extraInfoSpec) {
        if (typeof callback !== 'function' || ids.has(callback)) return;
        const id = nextWebRequestListener++;
        const blocking = Array.isArray(extraInfoSpec) && extraInfoSpec.includes('blocking');
        ids.set(callback, id);
        let options = null;
        if (sent) {
          const urls = filter && Array.isArray(filter.urls) ? filter.urls.map(String) : [];
          const types = filter && Array.isArray(filter.types) ? filter.types.map(String) : [];
          options = { id, event: name, urls, types, blocking };
          if (filter && typeof filter.tabId === 'number') options.tabId = filter.tabId;
        }
        // `options` is kept so a reconnect can register it again.
        webRequestListeners.set(id, { name, callback, blocking, options });
        if (options) ready.then(() => call('webRequest.addListener', options)).catch((error) => console.error(error));
      },
      removeListener(callback) {
        const id = ids.get(callback);
        if (id === undefined) return;
        ids.delete(callback);
        webRequestListeners.delete(id);
        if (sent) ready.then(() => call('webRequest.removeListener', { id })).catch(() => {});
      },
      hasListener(callback) { return ids.has(callback); },
      hasListeners() { return ids.size > 0; },
    };
  }

  function handleWebRequest(event) {
    const entry = webRequestListeners.get(event.listener);
    const details = event.details || {};
    const respond = (response) => {
      if (!event.blocking) return;
      const answer = {};
      if (response && typeof response === 'object') {
        if (response.cancel === true) answer.cancel = true;
        if (typeof response.redirectUrl === 'string') answer.redirectUrl = response.redirectUrl;
      }
      call('webRequest.respond', { listener: event.listener, requestId: details.requestId, response: answer }).catch(() => {});
    };
    if (!entry) {
      respond(undefined);
      return;
    }
    let result;
    try {
      result = entry.callback(details);
    } catch (error) {
      console.error(error);
      respond(undefined);
      return;
    }
    // Firefox lets blocking listeners answer with a promise.
    if (result && typeof result.then === 'function') result.then(respond, () => respond(undefined));
    else respond(result);
  }

  const webRequest = {
    onBeforeRequest: webRequestEvent('onBeforeRequest', true), onBeforeSendHeaders: webRequestEvent('onBeforeSendHeaders'),
    onSendHeaders: webRequestEvent('onSendHeaders'), onHeadersReceived: webRequestEvent('onHeadersReceived'),
    onAuthRequired: webRequestEvent('onAuthRequired'), onResponseStarted: webRequestEvent('onResponseStarted'),
    onBeforeRedirect: webRequestEvent('onBeforeRedirect'), onCompleted: webRequestEvent('onCompleted', true),
    onErrorOccurred: webRequestEvent('onErrorOccurred', true),
    handlerBehaviorChanged: resolved(undefined), MAX_HANDLER_BEHAVIOR_CHANGED_CALLS_PER_10_MINUTES: 20,
    ResourceType: {
      MAIN_FRAME: 'main_frame', SUB_FRAME: 'sub_frame', STYLESHEET: 'stylesheet', SCRIPT: 'script', IMAGE: 'image',
      FONT: 'font', OBJECT: 'object', XMLHTTPREQUEST: 'xmlhttprequest', PING: 'ping', CSP_REPORT: 'csp_report',
      MEDIA: 'media', WEBSOCKET: 'websocket', WEBTRANSPORT: 'webtransport', WEBBUNDLE: 'webbundle', OTHER: 'other',
    },
    OnBeforeRequestOptions: { BLOCKING: 'blocking', REQUEST_BODY: 'requestBody', EXTRA_HEADERS: 'extraHeaders' },
    OnBeforeSendHeadersOptions: { REQUEST_HEADERS: 'requestHeaders', BLOCKING: 'blocking', EXTRA_HEADERS: 'extraHeaders' },
    OnHeadersReceivedOptions: { BLOCKING: 'blocking', RESPONSE_HEADERS: 'responseHeaders', EXTRA_HEADERS: 'extraHeaders' },
  };
  const webNavigation = {
    onBeforeNavigate: quiet(), onCommitted: quiet(), onDOMContentLoaded: quiet(), onCompleted: quiet(), onErrorOccurred: quiet(),
    onCreatedNavigationTarget: quiet(), onReferenceFragmentUpdated: quiet(), onHistoryStateUpdated: quiet(), onTabReplaced: quiet(),
    // Prism knows each tab's page (frame 0), not its frames yet.
    getFrame: withCallback((details) => {
      if (!details || details.frameId !== 0) return null;
      return call('tabs.get', { tabId: details.tabId }).then((tab) => ({
        url: tab.url || '', parentFrameId: -1, errorOccurred: false, frameId: 0, documentLifecycle: 'active', frameType: 'outermost_frame',
      }), () => null);
    }),
    getAllFrames: withCallback((details) => call('tabs.get', { tabId: details && details.tabId }).then((tab) => [{
      url: tab.url || '', parentFrameId: -1, errorOccurred: false, frameId: 0, processId: 0, documentLifecycle: 'active', frameType: 'outermost_frame',
    }], () => null)),
  };

  const idle = { queryState: resolved('active'), setDetectionInterval: () => {}, onStateChanged: quiet() };

  // chrome.privacy: Prism's own privacy settings, shown read-only (Prism
  // already turns off network prediction and hyperlink auditing).
  const setting = (value) => ({
    get: withCallback(() => ({ value, levelOfControl: 'not_controllable' })),
    set: resolved(undefined),
    clear: resolved(undefined),
    onChange: quiet(),
  });
  const privacy = {
    network: {
      networkPredictionEnabled: setting(false),
      webRTCIPHandlingPolicy: setting('default'),
      webRTCMultipleRoutesEnabled: setting(true),
      webRTCNonProxiedUdpEnabled: setting(true),
    },
    services: {
      alternateErrorPagesEnabled: setting(false), autofillAddressEnabled: setting(false), autofillCreditCardEnabled: setting(false),
      passwordSavingEnabled: setting(false), safeBrowsingEnabled: setting(false), searchSuggestEnabled: setting(false),
      spellingServiceEnabled: setting(false), translationServiceEnabled: setting(false),
    },
    websites: {
      hyperlinkAuditingEnabled: setting(false), referrersEnabled: setting(true), thirdPartyCookiesAllowed: setting(false),
      doNotTrackEnabled: setting(false), protectedContentEnabled: setting(false),
    },
    IPHandlingPolicy: { DEFAULT: 'default', DEFAULT_PUBLIC_AND_PRIVATE_INTERFACES: 'default_public_and_private_interfaces',
      DEFAULT_PUBLIC_INTERFACE_ONLY: 'default_public_interface_only', DISABLE_NON_PROXIED_UDP: 'disable_non_proxied_udp' },
  };
  const dns = { resolve: notYet('dns.resolve') };
  // chrome.offscreen: a hidden page of the extension (one at a time).
  const offscreenReasons = ['TESTING', 'AUDIO_PLAYBACK', 'IFRAME_SCRIPTING', 'DOM_SCRAPING', 'BLOBS', 'DOM_PARSER', 'USER_MEDIA',
    'DISPLAY_MEDIA', 'WEB_RTC', 'CLIPBOARD', 'LOCAL_STORAGE', 'WORKERS', 'BATTERY_STATUS', 'MATCH_MEDIA', 'GEOLOCATION'];
  const offscreen = {
    createDocument: withCallback((options) => {
      if (!options || typeof options.url !== 'string') return Promise.reject(new TypeError('offscreen.createDocument needs a url.'));
      return call('offscreen.create', { url: new URL(options.url, base).href }).then(() => undefined);
    }),
    closeDocument: withCallback(() => call('offscreen.close').then(() => undefined)),
    hasDocument: withCallback(() => call('offscreen.has').then((has) => has === true)),
    Reason: Object.fromEntries(offscreenReasons.map((reason) => [reason, reason])),
  };

  // ---- Putting it together -------------------------------------------------------------------------
  const api = {
    runtime, storage, i18n, tabs, windows: windowsApi, scripting, alarms, permissions, declarativeNetRequest,
    contextMenus: menus, menus, commands, notifications, extension, management, webRequest, webNavigation, idle, offscreen,
    privacy,
  };
  // Firefox-only APIs only for Firefox extensions (others check for them).
  if (manifest.browser_specific_settings || manifest.applications) api.dns = dns;
  // MV3 has "action"; MV2 "browserAction" and "pageAction". Offer whichever
  // the manifest uses (and "action" either way, as Firefox does).
  api.action = action;
  if (manifest.browser_action || manifest.manifest_version === 2) api.browserAction = action;
  if (manifest.page_action) api.pageAction = pageAction;

  window.chrome = api;
  window.browser = api;

  // ---- fetch() to other sites -------------------------------------------------------------------
  // Extensions may fetch from the sites they have permission for without
  // those sites allowing it (CORS). Prism makes those requests (without
  // cookies) and hands back the answer; anything else is a normal fetch().
  const originalFetch = window.fetch.bind(window);
  const ownOrigin = new URL(base).origin;
  const toBase64 = (buffer) => {
    const bytes = new Uint8Array(buffer);
    let text = '';
    for (let i = 0; i < bytes.length; i += 0x8000) text += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(text);
  };
  window.fetch = function (input, init) {
    let request;
    try {
      request = new Request(input, init);
    } catch (error) {
      return originalFetch(input, init);
    }
    const target = new URL(request.url);
    if ((target.protocol !== 'http:' && target.protocol !== 'https:') || target.origin === ownOrigin || request.mode === 'no-cors') {
      return originalFetch(input, init);
    }
    const method = request.method;
    const bodyPromise = method === 'GET' || method === 'HEAD' ? Promise.resolve(null) : request.clone().arrayBuffer();
    return bodyPromise.then((body) => call('runtime.fetch', {
      url: request.url,
      method,
      headers: Array.from(request.headers.entries()),
      body: body ? toBase64(body) : '',
    })).then((answer) => {
      if (!answer || answer.fallback) return originalFetch(input, init);
      const bytes = Uint8Array.from(atob(answer.base64), (c) => c.charCodeAt(0));
      const noBody = method === 'HEAD' || answer.status === 204 || answer.status === 304;
      const response = new Response(noBody ? null : bytes, {
        status: answer.status < 200 || answer.status > 599 ? 200 : answer.status,
        statusText: answer.statusText || '',
        headers: answer.headers.filter((pair) => !/^(content-encoding|transfer-encoding|content-length)$/i.test(pair[0])),
      });
      Object.defineProperty(response, 'url', { value: answer.url || request.url });
      return response;
    }, (error) => {
      throw new TypeError('Load failed: ' + ((error && error.message) || error));
    });
  };

  // ---- Background service workers run as a page -------------------------------------------------------
  if (isBackground && config.worker) {
    window.importScripts = (...urls) => {
      for (const url of urls) {
        const absolute = new URL(url, location.href).href;
        const code = window.prompt('prism-ext:read\n' + absolute);
        if (code === null) throw new Error("importScripts: couldn't load " + url);
        (0, eval)(code + '\n//# sourceURL=' + absolute);
      }
    };
    window.registration = {
      scope: base, active: { state: 'activated', scriptURL: getURL(manifest.background && manifest.background.service_worker) },
      showNotification: () => Promise.resolve(), getNotifications: () => Promise.resolve([]),
      unregister: () => Promise.resolve(false), update: () => Promise.resolve(), addEventListener() {}, removeEventListener() {},
    };
    window.serviceWorker = window.registration.active;
    window.skipWaiting = () => Promise.resolve();
    window.clients = {
      claim: () => Promise.resolve(),
      matchAll: () => Promise.resolve([]),
      get: () => Promise.resolve(undefined),
      openWindow: (url) => api.tabs.create({ url }).then(() => null),
    };
    // Workers get "install" and "activate" once their script has run.
    window.addEventListener('load', () => {
      for (const type of ['install', 'activate']) {
        const event = new Event(type);
        event.waitUntil = () => {};
        window.dispatchEvent(event);
      }
    });
  }

  // ---- Popups fit their content (like Chrome: at most 800 x 600) -------------------------------------
  function watchPopupSize() {
    let scheduled = false;
    let last = '';
    const measure = () => {
      scheduled = false;
      const root = document.documentElement;
      if (!root || !document.body) return;
      const saved = root.getAttribute('style');
      root.style.setProperty('width', 'max-content', 'important');
      root.style.setProperty('height', 'auto', 'important');
      const width = Math.ceil(Math.max(root.getBoundingClientRect().width, document.body.scrollWidth));
      const height = Math.ceil(Math.max(root.scrollHeight, document.body.getBoundingClientRect().height));
      if (saved === null) root.removeAttribute('style');
      else root.setAttribute('style', saved);
      const size = width + 'x' + height;
      if (size === last || !width || !height) return;
      last = size;
      call('popup.resize', { width, height }).catch(() => {});
    };
    const schedule = () => {
      if (scheduled) return;
      scheduled = true;
      requestAnimationFrame(measure);
    };
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', schedule, { once: true });
    else schedule();
    window.addEventListener('load', schedule);
    const observe = () => {
      if (typeof ResizeObserver !== 'undefined' && document.body) new ResizeObserver(schedule).observe(document.body);
      new MutationObserver(schedule).observe(document.documentElement, { childList: true, subtree: true, attributes: true });
    };
    if (document.body) observe();
    else document.addEventListener('DOMContentLoaded', observe, { once: true });
  }
})
