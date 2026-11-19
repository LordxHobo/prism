// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Shared by Prism's own pages: Prism passes the theme in the URL (?theme=dark).
'use strict';

// Sets the accent color (#rrggbb) on the page.
window.prismApplyAccent = function (color) {
  if (!/^#[0-9a-f]{6}$/i.test(color || '')) return;
  const root = document.documentElement.style;
  const r = parseInt(color.slice(1, 3), 16), g = parseInt(color.slice(3, 5), 16), b = parseInt(color.slice(5, 7), 16);
  root.setProperty('--accent', color);
  root.setProperty('--accent-soft', 'rgba(' + r + ', ' + g + ', ' + b + ', 0.16)');
};
(function () {
  const params = new URLSearchParams(location.search);
  const theme = params.get('theme');
  document.documentElement.dataset.theme = theme === 'light' ? 'light' : 'dark';
  document.documentElement.dataset.palette = params.get('palette') || 'default';
  window.prismApplyAccent(params.get('accent'));
})();
