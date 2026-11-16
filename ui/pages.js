// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Shared by Prism's own pages: Prism passes the theme in the URL (?theme=dark).
'use strict';
(function () {
  const theme = new URLSearchParams(location.search).get('theme');
  document.documentElement.dataset.theme = theme === 'light' ? 'light' : 'dark';
})();
