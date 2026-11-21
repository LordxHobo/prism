// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// prism://passwords. Talks to Prism through window.prompt("prism:<command>
// \n<json>"), which Prism answers for this page only. Passwords only come
// to this page when you ask to see one.

'use strict';

const $ = (id) => document.getElementById(id);

function ask(command, data) {
  const answer = window.prompt('prism:' + command + '\n' + (data ? JSON.stringify(data) : ''));
  if (answer === null || answer === undefined) return null;
  try {
    return JSON.parse(answer);
  } catch (e) {
    return null;
  }
}

let state = null;
let logins = [];
let revealed = new Map(); // id -> password, while shown
let editing = null;       // login being edited (null = new)

function showError(id, text) {
  $(id).textContent = text || '';
  $(id).hidden = !text;
}

function flash(text) {
  $('message').textContent = text;
  $('message').hidden = !text;
  clearTimeout(flash.timer);
  if (text) flash.timer = setTimeout(() => { $('message').hidden = true; }, 6000);
}

// ---- Gates: set up / unlock -------------------------------------------------

function applyState(next) {
  if (!next) return;
  if (next.error) return next;
  state = next;
  const s = state.state;
  $('setup').hidden = s !== 'none';
  $('locked').hidden = s !== 'locked';
  $('vault').hidden = s !== 'unlocked';
  $('tools').hidden = s !== 'unlocked';
  $('lock').hidden = state.windowsUnlock;
  $('windowsUnlock').checked = !!state.windowsUnlock;
  $('autoLockRow').hidden = !!state.windowsUnlock;
  $('autoLock').value = String(state.autoLock);
  if (state.waiting) {
    $('waiting').textContent = (s === 'none' ? 'Once Prism Passwords is set up, ' : 'Once it\'s unlocked, ')
      + 'Prism saves your password for ' + (state.waiting.username || 'your login') + ' on ' + state.waiting.site + '.';
    $('waiting').hidden = false;
  } else
    $('waiting').hidden = true;
  if (state.message) flash(state.message);
  if (s === 'unlocked') loadList();
  else {
    revealed.clear();
    requestAnimationFrame(() => (s === 'none' ? $('newMaster') : $('master')).focus());
  }
  return next;
}

function strength(text) {
  let score = 0;
  if (text.length >= 8) score++;
  if (text.length >= 12) score++;
  if (text.length >= 16) score++;
  if (/[a-z]/.test(text) && /[A-Z]/.test(text)) score++;
  if (/\d/.test(text)) score++;
  if (/[^A-Za-z0-9]/.test(text)) score++;
  return Math.min(4, Math.floor(score * 4 / 6));
}
$('newMaster').addEventListener('input', () => {
  const value = $('newMaster').value;
  const score = value ? strength(value) : 0;
  $('meter').style.width = (score * 25) + '%';
  $('meter').dataset.score = String(score);
  $('meterText').textContent = !value ? 'At least 8 characters. A few random words make a strong one.'
    : value.length < 8 ? 'Too short: use at least 8 characters.'
    : ['Weak', 'Weak', 'Fair', 'Good', 'Strong'][score] + '.';
});

$('create').addEventListener('click', () => {
  const master = $('newMaster').value;
  if (master.length < 8) return showError('setupError', 'Use at least 8 characters for the master password.');
  if (master !== $('newMaster2').value) return showError('setupError', 'The two passwords aren\'t the same.');
  showError('setupError', '');
  $('create').disabled = true;
  $('create').textContent = 'Creating…';
  setTimeout(() => {
    const result = ask('create', { master, windowsUnlock: $('setupWindows').checked });
    $('create').disabled = false;
    $('create').textContent = 'Create vault';
    if (result && result.error) return showError('setupError', result.error);
    $('newMaster').value = $('newMaster2').value = '';
    applyState(result);
    if (result && !result.message) flash('Prism Passwords is ready. Log in to a site and Prism offers to save the password.');
  }, 30);
});
for (const id of ['newMaster', 'newMaster2'])
  $(id).addEventListener('keydown', (event) => { if (event.key === 'Enter') $('create').click(); });

function unlock() {
  const master = $('master').value;
  if (!master) return;
  $('unlock').disabled = true;
  setTimeout(() => {
    const result = ask('unlock', { master });
    $('unlock').disabled = false;
    $('master').value = '';
    if (result && result.error) return showError('unlockError', result.error);
    showError('unlockError', '');
    applyState(result);
  }, 30);
}
$('unlock').addEventListener('click', unlock);
$('master').addEventListener('keydown', (event) => { if (event.key === 'Enter') unlock(); });
$('lock').addEventListener('click', () => applyState(ask('lock')));

// Passwords need the master password again after a while.
function withMaster(action) {
  const result = action();
  if (!result || !result.needMaster) return Promise.resolve(result);
  return new Promise((resolve) => {
    $('verify').hidden = false;
    $('verifyMaster').value = '';
    showError('verifyError', '');
    $('verifyMaster').focus();
    $('verifyForm').onsubmit = (event) => {
      event.preventDefault();
      const answer = ask('verify', { master: $('verifyMaster').value });
      $('verifyMaster').value = '';
      if (answer && answer.error) return showError('verifyError', answer.error);
      $('verify').hidden = true;
      resolve(action());
    };
    $('verifyCancel').onclick = () => { $('verify').hidden = true; resolve(null); };
  });
}

// ---- The list ---------------------------------------------------------------

function loadList() {
  const result = ask('list');
  if (!result || result.error) {
    if (result && result.locked) applyState(ask('state'));
    return;
  }
  logins = result.logins;
  renderList();
  renderNever(result.never);
}

function icon(path) {
  return '<svg viewBox="0 0 16 16" aria-hidden="true">' + path + '</svg>';
}
const ICONS = {
  eye: '<path d="M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z"/><circle cx="8" cy="8" r="2"/>',
  copy: '<rect x="5" y="5" width="8.5" height="8.5" rx="1.5"/><path d="M11 5V3.5A1.5 1.5 0 0 0 9.5 2h-6A1.5 1.5 0 0 0 2 3.5v6A1.5 1.5 0 0 0 3.5 11H5"/>',
  edit: '<path d="M10.5 2.5l3 3-8 8H2.5v-3z"/>',
  remove: '<path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.7 9h5.6l.7-9"/>',
};

function button(kind, title, onClick) {
  const element = document.createElement('button');
  element.className = 'icon-button';
  element.title = title;
  element.setAttribute('aria-label', title);
  element.innerHTML = icon(ICONS[kind]);
  element.addEventListener('click', onClick);
  return element;
}

function renderList() {
  const query = $('search').value.trim().toLowerCase();
  const list = $('list');
  list.textContent = '';
  const shown = logins
    .filter((login) => !query || (login.site + ' ' + login.username + ' ' + login.note).toLowerCase().includes(query))
    .sort((a, b) => a.site.localeCompare(b.site) || a.username.localeCompare(b.username));
  for (const login of shown) {
    const row = document.createElement('div');
    row.className = 'row login';
    const site = document.createElement('div');
    site.className = 'site';
    const letter = document.createElement('span');
    letter.className = 'letter';
    letter.textContent = (login.site[0] || '?').toUpperCase();
    const name = document.createElement('a');
    name.href = login.origin;
    name.textContent = login.site;
    name.title = login.origin;
    site.append(letter, name);

    const user = document.createElement('div');
    user.className = 'user';
    user.textContent = login.username || '(no user name)';
    user.title = 'Click to copy the user name';
    user.addEventListener('click', () => {
      const result = ask('copy', { id: login.id, what: 'username' });
      if (result && result.copied) flash('User name copied.');
    });

    const secret = document.createElement('div');
    secret.className = 'secret';
    secret.textContent = revealed.has(login.id) ? revealed.get(login.id) : '••••••••••';
    secret.classList.toggle('shown', revealed.has(login.id));

    const actions = document.createElement('div');
    actions.className = 'actions';
    actions.append(
      button('eye', revealed.has(login.id) ? 'Hide password' : 'Show password', () => {
        if (revealed.has(login.id)) {
          revealed.delete(login.id);
          return renderList();
        }
        withMaster(() => ask('reveal', { id: login.id })).then((result) => {
          if (result && result.password !== undefined) {
            revealed.set(login.id, result.password);
            renderList();
            setTimeout(() => { revealed.delete(login.id); renderList(); }, 30000);
          }
        });
      }),
      button('copy', 'Copy password (cleared from the clipboard after 45 seconds)', () => {
        withMaster(() => ask('copy', { id: login.id, what: 'password' })).then((result) => {
          if (result && result.copied) flash('Password copied. Prism clears it from the clipboard in 45 seconds.');
        });
      }),
      button('edit', 'Edit', () => openEditor(login)),
      button('remove', 'Delete', () => {
        if (!window.confirm('Delete the password for ' + (login.username || 'this login') + ' on ' + login.site + '?')) return;
        const result = ask('delete', { id: login.id });
        if (result && result.logins) {
          logins = result.logins;
          renderList();
        }
      }),
    );
    row.append(site, user, secret, actions);
    list.appendChild(row);
  }
  $('empty').hidden = shown.length > 0;
  $('empty').textContent = query ? 'No passwords match.' : 'No saved passwords yet. Log in to a site and Prism offers to save it, or import them from another browser below.';
}
$('search').addEventListener('input', renderList);

function renderNever(never) {
  $('neverCount').textContent = String(never.length);
  const box = $('never');
  box.textContent = '';
  for (const item of never) {
    const row = document.createElement('div');
    row.className = 'never-row';
    const name = document.createElement('span');
    name.textContent = item.site;
    const undo = document.createElement('button');
    undo.className = 'button secondary';
    undo.textContent = 'Allow saving';
    undo.addEventListener('click', () => {
      const result = ask('allowSaving', { origin: item.origin });
      if (result && result.never) renderNever(result.never);
    });
    row.append(name, undo);
    box.appendChild(row);
  }
  if (!never.length) box.innerHTML = '<p class="dim small">None.</p>';
}

// ---- Add / edit -------------------------------------------------------------

function openEditor(login) {
  editing = login || null;
  $('editorTitle').textContent = login ? 'Edit password' : 'Add a password';
  $('editUrl').value = login ? login.origin : '';
  $('editUser').value = login ? login.username : '';
  $('editPassword').value = '';
  $('editPassword').placeholder = login ? 'Unchanged' : '';
  $('editPassword').type = 'password';
  $('editShow').textContent = 'Show';
  $('editNote').value = login ? login.note : '';
  showError('editError', '');
  $('editor').hidden = false;
  (login ? $('editPassword') : $('editUrl')).focus();
}
$('add').addEventListener('click', () => openEditor(null));
$('editCancel').addEventListener('click', () => { $('editor').hidden = true; $('editPassword').value = ''; });
$('editShow').addEventListener('click', () => {
  const show = $('editPassword').type === 'password';
  $('editPassword').type = show ? 'text' : 'password';
  $('editShow').textContent = show ? 'Hide' : 'Show';
});
$('editGenerate').addEventListener('click', () => {
  $('editPassword').value = ask('generate', { length: 20 }) || '';
  $('editPassword').type = 'text';
  $('editShow').textContent = 'Hide';
});
$('editorForm').addEventListener('submit', (event) => {
  event.preventDefault();
  const data = { url: $('editUrl').value.trim(), username: $('editUser').value, note: $('editNote').value };
  if (editing) data.id = editing.id;
  if ($('editPassword').value || !editing) data.password = $('editPassword').value;
  const result = ask('save', data);
  $('editPassword').value = '';
  if (!result || result.error) return showError('editError', (result && result.error) || 'Couldn\'t save that.');
  $('editor').hidden = true;
  logins = result.logins;
  revealed.clear();
  renderList();
  flash(editing ? 'Saved.' : 'Password added.');
});

// ---- Generator --------------------------------------------------------------

function generate() {
  $('lengthValue').textContent = $('length').value;
  $('generated').textContent = ask('generate', { length: Number($('length').value) }) || '';
}
$('length').addEventListener('input', generate);
$('regenerate').addEventListener('click', generate);
$('copyGenerated').addEventListener('click', () => {
  const text = $('generated').textContent;
  const area = document.createElement('textarea');
  area.value = text;
  document.body.appendChild(area);
  area.select();
  document.execCommand('copy');
  area.remove();
  flash('Copied.');
});

// ---- Import / export / settings ------------------------------------------------

$('import').addEventListener('click', () => {
  const result = ask('import');
  if (!result || result.cancelled) return;
  if (result.error) return flash(result.error);
  flash('Imported: ' + result.added + ' added, ' + result.updated + ' updated, ' + result.skipped + ' skipped.');
  loadList();
});
$('export').addEventListener('click', () => {
  withMaster(() => ask('export')).then((result) => {
    if (!result || result.cancelled) return;
    flash(result.error || ('Exported ' + result.exported + ' passwords. Delete the file when you no longer need it.'));
  });
});
$('windowsUnlock').addEventListener('change', () => {
  const result = ask('settings', { windowsUnlock: $('windowsUnlock').checked });
  if (result && result.error) flash(result.error);
  applyState(ask('state'));
});
$('autoLock').addEventListener('change', () => applyState(ask('settings', { autoLock: Number($('autoLock').value) })));
$('changeMaster').addEventListener('click', () => {
  const replacement = $('replacementMaster').value;
  if (replacement !== $('replacementMaster2').value) return showError('changeError', 'The two new passwords aren\'t the same.');
  const result = ask('changeMaster', { current: $('currentMaster').value, replacement });
  if (result && result.error) return showError('changeError', result.error);
  showError('changeError', '');
  for (const id of ['currentMaster', 'replacementMaster', 'replacementMaster2']) $(id).value = '';
  flash('Master password changed.');
});
$('erase').addEventListener('click', () => {
  if (!window.confirm('Delete every saved password from this computer? This can\'t be undone.')) return;
  const result = ask('erase', { master: $('eraseMaster').value });
  $('eraseMaster').value = '';
  if (result && result.error) return showError('eraseError', result.error);
  showError('eraseError', '');
  applyState(result);
  flash('All passwords were deleted.');
});

document.addEventListener('keydown', (event) => {
  if (event.key !== 'Escape') return;
  $('editor').hidden = true;
  $('editPassword').value = '';
  if (!$('verify').hidden) $('verifyCancel').click();
});

// Prism calls this when the vault locks by itself.
window.prismPasswordsChanged = () => applyState(ask('state'));

applyState(ask('state'));
generate();
