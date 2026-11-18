// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Runtime for extension content scripts.
//
// Prism adds one user script per extension to every page, made of this
// function expression followed by "(config, [scripts])". It checks whether
// the page matches each manifest "content_scripts" entry and runs that
// entry's styles and scripts at the right time, giving them a chrome.* /
// browser.* object for the APIs Prism supports.
//
// WebKit's Windows API can only run scripts in the page's own JavaScript
// world (not an isolated one like Chrome does), so this code is careful:
// - everything it relies on is captured before any page script runs, and
// - the per-extension token that proves a message comes from the extension
//   is never stored anywhere a page script can reach.
(function (config, scripts) {
  'use strict';

  const win = window;
  const doc = document;
  const handlers = win.webkit && win.webkit.messageHandlers;
  const handler = handlers && handlers.prismExtension;
  if (!handler) return;

  // ---- Captured built-ins -----------------------------------------------
  const post = handler.postMessage.bind(handler);
  const stringify = JSON.stringify;
  const parseJSON = JSON.parse;
  const PromiseCtor = Promise;
  const RegExpCtor = RegExp;
  const createObject = Object.create;
  const keysOf = Object.keys;
  const assign = Object.assign;
  const freeze = Object.freeze;
  const defineProperty = Object.defineProperty;
  const isArray = Array.isArray;
  const apply = Reflect.apply;
  const addEventListenerFn = EventTarget.prototype.addEventListener;
  const createElementFn = Document.prototype.createElement;
  const appendChildFn = Node.prototype.appendChild;
  const regExpTest = RegExp.prototype.test;
  const setTimeoutFn = win.setTimeout.bind(win);
  const consoleError = console.error.bind(console);
  const MapCtor = Map;
  const mapGet = Map.prototype.get;
  const mapSet = Map.prototype.set;
  const mapDelete = Map.prototype.delete;

  const location = win.location;
  if (location.protocol !== 'http:' && location.protocol !== 'https:' && location.protocol !== 'file:') return;
  if (location.protocol === 'file:' && !config.injected) return;
  const pageURL = location.href.split('#')[0];
  const isTopFrame = win.top === win;

  // Keep the token out of reach: only this closure can see it.
  const token = config.token;
  config.token = undefined;

  // ---- Match patterns and globs -------------------------------------------
  function escapeRegExp(text) {
    return text.replace(/[.+?^${}()|[\]\\]/g, '\\$&');
  }

  function patternToRegExp(pattern) {
    if (pattern === '<all_urls>') return /^(https?|wss?):\/\//;
    const parts = /^(\*|https?|wss?|ftp|file|urn):\/\/(\*|\*\.[^/*]+|[^/*]+)?(\/.*)?$/.exec(pattern);
    if (!parts) return null;
    const scheme = parts[1] === '*' ? '(https?|wss?)' : escapeRegExp(parts[1]);
    let host = parts[2] || '';
    if (host === '*') host = '[^/]+';
    else if (host.startsWith('*.')) host = '([^/]+\\.)?' + escapeRegExp(host.slice(2));
    else host = escapeRegExp(host);
    const path = escapeRegExp(parts[3] || '/').replace(/\*/g, '.*');
    return new RegExpCtor('^' + scheme + ':\\/\\/' + host + '(:\\d+)?' + path + '$', 'i');
  }

  function globToRegExp(glob) {
    return new RegExpCtor('^' + escapeRegExp(glob).replace(/\*/g, '.*').replace(/\\\?/g, '.') + '$');
  }

  function testAny(list, convert) {
    if (!list) return false;
    for (let i = 0; i < list.length; i++) {
      const regExp = convert(list[i]);
      if (regExp && apply(regExpTest, regExp, [pageURL])) return true;
    }
    return false;
  }

  function entryMatches(entry) {
    if (!entry.allFrames && !isTopFrame) return false;
    if (!testAny(entry.matches, patternToRegExp)) return false;
    if (testAny(entry.excludeMatches, patternToRegExp)) return false;
    if (entry.includeGlobs && entry.includeGlobs.length && !testAny(entry.includeGlobs, globToRegExp)) return false;
    if (testAny(entry.excludeGlobs, globToRegExp)) return false;
    return true;
  }

  // ---- Talking to Prism -----------------------------------------------------
  function call(op, data) {
    // A prototype-less object: a page can't hook into it while it's turned
    // into JSON (e.g. with an Object.prototype.toJSON getter).
    const message = assign(createObject(null), data || {});
    message.t = token;
    message.op = op;
    let text;
    try {
      text = stringify(message);
    } catch (error) {
      return PromiseCtor.reject(error);
    }
    return new PromiseCtor((resolve, reject) => {
      post(text).then((reply) => {
        let result;
        try {
          result = parseJSON(reply);
        } catch (error) {
          reject(new Error('Prism sent an unreadable reply.'));
          return;
        }
        if (result && result.ok) resolve(result.result);
        else reject(new Error((result && result.error) || 'Unknown error'));
      }, (error) => reject(new Error(String((error && error.message) || error))));
    });
  }

  // ---- API helpers ------------------------------------------------------------
  let lastError;

  // Supports both styles: promises (browser.*, MV3) and a trailing callback
  // (chrome.*, with chrome.runtime.lastError set on failure).
  function withCallback(fn) {
    return function (...args) {
      const callback = typeof args[args.length - 1] === 'function' ? args.pop() : null;
      let promise;
      try {
        promise = fn(...args);
      } catch (error) {
        promise = PromiseCtor.reject(error);
      }
      if (!callback) return promise;
      promise.then((value) => {
        try { callback(value); } catch (error) { consoleError(error); }
      }, (error) => {
        lastError = { message: String((error && error.message) || error) };
        try { callback(); } catch (callbackError) { consoleError(callbackError); }
        lastError = undefined;
      });
      return undefined;
    };
  }

  function makeEvent() {
    const listeners = [];
    return {
      listeners,
      event: freeze({
        addListener(listener) { if (typeof listener === 'function' && !listeners.includes(listener)) listeners.push(listener); },
        removeListener(listener) { const i = listeners.indexOf(listener); if (i >= 0) listeners.splice(i, 1); },
        hasListener(listener) { return listeners.includes(listener); },
        hasListeners() { return listeners.length > 0; },
      }),
      fire(...args) {
        for (const listener of listeners.slice()) {
          try { listener(...args); } catch (error) { consoleError(error); }
        }
      },
    };
  }

  // ---- storage ------------------------------------------------------------------
  const storageChanged = makeEvent();

  const areaEvents = createObject(null);

  function makeStorageArea(area) {
    const areaChanged = makeEvent();
    areaEvents[area] = (changes) => { areaChanged.fire(changes); storageChanged.fire(changes, area); };

    function fireChanged(changes) {
      if (!changes || !keysOf(changes).length) return;
      areaChanged.fire(changes);
      storageChanged.fire(changes, area);
    }

    function normalizeKeys(keys) {
      if (keys === null || keys === undefined) return { keys: null, defaults: null };
      if (typeof keys === 'string') return { keys: [keys], defaults: null };
      if (isArray(keys)) return { keys: keys.map(String), defaults: null };
      if (typeof keys === 'object') return { keys: keysOf(keys), defaults: keys };
      throw new TypeError('Invalid storage keys');
    }

    return freeze({
      get: withCallback((keys) => {
        const normalized = normalizeKeys(keys);
        return call('storage.get', { area, keys: normalized.keys }).then((result) => {
          const out = {};
          if (normalized.defaults) assign(out, normalized.defaults);
          return assign(out, result || {});
        });
      }),
      set: withCallback((items) => {
        if (!items || typeof items !== 'object') return PromiseCtor.reject(new TypeError('Invalid storage items'));
        return call('storage.set', { area, items, ctx: contextId }).then((result) => { fireChanged(result && result.changes); });
      }),
      remove: withCallback((keys) => {
        const list = typeof keys === 'string' ? [keys] : isArray(keys) ? keys.map(String) : [];
        return call('storage.remove', { area, keys: list, ctx: contextId }).then((result) => { fireChanged(result && result.changes); });
      }),
      clear: withCallback(() => call('storage.clear', { area, ctx: contextId }).then((result) => { fireChanged(result && result.changes); })),
      getBytesInUse: withCallback(() => call('storage.bytes', { area })),
      onChanged: lazyEvent(areaChanged),
    });
  }

  // ---- i18n -----------------------------------------------------------------------
  const messages = createObject(null);
  for (const key of keysOf(config.messages || {})) messages[key.toLowerCase()] = config.messages[key];

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
    const subs = substitutions === undefined || substitutions === null ? [] : isArray(substitutions) ? substitutions : [substitutions];
    const placeholders = createObject(null);
    for (const key of keysOf(entry.placeholders || {})) placeholders[key.toLowerCase()] = String(entry.placeholders[key].content || '');
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

  // ---- Messages from the rest of the extension ----------------------------------------
  // Registering with Prism is lazy: only content scripts that listen for
  // messages (or use ports or storage.onChanged) keep a request open.
  let contextId = 0;
  let hello = null;
  const ports = new MapCtor();

  function register() {
    if (!hello) {
      hello = call('hello', { url: pageURL }).then((info) => {
        contextId = info.ctx;
        pollLoop();
        return info;
      });
      hello.then(null, (error) => consoleError('[' + config.name + ']', error));
    }
    return hello;
  }

  function pollLoop() {
    call('poll', { ctx: contextId }).then((events) => {
      if (isArray(events)) for (const event of events) handleEvent(event);
      pollLoop();
    }, () => setTimeoutFn(pollLoop, 2000));
  }

  function handleEvent(event) {
    if (!event) return;
    if (event.type === 'message') dispatchMessage(onMessage.listeners, event);
    else if (event.type === 'connect') onConnect.fire(makePort(event.port, event.name, event.sender, true));
    else if (event.type === 'portMessage') {
      const entry = apply(mapGet, ports, [event.port]);
      if (entry) entry.onMessage.fire(event.message, entry.port);
    } else if (event.type === 'portDisconnect') {
      const entry = apply(mapGet, ports, [event.port]);
      if (entry) entry.close();
    } else if (event.type === 'storageChanged') {
      const fire = areaEvents[event.area];
      if (fire && event.changes && keysOf(event.changes).length) fire(event.changes);
    }
  }

  // onMessage listeners answer by calling sendResponse, returning true (and
  // calling it later) or returning a promise (Firefox style).
  function dispatchMessage(listeners, event) {
    let responded = false;
    let willRespond = false;
    const respond = (handled, response) => {
      if (responded) return;
      responded = true;
      const data = { id: event.id, handled, ctx: contextId };
      if (handled) data.response = response;
      call('respond', data).then(null, (error) => consoleError(error));
    };
    const sendResponse = (response) => respond(true, response);
    for (const listener of listeners.slice()) {
      let result;
      try {
        result = listener(event.message, event.sender, sendResponse);
      } catch (error) {
        consoleError(error);
        continue;
      }
      if (result === true) willRespond = true;
      else if (result && typeof result.then === 'function') {
        willRespond = true;
        result.then(sendResponse, (error) => { consoleError(error); respond(false); });
      }
    }
    if (!willRespond && !responded) respond(false);
  }

  function makePort(id, name, sender, connected) {
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
        else call('port.post', { ctx: contextId, port: portId, message }).then(null, () => {});
      },
      disconnect() {
        if (!open) return;
        open = false;
        if (portId !== null) {
          apply(mapDelete, ports, [portId]);
          call('port.disconnect', { ctx: contextId, port: portId }).then(null, () => {});
        }
      },
    };
    const entry = {
      port,
      onMessage: portMessage,
      close() {
        if (!open) return;
        open = false;
        apply(mapDelete, ports, [portId]);
        portDisconnect.fire(port);
      },
      setId(newId) {
        portId = newId;
        apply(mapSet, ports, [newId, entry]);
        if (!open) {
          call('port.disconnect', { ctx: contextId, port: newId }).then(null, () => {});
          return;
        }
        for (const message of queued.splice(0)) call('port.post', { ctx: contextId, port: newId, message }).then(null, () => {});
      },
    };
    if (connected) entry.setId(id);
    else portId = null;
    return { port, entry };
  }

  // ---- runtime ----------------------------------------------------------------------
  const onMessage = makeEvent();
  const onConnect = makeEvent();
  const base = config.base || ('chrome-extension://' + config.id + '/');
  const extensionURL = (path) => base + String(path || '').replace(/^\//, '');

  function lazyEvent(source) {
    return freeze({
      addListener(listener) { register(); source.event.addListener(listener); },
      removeListener: source.event.removeListener,
      hasListener: source.event.hasListener,
      hasListeners: source.event.hasListeners,
    });
  }

  // sendMessage([extensionId,] message [, options]), like Chrome reads it.
  function messageArguments(args) {
    if (args.length === 1) return { message: args[0] };
    if (args.length >= 3) return { id: args[0], message: args[1] };
    const optionsLike = args[1] && typeof args[1] === 'object' && 'includeTlsChannelId' in args[1];
    if ((typeof args[0] === 'string' || args[0] === null || args[0] === undefined) && !optionsLike) return { id: args[0], message: args[1] };
    return { message: args[0] };
  }

  const connectEvent = makeEvent();
  const runtime = {
    id: config.id,
    getURL: extensionURL,
    getManifest: () => parseJSON(stringify(config.manifest)),
    sendMessage: withCallback((...args) => {
      const target = messageArguments(args);
      if (target.id !== undefined && target.id !== null && target.id !== config.id) {
        return PromiseCtor.reject(new Error('Messaging other extensions isn\'t supported in Prism yet.'));
      }
      return register()
        .then(() => call('runtime.sendMessage', { ctx: contextId, message: target.message }))
        .then((result) => (result && 'response' in result ? result.response : undefined));
    }),
    connect(...args) {
      const info = args.find((arg) => arg && typeof arg === 'object') || {};
      const made = makePort(null, info.name, undefined, false);
      register().then(() => call('port.connect', { ctx: contextId, name: info.name || '' })).then((result) => {
        made.entry.setId(result.port);
        if (result.disconnected) {
          lastError = { message: 'Could not establish connection. Receiving end does not exist.' };
          made.entry.close();
          lastError = undefined;
        }
      }, () => made.entry.close());
      return made.port;
    },
    onMessage: lazyEvent(onMessage),
    onConnect: lazyEvent({ event: connectEvent.event }),
    getPlatformInfo: withCallback(() => PromiseCtor.resolve({ os: 'win', arch: 'x86-64', nacl_arch: 'x86-64' })),
  };
  // onConnect listeners get the port object, not the internal entry.
  onConnect.fire = (made) => connectEvent.fire(made.port);
  defineProperty(runtime, 'lastError', { get: () => lastError, enumerable: true });

  const api = {
    runtime: freeze(runtime),
    storage: freeze({
      local: makeStorageArea('local'),
      sync: makeStorageArea('sync'),
      session: makeStorageArea('session'),
      onChanged: lazyEvent(storageChanged),
    }),
    i18n: freeze({
      getMessage,
      getUILanguage: () => config.uiLanguage,
      getAcceptLanguages: withCallback(() => PromiseCtor.resolve([config.uiLanguage])),
    }),
    extension: freeze({
      getURL: extensionURL,
      inIncognitoContext: false,
      sendMessage: (...args) => runtime.sendMessage(...args),
      onMessage: lazyEvent(onMessage),
    }),
  };
  freeze(api);

  // ---- Running the content scripts -------------------------------------------------
  function addStyle(css) {
    const insert = () => {
      const style = apply(createElementFn, doc, ['style']);
      style.textContent = css;
      apply(appendChildFn, doc.documentElement || doc.head || doc.body, [style]);
    };
    if (doc.documentElement) insert();
    else apply(addEventListenerFn, doc, ['readystatechange', insert, { once: true }]);
  }

  function whenReady(runAt, task) {
    if (runAt === 'document_start') return task();
    const run = runAt === 'document_end' ? task : () => setTimeoutFn(task, 0);
    if (doc.readyState === 'loading') apply(addEventListenerFn, doc, ['DOMContentLoaded', run, { once: true }]);
    else run();
  }

  const entries = config.entries || [];
  for (let i = 0; i < entries.length; i++) {
    const entry = entries[i];
    if (!entryMatches(entry)) continue;
    // Styles go in straight away (like Chrome), scripts at their run_at time.
    if (entry.css) addStyle(entry.css);
    const script = scripts[i];
    if (typeof script !== 'function') continue;
    whenReady(entry.runAt, () => {
      try {
        apply(script, win, [api, api]);
      } catch (error) {
        consoleError('[' + config.name + '] content script error:', error);
      }
    });
  }
})
