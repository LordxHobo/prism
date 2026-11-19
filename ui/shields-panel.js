// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The Prism Shields panel (opens from the shield in the address bar).
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

const allowedNow = new Set();

function render(data) {
  if (!data) return;
  const state = data.state || {};
  const panel = $('panel');
  const globalOn = !!state.enabled && !!state.available;
  panel.classList.toggle('globalOff', !globalOn);
  panel.classList.toggle('siteOff', !data.siteOn);

  $('site').textContent = data.web ? data.site : 'Prism page';
  $('siteSwitch').checked = data.web && data.siteOn && globalOn;
  $('siteSwitch').disabled = !data.web || !globalOn;
  $('globalSwitch').checked = !!state.enabled;
  $('globalSwitch').disabled = !state.available;

  let title, hint;
  if (!state.available) {
    title = 'Not available';
    hint = state.message || 'Prism Shields couldn’t start.';
  } else if (!state.enabled) {
    title = 'Off everywhere';
    hint = 'Turn Shields back on below.';
  } else if (!data.web) {
    title = 'Nothing to block here';
    hint = 'Shields protects websites, not Prism’s own pages.';
  } else if (data.siteOn) {
    title = 'On for this site';
    hint = 'Ads and trackers are blocked.';
  } else {
    title = 'Off for this site';
    hint = 'Ads and trackers on ' + data.site + ' aren’t hidden. Tracker domains are still refused.';
  }
  $('siteState').textContent = title;
  $('siteHint').textContent = hint;

  const busy = state.status === 'downloading' || state.status === 'building' || state.status === 'starting';
  const notice = $('notice');
  if (state.available && state.enabled && (busy || state.status === 'error' || state.message)) {
    notice.hidden = false;
    notice.textContent = busy ? (state.message || 'Getting filter lists ready…') : state.message;
  } else {
    notice.hidden = true;
  }

  $('blocked').textContent = (state.blocked || 0).toLocaleString();
  $('domains').textContent = (state.domains || 0).toLocaleString();

  const list = $('recent');
  list.textContent = '';
  const recent = data.recent || [];
  $('recentEmpty').hidden = recent.length > 0;
  const allowed = new Set(state.allowedDomains || []);
  for (const item of recent) {
    const row = document.createElement('li');
    const domain = document.createElement('span');
    domain.className = 'domain';
    domain.textContent = item.domain;
    domain.title = item.domain;
    const count = document.createElement('span');
    count.className = 'count';
    count.textContent = '×' + item.count.toLocaleString();
    const button = document.createElement('button');
    const isAllowed = allowed.has(item.domain) || allowedNow.has(item.domain);
    row.classList.toggle('allowed', isAllowed);
    button.textContent = isAllowed ? 'Allowed' : 'Allow';
    button.disabled = isAllowed;
    button.title = 'Never block ' + item.domain + ' (you can undo this in Shields settings)';
    button.addEventListener('click', () => {
      allowedNow.add(item.domain);
      render(ask('allowDomain', item.domain));
    });
    row.append(domain, count, button);
    list.appendChild(row);
  }
  resize();
}

function resize() {
  ask('resize', Math.ceil($('panel').getBoundingClientRect().height));
}

$('siteSwitch').addEventListener('change', () => {
  render(ask('setSite', $('siteSwitch').checked ? '1' : '0'));
});
$('globalSwitch').addEventListener('change', () => {
  render(ask('setEnabled', $('globalSwitch').checked ? '1' : '0'));
});
$('settings').addEventListener('click', (event) => {
  event.preventDefault();
  ask('settings');
});
document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') ask('close');
});

// Privacy switches for this site (HTTPS, tracking parameters, fingerprinting).
function renderPrivacy(data) {
  const section = $('privacySection');
  if (!data || !section) return;
  $('privHttps').checked = !!data.https;
  $('privParams').checked = !!data.params;
  $('privFingerprinting').checked = !!data.fingerprinting;
  $('httpsHint').textContent = data.httpsMode === 'only'
    ? 'Only the secure version; asks before opening an unsecured one.'
    : 'Opens the secure version of this site when there is one.';
}
for (const [id, key] of [['privHttps', 'https'], ['privParams', 'params'], ['privFingerprinting', 'fingerprinting']]) {
  $(id).addEventListener('change', () => renderPrivacy(ask('setPrivacy', key + '\n' + ($(id).checked ? '1' : '0'))));
}
const privacyData = ask('privacy');
$('privacySection').hidden = !privacyData;
renderPrivacy(privacyData);

render(ask('get'));
// Keep the numbers fresh while the panel is open.
setInterval(() => render(ask('get')), 1500);
