// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// prism://bookmarks. Talks to Prism with window.prompt("prism:<command>"),
// which Prism answers for this page only. Prism calls
// window.prismBookmarksChanged() when bookmarks change anywhere.

'use strict';

const $ = (id) => document.getElementById(id);
const BAR_ID = 1;
const OTHER_ID = 2;
const FOLDER_ICON = '<svg viewBox="0 0 16 16"><path d="M2 4.5c0-.6.4-1 1-1h3.2l1.5 1.5H13c.6 0 1 .4 1 1V12c0 .6-.4 1-1 1H3c-.6 0-1-.4-1-1z"/></svg>';
const BAR_ICON = '<svg viewBox="0 0 16 16"><path d="M2.5 3.5h11v9h-11z"/><path d="M2.5 6.5h11"/></svg>';

function ask(command, argument = '') {
  const answer = window.prompt('prism:' + command + '\n' + argument);
  if (answer === null || answer === undefined) return null;
  try {
    return JSON.parse(answer);
  } catch (e) {
    return null;
  }
}

let tree = null;
let nodes = new Map(); // id -> node (with .parentId)
let currentFolder = BAR_ID;
let editingId = 0;
let dragId = 0;
let statusTimer = 0;

function setStatus(text) {
  $('status').textContent = text;
  clearTimeout(statusTimer);
  statusTimer = setTimeout(() => { $('status').textContent = ''; }, 6000);
}

function index(node, parentId) {
  node.parentId = parentId;
  nodes.set(node.id, node);
  for (const child of node.children || []) index(child, node.id);
}

function loadTree() {
  tree = ask('tree') || { bar: { id: BAR_ID, title: 'Bookmarks bar', children: [] }, other: { id: OTHER_ID, title: 'Other bookmarks', children: [] } };
  nodes = new Map();
  index(tree.bar, 0);
  index(tree.other, 0);
  if (!nodes.has(currentFolder)) currentFolder = BAR_ID;
}

function isFolder(node) {
  return Array.isArray(node.children);
}

function hostOf(url) {
  try {
    return new URL(url).hostname.replace(/^www\./, '') || url;
  } catch (e) {
    return url;
  }
}

// Is `id` the folder `ancestor` or inside it?
function isInside(id, ancestor) {
  for (let node = nodes.get(id); node; node = nodes.get(node.parentId)) {
    if (node.id === ancestor) return true;
  }
  return false;
}

// ---- Folder list ------------------------------------------------------------

function renderFolders() {
  const container = $('folders');
  container.textContent = '';
  const add = (node, depth) => {
    const item = document.createElement('div');
    item.className = 'folder' + (node.id === currentFolder && !searching() ? ' selected' : '');
    item.style.paddingLeft = 10 + depth * 16 + 'px';
    item.innerHTML = node.id === BAR_ID ? BAR_ICON : FOLDER_ICON;
    const label = document.createElement('span');
    label.textContent = node.title || '(no name)';
    item.appendChild(label);
    item.tabIndex = 0;
    item.addEventListener('click', () => {
      currentFolder = node.id;
      $('search').value = '';
      render();
    });
    item.addEventListener('keydown', (event) => {
      if (event.key === 'Enter') item.click();
    });
    // Dropping a bookmark on a folder moves it to the end of that folder.
    item.addEventListener('dragover', (event) => {
      if (!dragId || isInside(node.id, dragId)) return;
      event.preventDefault();
      item.classList.add('drop');
    });
    item.addEventListener('dragleave', () => item.classList.remove('drop'));
    item.addEventListener('drop', (event) => {
      event.preventDefault();
      item.classList.remove('drop');
      move(dragId, node.id, -1);
    });
    container.appendChild(item);
    for (const child of node.children) {
      if (isFolder(child)) add(child, depth + 1);
    }
  };
  add(tree.bar, 0);
  add(tree.other, 0);
}

// ---- Items ---------------------------------------------------------------------

function searching() {
  return $('search').value.trim() !== '';
}

function searchResults(text) {
  const terms = text.toLowerCase().split(/\s+/).filter(Boolean);
  const results = [];
  for (const node of nodes.values()) {
    if (node.id === BAR_ID || node.id === OTHER_ID) continue;
    const haystack = ((node.title || '') + ' ' + (node.url || '')).toLowerCase();
    if (terms.every((term) => haystack.includes(term))) results.push(node);
  }
  return results;
}

function pathOf(id) {
  const names = [];
  for (let node = nodes.get(id); node; node = nodes.get(node.parentId)) names.unshift(node.title);
  return names.join(' › ');
}

function openFromClick(event, url) {
  event.preventDefault();
  const mode = event.button === 1 || event.ctrlKey ? 'background' : event.shiftKey ? 'foreground' : 'current';
  ask('open', url + '\n' + mode);
}

function iconButton(title, path, onClick) {
  const button = document.createElement('button');
  button.className = 'icon-button';
  button.title = title;
  button.setAttribute('aria-label', title);
  button.innerHTML = '<svg viewBox="0 0 16 16">' + path + '</svg>';
  button.addEventListener('click', (event) => {
    event.stopPropagation();
    onClick();
  });
  return button;
}

function makeEditor(node) {
  const form = document.createElement('form');
  form.className = 'edit';
  const name = document.createElement('input');
  name.type = 'text';
  name.className = 'name';
  name.value = node.title || '';
  name.placeholder = 'Name';
  name.setAttribute('aria-label', 'Name');
  form.appendChild(name);
  let address = null;
  if (!isFolder(node)) {
    address = document.createElement('input');
    address.type = 'url';
    address.value = node.url;
    address.placeholder = 'https://…';
    address.setAttribute('aria-label', 'Address');
    form.appendChild(address);
  }
  const save = document.createElement('button');
  save.className = 'button';
  save.type = 'submit';
  save.textContent = 'Save';
  const cancel = document.createElement('button');
  cancel.className = 'button secondary';
  cancel.type = 'button';
  cancel.textContent = 'Cancel';
  cancel.addEventListener('click', () => {
    editingId = 0;
    render();
  });
  form.append(save, cancel);
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    const update = { id: node.id, title: name.value.trim() || (address ? address.value.trim() : 'New folder') };
    if (address) {
      let url = address.value.trim();
      if (url && !/^[a-z][a-z0-9+.-]*:/i.test(url)) url = 'https://' + url;
      update.url = url;
    }
    // A new bookmark (id -1) is only added when it's saved.
    const ok = node.id === -1
      ? ask('add', JSON.stringify({ parent: node.parentId, title: update.title, url: update.url }))
      : ask('update', JSON.stringify(update));
    if (!ok || (node.id === -1 && !/^(https?|file|ftp):\/\/[^/]/i.test(update.url))) {
      if (ok && node.id === -1) ask('remove', String(ok));
      setStatus('That address can\'t be bookmarked. Use one that starts with https://, http:// or file://.');
      return;
    }
    editingId = 0;
    refresh();
  });
  form.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') cancel.click();
  });
  setTimeout(() => name.focus(), 0);
  return form;
}

function makeEntry(node, showPath) {
  const row = document.createElement('div');
  row.className = 'entry' + (isFolder(node) ? ' is-folder' : '');
  row.dataset.id = node.id;

  if (editingId === node.id) {
    row.appendChild(makeEditor(node));
    return row;
  }

  const grip = document.createElement('span');
  grip.className = 'grip';
  grip.textContent = '⋮⋮';
  grip.setAttribute('aria-hidden', 'true');

  let mark;
  if (isFolder(node)) {
    mark = document.createElement('span');
    mark.className = 'folder-mark';
    mark.innerHTML = FOLDER_ICON;
  } else {
    mark = document.createElement('span');
    mark.className = 'mark';
    mark.textContent = hostOf(node.url)[0] || '•';
  }

  const title = document.createElement('a');
  title.className = 'title';
  title.textContent = node.title || (isFolder(node) ? '(no name)' : node.url);
  if (isFolder(node)) {
    title.href = '#';
    title.addEventListener('click', (event) => {
      event.preventDefault();
      currentFolder = node.id;
      $('search').value = '';
      render();
    });
  } else {
    title.href = node.url;
    title.title = node.url;
    title.addEventListener('click', (event) => openFromClick(event, node.url));
    title.addEventListener('auxclick', (event) => {
      if (event.button === 1) openFromClick(event, node.url);
    });
  }

  const detail = document.createElement('span');
  detail.className = 'host dim';
  if (isFolder(node)) {
    const count = node.children.length;
    detail.textContent = count === 1 ? '1 item' : count + ' items';
  } else {
    detail.textContent = showPath ? pathOf(node.parentId) + ' · ' + hostOf(node.url) : hostOf(node.url);
  }

  const actions = document.createElement('span');
  actions.className = 'actions';
  actions.append(
    iconButton('Edit', '<path d="M10.5 2.5l3 3-8 8H2.5v-3z"/>', () => {
      editingId = node.id;
      render();
    }),
    iconButton('Delete', '<path d="M3 4.5h10M6 4.5V3h4v1.5M4.5 4.5l.7 9h5.6l.7-9"/>', () => {
      if (isFolder(node) && node.children.length && !window.confirm('Delete the folder "' + node.title + '" and everything in it?'))
        return;
      ask('remove', String(node.id));
      setStatus('Deleted "' + (node.title || node.url) + '".');
      refresh();
    })
  );

  row.append(grip, mark, title, detail, actions);

  // Drag to reorder, or onto a folder (here or in the folder list) to move.
  row.draggable = !showPath;
  row.addEventListener('dragstart', (event) => {
    dragId = node.id;
    row.classList.add('dragging');
    event.dataTransfer.effectAllowed = 'move';
    event.dataTransfer.setData('text/plain', node.url || node.title || '');
  });
  row.addEventListener('dragend', () => {
    dragId = 0;
    row.classList.remove('dragging');
  });
  row.addEventListener('dragover', (event) => {
    if (!dragId || dragId === node.id) return;
    event.preventDefault();
    const rect = row.getBoundingClientRect();
    const y = event.clientY - rect.top;
    row.classList.remove('drop', 'drop-before', 'drop-after');
    if (isFolder(node) && !isInside(node.id, dragId) && y > rect.height * 0.25 && y < rect.height * 0.75)
      row.classList.add('drop');
    else
      row.classList.add(y < rect.height / 2 ? 'drop-before' : 'drop-after');
  });
  row.addEventListener('dragleave', () => row.classList.remove('drop', 'drop-before', 'drop-after'));
  row.addEventListener('drop', (event) => {
    event.preventDefault();
    const into = row.classList.contains('drop');
    const after = row.classList.contains('drop-after');
    row.classList.remove('drop', 'drop-before', 'drop-after');
    if (!dragId) return;
    if (into) {
      move(dragId, node.id, -1);
      return;
    }
    const parent = nodes.get(node.parentId);
    const position = parent.children.findIndex((child) => child.id === node.id);
    move(dragId, parent.id, position + (after ? 1 : 0));
  });
  return row;
}

function move(id, parentId, index) {
  if (!id || isInside(parentId, id)) return;
  ask('move', JSON.stringify({ id, parent: parentId, index }));
  refresh();
}

function render() {
  renderFolders();
  const list = $('list');
  list.textContent = '';
  const text = $('search').value.trim();
  let items;
  if (text) {
    items = searchResults(text);
    $('path').textContent = items.length === 1 ? '1 result' : items.length + ' results';
  } else {
    const folder = nodes.get(currentFolder);
    items = folder.children;
    $('path').textContent = pathOf(currentFolder);
  }
  for (const node of items) list.appendChild(makeEntry(node, !!text));
  if (editingId === -1 && !text)
    list.appendChild(makeEntry({ id: -1, title: '', url: '', parentId: currentFolder }, false));
  const empty = $('empty');
  empty.hidden = items.length > 0 || editingId === -1;
  empty.textContent = text ? 'No bookmarks match "' + text + '".'
    : currentFolder === BAR_ID ? 'Nothing on the bookmarks bar yet. Click the star in the address bar (or press Ctrl+D) to bookmark a page, or drag bookmarks here.'
      : 'This folder is empty.';
}

function refresh() {
  loadTree();
  render();
}

// Prism calls this when bookmarks change (the star, the bookmarks bar, ...).
window.prismBookmarksChanged = () => {
  if (editingId) return; // don't throw away an edit in progress
  refresh();
};

// ---- Adding -------------------------------------------------------------------------

$('addBookmark').addEventListener('click', () => {
  if (searching()) currentFolder = BAR_ID;
  $('search').value = '';
  editingId = -1;
  refresh();
});

$('addFolder').addEventListener('click', () => {
  const folder = searching() ? OTHER_ID : currentFolder;
  const id = ask('add', JSON.stringify({ parent: folder, title: 'New folder' }));
  if (id) editingId = id;
  $('search').value = '';
  currentFolder = folder;
  refresh();
});

$('search').addEventListener('input', () => render());

// ---- Import and export ----------------------------------------------------------------

function closeMenu() {
  $('morePopup').hidden = true;
}

function importResult(result, from) {
  if (!result || result.cancelled) return;
  if (result.error) setStatus(result.error);
  else setStatus('Imported ' + result.count + (result.count === 1 ? ' bookmark' : ' bookmarks') + ' from ' + from + '.');
  refresh();
}

$('moreButton').addEventListener('click', (event) => {
  event.stopPropagation();
  const popup = $('morePopup');
  if (!popup.hidden) {
    closeMenu();
    return;
  }
  popup.textContent = '';
  const browsers = ask('browsers') || [];
  for (const browser of browsers) {
    const button = document.createElement('button');
    button.textContent = 'Import from ' + browser.name;
    button.addEventListener('click', () => {
      closeMenu();
      importResult(ask('importBrowser', browser.id), browser.name);
    });
    popup.appendChild(button);
  }
  const fromFile = document.createElement('button');
  fromFile.textContent = 'Import from an HTML file…';
  fromFile.title = 'Firefox, Safari and most other browsers can export bookmarks as an HTML file.';
  fromFile.addEventListener('click', () => {
    closeMenu();
    importResult(ask('importFile'), 'the file');
  });
  popup.appendChild(fromFile);
  popup.appendChild(document.createElement('hr'));
  const exportButton = document.createElement('button');
  exportButton.textContent = 'Export to an HTML file…';
  exportButton.addEventListener('click', () => {
    closeMenu();
    const result = ask('exportFile');
    if (!result || result.cancelled) return;
    setStatus(result.error || 'Exported ' + result.count + (result.count === 1 ? ' bookmark.' : ' bookmarks.'));
  });
  popup.appendChild(exportButton);
  popup.hidden = false;
});

document.addEventListener('click', (event) => {
  if (!event.target.closest('.menu')) closeMenu();
});
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') closeMenu();
});

refresh();
