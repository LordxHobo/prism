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
  $('saveHistory').checked = !!settings.saveHistory;
  $('bookmarksBar').checked = !!settings.bookmarksBar;
  renderSegmented('locationDefault', settings.locationDefault);
  renderSegmented('palette', settings.palette);
  renderSegmented('httpsMode', settings.httpsMode);
  $('httpsHelp').textContent = {
    off: 'Sites open the way their address says, http:// or https://.',
    first: 'http:// addresses open with the secure https:// version. Sites without one open unsecured, with a message.',
    only: 'http:// addresses open with https://. If a site has no secure version, Prism asks before opening the unsecured one.',
  }[settings.httpsMode] || '';
  $('stripTrackingParams').checked = !!settings.stripTrackingParams;
  $('fingerprinting').checked = !!settings.fingerprinting;
  renderSegmented('tabLayout', settings.tabLayout);
  renderSegmented('groupStyle', settings.groupStyle);
  renderSegmented('newTabShortcuts', settings.newTabShortcuts);
  $('homeButton').checked = !!settings.homeButton;
  document.documentElement.dataset.palette = settings.palette || 'default';
  $('groupStyleHelp').textContent = settings.tabLayout === 'vertical'
    ? 'With tabs on the left side, groups are shown as folders in the list.'
    : settings.groupStyle === 'vivaldi'
      ? 'Two rows: your groups on top, the selected group\'s tabs below.'
      : 'One row: each group is a colored label followed by its tabs.';
  renderSegmented('notificationsDefault', settings.notificationsDefault);
}

// ---- Site permissions ---------------------------------------------------------

const PERMISSION_NAMES = { location: 'Location', notifications: 'Notifications' };

function renderPermissions() {
  const list = $('permissionSites');
  const sites = ask('permissions.list') || [];
  list.textContent = '';
  let count = 0;
  for (const site of sites) {
    for (const kind of ['location', 'notifications']) {
      if (site[kind] === null || site[kind] === undefined) continue;
      count++;
      const row = document.createElement('li');
      const name = document.createElement('span');
      name.textContent = site.name + ' · ' + PERMISSION_NAMES[kind] + ': ' + (site[kind] ? 'allowed' : 'blocked');
      const remove = document.createElement('button');
      remove.textContent = 'Remove';
      remove.addEventListener('click', () => {
        ask('permissions.remove', site.origin + '\n' + kind);
        renderPermissions();
      });
      row.append(name, remove);
      list.appendChild(row);
    }
  }
  if (!count) {
    const none = document.createElement('li');
    none.className = 'none';
    none.textContent = 'No sites have asked yet.';
    list.appendChild(none);
  }
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

for (const id of ['startup', 'theme', 'accent', 'tabStyle', 'cookiePolicy', 'locationDefault', 'notificationsDefault',
  'palette', 'tabLayout', 'groupStyle', 'newTabShortcuts', 'httpsMode']) {
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

$('stripTrackingParams').addEventListener('change', () => {
  setSetting('stripTrackingParams', $('stripTrackingParams').checked ? '1' : '0');
});
$('fingerprinting').addEventListener('change', () => {
  setSetting('fingerprinting', $('fingerprinting').checked ? '1' : '0');
});

const PRIVACY_NAMES = { https: 'HTTPS', params: 'Remove tracking', fingerprinting: 'Fingerprinting protection' };
function renderPrivacySites() {
  const list = $('privacySites');
  const sites = ask('privacy.list') || [];
  list.textContent = '';
  for (const site of sites) {
    const parts = Object.keys(PRIVACY_NAMES).filter((key) => site[key] !== null && site[key] !== undefined)
      .map((key) => PRIVACY_NAMES[key] + ' ' + (site[key] ? 'on' : 'off'));
    const row = document.createElement('li');
    const name = document.createElement('span');
    name.textContent = site.site + ' · ' + parts.join(', ');
    const remove = document.createElement('button');
    remove.textContent = 'Remove';
    remove.addEventListener('click', () => {
      ask('privacy.remove', site.site);
      renderPrivacySites();
    });
    row.append(name, remove);
    list.appendChild(row);
  }
  if (!sites.length) {
    const none = document.createElement('li');
    none.className = 'none';
    none.textContent = 'None.';
    list.appendChild(none);
  }
}

$('homeButton').addEventListener('change', () => {
  setSetting('homeButton', $('homeButton').checked ? '1' : '0');
});

// ---- New tab background ----------------------------------------------------------

function renderBackground() {
  const set = ask('newTabBackground') === 'set';
  $('backgroundStatus').textContent = set ? 'A picture of yours (a copy is kept in Prism\'s folder).' : 'None.';
  $('clearBackground').hidden = !set;
}
$('pickBackground').addEventListener('click', () => {
  ask('pickNewTabBackground');
  renderBackground();
});
$('clearBackground').addEventListener('click', () => {
  ask('clearNewTabBackground');
  renderBackground();
});

// ---- Custom CSS --------------------------------------------------------------------

function flash(id, text) {
  $(id).textContent = text;
  setTimeout(() => { $(id).textContent = ''; }, 4000);
}

function addSiteRuleEditor(rule) {
  const box = document.createElement('div');
  box.className = 'site-rule';
  const head = document.createElement('div');
  head.className = 'head';
  const sites = document.createElement('textarea');
  sites.className = 'sites';
  sites.rows = 1;
  sites.spellcheck = false;
  sites.placeholder = 'example.com';
  sites.value = rule.sites || '';
  sites.setAttribute('aria-label', 'Sites');
  const enabled = document.createElement('label');
  enabled.className = 'switch';
  enabled.title = 'On or off';
  enabled.innerHTML = '<input type="checkbox"><span></span>';
  enabled.querySelector('input').checked = rule.enabled !== false;
  const remove = document.createElement('button');
  remove.className = 'button secondary remove';
  remove.textContent = 'Remove';
  remove.addEventListener('click', () => box.remove());
  head.append(sites, enabled, remove);
  const css = document.createElement('textarea');
  css.className = 'css';
  css.rows = 5;
  css.spellcheck = false;
  css.placeholder = 'body { font-size: 18px !important; }';
  css.value = rule.css || '';
  css.setAttribute('aria-label', 'CSS');
  box.append(head, css);
  $('siteRules').appendChild(box);
}

function renderUserCSS() {
  const data = ask('usercss.get') || { interface: '', sites: [] };
  $('interfaceCSS').value = data.interface || '';
  $('siteRules').textContent = '';
  for (const rule of data.sites || []) addSiteRuleEditor(rule);
}

$('interfaceCSSSave').addEventListener('click', () => {
  flash('interfaceCSSStatus', ask('usercss.setInterface', $('interfaceCSS').value) ? 'Saved.' : 'Couldn\'t save (too long?).');
});
$('addSiteRule').addEventListener('click', () => addSiteRuleEditor({ sites: '', css: '', enabled: true }));
$('siteRulesSave').addEventListener('click', () => {
  const rules = [...document.querySelectorAll('.site-rule')].map((box) => ({
    sites: box.querySelector('.sites').value,
    css: box.querySelector('.css').value,
    enabled: box.querySelector('.switch input').checked,
  })).filter((rule) => rule.sites.trim() || rule.css.trim());
  flash('siteRulesStatus', ask('usercss.setSites', JSON.stringify(rules)) ? 'Saved. Reload open pages to see the change.' : 'Couldn\'t save.');
});

$('saveHistory').addEventListener('change', () => {
  setSetting('saveHistory', $('saveHistory').checked ? '1' : '0');
});
$('bookmarksBar').addEventListener('change', () => {
  setSetting('bookmarksBar', $('bookmarksBar').checked ? '1' : '0');
});

$('clearOnExit').addEventListener('change', () => {
  setSetting('clearOnExit', $('clearOnExit').checked ? '1' : '0');
});

$('clearNow').addEventListener('click', () => {
  const what = [];
  if ($('clearCookies').checked) what.push('cookies');
  if ($('clearCache').checked) what.push('cache');
  if ($('clearHistory').checked) what.push('history');
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
renderPermissions();
renderUserCSS();
renderBackground();
renderPrivacySites();

// Prism calls this when a setting changes elsewhere (Ctrl+Shift+B, the menu).
window.prismSettingsChanged = () => {
  const settings = ask('get');
  if (settings) render(settings);
  renderPermissions();
  renderPrivacySites();
};
const startSection = new URLSearchParams(location.search).get('section');
if (startSection) requestAnimationFrame(() => window.prismShowSection(startSection));
updateCurrentSection();
