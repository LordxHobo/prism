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
  actions: $('actions'),
  toast: $('toast'),
  menu: $('menu'),
  progress: $('progress'),
  star: $('star'),
  bookmarksbar: $('bookmarksbar'),
  bookmarkItems: $('bookmarkItems'),
  bookmarksEmpty: $('bookmarksEmpty'),
  bookmarkMore: $('bookmarkMore'),
  infobar: $('infobar'),
  infoIcon: $('infoIcon'),
  infoText: $('infoText'),
  findbar: $('findbar'),
  findfield: $('findfield'),
  findInput: $('findInput'),
  findCount: $('findCount'),
};

// Commands go to Prism through window.prompt(); Prism answers immediately
// (with a result for the commands that have one).
function send(command, argument = '') {
  return window.prompt('prism:' + command + '\n' + argument);
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
  if (/^chrome-extension:\/\//.test(url || '')) return url;
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

// Turns what was typed into where to go: an address, or a search.
function resolveTarget(text) {
  text = text.trim();
  if (!text) return null;

  // Already a full URL (https://..., prism://..., file:///...).
  if (/^[a-z][a-z0-9+.-]*:\/\//i.test(text)) return { url: text, search: false };
  if (/^(about|data|mailto):/i.test(text)) return { url: text, search: false };

  // localhost, IP addresses, and anything that looks like a domain name.
  const hostLike = /^(localhost|\[[0-9a-f:]+\]|(\d{1,3}\.){3}\d{1,3}|([a-z0-9-]+\.)+[a-z][a-z0-9-]{1,62})(:\d{1,5})?([/?#].*)?$/i;
  if (!/\s/.test(text) && hostLike.test(text)) {
    const local = /^(localhost|127\.|10\.|192\.168\.|\[)/i.test(text);
    return { url: (local ? 'http://' : 'https://') + text, search: false };
  }

  const template = (state && state.search) || 'https://duckduckgo.com/?q=%s';
  return { url: template.replace('%s', encodeURIComponent(text)), search: true };
}

function resolveInput(text) {
  const target = resolveTarget(text);
  return target && target.url;
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

// ---- Extension buttons ----------------------------------------------------
// One per extension with a toolbar button: its icon, a badge (text set by
// the extension) and its title as a tooltip. Click: popup or onClicked;
// right-click: Options / Manage extension.

function renderActions() {
  const list = (state && state.actions) || [];
  const container = elements.actions;
  const existing = new Map();
  for (const button of container.children) existing.set(button.dataset.id, button);
  const wanted = new Set(list.map((action) => action.id));
  for (const [id, button] of existing) if (!wanted.has(id)) button.remove();

  list.forEach((action, index) => {
    let button = existing.get(action.id);
    if (!button) {
      button = document.createElement('button');
      button.className = 'icon action';
      button.dataset.id = action.id;
      const img = document.createElement('img');
      img.alt = '';
      img.draggable = false;
      const badge = document.createElement('span');
      badge.className = 'badge';
      button.append(img, badge);
      button.addEventListener('click', () => {
        const rect = button.getBoundingClientRect();
        send('extensionAction', button.dataset.id + '\n' + Math.round(rect.right) + ',' + Math.round(rect.bottom + 4));
      });
      button.addEventListener('contextmenu', (event) => {
        event.preventDefault();
        const rect = button.getBoundingClientRect();
        send('extensionMenu', button.dataset.id + '\n' + Math.round(rect.left) + ',' + Math.round(rect.bottom + 4));
      });
    }
    if (container.children[index] !== button) container.insertBefore(button, container.children[index] || null);
    const img = button.firstChild;
    if (action.icon && img.getAttribute('src') !== action.icon) img.src = action.icon;
    img.hidden = !action.icon;
    button.title = action.title || action.name;
    button.setAttribute('aria-label', action.title || action.name);
    button.disabled = false;
    button.classList.toggle('dimmed', !action.enabled);
    button.classList.toggle('open', !!action.open);
    const badge = button.lastChild;
    const text = String(action.badge || '').slice(0, 4);
    badge.textContent = text;
    badge.hidden = !text;
    badge.style.background = action.badgeColor || '#d93025';
    badge.style.color = action.badgeTextColor || '#ffffff';
  });
}

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

// ---- Bookmark star and bookmarks bar -----------------------------------------------

function renderStar() {
  const tab = activeTab();
  const show = !!(tab && tab.bookmarkable);
  elements.star.hidden = !show;
  elements.star.classList.toggle('on', !!(tab && tab.bookmarked));
  elements.star.title = tab && tab.bookmarked ? 'Edit bookmark (Ctrl+D)' : 'Bookmark this page (Ctrl+D)';
}

function bookmarkMark(item) {
  if (item.folder) {
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('viewBox', '0 0 16 16');
    svg.innerHTML = '<path d="M2 4.5c0-.6.4-1 1-1h3.2l1.5 1.5H13c.6 0 1 .4 1 1V12c0 .6-.4 1-1 1H3c-.6 0-1-.4-1-1z"/>';
    return svg;
  }
  const mark = document.createElement('span');
  mark.className = 'mark';
  let letter = '•';
  try {
    letter = new URL(item.url).hostname.replace(/^www\./, '')[0] || letter;
  } catch (e) { /* not a URL */ }
  mark.textContent = letter;
  return mark;
}

let bookmarksKey = '';

function renderBookmarksBar() {
  const show = !!(state && state.bookmarksBar);
  elements.bookmarksbar.hidden = !show;
  if (!show) return;
  const list = state.bookmarks || [];
  const key = JSON.stringify(list);
  elements.bookmarksEmpty.hidden = list.length > 0;
  if (key === bookmarksKey) return;
  bookmarksKey = key;
  const container = elements.bookmarkItems;
  container.textContent = '';
  for (const item of list) {
    const button = document.createElement('button');
    button.className = 'bookmark';
    button.dataset.id = item.id;
    button.title = item.folder ? item.title : (item.title ? item.title + '\n' : '') + item.url;
    const label = document.createElement('span');
    label.className = 'label';
    label.textContent = item.title || item.url;
    button.append(bookmarkMark(item), label);
    button.addEventListener('click', (event) => {
      if (item.folder) {
        const rect = button.getBoundingClientRect();
        send('bookmarkFolder', item.id + '\n' + Math.round(rect.left) + ',' + Math.round(rect.bottom + 2));
      } else {
        const mode = event.ctrlKey ? 'background' : event.shiftKey ? 'foreground' : 'current';
        send('bookmarkOpen', item.id + '\n' + mode);
      }
    });
    button.addEventListener('auxclick', (event) => {
      if (event.button === 1 && !item.folder) send('bookmarkOpen', item.id + '\nbackground');
    });
    button.addEventListener('contextmenu', (event) => {
      event.preventDefault();
      event.stopPropagation();
      send('bookmarkMenu', item.id + '\n' + Math.round(event.clientX) + ',' + Math.round(event.clientY));
    });
    container.appendChild(button);
  }
  updateBookmarkOverflow();
}

// Bookmarks that don't fit are hidden and listed under ».
function updateBookmarkOverflow() {
  const container = elements.bookmarkItems;
  const limit = container.getBoundingClientRect().right;
  const hidden = [];
  for (const button of container.children) {
    button.classList.remove('overflowing');
    if (button.getBoundingClientRect().right > limit + 0.5) {
      button.classList.add('overflowing');
      hidden.push(button.dataset.id);
    }
  }
  elements.bookmarkMore.hidden = hidden.length === 0;
  elements.bookmarkMore.dataset.ids = hidden.join(',');
}

window.addEventListener('resize', () => {
  if (state && state.bookmarksBar) updateBookmarkOverflow();
});

// ---- Permission bar -----------------------------------------------------------------

const PROMPT_TEXT = {
  location: ' wants to know your location',
  notifications: ' wants to show notifications',
};
const PROMPT_ICONS = {
  location: '<path d="M8 14.5s4.5-4.2 4.5-8a4.5 4.5 0 0 0-9 0c0 3.8 4.5 8 4.5 8z"/><circle cx="8" cy="6.5" r="1.6"/>',
  notifications: '<path d="M4 11V7a4 4 0 0 1 8 0v4l1.2 1.5H2.8z"/><path d="M6.5 14a1.6 1.6 0 0 0 3 0"/>',
};

function renderInfobar() {
  const tab = activeTab();
  const prompt = tab && tab.prompt;
  elements.infobar.hidden = !prompt;
  if (!prompt) return;
  elements.infoIcon.innerHTML = PROMPT_ICONS[prompt.kind] || '';
  elements.infoText.textContent = '';
  const site = document.createElement('b');
  site.textContent = prompt.site;
  elements.infoText.append(site, PROMPT_TEXT[prompt.kind] || ' wants a permission');
}

// ---- Find bar -------------------------------------------------------------------------

function renderFindbar() {
  const tab = activeTab();
  const find = tab && tab.find;
  const open = !!(find && find.open);
  elements.findbar.hidden = !open;
  if (!open) return;
  if (document.activeElement !== elements.findInput)
    elements.findInput.value = find.query || '';
  let text = '';
  if (find.query && find.matches === 0) text = 'No matches';
  else if (find.query && find.matches > 0)
    text = (find.index || 1) + ' of ' + (find.matches > 1000 ? '1000+' : find.matches);
  elements.findCount.textContent = text;
  elements.findfield.classList.toggle('nomatch', !!find.query && find.matches === 0);
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
    renderActions();
    renderStar();
    renderBookmarksBar();
    renderInfobar();
    renderFindbar();
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

  focusFind() {
    elements.findInput.focus();
    elements.findInput.select();
  },

  // Ctrl+D
  clickBookmarkButton() {
    if (!elements.star.hidden) elements.star.click();
  },

  // A suggestion was clicked (Prism opens it).
  suggestionsClosed() {
    suggestions = [];
    selected = -1;
    editing = false;
    elements.address.blur();
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

// ---- Suggestions while typing -------------------------------------------------------
// Prism shows the list (from history and bookmarks) under the address bar;
// the arrow keys move through it from here.

let suggestions = [];
let selected = -1;
let typed = '';

function closeSuggestions() {
  if (suggestions.length) send('suggestClose');
  suggestions = [];
  selected = -1;
}

function requestSuggestions() {
  const text = elements.address.value;
  const target = text.trim() ? resolveTarget(text) : null;
  if (!target) {
    closeSuggestions();
    return;
  }
  const rect = elements.form.getBoundingClientRect();
  const request = {
    text: text.trim(),
    go: target.url,
    search: target.search,
    left: Math.round(rect.left),
    bottom: Math.round(rect.bottom),
    width: Math.round(rect.width),
  };
  try {
    suggestions = JSON.parse(send('suggest', JSON.stringify(request)) || '[]');
  } catch (e) {
    suggestions = [];
  }
  selected = -1;
}

function moveSelection(delta) {
  const count = suggestions.length;
  if (!count) return;
  // -1 is "what I typed".
  selected = selected + delta;
  if (selected < -1) selected = count - 1;
  if (selected >= count) selected = -1;
  const item = suggestions[selected];
  elements.address.value = !item || item.kind === 'search' ? typed : item.url;
  send('suggestSelect', String(selected));
}

elements.address.addEventListener('input', () => {
  editing = true;
  typed = elements.address.value;
  requestSuggestions();
});

elements.address.addEventListener('focus', () => {
  // Select everything on the first click so typing replaces the address.
  setTimeout(() => elements.address.select(), 0);
});

elements.address.addEventListener('blur', () => {
  closeSuggestions();
  editing = false;
  if (state) renderNavigation();
});

elements.address.addEventListener('keydown', (event) => {
  if ((event.key === 'ArrowDown' || event.key === 'ArrowUp') && suggestions.length) {
    event.preventDefault();
    moveSelection(event.key === 'ArrowDown' ? 1 : -1);
    return;
  }
  if (event.key === 'Escape' && suggestions.length) {
    // First Escape closes the list and puts back what was typed.
    elements.address.value = typed;
    closeSuggestions();
    return;
  }
  if (event.key === 'Escape') {
    editing = false;
    renderNavigation();
    elements.address.blur();
    send('focusContent');
  }
});

elements.form.addEventListener('submit', (event) => {
  event.preventDefault();
  const chosen = selected >= 0 ? suggestions[selected] : null;
  closeSuggestions();
  const url = chosen ? chosen.url : resolveInput(elements.address.value);
  if (!url) return;
  editing = false;
  elements.address.blur();
  send('navigate', url);
});

elements.star.addEventListener('click', () => {
  const rect = elements.star.getBoundingClientRect();
  send('bookmarkButton', Math.round(rect.right) + ',' + Math.round(rect.bottom + 4));
});

elements.bookmarkMore.addEventListener('click', () => {
  const rect = elements.bookmarkMore.getBoundingClientRect();
  send('bookmarkOverflow', Math.round(rect.right) + ',' + Math.round(rect.bottom + 2) + '\n' + (elements.bookmarkMore.dataset.ids || ''));
});

elements.bookmarksbar.addEventListener('contextmenu', (event) => {
  event.preventDefault();
  send('bookmarksBarMenu', Math.round(event.clientX) + ',' + Math.round(event.clientY));
});

$('permAllow').addEventListener('click', () => send('permission', 'allow'));
$('permBlock').addEventListener('click', () => send('permission', 'block'));
$('permDismiss').addEventListener('click', () => send('permission', 'dismiss'));

elements.findInput.addEventListener('input', () => send('find', elements.findInput.value));
elements.findInput.addEventListener('keydown', (event) => {
  if (event.key === 'Enter') {
    event.preventDefault();
    send(event.shiftKey ? 'findPrevious' : 'findNext');
  } else if (event.key === 'Escape') {
    event.preventDefault();
    send('findClose');
  }
});
$('findPrev').addEventListener('click', () => send('findPrevious'));
$('findNext').addEventListener('click', () => send('findNext'));
$('findClose').addEventListener('click', () => send('findClose'));

send('ready');
