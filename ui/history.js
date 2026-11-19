// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// prism://history. Asks Prism for visits with window.prompt("prism:<command>"),
// which Prism answers for this page only.

'use strict';

const $ = (id) => document.getElementById(id);
const PAGE_SIZE = 150;

function ask(command, argument = '') {
  const answer = window.prompt('prism:' + command + '\n' + argument);
  if (answer === null || answer === undefined) return null;
  try {
    return JSON.parse(answer);
  } catch (e) {
    return null;
  }
}

let visits = [];
let lastDay = '';
let searchTimer = 0;

function hostOf(url) {
  try {
    const parsed = new URL(url);
    return parsed.protocol === 'file:' ? 'File on this PC' : parsed.hostname.replace(/^www\./, '');
  } catch (e) {
    return url;
  }
}

function dayLabel(date) {
  const today = new Date();
  today.setHours(0, 0, 0, 0);
  const day = new Date(date);
  day.setHours(0, 0, 0, 0);
  const diff = Math.round((today - day) / 86400000);
  const full = date.toLocaleDateString(undefined, { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' });
  if (diff === 0) return 'Today · ' + full;
  if (diff === 1) return 'Yesterday · ' + full;
  return full;
}

// Opens a link the way the click asked: Ctrl or middle click in a background
// tab, Shift in a new tab, otherwise right here.
function openFromClick(event, url) {
  event.preventDefault();
  const mode = event.button === 1 || event.ctrlKey ? 'background' : event.shiftKey ? 'foreground' : 'current';
  ask('open', url + '\n' + mode);
}

function makeEntry(visit) {
  const row = document.createElement('div');
  row.className = 'entry';

  const time = document.createElement('span');
  time.className = 'time';
  time.textContent = new Date(visit.time).toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });

  const mark = document.createElement('span');
  mark.className = 'mark';
  mark.textContent = (hostOf(visit.url)[0] || '•');

  const link = document.createElement('a');
  link.className = 'title';
  link.href = visit.url;
  link.textContent = visit.title || visit.url;
  link.title = visit.url;
  link.addEventListener('click', (event) => openFromClick(event, visit.url));
  link.addEventListener('auxclick', (event) => {
    if (event.button === 1) openFromClick(event, visit.url);
  });

  const host = document.createElement('span');
  host.className = 'host dim';
  host.textContent = hostOf(visit.url);

  const remove = document.createElement('button');
  remove.className = 'icon-button';
  remove.title = 'Remove from history';
  remove.setAttribute('aria-label', 'Remove from history');
  remove.innerHTML = '<svg viewBox="0 0 16 16"><path d="M4 4l8 8M12 4l-8 8"/></svg>';
  remove.addEventListener('click', () => {
    ask('delete', JSON.stringify([{ time: visit.time, url: visit.url }]));
    row.remove();
  });

  row.append(time, mark, link, host, remove);
  return row;
}

function render(newVisits, append) {
  const list = $('list');
  if (!append) {
    list.textContent = '';
    lastDay = '';
  }
  for (const visit of newVisits) {
    const date = new Date(visit.time);
    const day = date.toDateString();
    if (day !== lastDay) {
      lastDay = day;
      const heading = document.createElement('h2');
      heading.className = 'day';
      heading.textContent = dayLabel(date);
      list.appendChild(heading);
    }
    list.appendChild(makeEntry(visit));
  }
}

function load(append) {
  const text = $('search').value.trim();
  const before = append && visits.length ? visits[visits.length - 1].time : 0;
  const result = ask('query', JSON.stringify({ text, before, limit: PAGE_SIZE })) || { visits: [], more: false, saving: true };
  visits = append ? visits.concat(result.visits) : result.visits;
  render(result.visits, append);
  $('more').hidden = !result.more;
  $('notSaving').hidden = result.saving;
  const empty = $('empty');
  empty.hidden = visits.length > 0;
  empty.textContent = text ? 'Nothing in your history matches "' + text + '".' : 'Pages you visit will show up here.';
}

$('search').addEventListener('input', () => {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(() => load(false), 120);
});

$('more').addEventListener('click', () => load(true));

$('clear').addEventListener('click', () => {
  const range = $('clearRange');
  const label = range.options[range.selectedIndex].textContent.toLowerCase();
  const question = range.value === 'all' ? 'Clear all of your history?' : 'Clear history from the ' + label + '?';
  if (!window.confirm(question)) return;
  ask('clear', range.value);
  load(false);
});

load(false);
$('search').focus();
