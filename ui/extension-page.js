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
  const manifest = config.manifest || {};

  // ---- Talking to Prism -----------------------------------------------------------
  function call(op, data) {
    const message = Object.assign({}, data || {}, { op });
    let text;
    try {
      text = JSON.stringify(message);
    } catch (error) {
      return Promise.reject(error);
    }
    return handler.postMessage(text).then((reply) => {
      const result = JSON.parse(reply);
      if (result && result.ok) return result.result;
      throw new Error((result && result.error) || 'Unknown error');
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
    }, () => setTimeout(pollLoop, 2000));
  }

  function handleEvent(event) {
    if (!event) return;
    switch (event.type) {
    case 'message':
      dispatchMessage(event);
      break;
    case 'connect':
      onConnect.fire(makePort(event.port, event.name, event.sender).port);
      break;
    case 'portMessage': {
      const entry = ports.get(event.port);
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
    getBrowserInfo: resolved({ name: 'Prism', vendor: 'Prism', version: '0.5.1', buildID: '0' }),
    getContexts: resolved([]),
    getBackgroundPage: notYet('Reaching the background page directly'),
    getPackageDirectoryEntry: notYet('getPackageDirectoryEntry'),
    OnInstalledReason: { INSTALL: 'install', UPDATE: 'update', CHROME_UPDATE: 'chrome_update', SHARED_MODULE_UPDATE: 'shared_module_update' },
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
  let menuCounter = 0;
  const menus = {
    create: (properties, callback) => {
      if (typeof callback === 'function') setTimeout(callback, 0);
      return (properties && properties.id) || ++menuCounter;
    },
    update: resolved(undefined),
    remove: resolved(undefined),
    removeAll: resolved(undefined),
    onClicked: quiet(),
    onShown: quiet(),
    onHidden: quiet(),
    ContextType: {},
  };

  const commands = {
    getAll: resolved(Object.keys(manifest.commands || {}).map((name) => ({
      name, description: (manifest.commands[name] || {}).description || '', shortcut: '',
    }))),
    onCommand: quiet(),
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

  // webRequest listeners are accepted but don't see anything yet.
  const webRequestEvent = () => ({ addListener() {}, removeListener() {}, hasListener: () => false, hasListeners: () => false });
  const webRequest = {
    onBeforeRequest: webRequestEvent(), onBeforeSendHeaders: webRequestEvent(), onSendHeaders: webRequestEvent(),
    onHeadersReceived: webRequestEvent(), onAuthRequired: webRequestEvent(), onResponseStarted: webRequestEvent(),
    onBeforeRedirect: webRequestEvent(), onCompleted: webRequestEvent(), onErrorOccurred: webRequestEvent(),
    handlerBehaviorChanged: resolved(undefined), MAX_HANDLER_BEHAVIOR_CHANGED_CALLS_PER_10_MINUTES: 20,
  };
  const webNavigation = {
    onBeforeNavigate: quiet(), onCommitted: quiet(), onDOMContentLoaded: quiet(), onCompleted: quiet(), onErrorOccurred: quiet(),
    onCreatedNavigationTarget: quiet(), onReferenceFragmentUpdated: quiet(), onHistoryStateUpdated: quiet(), onTabReplaced: quiet(),
    getFrame: resolved(null), getAllFrames: resolved(null),
  };

  const idle = { queryState: resolved('active'), setDetectionInterval: () => {}, onStateChanged: quiet() };
  const offscreen = { createDocument: notYet('Offscreen documents'), closeDocument: resolved(undefined), hasDocument: resolved(false), Reason: {} };

  // ---- Putting it together -------------------------------------------------------------------------
  const api = {
    runtime, storage, i18n, tabs, windows: windowsApi, scripting, alarms, permissions, declarativeNetRequest,
    contextMenus: menus, menus, commands, notifications, extension, management, webRequest, webNavigation, idle, offscreen,
  };
  // MV3 has "action"; MV2 "browserAction" and "pageAction". Offer whichever
  // the manifest uses (and "action" either way, as Firefox does).
  api.action = action;
  if (manifest.browser_action || manifest.manifest_version === 2) api.browserAction = action;
  if (manifest.page_action) api.pageAction = pageAction;

  window.chrome = api;
  window.browser = api;

  // ---- fetch() of the extension's own files --------------------------------------------------------
  // Pages loaded from files can't fetch() other files, so Prism reads them.
  const types = { json: 'application/json', js: 'text/javascript', mjs: 'text/javascript', css: 'text/css', html: 'text/html', htm: 'text/html',
    txt: 'text/plain', svg: 'image/svg+xml', png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', gif: 'image/gif', webp: 'image/webp',
    wasm: 'application/wasm', woff2: 'font/woff2', woff: 'font/woff', ttf: 'font/ttf', xml: 'text/xml', map: 'application/json' };
  const originalFetch = window.fetch.bind(window);
  window.fetch = function (input, init) {
    const target = typeof input === 'string' ? input : input instanceof URL ? input.href : input && input.url;
    let absolute;
    try { absolute = new URL(String(target), location.href).href; } catch (error) { absolute = ''; }
    if (absolute && normalize(absolute).startsWith(normalize(base))) {
      return call('runtime.readFile', { url: absolute }).then((file) => {
        const bytes = Uint8Array.from(atob(file.base64), (c) => c.charCodeAt(0));
        const extensionOf = /\.([a-z0-9]+)(?:[?#]|$)/i.exec(absolute);
        const type = types[extensionOf ? extensionOf[1].toLowerCase() : ''] || 'application/octet-stream';
        return new Response(bytes, { status: 200, headers: { 'Content-Type': type } });
      }, () => new Response('', { status: 404 }));
    }
    return originalFetch(input, init);
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
