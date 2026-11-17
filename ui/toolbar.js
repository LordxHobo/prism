// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Prism's toolbar. Prism (the Windows app) sends the current state with
// prism.update(...); the toolbar sends commands back with send(...).

'use strict';

const $ = (id) => document.getElementById(id);
const elements = {
  tabs: $('tabs'),
  newtab: $('newtab'),
  back: $('back'),
  forward: $('forward'),
  reload: $('reload'),
  form: $('addressform'),
  address: $('address'),
  shield: $('shield'),
  addExtension: $('addExtension'),
  extensions: $('extensions'),
  toast: $('toast'),
  menu: $('menu'),
  progress: $('progress'),
};

// Commands go to Prism through window.prompt(); Prism answers immediately.
function send(command, argument = '') {
  window.prompt('prism:' + command + '\n' + argument);
}

const params = new URLSearchParams(location.search);
document.documentElement.dataset.theme = params.get('theme') || 'dark';

let state = null;
let editing = false; // true while the user is typing in the address bar
let toastTimer = 0;
let toastAction = null;

// ---- URLs ------------------------------------------------------------------

// Internal pages are files in Prism's ui folder. Match them by their name so
// different spellings of the same file:// path all work.
function internalPageName(url) {
  const match = /^file:.*\/ui\/([a-z]+)\.html(?:[?#]|$)/i.exec(url || '');
  return match ? match[1].toLowerCase() : null;
}

function queryParam(url, name) {
  try {
    return new URL(url).searchParams.get(name) || '';
  } catch (e) {
    return '';
  }
}

// What the address bar shows for a URL.
function displayURL(url) {
  const page = internalPageName(url);
  if (page === 'newtab') return '';
  if (page === 'error') return queryParam(url, 'url');
  if (page) return 'prism://' + page;
  return url || '';
}

function tabTitle(tab) {
  const page = internalPageName(tab.url);
  if (page === 'newtab') return 'New Tab';
  if (tab.title) return tab.title;
  const shown = displayURL(tab.url);
  return shown || 'New Tab';
}

function tabBadge(tab) {
  const page = internalPageName(tab.url);
  if (page && page !== 'error') return '△'; // a small triangle for Prism's own pages
  try {
    const host = new URL(displayURL(tab.url)).hostname.replace(/^www\./, '');
    if (host) return host[0];
  } catch (e) { /* not a URL */ }
  return (tabTitle(tab)[0] || '•');
}

// Turns what was typed into a URL: an address, or a search.
function resolveInput(text) {
  text = text.trim();
  if (!text) return null;

  // Already a full URL (https://..., prism://..., file:///...).
  if (/^[a-z][a-z0-9+.-]*:\/\//i.test(text)) return text;
  if (/^(about|data|mailto):/i.test(text)) return text;

  // localhost, IP addresses, and anything that looks like a domain name.
  const hostLike = /^(localhost|\[[0-9a-f:]+\]|(\d{1,3}\.){3}\d{1,3}|([a-z0-9-]+\.)+[a-z][a-z0-9-]{1,62})(:\d{1,5})?([/?#].*)?$/i;
  if (!/\s/.test(text) && hostLike.test(text)) {
    const local = /^(localhost|127\.|10\.|192\.168\.|\[)/i.test(text);
    return (local ? 'http://' : 'https://') + text;
  }

  const template = (state && state.search) || 'https://duckduckgo.com/?q=%s';
  return template.replace('%s', encodeURIComponent(text));
}

// ---- Rendering ---------------------------------------------------------------

function activeTab() {
  if (!state) return null;
  return state.tabs.find((tab) => tab.id === state.active) || null;
}

function renderTabs() {
  const container = elements.tabs;
  container.textContent = '';
  for (const tab of state.tabs) {
    const element = document.createElement('div');
    element.className = 'tab' + (tab.id === state.active ? ' active' : '') + (tab.loading ? ' loading' : '');
    element.setAttribute('role', 'tab');
    element.setAttribute('aria-selected', tab.id === state.active ? 'true' : 'false');
    element.title = tabTitle(tab);
    element.dataset.id = tab.id;

    const badge = document.createElement('span');
    badge.className = 'badge';
    badge.textContent = tabBadge(tab);

    const title = document.createElement('span');
    title.className = 'title';
    title.textContent = tabTitle(tab);

    const close = document.createElement('button');
    close.className = 'icon close';
    close.title = 'Close tab (Ctrl+W)';
    close.setAttribute('aria-label', 'Close tab');
    close.innerHTML = '<svg viewBox="0 0 16 16"><path d="M3 3l10 10M13 3L3 13"/></svg>';
    close.addEventListener('click', (event) => {
      event.stopPropagation();
      send('close', String(tab.id));
    });

    element.append(badge, title, close);
    element.addEventListener('mousedown', (event) => {
      // Don't re-render under the close button before its click arrives.
      if (event.target.closest('.close')) return;
      if (event.button === 0) send('select', String(tab.id));
    });
    element.addEventListener('auxclick', (event) => {
      // Middle click closes a tab.
      if (event.button === 1) send('close', String(tab.id));
    });
    container.appendChild(element);
  }

  updateTabWidths();
}

// Very narrow tabs only show their badge. Worked out from the window width
// (not measured), so it's right even before the first layout.
function updateTabWidths() {
  const count = elements.tabs.children.length;
  if (!count) return;
  const available = window.innerWidth - 12 - 34 - 2 * count;
  const narrow = available / count < 90;
  for (const element of elements.tabs.children)
    element.classList.toggle('narrow', narrow);
}

window.addEventListener('resize', updateTabWidths);

let shieldsOn = false;
let lastBlocked = 0;

function renderNavigation() {
  const tab = activeTab();
  elements.back.disabled = !tab || !tab.canBack;
  elements.forward.disabled = !tab || !tab.canForward;
  elements.reload.classList.toggle('loading', !!(tab && tab.loading));
  elements.reload.title = tab && tab.loading ? 'Stop' : 'Reload (F5)';

  if (!editing)
    elements.address.value = tab ? displayURL(tab.url) : '';

  // The shield is grey where Prism Shields is off (or on Prism's own pages).
  shieldsOn = !!(tab && tab.shields);
  elements.shield.classList.toggle('off', !shieldsOn);
  prism.stats(lastBlocked);

  // "Add to Prism" on Chrome Web Store / Firefox Add-ons extension pages.
  elements.addExtension.hidden = !(tab && tab.store);

  const loading = !!(tab && tab.loading);
  elements.progress.classList.toggle('visible', loading);
  elements.progress.style.width = loading ? Math.max(5, Math.round(tab.progress * 100)) + '%' : '0';
}

// ---- Called by Prism -----------------------------------------------------------

window.prism = {
  update(newState) {
    const previousActive = state && state.active;
    state = newState;
    const root = document.documentElement;
    root.dataset.theme = state.theme;
    root.classList.toggle('compact', !!state.compact);
    if (/^#[0-9a-f]{6}$/i.test(state.accent || '')) {
      root.style.setProperty('--accent', state.accent);
      root.style.setProperty('--field-focus', state.accent);
    }
    // Switching tabs throws away anything half-typed in the address bar.
    if (previousActive !== state.active)
      editing = false;
    renderTabs();
    renderNavigation();
    prism.stats(state.blocked || 0);
  },

  // How many ads and trackers were blocked (Prism Shields and extensions).
  stats(blocked) {
    lastBlocked = blocked;
    const count = blocked ? ' ' + blocked.toLocaleString() + ' ads and trackers blocked so far.' : '';
    elements.shield.title = (shieldsOn ? 'Prism Shields is on for this site.' : 'Prism Shields is off for this site.')
      + count + ' Click for Shields.';
  },

  toast(text) {
    showToast(text);
  },

  focusAddress() {
    elements.address.focus();
    elements.address.select();
  },
};

function showToast(text) {
  const toast = elements.toast;
  toast.textContent = text;
  toast.classList.add('visible');
  // Download messages open the Downloads folder when clicked.
  toastAction = /^Downloaded /.test(text) ? () => send('openDownloads') : null;
  toast.classList.toggle('clickable', !!toastAction);
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('visible'), toastAction ? 6000 : 3000);
}

// ---- User input ------------------------------------------------------------------

elements.toast.addEventListener('click', () => {
  if (toastAction) toastAction();
  elements.toast.classList.remove('visible');
});

elements.newtab.addEventListener('click', () => send('newtab'));
elements.back.addEventListener('click', () => send('back'));
elements.forward.addEventListener('click', () => send('forward'));
elements.reload.addEventListener('click', () => {
  const tab = activeTab();
  send(tab && tab.loading ? 'stop' : 'reload');
});

elements.menu.addEventListener('click', () => {
  const rect = elements.menu.getBoundingClientRect();
  send('menu', Math.round(rect.right) + ',' + Math.round(rect.bottom));
});

elements.shield.addEventListener('click', () => {
  const rect = elements.shield.getBoundingClientRect();
  // The panel's top-right corner goes under the shield's right edge...
  // except the shield is on the left, so anchor the panel's left edge there.
  send('shields', Math.round(rect.left + 340) + ',' + Math.round(rect.bottom + 4));
});
elements.addExtension.addEventListener('click', () => send('addExtension'));
elements.extensions.addEventListener('click', () => send('extensions'));

// Double-clicking empty tab strip space opens a new tab.
$('tabstrip').addEventListener('dblclick', (event) => {
  if (event.target.id === 'tabstrip' || event.target.id === 'tabs') send('newtab');
});

elements.address.addEventListener('input', () => {
  editing = true;
});

elements.address.addEventListener('focus', () => {
  // Select everything on the first click so typing replaces the address.
  setTimeout(() => elements.address.select(), 0);
});

elements.address.addEventListener('blur', () => {
  editing = false;
  if (state) renderNavigation();
});

elements.address.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') {
    editing = false;
    renderNavigation();
    elements.address.blur();
    send('focusContent');
  }
});

elements.form.addEventListener('submit', (event) => {
  event.preventDefault();
  const url = resolveInput(elements.address.value);
  if (!url) return;
  editing = false;
  elements.address.blur();
  send('navigate', url);
});

send('ready');
