// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Prism's translator, the part in web pages. PrismBundle runs this in a
// JavaScript world of Prism's own as each page starts loading, so the page
// can't see or change it. It samples the page's text for Prism to detect the
// language, and if Prism says to translate, it translates the page while it
// loads: what's on screen first, then the rest, then whatever the page adds
// later. The text goes to Prism (Translate.cpp), which translates it on this
// computer with Bergamot; nothing leaves the PC.
//
// Like Firefox Translations, each block of text (a paragraph, a heading, a
// list item) is translated as one piece, with its inline markup (links,
// bold, ...) kept: the translated HTML is merged back into the page's own
// elements, so links, buttons and the page's scripts keep working.

(function () {
  'use strict';

  let isTop = false;
  try { isTop = window.top === window; } catch (e) { isTop = false; }
  if (!isTop || !/^https?:$/.test(location.protocol)) return;
  const host = location.hostname;
  if (host === 'localhost' || host.endsWith('.localhost')) return; // Prism's and extensions' own pages
  const native = window.__prismNative;
  if (typeof native !== 'function' || window.__prismTranslate) return;

  const HTML_NS = 'http://www.w3.org/1999/xhtml';
  // Not translated (from Firefox Translations), and things that would reload
  // or lose their state if they were moved.
  const SKIPPED = new Set(['code', 'kbd', 'samp', 'var', 'acronym', 'dir', 'applet', 'math', 'embed', 'object', 'iframe',
    'frame', 'metadata', 'noscript', 'noembed', 'noframes', 'head', 'style', 'script', 'template', 'textarea', 'select',
    'input', 'video', 'audio', 'canvas', 'svg', 'img', 'picture', 'br', 'hr', 'wbr', 'option', 'datalist']);
  // Inline elements that keep their content as it is (sent as placeholders).
  const KEPT_INLINE = new Set(['code', 'kbd', 'samp', 'var', 'img', 'picture', 'svg', 'br', 'wbr', 'input', 'math', 'canvas', 'select']);
  // These break a block into pieces, so they're never moved around.
  const STATEFUL = new Set(['iframe', 'frame', 'video', 'audio', 'object', 'embed', 'textarea']);
  const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr']);
  const ATTRIBUTES = ['title', 'alt', 'placeholder', 'aria-label', 'aria-description'];
  const LETTER = /\p{L}/u;
  const BATCH_ITEMS = 24;
  const BATCH_CHARS = 5000;
  const IN_FLIGHT = 2;

  // ---- Talking to Prism ---------------------------------------------------------------

  let nextCall = 1;
  const calls = new Map();
  Object.defineProperty(window, '__prismReceive', {
    value: (id, text) => {
      const done = calls.get(id);
      if (!done) return;
      calls.delete(id);
      try {
        const parsed = JSON.parse(text);
        done(parsed && parsed.ok ? parsed.result : null);
      } catch (e) {
        done(null);
      }
    },
  });
  function send(message) {
    return new Promise((resolve) => {
      const id = nextCall++;
      calls.set(id, resolve);
      try {
        native(JSON.stringify(message), id);
      } catch (e) {
        calls.delete(id);
        resolve(null);
      }
    });
  }

  // ---- What counts as text --------------------------------------------------------------

  let styleCache = new WeakMap();
  function displayOf(element) {
    let display = styleCache.get(element);
    if (display === undefined) {
      try { display = getComputedStyle(element).display; } catch (e) { display = 'inline'; }
      styleCache.set(element, display);
    }
    return display;
  }
  function isBlock(element) {
    const display = displayOf(element);
    return display !== 'inline' && display !== 'none' && display !== 'contents';
  }
  function name(element) {
    return element.namespaceURI === HTML_NS ? element.localName : element.localName === 'svg' ? 'svg' : 'math';
  }
  function skipped(element) {
    if (element.namespaceURI !== HTML_NS) return true;
    if (SKIPPED.has(element.localName)) return true;
    if (element.getAttribute('translate') === 'no' || element.classList.contains('notranslate')) return true;
    return element.isContentEditable; // what you're typing
  }
  function hasLetters(text) {
    return !!text && LETTER.test(text);
  }
  // The text a block would send (not counting what's kept as it is).
  function ownText(element) {
    let text = '';
    for (const child of element.childNodes) {
      if (child.nodeType === 3) text += child.data;
      else if (child.nodeType === 1 && !skipped(child)) text += ownText(child);
      if (text.length > 64) break;
    }
    return text;
  }
  // Does this element hold other blocks (so its pieces are translated one by one)?
  function needsSplitting(element) {
    for (const child of element.childNodes) {
      if (child.nodeType !== 1) continue;
      if (STATEFUL.has(name(child))) return true;
      if (skipped(child)) {
        if (!KEPT_INLINE.has(name(child)) && isBlock(child)) return true;
        continue;
      }
      if (isBlock(child) || needsSplitting(child)) return true;
    }
    return false;
  }

  // ---- Pieces to translate --------------------------------------------------------------

  // node -> { after: what we put there, count: times translated lately, at, pending }
  const records = new WeakMap();
  const cache = new Map(); // "h|text" -> translation
  const queues = { visible: [], rest: [] };
  let inFlight = 0;
  let failures = 0;
  let from = '';
  let to = '';
  let active = false;
  let applying = false;
  let observer = null;

  function escapeText(text) {
    return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  // The block as simple HTML: tags with an id each (no other attributes),
  // and placeholders for what stays as it is.
  function serialize(element, live, kept) {
    let html = '';
    for (const child of element.childNodes) {
      if (child.nodeType === 3) {
        html += escapeText(child.data);
      } else if (child.nodeType === 1) {
        const id = live.length;
        live.push(child);
        if (skipped(child)) {
          kept.add(id);
          html += '<span data-p="' + id + '"></span>';
          continue;
        }
        const tag = child.localName.replace(/[^a-z0-9-]/g, '') || 'span';
        if (VOID.has(tag)) {
          kept.add(id);
          html += '<' + tag + ' data-p="' + id + '">';
          continue;
        }
        html += '<' + tag + ' data-p="' + id + '">' + serialize(child, live, kept) + '</' + tag + '>';
      }
    }
    return html;
  }

  function isDone(node) {
    const record = records.get(node);
    if (!record) return false;
    if (record.pending) return true;
    // The page keeps changing it back: leave it for a while.
    if (record.count >= 4 && Date.now() - record.at < 30000) return true;
    return (node.nodeType === 3 ? node.data : node.innerHTML) === record.after;
  }

  function makeUnit(node) {
    if (node.nodeType === 3) {
      return { kind: 'text', node, html: false, source: node.data, snapshot: node.data };
    }
    if (!node.firstElementChild) {
      return { kind: 'plain', node, html: false, source: node.textContent, snapshot: node.innerHTML };
    }
    const live = [];
    const kept = new Set();
    const source = serialize(node, live, kept);
    return { kind: 'html', node, html: true, source, snapshot: node.innerHTML, live, kept };
  }

  function collect(node, out) {
    if (node.nodeType === 3) {
      if (hasLetters(node.data) && !isDone(node)) out.push(makeUnit(node));
      return;
    }
    if (node.nodeType !== 1 || skipped(node)) return;
    if (!needsSplitting(node)) {
      if (!isDone(node) && hasLetters(ownText(node))) out.push(makeUnit(node));
      return;
    }
    for (const child of node.childNodes) collect(child, out);
  }

  function collectAttributes(root, out) {
    if (root.nodeType !== 1) return;
    const selector = ATTRIBUTES.map((attribute) => '[' + attribute + ']').join(',') + ',input[type=button][value],input[type=reset][value]';
    const elements = root.matches(selector) ? [root, ...root.querySelectorAll(selector)] : root.querySelectorAll(selector);
    for (const element of elements) {
      if (element.namespaceURI !== HTML_NS || element.closest('[translate="no"],.notranslate,script,style,template')) continue;
      const names = ATTRIBUTES.slice();
      if (element.localName === 'input' && /^(button|reset)$/i.test(element.type)) names.push('value');
      for (const attribute of names) {
        const value = element.getAttribute(attribute);
        if (!hasLetters(value)) continue;
        const done = records.get(element);
        if (done && done['@' + attribute] === value) continue;
        out.push({ kind: 'attribute', node: element, attribute, html: false, source: value, snapshot: value });
      }
    }
  }

  function onScreen(unit) {
    const element = unit.node.nodeType === 3 ? unit.node.parentElement : unit.node;
    if (!element) return false;
    try {
      const rect = element.getBoundingClientRect();
      return rect.bottom > -200 && rect.top < innerHeight + 400 && rect.right > 0 && rect.left < innerWidth && (rect.width || rect.height);
    } catch (e) {
      return false;
    }
  }

  const MAX_PIECE = 20000; // characters; bigger pieces (rare) stay as they are

  function enqueue(units) {
    for (const unit of units) {
      if (unit.source.length > MAX_PIECE) continue;
      if (unit.kind !== 'attribute') {
        const record = records.get(unit.node) || { count: 0 };
        record.pending = true;
        records.set(unit.node, record);
      }
      (unit.kind !== 'attribute' && onScreen(unit) ? queues.visible : queues.rest).push(unit);
    }
    pump();
  }

  // ---- Putting translations into the page -----------------------------------------------

  function unchanged(unit) {
    if (!unit.node.isConnected) return false;
    if (unit.kind === 'text') return unit.node.data === unit.snapshot;
    if (unit.kind === 'attribute') return unit.node.getAttribute(unit.attribute) === unit.snapshot;
    return unit.node.innerHTML === unit.snapshot;
  }

  // Rebuilds `liveParent`'s children in the translated order, reusing the
  // page's own elements and text nodes.
  function merge(liveParent, translatedParent, unit, used) {
    const oldChildren = Array.from(liveParent.childNodes);
    const texts = oldChildren.filter((child) => child.nodeType === 3);
    const children = [];
    for (const translated of translatedParent.childNodes) {
      if (translated.nodeType === 3) {
        const text = texts.shift();
        if (text) {
          if (text.data !== translated.data) text.data = translated.data;
          children.push(text);
        } else {
          children.push(document.createTextNode(translated.data));
        }
      } else if (translated.nodeType === 1) {
        const id = Number(translated.getAttribute('data-p'));
        let element = Number.isInteger(id) ? unit.live[id] : null;
        if (!element) {
          if (translated.textContent) children.push(document.createTextNode(translated.textContent));
          continue;
        }
        if (used.has(element)) {
          // The engine repeated a tag: a copy, with the translated text.
          const copy = element.cloneNode(true);
          if (!unit.kept.has(id)) copy.textContent = translated.textContent;
          children.push(copy);
          continue;
        }
        used.add(element);
        if (!unit.kept.has(id)) merge(element, translated, unit, used);
        children.push(element);
      }
    }
    // Elements the translation lost (an icon, an image): back near where they were.
    oldChildren.forEach((child, index) => {
      if (child.nodeType === 1 && !used.has(child)) {
        used.add(child);
        children.splice(Math.min(index, children.length), 0, child);
      }
    });
    const same = children.length === oldChildren.length && children.every((child, index) => child === oldChildren[index]);
    if (!same) liveParent.replaceChildren(...children);
  }

  const parser = new DOMParser();

  function apply(unit, translation) {
    if (typeof translation !== 'string' || !unchanged(unit)) return false;
    const node = unit.node;
    if (unit.kind === 'attribute') {
      node.setAttribute(unit.attribute, translation);
      const record = records.get(node) || { count: 0 };
      record['@' + unit.attribute] = translation;
      records.set(node, record);
      return true;
    }
    if (unit.kind === 'text') {
      node.data = translation;
    } else if (unit.kind === 'plain') {
      if (node.childNodes.length === 1 && node.firstChild.nodeType === 3) node.firstChild.data = translation;
      else node.textContent = translation;
    } else {
      const parsed = parser.parseFromString('<!DOCTYPE html><body>' + translation, 'text/html');
      merge(node, parsed.body, unit, new Set());
    }
    return true;
  }

  function finish(unit, translation) {
    if (unit.kind !== 'attribute') {
      const record = records.get(unit.node) || { count: 0 };
      record.pending = false;
      records.set(unit.node, record);
    }
    applying = true;
    let changed = false;
    try {
      changed = apply(unit, translation);
    } catch (e) {
      changed = false;
    }
    if (observer) observer.takeRecords(); // our own changes
    applying = false;
    if (unit.kind !== 'attribute') {
      const record = records.get(unit.node);
      if (changed) {
        const now = Date.now();
        record.count = record.at && now - record.at > 30000 ? 1 : record.count + 1;
        record.at = now;
        record.after = unit.node.nodeType === 3 ? unit.node.data : unit.node.innerHTML;
      } else if (unit.node.isConnected && translation !== undefined) {
        // It changed while being translated: try again with what's there now.
        schedule(unit.node);
      }
    }
  }

  function takeBatch() {
    const queue = queues.visible.length ? queues.visible : queues.rest;
    const batch = [];
    let size = 0;
    while (queue.length && batch.length < BATCH_ITEMS) {
      const unit = queue[0];
      if (batch.length && size + unit.source.length > BATCH_CHARS) break;
      queue.shift();
      const cached = cache.get((unit.html ? 'h|' : 't|') + unit.source);
      if (cached !== undefined) {
        finish(unit, cached);
        continue;
      }
      batch.push(unit);
      size += unit.source.length;
    }
    return batch;
  }

  function pump() {
    if (!active) return;
    while (inFlight < IN_FLIGHT && (queues.visible.length || queues.rest.length)) {
      const batch = takeBatch();
      if (!batch.length) continue;
      inFlight++;
      send({ op: 'translate', from, to, items: batch.map((unit) => ({ h: unit.html, t: unit.source })) }).then((answer) => {
        inFlight--;
        const results = answer && Array.isArray(answer.results) ? answer.results : null;
        if (!results) {
          failures++;
          batch.forEach((unit) => finish(unit, undefined));
          if (failures >= 3) stop();
        } else {
          failures = 0;
          batch.forEach((unit, index) => {
            const translation = results[index];
            if (typeof translation === 'string') cache.set((unit.html ? 'h|' : 't|') + unit.source, translation);
            finish(unit, translation === null ? undefined : translation);
          });
        }
        pump();
      });
    }
  }

  // ---- Following the page as it loads and changes --------------------------------------

  const scheduled = new Set();
  let scheduleTimer = 0;

  function blockAround(node) {
    let element = node.nodeType === 1 ? node : node.parentElement;
    while (element && element !== document.body && element.parentElement && !isBlock(element)) element = element.parentElement;
    return element;
  }

  function schedule(node) {
    if (!active) return;
    scheduled.add(node);
    if (!scheduleTimer) scheduleTimer = setTimeout(processScheduled, 120);
  }

  function processScheduled() {
    scheduleTimer = 0;
    styleCache = new WeakMap(); // the page's styles may have changed
    const roots = new Set();
    for (const node of scheduled) {
      if (!node.isConnected) continue;
      if (node.nodeType === 1 && node.closest && node.closest('title')) {
        translateTitle();
        continue;
      }
      if (node.parentNode && node.parentNode.nodeName === 'TITLE') {
        translateTitle();
        continue;
      }
      const root = node.nodeType === 1 && (isBlock(node) || needsSplitting(node)) ? node : blockAround(node);
      if (root && document.body && document.body.contains(root)) roots.add(root);
    }
    scheduled.clear();
    // Only the outermost of nested roots.
    const units = [];
    for (const root of roots) {
      let inner = false;
      for (let parent = root.parentElement; parent; parent = parent.parentElement) {
        if (roots.has(parent)) {
          inner = true;
          break;
        }
      }
      if (inner) continue;
      collect(root, units);
      collectAttributes(root, units);
    }
    if (units.length) enqueue(units);
  }

  function watch() {
    observer = new MutationObserver((mutations) => {
      if (applying || !active) return;
      for (const mutation of mutations) {
        if (mutation.type === 'characterData') {
          schedule(mutation.target);
        } else {
          for (const node of mutation.addedNodes) {
            if (node.nodeType === 1 || node.nodeType === 3) schedule(node);
          }
          if (mutation.target.nodeName === 'TITLE') schedule(mutation.target);
        }
      }
    });
    observer.observe(document.documentElement, { childList: true, subtree: true, characterData: true });
  }

  let titleRecord = '';
  function translateTitle() {
    const title = document.title;
    if (!hasLetters(title) || title === titleRecord) return;
    const unit = { kind: 'title', node: document, html: false, source: title, snapshot: title };
    send({ op: 'translate', from, to, items: [{ h: false, t: title }] }).then((answer) => {
      const translation = answer && Array.isArray(answer.results) ? answer.results[0] : null;
      if (typeof translation === 'string' && document.title === unit.snapshot) {
        titleRecord = translation;
        document.title = translation;
      }
    });
  }

  function start(source, target) {
    if (active) return;
    from = source;
    to = target;
    active = true;
    if (!document.documentElement) return;
    watch();
    const begin = () => {
      if (!document.body) return;
      styleCache = new WeakMap();
      const units = [];
      collect(document.body, units);
      collectAttributes(document.body, units);
      enqueue(units);
      translateTitle();
    };
    if (document.body) begin();
    else document.addEventListener('DOMContentLoaded', begin, { once: true });
  }

  function stop() {
    active = false;
    queues.visible.length = 0;
    queues.rest.length = 0;
    if (observer) observer.disconnect();
    observer = null;
  }

  // ---- Which language is this? -----------------------------------------------------------

  function sample(limit) {
    if (!document.body) return '';
    let text = '';
    const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, {
      acceptNode(node) {
        for (let element = node.parentElement; element && element !== document.body; element = element.parentElement) {
          if (SKIPPED.has(element.localName) || element.getAttribute('translate') === 'no') return NodeFilter.FILTER_REJECT;
        }
        return hasLetters(node.data) ? NodeFilter.FILTER_ACCEPT : NodeFilter.FILTER_REJECT;
      },
    });
    let node;
    while (text.length < limit && (node = walker.nextNode())) text += node.data.replace(/\s+/g, ' ') + ' ';
    return text.slice(0, limit);
  }

  let asked = false;
  let watcher = null;
  let watchTimer = 0;

  function ask(force) {
    if (asked) return;
    const text = sample(4000);
    if (!force && text.length < 1200) return;
    if (text.replace(/\s/g, '').length < 20) return;
    asked = true;
    if (watcher) watcher.disconnect();
    watcher = null;
    const lang = document.documentElement ? document.documentElement.getAttribute('lang') || '' : '';
    send({ op: 'detect', text: ((document.title || '') + '. ' + text).slice(0, 4000), lang }).then((answer) => {
      if (answer && answer.action === 'translate' && answer.from && answer.to) start(answer.from, answer.to);
    });
  }

  // Ask as soon as there's enough text, so translating starts while the
  // page is still loading; otherwise when it has loaded.
  watcher = new MutationObserver(() => {
    if (!watchTimer) watchTimer = setTimeout(() => { watchTimer = 0; ask(false); }, 250);
  });
  watcher.observe(document, { childList: true, subtree: true });
  document.addEventListener('DOMContentLoaded', () => ask(false), { once: true });
  window.addEventListener('load', () => {
    ask(true);
    // Pages that build themselves after loading.
    setTimeout(() => ask(true), 2500);
  }, { once: true });

  // Prism, when you choose "Translate" in the address bar.
  Object.defineProperty(window, '__prismTranslate', {
    value: Object.freeze({
      start(source, target) {
        asked = true;
        if (watcher) watcher.disconnect();
        start(String(source), String(target));
        return 'ok';
      },
    }),
  });
})();
