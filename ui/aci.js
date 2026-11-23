// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Prism ACI (Adaptive Cognitive Interface): reading views for any page.
//
// Prism runs this in a JavaScript world of its own (PrismBundle), so the
// page can't see or change it, with a config from Aci.cpp. It never edits
// the page: for articles it shows an adapted copy on top (in a closed
// shadow root) and leaves the original underneath, untouched and working;
// on other pages (apps, video sites) it only adds a removable style sheet.
// Closing the view removes everything it added.
//
// Views: "focus" (ADHD: one calm column, no distractions, reading
// progress, optional bold word starts), "dyslexia" (a dyslexia-friendly font,
// spacing and tint) and "dense" (more on screen).
(function (config) {
  'use strict';

  const win = window;
  const doc = document;
  const HOST_ID = 'prism-aci-' + config.key;

  // Talking to Prism: __prismNative in Prism's own world, or (without
  // PrismBundle) the page-world handler with a per-session token.
  function send(message) {
    return new Promise((resolve) => {
      const native = win.__prismNative;
      if (native) {
        let shared = win.__prismShared;
        if (!shared) {
          shared = { next: 1, pending: new Map() };
          Object.defineProperty(win, '__prismShared', { value: shared });
          Object.defineProperty(win, '__prismReceive', {
            value: (id, answer) => {
              const done = shared.pending.get(id);
              if (!done) return;
              shared.pending.delete(id);
              done(answer);
            },
          });
        }
        const id = shared.next++;
        shared.pending.set(id, (answer) => {
          try {
            const parsed = JSON.parse(answer);
            resolve(parsed && parsed.ok ? parsed.result : null);
          } catch (e) {
            resolve(null);
          }
        });
        native(JSON.stringify(Object.assign({ aci: true }, message)), id);
        return;
      }
      const handler = win.webkit && win.webkit.messageHandlers && win.webkit.messageHandlers.prismAci;
      if (!handler) return resolve(null);
      handler.postMessage(JSON.stringify(Object.assign({ token: config.token }, message)))
        .then((answer) => { try { resolve(JSON.parse(answer)); } catch (e) { resolve(null); } }, () => resolve(null));
    });
  }

  // ---- One instance per page ----------------------------------------------
  const state = win.__prismAciState || (win.__prismAciState = { open: false, close: null });
  if (config.command === 'toggle' && state.open) {
    state.close();
    return;
  }
  if (config.command === 'auto' && state.open) return;
  if (state.open) state.close();

  let settings = Object.assign({}, config.settings);
  let view = config.view || 'focus';

  // ---- Fonts ------------------------------------------------------------
  const FONTS = {
    comic: { label: 'Comic Sans', family: '"Comic Sans MS", "Comic Sans", "Comic Neue", cursive' },
    opendyslexic: { label: 'OpenDyslexic', family: '"Prism OpenDyslexic", "OpenDyslexic", sans-serif' },
    lexend: { label: 'Lexend', family: '"Prism Lexend", "Lexend", sans-serif' },
    verdana: { label: 'Verdana', family: 'Verdana, Geneva, sans-serif' },
    arial: { label: 'Arial', family: 'Arial, Helvetica, sans-serif' },
    century: { label: 'Century Gothic', family: '"Century Gothic", "URW Gothic", sans-serif' },
    trebuchet: { label: 'Trebuchet', family: '"Trebuchet MS", sans-serif' },
  };
  function fontFaces() {
    const faces = [];
    const add = (family, data, weight) => {
      if (data) faces.push('@font-face{font-family:"' + family + '";src:url("' + data + '") format("woff");font-weight:' + weight + ';font-style:normal;font-display:swap}');
    };
    const f = config.fonts || {};
    add('Prism OpenDyslexic', f.opendyslexic400, 400);
    add('Prism OpenDyslexic', f.opendyslexic700, 700);
    add('Prism Lexend', f.lexend400, 400);
    add('Prism Lexend', f.lexend700, 700);
    if (f.file) faces.push('@font-face{font-family:"Prism Custom";src:url("' + f.file + '");font-display:swap}');
    return faces.join('\n');
  }
  function fontFamily() {
    const font = settings.font || 'comic';
    if (font === 'file') return '"Prism Custom", sans-serif';
    if (font.startsWith('system:')) return '"' + font.slice(7).replace(/["\\]/g, '') + '", sans-serif';
    return (FONTS[font] || FONTS.comic).family;
  }

  // ---- Finding the article ----------------------------------------------
  const JUNK = /(^|[\s_-])(ad|ads|advert|advertisement|promo|sponsor|sponsored|share|sharing|social|comment|comments|related|recommend|newsletter|subscribe|signup|footer|nav|navbar|menu|sidebar|cookie|consent|banner|popup|modal|breadcrumb|pagination|toolbar|outbrain|taboola)([\s_-]|$)/i;
  const KEEP = new Set(['H1', 'H2', 'H3', 'H4', 'H5', 'H6', 'P', 'UL', 'OL', 'LI', 'BLOCKQUOTE', 'PRE', 'CODE', 'EM', 'STRONG', 'B', 'I', 'U', 'A', 'IMG',
    'FIGURE', 'FIGCAPTION', 'TABLE', 'THEAD', 'TBODY', 'TFOOT', 'TR', 'TD', 'TH', 'CAPTION', 'BR', 'HR', 'DL', 'DT', 'DD', 'SUP', 'SUB', 'MARK', 'SMALL', 'Q', 'TIME', 'ABBR']);
  const UNWRAP = new Set(['DIV', 'SECTION', 'SPAN', 'ARTICLE', 'MAIN', 'HEADER', 'FONT', 'CENTER', 'PICTURE', 'LABEL']);
  const DROP = new Set(['SCRIPT', 'STYLE', 'NOSCRIPT', 'IFRAME', 'FORM', 'BUTTON', 'INPUT', 'SELECT', 'TEXTAREA', 'NAV', 'ASIDE', 'FOOTER', 'SVG', 'CANVAS', 'VIDEO', 'AUDIO', 'OBJECT', 'EMBED', 'DIALOG', 'TEMPLATE', 'LINK', 'META']);

  const textLength = (el) => (el.textContent || '').replace(/\s+/g, ' ').trim().length;
  const junky = (el) => JUNK.test((el.id || '') + ' ' + (typeof el.className === 'string' ? el.className : '')) || el.getAttribute('aria-hidden') === 'true' || el.hidden;

  function findArticle() {
    // Score blocks by the paragraph text inside them (like Firefox's reader view, simplified).
    const scores = new Map();
    for (const p of doc.querySelectorAll('p, pre, blockquote, li')) {
      if (p.closest('nav, aside, footer, header nav, [role="navigation"], [role="complementary"]')) continue;
      const length = textLength(p);
      if (length < 40) continue;
      const points = 1 + Math.min(3, Math.floor(length / 100)) + (p.textContent.split(',').length - 1) * 0.5;
      let node = p.parentElement;
      for (let level = 0; node && node !== doc.body && level < 3; level++, node = node.parentElement) {
        if (junky(node)) break;
        scores.set(node, (scores.get(node) || 0) + points / (level + 1));
      }
    }
    let best = null;
    let bestScore = 0;
    for (const [node, score] of scores) {
      const adjusted = score * (1 - Math.min(0.9, linkDensity(node)));
      if (adjusted > bestScore) {
        best = node;
        bestScore = adjusted;
      }
    }
    // Prefer an <article>/<main> that holds the best block.
    const semantic = best && best.closest('article, main, [role="main"], [itemprop="articleBody"]');
    if (semantic && textLength(semantic) < textLength(best) * 3) best = semantic;
    if (!best || textLength(best) < 600) return null;
    return best;
  }
  function linkDensity(el) {
    const total = textLength(el) || 1;
    let links = 0;
    for (const a of el.querySelectorAll('a')) links += textLength(a);
    return links / total;
  }

  function absolute(url) {
    try {
      return new URL(url, doc.baseURI).href;
    } catch (e) {
      return '';
    }
  }
  function clean(source, into) {
    for (const node of source.childNodes) {
      if (node.nodeType === Node.TEXT_NODE) {
        into.appendChild(doc.createTextNode(node.textContent));
        continue;
      }
      if (node.nodeType !== Node.ELEMENT_NODE) continue;
      const tag = node.tagName;
      if (DROP.has(tag) || junky(node)) continue;
      const style = win.getComputedStyle(node);
      if (style.display === 'none' || style.visibility === 'hidden') continue;
      if (UNWRAP.has(tag)) {
        // Blocks stay blocks (a <div> of text becomes a paragraph-ish block).
        if (style.display === 'block' || style.display === 'flex' || style.display === 'grid') {
          const block = doc.createElement('div');
          clean(node, block);
          if (block.childNodes.length) into.appendChild(block);
        } else
          clean(node, into);
        continue;
      }
      if (!KEEP.has(tag)) {
        clean(node, into);
        continue;
      }
      if (tag === 'IMG') {
        const src = node.currentSrc || node.src || node.getAttribute('data-src') || node.getAttribute('data-lazy-src') || '';
        const width = node.naturalWidth || node.width || 0;
        if (!src || (width && width < 60)) continue;
        const img = doc.createElement('img');
        img.src = absolute(src);
        img.alt = node.alt || '';
        into.appendChild(img);
        continue;
      }
      const copy = doc.createElement(tag);
      if (tag === 'A') {
        const href = node.getAttribute('href');
        if (href && !/^\s*javascript:/i.test(href)) {
          copy.href = absolute(href);
          copy.target = '_top';
          copy.rel = 'noopener noreferrer';
        }
      }
      for (const name of ['colspan', 'rowspan', 'datetime', 'title']) {
        if (node.hasAttribute(name)) copy.setAttribute(name, node.getAttribute(name));
      }
      clean(node, copy);
      if (copy.childNodes.length || tag === 'BR' || tag === 'HR') into.appendChild(copy);
    }
  }

  // Bold word starts ("bionic" reading) for Focus, if turned on.
  function boldStarts(root) {
    const walker = doc.createTreeWalker(root, NodeFilter.SHOW_TEXT, {
      acceptNode: (node) => node.parentElement && /^(PRE|CODE|B|STRONG|H1|H2|H3|H4|H5|H6)$/.test(node.parentElement.tagName) ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT,
    });
    const nodes = [];
    while (walker.nextNode()) nodes.push(walker.currentNode);
    for (const node of nodes) {
      const parts = node.textContent.split(/(\s+)/);
      if (parts.length < 2 && parts[0].length < 2) continue;
      const fragment = doc.createDocumentFragment();
      for (const part of parts) {
        if (!part || /^\s+$/.test(part) || part.length < 2) {
          fragment.appendChild(doc.createTextNode(part));
          continue;
        }
        const cut = Math.ceil(part.length * 0.45);
        const strong = doc.createElement('b');
        strong.className = 'start';
        strong.textContent = part.slice(0, cut);
        fragment.append(strong, doc.createTextNode(part.slice(cut)));
      }
      node.replaceWith(fragment);
    }
  }

  // ---- Colours ----------------------------------------------------------
  const TINTS = config.dark
    ? { none: ['#17181c', '#e6e6ea'], warm: ['#1f1b16', '#ece3d3'], blue: ['#151b24', '#dfe7f2'], green: ['#161d18', '#dde9df'], grey: ['#1c1c1c', '#e0e0e0'] }
    : { none: ['#ffffff', '#1b1b1f'], warm: ['#fbf5e6', '#2a241b'], blue: ['#eef4fb', '#17202b'], green: ['#eff6ec', '#1a251b'], grey: ['#f1f1f1', '#1d1d1d'] };
  const TINT_NAMES = { none: 'None', warm: 'Cream', blue: 'Blue', green: 'Green', grey: 'Grey' };
  function tint() {
    let name = settings.tint || 'auto';
    if (name === 'auto') name = view === 'dyslexia' ? 'warm' : 'none';
    return TINTS[name] || TINTS.none;
  }

  // ---- The view ---------------------------------------------------------
  const article = findArticle();
  const host = doc.createElement('div');
  host.id = HOST_ID;
  host.style.cssText = 'all:initial;position:fixed;inset:0;z-index:2147483647;display:block;';
  const shadow = host.attachShadow({ mode: 'closed' });
  let pageStyle = null; // page mode: the style sheet added to the page
  const pausedMedia = [];

  function readerCSS() {
    const [background, text] = tint();
    const size = settings.textSize || 19;
    const spacing = settings.spacing === undefined ? (view === 'dyslexia' ? 1 : 0) : settings.spacing; // 0..3
    const dense = view === 'dense';
    const width = settings.width || (dense ? 95 : view === 'dyslexia' ? 62 : 68);
    const accent = config.accent || '#3aa0ff';
    return fontFaces() + `
:host { all: initial; }
* { box-sizing: border-box; }
.root { position: fixed; inset: 0; overflow: auto; background: ${background}; color: ${text}; font-family: ${view === 'dyslexia' ? fontFamily() : 'system-ui, "Segoe UI", sans-serif'}; }
.bar { position: sticky; top: 0; z-index: 2; display: flex; align-items: center; gap: 6px; padding: 8px 14px; background: ${background}; border-bottom: 1px solid color-mix(in srgb, ${text} 14%, transparent); font: 13px system-ui, "Segoe UI", sans-serif; }
.bar .brand { font-weight: 700; margin-right: 8px; opacity: .8; }
.bar button, .panel button, .panel select, .panel input[type=text] { font: inherit; color: ${text}; background: color-mix(in srgb, ${text} 8%, transparent); border: 1px solid color-mix(in srgb, ${text} 16%, transparent); border-radius: 8px; padding: 6px 12px; cursor: pointer; }
.bar button[aria-pressed="true"] { background: ${accent}; border-color: ${accent}; color: #fff; }
.bar .spacer { flex: 1; }
.progress { position: sticky; top: 45px; z-index: 2; height: 3px; background: transparent; }
.progress span { display: block; height: 100%; width: 0; background: ${accent}; }
main { max-width: ${width}ch; margin: 0 auto; padding: ${dense ? '18px 20px 60px' : '40px 24px 120px'}; font-size: ${dense ? Math.max(13, size - 4) : size}px;
  line-height: ${dense ? 1.42 : (1.6 + spacing * 0.15).toFixed(2)}; letter-spacing: ${view === 'dyslexia' ? (0.03 + spacing * 0.02).toFixed(3) : (spacing * 0.01).toFixed(3)}em;
  word-spacing: ${view === 'dyslexia' ? (0.12 + spacing * 0.06).toFixed(2) : (spacing * 0.03).toFixed(2)}em; text-align: left; }
main h1 { font-size: 1.7em; line-height: 1.25; margin: 0 0 .3em; letter-spacing: normal; }
.meta { opacity: .7; font-size: .8em; margin: 0 0 2em; }
main h2, main h3, main h4 { line-height: 1.3; margin: 1.6em 0 .5em; ${view === 'focus' ? `color: ${accent};` : ''} }
main p, main div, main li, main blockquote { margin: 0 0 ${dense ? '.55em' : view === 'dyslexia' ? (1.1 + spacing * 0.2).toFixed(2) + 'em' : '1em'}; }
main div:empty { display: none; }
main a { color: ${accent}; }
main img { max-width: 100%; height: auto; ${dense ? 'max-height: 220px; object-fit: contain;' : ''} border-radius: 6px; display: block; margin: 1em auto; }
main blockquote { border-left: 3px solid ${accent}; padding-left: 1em; opacity: .9; }
main pre { white-space: pre-wrap; font-size: .85em; padding: .8em; border-radius: 8px; background: color-mix(in srgb, ${text} 7%, transparent); }
main table { border-collapse: collapse; font-size: .9em; }
main td, main th { border: 1px solid color-mix(in srgb, ${text} 20%, transparent); padding: .3em .5em; }
${view === 'dyslexia' ? 'main em, main i { font-style: normal; font-weight: 700; } main * { text-align: left !important; }' : ''}
b.start { font-weight: 700; }
.panel { position: fixed; top: 52px; right: 14px; z-index: 3; width: 300px; max-height: calc(100vh - 70px); overflow: auto; padding: 14px; border-radius: 12px; background: ${background};
  border: 1px solid color-mix(in srgb, ${text} 18%, transparent); box-shadow: 0 14px 40px rgba(0,0,0,.3); font: 13px system-ui, "Segoe UI", sans-serif; }
.panel[hidden] { display: none; }
.panel label { display: block; margin: 10px 0 4px; font-weight: 600; }
.panel select, .panel input[type=text] { width: 100%; cursor: default; }
.panel input[type=range] { width: 100%; accent-color: ${accent}; }
.panel .row { display: flex; gap: 6px; flex-wrap: wrap; }
.panel .check { display: flex; gap: 8px; align-items: center; font-weight: 400; }
.panel .hint { opacity: .7; font-weight: 400; margin: 4px 0 0; }
.notice { max-width: 640px; margin: 18vh auto 0; padding: 24px; text-align: center; font: 15px system-ui, "Segoe UI", sans-serif; }
`;
  }

  // Page mode (no article): a removable style sheet on the page itself.
  function pageCSS() {
    if (view === 'dyslexia') {
      const spacing = settings.spacing === undefined ? 1 : settings.spacing;
      // Icon fonts use ligatures, so leave anything that looks like an icon alone.
      return fontFaces() + `
:where(body) :not(svg, svg *, [class*="icon" i], [class*="material" i], [class*="fa-"], [aria-hidden="true"], code, pre, kbd, samp) {
  font-family: ${fontFamily()} !important; letter-spacing: ${(0.03 + spacing * 0.02).toFixed(3)}em !important; word-spacing: ${(0.12 + spacing * 0.06).toFixed(2)}em !important; }
p, li, dd, blockquote { line-height: ${(1.6 + spacing * 0.15).toFixed(2)} !important; text-align: left !important; }
em, i { font-style: normal !important; }`;
    }
    // Focus: nothing moving.
    return `*, *::before, *::after { animation-duration: 0s !important; animation-iteration-count: 1 !important; transition: none !important; scroll-behavior: auto !important; }
marquee, blink { display: none !important; }`;
  }

  let panelOpen = false;
  function render() {
    host.style.cssText = 'all:initial;position:fixed;inset:0;z-index:2147483647;display:block;';
    shadow.textContent = '';
    const style = doc.createElement('style');
    style.textContent = readerCSS();
    const root = doc.createElement('div');
    root.className = 'root';

    // Bar: views, settings, back to the page.
    const bar = doc.createElement('div');
    bar.className = 'bar';
    const brand = doc.createElement('span');
    brand.className = 'brand';
    brand.textContent = 'Reading view';
    bar.appendChild(brand);
    for (const [name, label] of [['focus', 'Focus'], ['dyslexia', 'Dyslexia-friendly'], ['dense', 'Dense']]) {
      const button = doc.createElement('button');
      button.textContent = label;
      button.setAttribute('aria-pressed', String(view === name));
      button.addEventListener('click', () => {
        view = name;
        send({ op: 'view', view });
        apply();
      });
      bar.appendChild(button);
    }
    const spacer = doc.createElement('span');
    spacer.className = 'spacer';
    const settingsButton = doc.createElement('button');
    settingsButton.textContent = 'Aa';
    settingsButton.title = 'Text and colours';
    settingsButton.addEventListener('click', () => { panelOpen = !panelOpen; panel.hidden = !panelOpen; });
    const original = doc.createElement('button');
    original.textContent = 'Original page';
    original.title = 'Back to the page (F9 or Esc)';
    original.addEventListener('click', close);
    bar.append(spacer, settingsButton, original);
    root.appendChild(bar);

    const panel = buildPanel();
    panel.hidden = !panelOpen;

    if (article) {
      const progress = doc.createElement('div');
      progress.className = 'progress';
      const bar2 = doc.createElement('span');
      progress.appendChild(bar2);
      if (view === 'focus') root.appendChild(progress);
      const main = doc.createElement('main');
      const title = doc.createElement('h1');
      const heading = article.querySelector('h1') || doc.querySelector('h1');
      title.textContent = (heading && textLength(heading) > 5 ? heading.textContent : doc.title).trim();
      const meta = doc.createElement('p');
      meta.className = 'meta';
      const body = doc.createElement('div');
      clean(article, body);
      // The article's own first heading is already the title.
      const first = body.querySelector('h1');
      if (first && first.textContent.trim() === title.textContent) first.remove();
      const words = (body.textContent.match(/\S+/g) || []).length;
      meta.textContent = location.hostname.replace(/^www\./, '') + ' · ' + Math.max(1, Math.round(words / 220)) + ' min read';
      if (view === 'focus' && settings.bionic) boldStarts(body);
      main.append(title, meta, body);
      root.appendChild(main);
      root.addEventListener('scroll', () => {
        const max = root.scrollHeight - root.clientHeight;
        bar2.style.width = (max > 0 ? Math.min(100, (root.scrollTop / max) * 100) : 100) + '%';
      }, { passive: true });
    } else {
      const notice = doc.createElement('div');
      notice.className = 'notice';
      notice.textContent = view === 'dense'
        ? 'This page isn\u2019t an article, so there\u2019s no dense view for it. Focus and Dyslexia-friendly still adjust the page itself.'
        : 'This page isn\u2019t an article, so Prism adjusted the page itself (' + (view === 'focus' ? 'no motion, videos paused' : 'font and spacing') + '). Choose \u201cOriginal page\u201d to undo.';
      root.appendChild(notice);
    }
    shadow.append(style, root, panel);
  }

  function buildPanel() {
    const panel = doc.createElement('div');
    panel.className = 'panel';
    const label = (text) => { const l = doc.createElement('label'); l.textContent = text; return l; };

    // Font (used by Dyslexia-friendly).
    const fontSelect = doc.createElement('select');
    for (const [key, font] of Object.entries(FONTS)) fontSelect.add(new Option(font.label + (key === 'comic' ? ' (default)' : ''), key));
    fontSelect.add(new Option('A font on this PC…', 'system'));
    fontSelect.add(new Option(config.fonts && config.fonts.file ? 'My font file (' + (settings.fileName || 'custom') + ')' : 'A font file…', 'file'));
    const current = settings.font || 'comic';
    fontSelect.value = current.startsWith('system:') ? 'system' : current;
    const systemBox = doc.createElement('div');
    const systemInput = doc.createElement('input');
    systemInput.type = 'text';
    systemInput.placeholder = 'Type a font name, like Calibri';
    systemInput.value = current.startsWith('system:') ? current.slice(7) : '';
    const list = doc.createElement('datalist');
    list.id = 'prism-aci-fonts';
    systemInput.setAttribute('list', list.id);
    systemBox.append(systemInput, list);
    systemBox.hidden = fontSelect.value !== 'system';
    let fontsLoaded = false;
    const loadFonts = () => {
      if (fontsLoaded) return;
      fontsLoaded = true;
      send({ op: 'systemFonts' }).then((fonts) => {
        for (const name of fonts || []) list.appendChild(new Option(name));
      });
    };
    fontSelect.addEventListener('change', () => {
      systemBox.hidden = fontSelect.value !== 'system';
      if (fontSelect.value === 'system') {
        loadFonts();
        systemInput.focus();
        return;
      }
      if (fontSelect.value === 'file') {
        send({ op: 'fontFile' }).then((result) => {
          if (result && result.data) {
            config.fonts = Object.assign({}, config.fonts, { file: result.data });
            update({ font: 'file', fileName: result.name });
          } else
            fontSelect.value = settings.font || 'comic';
        });
        return;
      }
      update({ font: fontSelect.value });
    });
    systemInput.addEventListener('focus', loadFonts);
    systemInput.addEventListener('change', () => { if (systemInput.value.trim()) update({ font: 'system:' + systemInput.value.trim() }); });
    const fontHint = doc.createElement('p');
    fontHint.className = 'hint';
    fontHint.textContent = 'The font is used in the Dyslexia-friendly view.';
    panel.append(label('Font'), fontSelect, systemBox, fontHint);

    const slider = (text, key, min, max, step, fallback) => {
      const input = doc.createElement('input');
      input.type = 'range';
      input.min = min;
      input.max = max;
      input.step = step;
      input.value = settings[key] === undefined ? fallback : settings[key];
      input.addEventListener('change', () => update({ [key]: Number(input.value) }));
      panel.append(label(text), input);
    };
    slider('Text size', 'textSize', 14, 30, 1, 19);
    slider('Spacing', 'spacing', 0, 3, 1, view === 'dyslexia' ? 1 : 0);
    slider('Line length', 'width', 40, 110, 5, view === 'dense' ? 95 : view === 'dyslexia' ? 62 : 68);

    panel.appendChild(label('Background'));
    const tints = doc.createElement('div');
    tints.className = 'row';
    for (const name of ['auto', ...Object.keys(TINTS)]) {
      const button = doc.createElement('button');
      button.textContent = name === 'auto' ? 'Automatic' : TINT_NAMES[name];
      if (name !== 'auto') button.style.background = TINTS[name][0];
      if ((settings.tint || 'auto') === name) button.style.outline = '2px solid ' + (config.accent || '#3aa0ff');
      button.addEventListener('click', () => update({ tint: name }));
      tints.appendChild(button);
    }
    panel.appendChild(tints);

    const check = (text, checked, onChange) => {
      const row = doc.createElement('label');
      row.className = 'check';
      const box = doc.createElement('input');
      box.type = 'checkbox';
      box.checked = checked;
      box.addEventListener('change', () => onChange(box.checked));
      row.append(box, doc.createTextNode(text));
      panel.appendChild(row);
    };
    check('Bold the start of words (Focus)', !!settings.bionic, (on) => update({ bionic: on }));
    check('Always open ' + location.hostname.replace(/^www\./, '') + ' in this view', config.siteView === view, (on) => {
      config.siteView = on ? view : '';
      send({ op: 'site', view: on ? view : '' });
    });
    return panel;
  }

  function update(changes) {
    Object.assign(settings, changes);
    send({ op: 'settings', settings: changes }).then((answer) => {
      // A bundled font that wasn't loaded yet: Prism sends its files.
      if (answer && answer.fonts && Object.keys(answer.fonts).length) {
        config.fonts = Object.assign({}, config.fonts, answer.fonts);
        apply();
      }
    });
    apply();
  }

  function pauseMedia() {
    for (const media of doc.querySelectorAll('video, audio')) {
      if (!media.paused) {
        media.pause();
        pausedMedia.push(media);
      }
    }
  }

  function apply() {
    if (pageStyle) {
      pageStyle.remove();
      pageStyle = null;
    }
    if (article || view === 'dense') {
      if (!host.isConnected) (doc.documentElement || doc.body).appendChild(host);
      render();
      // The page underneath shouldn't scroll or play behind the view.
      doc.documentElement.style.setProperty('overflow', 'hidden', 'important');
      pauseMedia();
    } else {
      // Page mode: adjust the page itself, plus a small bar to undo it.
      pageStyle = doc.createElement('style');
      pageStyle.textContent = pageCSS();
      (doc.head || doc.documentElement).appendChild(pageStyle);
      if (view === 'focus') pauseMedia();
      renderPageBar();
    }
  }

  // In page mode, a small floating bar (views and "Original page").
  function renderPageBar() {
    if (!host.isConnected) (doc.documentElement || doc.body).appendChild(host);
    host.style.cssText = 'all:initial;position:fixed;top:10px;right:10px;z-index:2147483647;display:block;';
    shadow.textContent = '';
    const style = doc.createElement('style');
    const [background, text] = tint();
    style.textContent = `.bar { display: flex; gap: 6px; align-items: center; padding: 6px; border-radius: 12px; background: ${background}; color: ${text};
      box-shadow: 0 8px 30px rgba(0,0,0,.3); font: 13px system-ui, "Segoe UI", sans-serif; }
      button { font: inherit; color: ${text}; background: color-mix(in srgb, ${text} 8%, transparent); border: 1px solid color-mix(in srgb, ${text} 16%, transparent); border-radius: 8px; padding: 6px 10px; cursor: pointer; }
      button[aria-pressed="true"] { background: ${config.accent || '#3aa0ff'}; color: #fff; border-color: transparent; }`;
    const bar = doc.createElement('div');
    bar.className = 'bar';
    for (const [name, label] of [['focus', 'Focus'], ['dyslexia', 'Dyslexia-friendly']]) {
      const button = doc.createElement('button');
      button.textContent = label;
      button.setAttribute('aria-pressed', String(view === name));
      button.addEventListener('click', () => { view = name; send({ op: 'view', view }); apply(); });
      bar.appendChild(button);
    }
    const original = doc.createElement('button');
    original.textContent = 'Original page';
    original.addEventListener('click', close);
    bar.appendChild(original);
    shadow.append(style, bar);
  }

  function onKey(event) {
    if (event.key === 'Escape' && !panelOpen && article) close();
    else if (event.key === 'Escape' && panelOpen) {
      panelOpen = false;
      render();
    }
  }

  function close() {
    host.remove();
    if (pageStyle) pageStyle.remove();
    pageStyle = null;
    doc.documentElement.style.removeProperty('overflow');
    for (const media of pausedMedia.splice(0)) {
      // Only what the view paused, and only if it's still there.
      if (media.isConnected && media.paused) media.play().catch(() => {});
    }
    win.removeEventListener('keydown', onKey, true);
    state.open = false;
    state.close = null;
    send({ op: 'closed' });
  }

  state.open = true;
  state.close = close;
  win.addEventListener('keydown', onKey, true);
  apply();
})
