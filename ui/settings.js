// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// prism://settings. Reads and changes settings through Prism: window.prompt()
// with a "prism:" command, which Prism answers for this page only.

'use strict';

const $ = (id) => document.getElementById(id);

function ask(command, argument) {
  const answer = window.prompt('prism:' + command + (argument === undefined ? '' : '\n' + argument));
  if (answer === null || answer === undefined) return null;
  try {
    return JSON.parse(answer);
  } catch (e) {
    return null;
  }
}

const ACCENTS = [
  ['#8b7cf6', 'Violet'],
  ['#3aa0ff', 'Blue'],
  ['#2dd4bf', 'Teal'],
  ['#3ddc84', 'Green'],
  ['#ffb340', 'Amber'],
  ['#ff6b6b', 'Red'],
  ['#ff6ac1', 'Pink'],
  ['#a3a3b5', 'Graphite'],
];

const COOKIE_HELP = {
  standard: 'Blocks cookies from sites you haven’t visited directly. Sites you use (like a login provider) can still work when embedded elsewhere.',
  strict: 'Blocks every cookie from other sites. Most private, but some sign-ins and embedded content may break.',
};

let current = null;

function setSetting(key, value) {
  const updated = ask('set', key + '\n' + value);
  if (!updated || updated.error) return false;
  render(updated);
  return true;
}

// ---- Rendering --------------------------------------------------------------

function renderSegmented(id, value) {
  for (const button of $(id).querySelectorAll('button'))
    button.setAttribute('aria-checked', button.dataset.value === value ? 'true' : 'false');
}

function render(settings) {
  current = settings;
  document.documentElement.dataset.theme = settings.effectiveTheme === 'light' ? 'light' : 'dark';
  window.prismApplyAccent(settings.accent);

  const select = $('searchEngine');
  if (!select.options.length) {
    for (const engine of settings.searchEngines)
      select.add(new Option(engine.name, engine.id));
    select.add(new Option('Custom…', 'custom'));
  }
  select.value = settings.searchEngine;
  $('customSearchRow').hidden = settings.searchEngine !== 'custom';
  if (document.activeElement !== $('searchURL'))
    $('searchURL').value = settings.searchURL || '';

  renderSegmented('startup', settings.startup);
  $('downloadDir').textContent = settings.downloadDir;

  renderSegmented('theme', settings.theme);
  for (const button of $('accent').querySelectorAll('button'))
    button.setAttribute('aria-checked', button.dataset.value.toLowerCase() === settings.accent.toLowerCase() ? 'true' : 'false');
  renderSegmented('tabStyle', settings.tabStyle);

  renderSegmented('cookiePolicy', settings.cookiePolicy);
  $('cookieHelp').textContent = COOKIE_HELP[settings.cookiePolicy] || '';
  $('clearOnExit').checked = !!settings.clearOnExit;
}

// ---- Controls ---------------------------------------------------------------

for (const [color, name] of ACCENTS) {
  const button = document.createElement('button');
  button.dataset.value = color;
  button.style.background = color;
  button.title = name;
  button.setAttribute('role', 'radio');
  button.setAttribute('aria-label', name);
  $('accent').appendChild(button);
}

for (const id of ['startup', 'theme', 'accent', 'tabStyle', 'cookiePolicy']) {
  $(id).addEventListener('click', (event) => {
    const button = event.target.closest('button');
    if (button) setSetting(id, button.dataset.value);
  });
}

$('searchEngine').addEventListener('change', () => {
  const value = $('searchEngine').value;
  if (value === 'custom') {
    // Only switch once there's a valid custom address.
    $('customSearchRow').hidden = false;
    if (current && current.searchURL) setSetting('searchEngine', 'custom');
    else $('searchURL').focus();
  } else {
    setSetting('searchEngine', value);
  }
});

function saveCustomSearch() {
  const value = $('searchURL').value.trim();
  const valid = /^https?:\/\/.+/i.test(value) && value.includes('%s');
  $('searchURLError').hidden = valid || !value;
  if (valid) setSetting('searchURL', value);
}
$('searchURL').addEventListener('change', saveCustomSearch);
$('searchURL').addEventListener('keydown', (event) => {
  if (event.key === 'Enter') saveCustomSearch();
});

$('changeDownloads').addEventListener('click', () => {
  const updated = ask('pickDownloadFolder');
  if (updated && !updated.error) render(updated);
});
$('openDownloads').addEventListener('click', () => ask('openDownloads'));

$('clearOnExit').addEventListener('change', () => {
  setSetting('clearOnExit', $('clearOnExit').checked ? '1' : '0');
});

$('clearNow').addEventListener('click', () => {
  const what = [];
  if ($('clearCookies').checked) what.push('cookies');
  if ($('clearCache').checked) what.push('cache');
  if (!what.length) {
    $('clearStatus').textContent = 'Pick what to clear first.';
    return;
  }
  const started = ask('clearData', what.join(','));
  $('clearStatus').textContent = started ? 'Clearing… you’ll see a message when it’s done.' : 'Couldn’t clear data.';
});

$('openData').addEventListener('click', () => ask('openDataFolder'));

// Highlight the section you're looking at in the sidebar.
const links = [...document.querySelectorAll('#sidebar a')];
function updateCurrentSection() {
  let currentId = 'general';
  for (const section of document.querySelectorAll('section')) {
    if (section.getBoundingClientRect().top < 120) currentId = section.id;
  }
  // At the bottom of the page, the last section counts as current.
  if (window.innerHeight + window.scrollY >= document.body.scrollHeight - 4)
    currentId = document.querySelectorAll('section')[document.querySelectorAll('section').length - 1].id;
  for (const link of links)
    link.classList.toggle('current', link.getAttribute('href') === '#' + currentId);
}
window.addEventListener('scroll', updateCurrentSection, { passive: true });


// ---- Shields ---------------------------------------------------------------------

const LIST_GROUPS = [
  ['ads', 'Ads'],
  ['privacy', 'Privacy'],
  ['malware', 'Malware protection'],
  ['fixes', 'Fixes for broken sites'],
  ['annoyances', 'Annoyances (cookie notices, popups, social widgets)'],
];

let shieldsCustomDirty = false;

function formatSize(bytes) {
  if (!bytes) return 'not downloaded';
  return bytes >= 1024 * 1024 ? (bytes / (1024 * 1024)).toFixed(1) + ' MB' : Math.round(bytes / 1024) + ' KB';
}

function shieldsStatusText(state) {
  if (!state.available) return state.message || 'Prism Shields isn’t available in this copy of Prism.';
  if (!state.enabled) return 'Off. Ads and trackers aren’t blocked.';
  if (state.status === 'downloading' || state.status === 'building' || state.status === 'starting')
    return state.message || 'Getting filter lists ready…';
  if (state.status === 'error') return state.message;
  return 'On. ' + (state.message ? state.message : 'Blocking ads and trackers on every site.');
}

function fillEntries(list, items, emptyText, command) {
  list.textContent = '';
  if (!items.length) {
    const none = document.createElement('li');
    none.className = 'none';
    none.textContent = emptyText;
    list.appendChild(none);
    return;
  }
  for (const item of items) {
    const row = document.createElement('li');
    const name = document.createElement('span');
    name.textContent = item;
    const remove = document.createElement('button');
    remove.textContent = 'Remove';
    remove.addEventListener('click', () => renderShields(ask(command, item)));
    row.append(name, remove);
    list.appendChild(row);
  }
}

function renderShields(state) {
  if (!state) return;
  $('shieldsEnabled').checked = !!state.enabled;
  $('shieldsEnabled').disabled = !state.available;
  $('shieldsStatus').textContent = shieldsStatusText(state);

  const lists = $('shieldsLists');
  lists.textContent = '';
  for (const [group, title] of LIST_GROUPS) {
    const inGroup = (state.lists || []).filter((list) => list.group === group);
    if (!inGroup.length) continue;
    const heading = document.createElement('h3');
    heading.textContent = title;
    lists.appendChild(heading);
    for (const list of inGroup) {
      const label = document.createElement('label');
      const box = document.createElement('input');
      box.type = 'checkbox';
      box.checked = list.on;
      box.disabled = !state.available;
      box.addEventListener('change', () => renderShields(ask('shields.setList', list.id + '\n' + (box.checked ? '1' : '0'))));
      const name = document.createElement('span');
      name.textContent = list.name;
      const size = document.createElement('span');
      size.className = 'size';
      size.textContent = list.on ? formatSize(list.size) : '';
      label.append(box, name, size);
      lists.appendChild(label);
    }
  }

  $('shieldsUpdated').textContent = state.updated
    ? 'Last checked ' + new Date(state.updated * 1000).toLocaleString() + '.'
    : '';
  const busy = state.status === 'downloading' || state.status === 'building' || state.status === 'starting';
  $('shieldsUpdate').disabled = busy || !state.available || !state.enabled;
  $('shieldsUpdate').textContent = busy ? 'Updating…' : 'Update now';
  $('shieldsStats').textContent = state.domains
    ? state.domains.toLocaleString() + ' ad and tracker domains · ' + (state.blocked || 0).toLocaleString() + ' blocked since Prism started'
    : '';

  if (!shieldsCustomDirty) $('shieldsCustom').value = state.custom || '';
  fillEntries($('shieldsSites'), state.disabledSites || [], 'None. Shields is on everywhere.', 'shields.removeDisabledSite');
  fillEntries($('shieldsAllowed'), state.allowedDomains || [], 'None.', 'shields.removeAllowedDomain');
}

$('shieldsEnabled').addEventListener('change', () => renderShields(ask('shields.setEnabled', $('shieldsEnabled').checked ? '1' : '0')));
$('shieldsUpdate').addEventListener('click', () => renderShields(ask('shields.update')));
$('shieldsCustom').addEventListener('input', () => {
  shieldsCustomDirty = true;
  $('shieldsCustomStatus').textContent = '';
});
$('shieldsCustomSave').addEventListener('click', () => {
  shieldsCustomDirty = false;
  renderShields(ask('shields.setCustom', $('shieldsCustom').value));
  $('shieldsCustomStatus').textContent = 'Saved. Applies to pages you load from now on.';
});

// Live updates from Prism (download progress, finished updates).
window.prismShieldsEvent = function (event) {
  if (event && event.type === 'shields' && event.state) renderShields(event.state);
};

// prism://settings#shields (from the Shields panel and elsewhere).
window.prismShowSection = function (id) {
  const section = document.getElementById(id);
  if (section) section.scrollIntoView({ block: 'start' });
};

// ---- Start ------------------------------------------------------------------

const version = ask('version');
$('version').textContent = version ? version : '';

const initial = ask('get');
if (initial) {
  render(initial);
} else {
  document.body.insertAdjacentHTML('afterbegin', '<p class="card" style="margin:16px">Open this page in Prism to change settings.</p>');
}
renderShields(ask('shields.get'));
const startSection = new URLSearchParams(location.search).get('section');
if (startSection) requestAnimationFrame(() => window.prismShowSection(startSection));
updateCurrentSection();
