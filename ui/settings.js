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

// ---- Start ------------------------------------------------------------------

const version = ask('version');
$('version').textContent = version ? version : '';

const initial = ask('get');
if (initial) {
  render(initial);
} else {
  document.body.insertAdjacentHTML('afterbegin', '<p class="card" style="margin:16px">Open this page in Prism to change settings.</p>');
}
updateCurrentSection();
