// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// prism://extensions. Talks to Prism with window.prompt("prism:<command>"),
// which Prism answers for this page only. Installs run in the background;
// Prism reports back through window.prismExtensionsEvent().

'use strict';

const $ = (id) => document.getElementById(id);
const UBOL_LINK = 'https://chromewebstore.google.com/detail/ublock-origin-lite/ddkjiahejlhfcafbddmgiahcphecmpfh';

function ask(command, ...args) {
  const answer = window.prompt('prism:' + command + (args.length ? '\n' + args.join('\n') : ''));
  if (answer === null || answer === undefined) return null;
  try {
    return JSON.parse(answer);
  } catch (e) {
    return null;
  }
}

let currentJob = 0;
let pending = null; // the extension waiting for "Add extension"
const openDetails = new Set();

// ---- Status line -------------------------------------------------------------

function setStatus(text, isError) {
  const status = $('installStatus');
  status.hidden = !text;
  status.textContent = text || '';
  status.classList.toggle('error', !!isError);
}

function setBusy(busy) {
  $('installButton').disabled = busy;
  $('fromFile').disabled = busy;
  $('fromFolder').disabled = busy;
}

// ---- Installing ----------------------------------------------------------------

function install(input) {
  input = (input || '').trim();
  if (!input) {
    setStatus('Paste a link to an extension first.', true);
    return;
  }
  const result = ask('install', input);
  if (!result || result.error) {
    setStatus((result && result.error) || 'Couldn’t start the install.', true);
    return;
  }
  currentJob = result.job;
  setBusy(true);
  setStatus(/^https?:/i.test(input) ? 'Downloading…' : 'Reading the extension…');
}

function formatMB(bytes) {
  const mb = bytes / (1024 * 1024);
  return mb < 0.1 ? Math.max(1, Math.round(bytes / 1024)) + ' KB' : mb.toFixed(1) + ' MB';
}

function progressText(event) {
  if (!event.received && !event.total) return event.step;
  if (event.total) {
    const percent = Math.min(100, Math.round((event.received / event.total) * 100));
    return event.step + ': ' + formatMB(event.received) + ' of ' + formatMB(event.total) + ' (' + percent + '%)';
  }
  return event.step + ': ' + formatMB(event.received);
}

function supportItems(support) {
  const items = [];
  if (support.contentScripts) items.push([true, 'Changes pages it’s made for (content scripts)']);
  if (support.rulesets) {
    const count = support.blockingDomains ? ' (' + support.blockingDomains.toLocaleString() + ' domains)' : '';
    items.push([true, 'Blocks ads and trackers by domain' + count]);
  }
  if (support.background) items.push([true, 'Runs in the background']);
  if (support.action) items.push([true, support.popup ? 'Toolbar button with a popup' : 'Toolbar button']);
  if (support.options) items.push([true, 'Options page']);
  for (const missing of support.unsupported || []) items.push([false, missing + ': not yet']);
  if (!items.length) items.push([false, 'Nothing this extension does works in Prism yet']);
  return items;
}

function fillList(list, items) {
  list.textContent = '';
  for (const item of items) {
    const li = document.createElement('li');
    if (Array.isArray(item)) {
      li.className = item[0] ? 'yes' : 'no';
      li.textContent = item[1];
    } else {
      li.textContent = item;
    }
    list.appendChild(li);
  }
}

function setIcon(img, src) {
  if (src) {
    img.src = src;
    img.classList.remove('placeholder');
  } else {
    img.removeAttribute('src');
    img.classList.add('placeholder');
  }
}

// Each content script must be a complete program on its own, so it can't
// break out of the wrapper Prism puts around it (which limits it to the
// sites it says it's for). Parsing doesn't run anything.
function checkScripts(scripts) {
  for (const script of scripts || []) {
    try {
      // eslint-disable-next-line no-new-func
      new Function('chrome', 'browser', script.source);
    } catch (error) {
      return 'Its script ' + script.files + ' isn’t valid JavaScript (' + error.message + '), so Prism won’t install it.';
    }
  }
  return null;
}

function showConfirm(event) {
  const preview = event.preview;
  pending = event;
  setIcon($('confirmIcon'), preview.icon);
  $('confirmTitle').textContent = (event.replaces !== undefined ? 'Update ' : 'Add ') + preview.name + '?';
  const from = { chrome: 'Chrome Web Store', firefox: 'Firefox Add-ons', file: 'a file on your PC', folder: 'a folder on your PC' }[preview.source] || '';
  $('confirmSub').textContent = 'Version ' + preview.version + (event.replaces !== undefined ? ' (you have ' + event.replaces + ')' : '') + (from ? ' · from ' + from : '');
  $('confirmDescription').textContent = preview.description || '';
  fillList($('confirmPermissions'), preview.permissions.length ? preview.permissions : ['Nothing special: it doesn’t ask for any permissions.']);
  fillList($('confirmSupport'), supportItems(preview.support));

  const problem = checkScripts(preview.scripts);
  $('confirmError').hidden = !problem;
  $('confirmError').textContent = problem || '';
  $('confirmOk').disabled = !!problem;
  $('confirmOk').textContent = event.replaces !== undefined ? 'Update' : 'Add extension';

  $('confirm').hidden = false;
  (problem ? $('confirmCancel') : $('confirmOk')).focus();
}

function closeConfirm() {
  $('confirm').hidden = true;
  pending = null;
}

$('confirmCancel').addEventListener('click', () => {
  if (pending) ask('cancel', pending.job);
  closeConfirm();
  setStatus('');
});

$('confirmOk').addEventListener('click', () => {
  if (!pending) return;
  const name = pending.preview.name;
  const result = ask('confirm', pending.job);
  closeConfirm();
  if (!result || result.error) {
    setStatus((result && result.error) || 'Couldn’t install the extension.', true);
    return;
  }
  render(result);
  $('installInput').value = '';
  setStatus(name + ' is installed.' + (result.extensions.length ? ' Reload open tabs to use it there.' : ''));
});

document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape' && !$('confirm').hidden) $('confirmCancel').click();
});

// ---- Events from Prism -----------------------------------------------------------

window.prismExtensionsEvent = function (event) {
  if (!event) return;
  if (event.type === 'progress' && event.job === currentJob) {
    setStatus(progressText(event));
  } else if (event.type === 'prepared' && event.job === currentJob) {
    setBusy(false);
    setStatus('');
    showConfirm(event);
  } else if (event.type === 'failed' && event.job === currentJob) {
    setBusy(false);
    setStatus(event.error, true);
  } else if (event.type === 'changed' && event.list) {
    render(event.list);
  } else if (event.type === 'checkPending') {
    const list = ask('list');
    if (list) render(list);
    if (list && list.pendingInstall) {
      $('installInput').value = list.pendingInstall;
      install(list.pendingInstall);
    }
  }
};

// ---- Installed list ----------------------------------------------------------------

function render(data) {
  const list = $('list');
  list.textContent = '';
  const extensions = data.extensions || [];
  $('empty').hidden = extensions.length > 0;
  $('blockedStat').textContent = data.blocked ? data.blocked.toLocaleString() + ' ads and trackers blocked since Prism started.' : '';

  for (const ext of extensions) {
    const card = document.createElement('article');
    card.className = 'ext' + (ext.enabled ? '' : ' off');

    const head = document.createElement('div');
    head.className = 'extHead';
    const icon = document.createElement('img');
    icon.alt = '';
    setIcon(icon, ext.icon);

    const info = document.createElement('div');
    info.className = 'extInfo';
    const name = document.createElement('div');
    name.className = 'extName';
    name.textContent = ext.name;
    const version = document.createElement('span');
    version.className = 'version';
    version.textContent = ext.version;
    const badge = document.createElement('span');
    badge.className = 'badge ' + ext.support.level;
    badge.textContent = { full: 'Works', partial: 'Partly works', none: 'Not supported yet' }[ext.support.level];
    name.append(version, badge);
    // Is this extension's ad blocking running right now?
    if (ext.enabled && ext.support.blockingDomains) {
      const blocking = document.createElement('span');
      blocking.className = 'badge blocking' + (data.blockingActive ? '' : ' waiting');
      blocking.textContent = data.blockingActive ? 'Blocking on' : 'Blocking starting…';
      blocking.title = ext.support.blockingDomains.toLocaleString() + ' ad and tracker domains blocked in every tab';
      name.appendChild(blocking);
    }
    const description = document.createElement('p');
    description.className = 'extDescription';
    description.textContent = ext.description;
    info.append(name, description);

    const actions = document.createElement('div');
    actions.className = 'extActions';
    const remove = document.createElement('button');
    remove.className = 'button secondary';
    remove.textContent = 'Remove';
    remove.addEventListener('click', () => {
      const result = ask('remove', ext.id);
      if (result) render(result);
    });
    const toggle = document.createElement('label');
    toggle.className = 'switch';
    toggle.title = ext.enabled ? 'Turn off' : 'Turn on';
    const checkbox = document.createElement('input');
    checkbox.type = 'checkbox';
    checkbox.checked = ext.enabled;
    checkbox.setAttribute('aria-label', 'Enable ' + ext.name);
    checkbox.addEventListener('change', () => {
      const result = ask('setEnabled', ext.id, checkbox.checked ? '1' : '0');
      if (result) render(result);
      $('reloadHint').hidden = false;
    });
    toggle.append(checkbox, document.createElement('span'));
    if (ext.support.options && ext.enabled) {
      const options = document.createElement('button');
      options.className = 'button secondary';
      options.textContent = 'Options';
      options.addEventListener('click', () => ask('openOptions', ext.id));
      actions.append(options);
    }
    actions.append(remove, toggle);

    head.append(icon, info, actions);
    card.appendChild(head);

    // Details: what works, permissions, filter lists.
    const details = document.createElement('details');
    details.open = openDetails.has(ext.id);
    details.addEventListener('toggle', () => {
      if (details.open) openDetails.add(ext.id);
      else openDetails.delete(ext.id);
    });
    const summary = document.createElement('summary');
    summary.textContent = 'Details';
    details.appendChild(summary);

    const supportTitle = document.createElement('h3');
    supportTitle.textContent = 'In Prism right now';
    const supportList = document.createElement('ul');
    supportList.className = 'support';
    fillList(supportList, supportItems(ext.support));
    details.append(supportTitle, supportList);

    if (ext.rulesets.length) {
      const title = document.createElement('h3');
      title.textContent = 'Filter lists (' + ext.support.rulesetsEnabled + ' of ' + ext.rulesets.length + ' on)';
      const grid = document.createElement('div');
      grid.className = 'rulesets';
      for (const ruleset of ext.rulesets) {
        const label = document.createElement('label');
        const box = document.createElement('input');
        box.type = 'checkbox';
        box.checked = ruleset.enabled;
        box.addEventListener('change', () => {
          const result = ask('setRuleset', ext.id, ruleset.id, box.checked ? '1' : '0');
          if (result) render(result);
        });
        const text = document.createElement('span');
        text.textContent = ruleset.name;
        label.append(box, text);
        grid.appendChild(label);
      }
      details.append(title, grid);
    }

    const permissionsTitle = document.createElement('h3');
    permissionsTitle.textContent = 'Permissions';
    const permissions = document.createElement('ul');
    fillList(permissions, ext.permissions.length ? ext.permissions : ['None']);
    details.append(permissionsTitle, permissions);

    if (ext.sourceURL) {
      const source = document.createElement('p');
      source.className = 'dim';
      source.textContent = 'Installed from: ' + ext.sourceURL;
      details.appendChild(source);
    }
    card.appendChild(details);
    list.appendChild(card);
  }
}

// ---- Buttons -------------------------------------------------------------------------

$('installForm').addEventListener('submit', (event) => {
  event.preventDefault();
  install($('installInput').value);
});

$('fromFile').addEventListener('click', () => {
  const result = ask('pickFile');
  if (result && result.path) install(result.path);
});

$('fromFolder').addEventListener('click', () => {
  const result = ask('pickFolder');
  if (result && result.path) install(result.path);
});

$('tryUbol').addEventListener('click', (event) => {
  event.preventDefault();
  $('installInput').value = UBOL_LINK;
  install(UBOL_LINK);
});

// ---- Start ---------------------------------------------------------------------------------

const initial = ask('list');
if (initial) {
  render(initial);
  // "Add to Prism" from a store page.
  if (initial.pendingInstall) {
    $('installInput').value = initial.pendingInstall;
    install(initial.pendingInstall);
  }
} else {
  document.querySelector('main').insertAdjacentHTML('afterbegin', '<p class="card">Open this page in Prism to manage extensions.</p>');
}
