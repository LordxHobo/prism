// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Prism Shields, in the page. Prism adds this (with the site's filters as
// `config`) to every site you visit, right before the page loads:
// 1. hides the site's known ad elements (CSS from the filter lists),
// 2. asks Prism about the page's class names and ids, and hides any that
//    generic filters target ("generic hiding"),
// 3. checks the page's images and frames against the network filters and
//    hides the ads that come from the site's own servers.
// Scriptlets run separately, before this, in the same user script.
(function (config) {
  'use strict';

  const win = window;
  const doc = document;
  const handlers = win.webkit && win.webkit.messageHandlers;
  const handler = handlers && handlers.prismShields;
  const post = handler ? handler.postMessage.bind(handler) : null;
  const stringify = JSON.stringify;
  const parseJSON = JSON.parse;
  const apply = Reflect.apply;
  const createElementFn = Document.prototype.createElement;
  const appendChildFn = Node.prototype.appendChild;
  const querySelectorAllFn = Document.prototype.querySelectorAll;
  const elementQuerySelectorAllFn = Element.prototype.querySelectorAll;
  const setTimeoutFn = win.setTimeout.bind(win);
  const MutationObserverCtor = win.MutationObserver;
  const URLCtor = win.URL;

  // ---- Our style sheet -----------------------------------------------------
  // One rule per selector, and only valid selectors (an unbalanced "[" in one
  // would swallow the rules after it).
  let cssText = '';
  let style = null;
  function ensureStyle() {
    const parent = doc.documentElement;
    if (!parent) return;
    if (!style) {
      style = apply(createElementFn, doc, ['style']);
      style.setAttribute('data-prism-shields', '');
    }
    if (style.textContent !== cssText) style.textContent = cssText;
    if (style.parentNode !== parent) apply(appendChildFn, parent, [style]);
  }

  // A rule that parses on its own parses inside our style sheet too.
  let probe = null;
  try {
    probe = new win.CSSStyleSheet();
  } catch (e) { }
  function validSelector(selector) {
    if (typeof selector !== 'string' || !selector || selector.includes('{') || selector.includes('}')) return false;
    if (!probe) return true;
    try {
      probe.insertRule(selector + '{display:none}', 0);
      probe.deleteRule(0);
      return true;
    } catch (e) {
      return false;
    }
  }

  function hideSelectors(selectors) {
    if (!selectors || !selectors.length) return;
    let added = '';
    for (const selector of selectors) {
      if (validSelector(selector)) added += selector + '{display:none!important}\n';
    }
    if (!added) return;
    cssText += added;
    ensureStyle();
  }

  hideSelectors(config.hide);

  if (!post) return;

  function ask(message) {
    return post(stringify(message)).then((reply) => parseJSON(reply), () => null);
  }

  // ---- Generic hiding ----------------------------------------------------------
  const seenClasses = new Set();
  const seenIds = new Set();
  let newClasses = [];
  let newIds = [];

  function collectFrom(element) {
    const id = element.id;
    if (id && !seenIds.has(id)) {
      seenIds.add(id);
      newIds.push(id);
    }
    const list = element.classList;
    if (list) {
      for (let i = 0; i < list.length; i++) {
        const name = list[i];
        if (!seenClasses.has(name)) {
          seenClasses.add(name);
          newClasses.push(name);
        }
      }
    }
  }

  // ---- Ads from the site's own servers --------------------------------------
  const mediaSelector = 'img[src],iframe[src],video[src],audio[src],source[src],embed[src],object[data]';
  const typeOf = { IMG: 'image', IFRAME: 'sub_frame', VIDEO: 'media', AUDIO: 'media', SOURCE: 'media', EMBED: 'object', OBJECT: 'object' };
  const verdicts = new Map(); // url -> true (blocked) / false / promise pending
  let pendingElements = [];

  function resolveURL(element) {
    const raw = element.tagName === 'OBJECT' ? element.getAttribute('data') : element.getAttribute('src');
    if (!raw || raw.startsWith('data:') || raw.startsWith('blob:') || raw.startsWith('about:')) return null;
    try {
      return new URLCtor(raw, doc.baseURI).href;
    } catch (e) {
      return null;
    }
  }

  function hideElement(element) {
    element.style.setProperty('display', 'none', 'important');
    if (element.tagName === 'IFRAME') element.setAttribute('src', 'about:blank');
    else if (element.tagName === 'IMG') {
      element.removeAttribute('srcset');
      element.setAttribute('src', 'data:,');
    }
  }

  function noteMedia(element) {
    if (!typeOf[element.tagName]) return;
    pendingElements.push(element);
  }

  // ---- Batching -------------------------------------------------------------------
  let scheduled = false;

  function scan(root) {
    if (!config.generichide) {
      if (root.nodeType === 1) collectFrom(root);
      const all = root === doc
        ? apply(querySelectorAllFn, doc, ['[id],[class]'])
        : apply(elementQuerySelectorAllFn, root, ['[id],[class]']);
      for (let i = 0; i < all.length; i++) collectFrom(all[i]);
    }
    if (root.nodeType === 1 && root.matches && root.matches(mediaSelector)) noteMedia(root);
    const media = root === doc
      ? apply(querySelectorAllFn, doc, [mediaSelector])
      : apply(elementQuerySelectorAllFn, root, [mediaSelector]);
    for (let i = 0; i < media.length; i++) noteMedia(media[i]);
    schedule();
  }

  function schedule() {
    if (scheduled) return;
    scheduled = true;
    setTimeoutFn(flush, 40);
  }

  function flush() {
    scheduled = false;

    if (newClasses.length || newIds.length) {
      const classes = newClasses;
      const ids = newIds;
      newClasses = [];
      newIds = [];
      ask({ op: 'generic', classes, ids, exceptions: config.exceptions || [] }).then((selectors) => {
        if (Array.isArray(selectors)) hideSelectors(selectors);
      });
    }

    if (pendingElements.length) {
      const elements = pendingElements;
      pendingElements = [];
      const waiting = new Map(); // url -> elements
      const items = [];
      for (const element of elements) {
        const url = resolveURL(element);
        if (!url) continue;
        const known = verdicts.get(url);
        if (known === true) {
          hideElement(element);
          continue;
        }
        if (known === false) continue;
        if (!waiting.has(url)) {
          waiting.set(url, []);
          items.push([url, typeOf[element.tagName]]);
        }
        waiting.get(url).push(element);
      }
      if (items.length) {
        ask({ op: 'check', page: location.href, items }).then((blocked) => {
          if (!Array.isArray(blocked)) return;
          items.forEach((item, i) => {
            verdicts.set(item[0], !!blocked[i]);
            if (blocked[i]) for (const element of waiting.get(item[0])) hideElement(element);
          });
        });
      }
    }
  }

  // ---- Watching the page -------------------------------------------------------------
  const observer = new MutationObserverCtor((mutations) => {
    for (const mutation of mutations) {
      if (mutation.type === 'attributes') {
        const target = mutation.target;
        if (mutation.attributeName === 'src' || mutation.attributeName === 'data') noteMedia(target);
        else if (!config.generichide) collectFrom(target);
        schedule();
        continue;
      }
      const added = mutation.addedNodes;
      for (let i = 0; i < added.length; i++) {
        if (added[i].nodeType === 1) scan(added[i]);
      }
      // Pages sometimes remove our style sheet; put it back.
      if (style && style.parentNode !== doc.documentElement) ensureStyle();
    }
  });
  observer.observe(doc, { childList: true, subtree: true, attributes: true, attributeFilter: ['class', 'id', 'src', 'data'] });

  if (doc.readyState === 'loading') {
    doc.addEventListener('DOMContentLoaded', () => { ensureStyle(); scan(doc); }, { once: true });
  } else {
    scan(doc);
  }
})
