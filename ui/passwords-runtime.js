// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Prism Passwords on web pages (top frame of http/https pages only).
//  - When a login is sent (form submit, a click on its button, or Enter in
//    its password field), tells Prism the user name and password, so Prism
//    can offer to save them. Prism works out the site itself.
//  - A click on a login field asks Prism to show the saved logins for the
//    site; Prism fills the one picked through fill() below.
// Prism never hands a page a password it didn't pick from this menu, and
// only for the site the login was saved for.
(function () {
  'use strict';
  if (window.top !== window) return;
  if (location.protocol !== 'https:' && location.protocol !== 'http:') return;
  if (/(^|\.)localhost$/i.test(location.hostname) && location.port) return; // extension pages
  const handler = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.prismPasswords;
  if (!handler) return;
  const post = handler.postMessage.bind(handler);
  const send = (message) => { try { post(JSON.stringify(message)); } catch (e) { /* page went away */ } };

  const isPassword = (el) => el instanceof HTMLInputElement && el.type === 'password';
  const isTextLike = (el) => el instanceof HTMLInputElement && /^(text|email|tel|)$/i.test(el.type);
  const visible = (el) => {
    if (!el.isConnected || el.disabled) return false;
    const box = el.getBoundingClientRect();
    if (box.width < 2 || box.height < 2) return false;
    const style = getComputedStyle(el);
    return style.visibility !== 'hidden' && style.display !== 'none';
  };
  const scopeOf = (el) => (el && el.form) || document;
  // Logins without a <form>: the nearest element around `el` that has what
  // `wanted` looks for.
  function around(el, wanted) {
    if (el && el.form) return el.form;
    for (let node = el && el.parentElement; node && node !== document.documentElement; node = node.parentElement)
      if (wanted(node)) return node;
    return document;
  }
  const passwordsIn = (scope) => Array.from(scope.querySelectorAll('input[type="password"]')).filter(visible);
  const USERNAME_HINT = /user|login|email|e-mail|account|ident|phone|handle/i;

  // The user name field for a password field: autocomplete="username" first,
  // then an email field, then the nearest text field before it.
  function usernameFor(password) {
    const scope = around(password, (node) => Array.from(node.querySelectorAll('input')).some((el) => isTextLike(el) && visible(el)));
    const fields = Array.from(scope.querySelectorAll('input')).filter((el) => isTextLike(el) && visible(el));
    const tagged = fields.find((el) => /\busername\b/i.test(el.autocomplete || '')) || fields.find((el) => el.type === 'email');
    if (tagged) return tagged;
    const before = fields.filter((el) => password.compareDocumentPosition(el) & Node.DOCUMENT_POSITION_PRECEDING);
    const hinted = before.filter((el) => USERNAME_HINT.test(el.name + ' ' + el.id + ' ' + (el.autocomplete || '') + ' ' + (el.placeholder || '')));
    return hinted.pop() || before.pop() || null;
  }

  // Remembered for logins that ask for the user name on one screen and the
  // password on the next (same page, or the next page of the same site).
  let lastUsername = '';
  try { lastUsername = sessionStorage.getItem('__prismLastUsername') || ''; } catch (e) { /* storage off */ }
  function rememberUsername(value) {
    if (!value) return;
    lastUsername = value;
    try { sessionStorage.setItem('__prismLastUsername', value); } catch (e) { /* storage off */ }
  }
  document.addEventListener('change', (event) => {
    const el = event.target;
    if (isTextLike(el) && el.value && (el.type === 'email' || USERNAME_HINT.test(el.name + ' ' + el.id + ' ' + (el.autocomplete || ''))))
      rememberUsername(el.value.trim());
  }, true);

  // ---- Logins being sent ----------------------------------------------------
  let lastSent = '';
  function captureFrom(scope) {
    const fields = passwordsIn(scope).filter((el) => el.value);
    if (!fields.length) return;
    // Sign-up and change-password forms: the new password (the last one,
    // or the one marked new-password).
    let password = fields.find((el) => /new-password/i.test(el.autocomplete || '')) || fields[fields.length - 1];
    if (fields.length >= 3) password = fields[1]; // current, new, confirm
    const usernameField = usernameFor(password);
    const username = (usernameField && usernameField.value.trim()) || lastUsername;
    const key = username + '\n' + password.value;
    if (key === lastSent || password.value.length > 512) return;
    lastSent = key;
    if (usernameField && usernameField.value) rememberUsername(usernameField.value.trim());
    send({ op: 'submitted', username, password: password.value });
  }
  document.addEventListener('submit', (event) => captureFrom(event.target), true);
  document.addEventListener('click', (event) => {
    const button = event.target instanceof Element && event.target.closest('button, input[type="submit"], input[type="button"], [role="button"]');
    if (!button) return;
    const scope = button.form || button.closest('form') || around(button, (node) => passwordsIn(node).some((el) => el.value));
    if (passwordsIn(scope).some((el) => el.value)) setTimeout(() => captureFrom(scope), 0);
  }, true);
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Enter' && isPassword(event.target) && event.target.value) captureFrom(scopeOf(event.target));
  }, true);

  // ---- Filling --------------------------------------------------------------
  let target = null; // the field the menu was opened for
  function fieldKind(el) {
    if (isPassword(el)) {
      const scope = scopeOf(el);
      const count = passwordsIn(scope).length;
      return /new-password/i.test(el.autocomplete || '') || count >= 2 ? 'new-password' : 'password';
    }
    if (isTextLike(el)) {
      const scope = scopeOf(el);
      if (!passwordsIn(scope).length && !/\busername\b/i.test(el.autocomplete || '')) return '';
      if (/\busername\b/i.test(el.autocomplete || '') || el.type === 'email' || USERNAME_HINT.test(el.name + ' ' + el.id + ' ' + (el.placeholder || ''))) return 'username';
    }
    return '';
  }
  document.addEventListener('click', (event) => {
    const el = event.target;
    if (!(el instanceof HTMLInputElement) || !event.isTrusted) return;
    const kind = fieldKind(el);
    if (!kind) return;
    target = el;
    const box = el.getBoundingClientRect();
    const ratio = window.devicePixelRatio || 1;
    send({ op: 'field', kind, rect: { left: box.left * ratio, top: box.top * ratio, width: box.width * ratio, height: box.height * ratio } });
  }, true);

  function setValue(el, value) {
    if (!el) return;
    const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
    el.focus();
    setter.call(el, value);
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }
  function fill(data) {
    const el = target && target.isConnected ? target : document.activeElement;
    if (!(el instanceof HTMLInputElement)) return false;
    if (data.mode === 'new') {
      // A suggested password: every new password field of the form.
      const fields = passwordsIn(scopeOf(el));
      const current = fields.length >= 3 ? fields[0] : null;
      for (const field of fields) if (field !== current) setValue(field, data.password);
      return true;
    }
    const password = isPassword(el) ? el : passwordsIn(scopeOf(el))[0] || null;
    const username = isPassword(el) ? usernameFor(el) : el;
    if (username && data.username) setValue(username, data.username);
    if (password) setValue(password, data.password);
    else if (data.username) rememberUsername(data.username); // user name now, password on the next screen
    lastSent = (data.username || '') + '\n' + data.password; // no "save?" for what Prism just filled
    return true;
  }
  Object.defineProperty(window, '__prismPasswords', { value: Object.freeze({ fill }), writable: false, configurable: false, enumerable: false });
})();
