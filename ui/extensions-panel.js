// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The Extensions panel (opens from the puzzle button in the toolbar).
'use strict';

const $ = (id) => document.getElementById(id);

function ask(command, argument) {
  const answer = window.prompt('prism:' + command + (argument === undefined ? '' : '\n' + argument));
  try {
    return answer ? JSON.parse(answer) : null;
  } catch (e) {
    return null;
  }
}

const PIN = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M6 2h4l-.6 4 2.6 2.5H4L6.6 6z"/><path d="M8 8.5V14"/></svg>';
const MENU = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 4.5h10M3 8h10M3 11.5h10"/></svg>';

const GROUPS = [
  { access: 'full', title: 'Full access', text: 'These extensions can see and change information on this site.' },
  { access: 'none', title: 'No access needed', text: 'These extensions don’t need to see and change information on this site.' },
];

function iconFor(ext) {
  if (ext.icon) {
    const img = document.createElement('img');
    img.className = 'icon';
    img.alt = '';
    img.src = ext.icon;
    img.draggable = false;
    return img;
  }
  const letter = document.createElement('span');
  letter.className = 'icon letter';
  letter.textContent = (ext.name || '?')[0];
  return letter;
}

function render(data) {
  if (!data) return;
  const groups = $('groups');
  groups.textContent = '';
  const list = data.extensions || [];
  $('empty').hidden = list.length > 0;
  // Pages that aren't websites: one plain list.
  const sections = data.site ? GROUPS : [{ access: null, title: '', text: '' }];
  for (const group of sections) {
    const members = list.filter((ext) => group.access === null || (ext.access || 'none') === group.access);
    if (!members.length) continue;
    const section = document.createElement('section');
    if (group.title) {
      const title = document.createElement('h2');
      title.textContent = group.title;
      const text = document.createElement('p');
      text.className = 'dim';
      text.textContent = group.text;
      section.append(title, text);
    }
    for (const ext of members) {
      const row = document.createElement('div');
      row.className = 'row' + (ext.enabled ? '' : ' disabled');
      const open = document.createElement('button');
      open.className = 'open';
      open.title = ext.enabled ? 'Open ' + ext.name : ext.name + ' isn’t available on this page';
      const name = document.createElement('span');
      name.className = 'name';
      name.textContent = ext.name;
      open.append(iconFor(ext), name);
      if (ext.badge) {
        const badge = document.createElement('span');
        badge.className = 'badge';
        badge.textContent = ext.badge;
        open.append(badge);
      }
      open.addEventListener('click', () => ask('open', ext.id));

      const pin = document.createElement('button');
      pin.className = 'tool pin' + (ext.pinned ? ' pinned' : '');
      pin.innerHTML = PIN;
      pin.title = ext.pinned ? 'Unpin from the toolbar' : 'Pin to the toolbar';
      pin.setAttribute('aria-pressed', ext.pinned ? 'true' : 'false');
      pin.addEventListener('click', () => render(ask(ext.pinned ? 'unpin' : 'pin', ext.id)));

      const menu = document.createElement('button');
      menu.className = 'tool';
      menu.innerHTML = MENU;
      menu.title = 'More';
      menu.addEventListener('click', () => {
        const box = menu.getBoundingClientRect();
        ask('menu', ext.id + '\n' + Math.round(box.right) + ',' + Math.round(box.bottom));
      });
      row.append(open, pin, menu);
      section.appendChild(row);
    }
    groups.appendChild(section);
  }
  resize();
}

function resize() {
  ask('resize', Math.ceil($('panel').getBoundingClientRect().height));
}

$('close').addEventListener('click', () => ask('close'));
$('manage').addEventListener('click', () => ask('manage'));
document.addEventListener('keydown', (event) => { if (event.key === 'Escape') ask('close'); });

render(ask('list'));
