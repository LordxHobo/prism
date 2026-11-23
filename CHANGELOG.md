# Changelog

## 0.9.20

- `webkit/ci.ps1`: the saved build is archived with `7z -snl`, so symbolic links stay links. Restoring removes `Skia/Headers` before configuring: CMake creates `top/skia` and `mod/skia/modules/svg` there with `file(CREATE_LINK)` and failed ("existing path cannot be removed: Access is denied") when the previous archive had stored them as real folders. That happens whenever build-webkit reconfigures (it drops CMakeCache.txt when the patched CMake files change).
- `webkit-stage.yml`: the vcpkg binary cache is saved under `vcpkg-<manifest hash>-<run id>` and restored by prefix, so a compiler update on GitHub's runners (new package ABIs) doesn't leave an unusable cache that can never be replaced.

## 0.9.19

- `webkit/patches/0001` (first CI round): `USE_MEDIA_FOUNDATION` is off with `USE_GSTREAMER` (the Media Foundation player doesn't implement the MSE `load()` and is abstract with `ENABLE_MEDIA_SOURCE`); `dwrite` stays linked for FontCacheSkiaWin. `wtf/glib/GRefPtr.h` and `GSpanExtras.h` include `<gio/gio.h>`/`<glib.h>` on Windows instead of forward-declaring GLib functions (GLib declares them dllimport, and a plain declaration first makes that an error). SharedBuffer.h includes GStreamerCommon.h, so this reached nearly every WebCore file.
- `webkit/ci.ps1`: the error report keeps only errors (each with the 6 lines after it, up to 80 lines per failed step), drops warnings, and starts with the distinct error messages and how often each occurred.

## 0.9.18

- **Prism's own WebKit build** (`webkit/`, `.github/workflows/webkit.yml`, `webkit-stage.yml`): WebKit's Windows builds use Media Foundation and have no Media Source Extensions, so YouTube ("Your browser can't play this video") and most video sites can't play. Prism builds the WebKit commit in `webkit/BASE` with `webkit/patches` on GitHub Actions:
  - `0001` adds `USE_GSTREAMER` to the Windows port: WebKit's GStreamer player and its MSE support, built against GStreamer's MSVC files (`cmake/GStreamerWin.cmake` sets the GLib targets and GSTREAMER_* variables from `GSTREAMER_ROOT` without pkg-config, so GStreamer's own zlib/libxml2 aren't picked over vcpkg's). WTF builds its GLib smart pointers (`USE(GLIB) || USE(GSTREAMER)`) with RunLoopWin kept as the event loop; GLib's default main context (bus signal watches) is dispatched from an 8 ms main RunLoop timer; `fnmatch` comes from `g_pattern_match_simple`; `ImageDecoderGStreamer` copies into a GstBuffer without GBytes. Media Foundation stays registered after GStreamer. No GL sink: frames are painted through Skia.
  - `0002` adds `WEBKIT_MSVC_LINE_TABLES_ONLY` (line tables instead of full debug info, for a small build directory).
  - `webkit/ci.ps1`: LLVM, gperf, ninja, vcpkg (binary cache in actions/cache), GStreamer 1.28.9 (`/TYPE=devel`, checksum checked); a sparse, blob-less WebKit checkout with file times set to 2001 so saved builds stay valid; `build-webkit --win --release --generate-project-only`, then `ninja -k 0` for up to 305 minutes per stage (6 chained stages). The build directory is saved with 7-Zip (file times kept) as an artifact and restored by the next stage or run; patched files that didn't change since get their old time back. Errors (ninja's FAILED blocks) go to the run summary and a `webkit-errors-stage<n>` artifact. The finished build is a Release `webkit-<commit>-r<run>` with `webkit-win64.zip`: the WebKit runtime plus `gstreamer\` (39 plugins, the libraries they and WebKit import found with dumpbin, licenses) and `prism-webkit.json`.
- `scripts/fetch-webkit.ps1`: `-Prism` (newest `webkit-*` Release in the repository from `git remote`, or `-Repo`), `-Stock` (back to build.webkit.org). webkit.lock gets `source: prism` and the asset URL; `-Update` keeps to the lock's source.
- `wk::load`: with a `gstreamer\bin` folder next to WebKit, it's added as a DLL directory and to PATH (WebKit's processes inherit it), and `GST_PLUGIN_SYSTEM_PATH_1_0`, `GST_PLUGIN_SCANNER_1_0` and `GST_REGISTRY_1_0` (data folder) point at Prism's copy; `GST_PLUGIN_PATH(_1_0)` are cleared.

## 0.9.17

- **ACI reading views** (`ui/aci.js`, `src/Aci.*`): Focus, Dyslexia-friendly and Dense. Prism runs ui/aci.js in a world of its own through PrismBundle (`prism:exec` with the pseudo-extension `prism-aci`; its messages arrive as `prism:content` and go to `aci::handleMessage`), or in the page's world with the `prismAci` handler and a per-start key when the bundle isn't there. Articles are found by scoring paragraph text per block (link density, junk class/id names, `<article>`/`<main>` preferred) and copied cleanly (allowed tags only, absolute links opening in the top frame, small images dropped) into a full-window view in a closed shadow root; the page underneath isn't changed (its scrolling is held and its media paused while the view is open, and resumed after). Other pages get a removable style sheet (Dyslexia-friendly: font and spacing, leaving icon fonts, code and SVG alone; Focus: no animations or transitions, media paused) and a small floating bar. Esc/"Original page"/F9 close it.
- View options (Aa): font (Comic Sans default; OpenDyslexic and Lexend bundled as WOFF in ui/fonts with their OFL licenses and passed as data: URLs only when used; Verdana, Arial, Century Gothic, Trebuchet; any installed font by name, listed with `EnumFontFamiliesExW`; a font file copied to `aci-font.<ext>` in the data folder, up to 8 MB), text size, spacing, line length, background tint (automatic, none, cream, blue, green, grey; dark variants in dark theme), bold word starts (Focus), and "always open this site in this view" (`aci-sites.json`). prism.ini: `AciDefault` (off/focus/dyslexia/dense), `AciLastView`, `AciFont`, `AciFileName`, `AciTextSize`, `AciSpacing`, `AciWidth`, `AciTint`, `AciBionic`.
- Address bar **Aa** button (http/https pages, not extension pages; toolbar state `readable`), F9, and the site's or default view opening by itself when a page finishes loading. Settings › Reading views (`aci.get`, `aci.set`, `aci.removeSite`, `aci.fontFile`, `aci.systemFonts`).

## 0.9.16

- **Content scripts in isolated worlds** (`bundle/ContentScripts.*`): PrismBundle creates one `WKBundleScriptWorld` per extension and, on each `didClearWindowObjectForFrame` for the normal world (every new document, all frames), evaluates each extension's content-script runtime (ui/extension-runtime.js + config + scripts; it still checks matches/run_at itself) in that extension's world, with `__prismNative(text, id)` defined there. Messages go to Prism as page messages `prism:content` ("<document>\t<main frame>\t<extension>\t<call id>\t<message>"; no token needed, only the bundle can send them); answers come back with `WKPagePostMessageToInjectedBundle` `prism:contentReply` and are handed to the world's `__prismReceive` (shared by every runtime copy in that world). Worlds' contexts are retained per document and released on the next document in the frame, `didRemoveFrameFromHierarchy`, page destruction, or past 2000. New web processes ask for the scripts with the synchronous `prism:contentScripts?`; changes are broadcast as `prism:contentScripts` (length-prefixed payload).
- tabs/scripting.executeScript and insertCSS/removeCSS run in the extension's world in the main frame (`prism:exec` → `prism:execResult`, 10 s timeout).
- In their world, content scripts also get `chrome` (and `browser` for Firefox Add-ons installs) as globals.
- `extensions::Reply` (WebKit completion listener or a callback) replaces `Ref<WKCompletionListenerRef>` in ExtensionHost, so content-script answers can go either way. Falls back to the page world when the bundle is missing or WebKit lacks the bundle world/JavaScriptCore functions (`isolatedContentScripts()` checks the same exports in the UI process).
- Bundle function tables: `PRISM_BUNDLE_CONTENT` (page loader client V0, script worlds, frame JS contexts, main frame) and `PRISM_JSC` (JavaScriptCore.dll), verified against WebKit's headers. UI: `WKPagePostMessageToInjectedBundle`.
- Toolbar: a lock button (Passwords) between the Extensions button and the menu.

## 0.9.15

- **Extensions panel** (`src/ExtensionsPanel.*`, `ui/extensions-panel.*`, like the Shields panel): the puzzle button opens a panel instead of a native menu. Extensions with buttons, grouped "Full access" / "No access needed" for the current website (`actionsJSON` `access`: host permissions or content script matches for the tab's URL); click opens the popup (or sends action.onClicked) under the puzzle button, pin = show on the toolbar (`settings::setActionHidden`), ≡ = Options, Manage extension, Inspect popup, Inspect background page; "Manage extensions" footer.
- **Icons the page fetches**: when Prism's own icon request fails (including sites that failed while prefetching bookmarks), `icons::pageLoaded`'s `askPage` runs once per site: the tab fetches the icon (and /favicon.ico) itself with its own connection and cookies and posts it base64 to the `prismIcon` handler (main frame only, stored for the tab's own site after the usual type check; ≤512 KB).
- **Performance log** (`src/Perf.*`): a 50 ms main-thread heartbeat plus a watchdog thread note stalls of 250 ms or more with what the main thread was doing (`perf::Scope` around extension page/content messages with extension and operation, Shields page questions, request checks, storage saves); webRequest answers that kept a request waiting 250 ms or more are noted per extension (at most one line per extension per 5 s, timeouts counted). Settings › About › Performance (commands `perf.log`, `perf.clear`).

## 0.9.14

- **Site icons**: icon and front-page requests send Prism's browser user agent (`http::Request::userAgent`; Prism's own "Prism/<version>" stays the default) and no longer offer AVIF in `Accept` (image CDNs such as Shopify's and GitBook's answered with AVIF, which `extensionFor` rejected). AVIF answers are recognized anyway (`ftyp` avif/avis → .avif), and SVG is sniffed in the first 16 KB instead of 1 KB (long XML headers/comments).

## 0.9.13

- **Bookmark icons**: the bookmarks bar (`toolbar.js` `bookmarkMark`) and prism://bookmarks show `item.icon` (letter if missing or it fails to load); bookmark folder and overflow menus get the icon as a 32-bit menu bitmap (`src/MenuIcons.*`, WIC, cached per file; SVG icons can't be decoded there). `icons::prefetch(url)` fetches icons for sites not visited yet: one site at a time, the front page's `<link rel="icon">` (SVG or ~32 px preferred, then shortcut icon, then apple-touch-icon; relative, `//` and `&amp;` handled) or /favicon.ico, no cookies, public hosts only. Bookmarks prefetch 4 s after start and after each change. `icons::filePathFor`. Links windowscodecs.
- **Window placement**: windows open maximized unless the last closed window wasn't (prism.ini `WindowPlacement` = maximized,left,top,right,bottom, saved on WM_DESTROY; the first window gets the saved normal size and place when it's on a monitor).

## 0.9.12

- **Extensions see the browser they were made for**: in extension pages, `navigator.userAgent`, `appVersion`, `vendor` (and `userAgentData` for Chrome) say Chrome 142 for Chrome Web Store, file and folder installs, and Firefox 145 for Firefox Add-ons installs (`config.family`, from `Extension::source`). With Prism's Safari user agent, Bitwarden ran its Safari code paths (`isBrowserSafariApi`, `SafariApp.sendMessageToApp` → `browser.runtime.sendNativeMessage`, "Phishing detection is unavailable on Safari"). Content scripts and websites are unchanged.
- Extension log: arrivals are traced too (first 3 of each kind): messages by command, connects with the port name, sender URL/origin/frame and the number of onConnect listeners, port messages with their action and listener count (or "port unknown here").

## 0.9.11

- Extension log trace (`ui/extension-page.js` `call()`): level `trace` lines for the first use of each operation, failures (up to 3 per operation and message; `runtime.sendMessage` / `tabs.sendMessage` show the message's `command`/`type`/`name`/`action`/`method`, ports their name, storage calls the area and keys), calls unanswered after 5 s (and when they're answered), and pages without WebAssembly. `poll`, `console`, `log`, `respond`, `hello` and `popup.resize` aren't traced. The console buffer now starts before anything else, so early lines aren't lost.

## 0.9.10

- **Background page status and log** on the Extensions page (Details › Background page): `host::backgroundStatusJSON` (none / loading / running / silent = loaded but never said hello / stopped, crash restarts, `runtime.reload` count, offscreen document open) and `Extension::consoleLog` (400 lines this session). `ui/extension-page.js` forwards console.log/info/warn/debug/error from every extension page in batches (op `console`, ≤200 lines per 0.7 s); Prism adds its own notes (starting, loaded, connected, offscreen document, self-reloads, crashes, load failures). Command `backgroundLog <id> [clear]`. Show log / Refresh, Copy log, Clear log.
- An extension that calls `runtime.reload()` 3 times within a minute gets an error on the Extensions page.
- Extensions page repaint: after `setEnabled`, Prism redraws the window 250 ms later, and the page nudges its own paint after each re-render (WebKit on Windows left black, unpainted areas until scrolling).

## 0.9.9

- **Prism Passwords** (`src/PasswordVault.*`, `src/Passwords.*`, `ui/passwords.*`, `ui/passwords-runtime.js`):
  - Vault `%LOCALAPPDATA%\Prism\passwords.vault` (JSON, version 1): a random 256-bit vault key encrypts the contents (logins and the never-save list) with AES-256-GCM (Windows CNG; 12-byte nonce, authenticated purpose string). The vault key is stored wrapped with a key derived from the master password (PBKDF2-SHA256, 600,000 rounds, 16-byte salt) and, when "Stay unlocked while I'm signed in to Windows" is on (default), with DPAPI (`CryptProtectData`, current user). Seeing, copying or exporting a password needs the master password (valid for 5 minutes). Auto-lock (`PasswordsAutoLock`, minutes, default 15, 0 = only at exit) when Windows unlock is off. Strings holding secrets are zeroed when dropped.
  - Page script (group "passwords", top frame of http(s) pages, not extension pages): reports logins on submit, on clicks of buttons near a filled password field, and on Enter (user name from autocomplete="username", an email field, or the nearest text field before it; remembered for two-step logins; sign-up/change-password forms use the new password). A click on a login field asks for the login menu. `window.__prismPasswords.fill` (non-writable) fills what Prism picked and dispatches input/change.
  - Prism takes the site from the tab's committed URL, never from the page, and only accepts messages from the main frame. Login menu (native): saved logins for the origin first, then the same site (https logins never on http pages), "Suggest a strong password" on new-password fields, "Unlock Prism Passwords…" once per page when locked, "Manage passwords". The origin is checked again just before filling.
  - "Save password?" / "Update password?" on the info bar (Save / Never / ×). Kept across the page loads that follow a login (same site, up to 4; other sites, 2). Saving while locked or before setup opens prism://passwords and saves after unlocking.
  - prism://passwords (menu: Passwords): set up, unlock, list/search, show (30 s), copy (clipboard cleared after 45 s if unchanged; `ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory` = 0, `CanUploadToCloudClipboard` = 0), add/edit/delete, generator (12–64, no look-alikes, no modulo bias), CSV import (Chrome/Edge/Brave, Firefox, Bitwarden, 1Password column names) and export (Chrome format), Windows unlock, auto-lock, change master password, never-save sites, delete everything.
  - Links bcrypt and crypt32.
- **Icon**: Ocean everywhere (`res/prism.ico` and `res/prism.svg` are Ocean; `prism-dark.*` removed; one icon resource; `util::taskbarIsLight` removed).

## 0.9.8

- **Accent follows the theme**: the Accent setting is `theme` (default) or a custom #rrggbb. `settings::accent()` returns the custom colour, else Ocean #3aa0ff when `engine::darkTheme()` and Amber #f29e0c when light; `settings::customAccent()` (also in the settings JSON as `customAccent`). Saved values #8b7cf6 (old default), #3aa0ff and #ffb340 (0.9.7's presets) count as `theme`. Settings › Appearance: a split Ocean/Amber "Match the theme" swatch and the custom colour picker. The settings page refreshes on Windows' "ImmersiveColorSet" change. Light-theme defaults in toolbar.css / pages.css are Amber.

## 0.9.7

- **Accent colour**: the presets are now Ocean (#3aa0ff, the new default; was Violet #8b7cf6) and Amber (#ffb340), plus a custom swatch that opens Prism's own colour picker (saturation/brightness square, hue slider, hex field; WebKit on Windows has no `<input type="color">` dialog). A saved colour that isn't a preset shows as the custom swatch. Default `--accent` in toolbar.css / pages.css follows (light: #1f7fd6).
- **Inspecting extensions**: `extensions::inspectBackground` / `inspectPopup` (`WKInspectorShow` on the host page). Extensions page: "Inspect" for extensions with a background (command `inspect`). Toolbar button menu: "Inspect popup" (opens it if needed; `HostView::inspecting` keeps it open when it loses focus) and "Inspect background page".

## 0.9.6

- **chrome.offscreen**: `createDocument` opens a hidden extension page (a host view like the background, one per extension; "Only a single offscreen document may be created." otherwise) and resolves when it has loaded; `closeDocument`, `hasDocument`, `Reason`. A closed document is kept 30 s and reused by the next `createDocument` (Bitwarden opens and closes one around every localStorage call). Closed with the background (stop, reload, disable), on `window.close()` and when its process ends. Its context lives as long as the view (no re-poll expiry), listed as `OFFSCREEN_DOCUMENT`. Bitwarden MV3 keeps its secondary storage there (`OffscreenStorageService`); the old stub rejected, so storage reads in its background failed.
- **runtime.getContexts** lists the extension's open pages (BACKGROUND, POPUP, TAB, OFFSCREEN_DOCUMENT) with Chrome's fields, filtered like Chrome; `runtime.ContextType`. It returned `[]`, so Bitwarden's `isAnyPopupOrPopoutOpen` was always false and it called `runtime.reload()` with its popup open ("Could not establish connection. Receiving end does not exist." while the background restarted).
- `runtime.reload()` closes the extension's popup, like Chrome.
- Background pages report problems on the Extensions page: process ended (memory, CPU, stopped, crashed; restart count resets after 5 minutes; when Prism gives up after 3), failed loads (`didFailProvisionalNavigation` / `didFailNavigation`, with the error), and loading without saying hello within 15 s.
- Build fix: `util::appIcon`'s parameter was named `small`, which the Windows SDK defines as `char` (rpcndr.h), so MSVC failed on Util.cpp.
- **Logo**: the Facet logo, Amber (`res/prism.ico` / `prism.svg`, icon 1, also the exe's file icon) for a light taskbar and Ocean (`res/prism-dark.ico` / `prism-dark.svg`, icon 2) for a dark one, picked from `SystemUsesLightTheme` (`util::appIcon`, `util::taskbarIsLight`) for windows, the window class and the tray icon, and updated on `WM_SETTINGCHANGE` "ImmersiveColorSet". Prism's pages show Ocean on dark pages and Amber (`ui/prism-light.svg`) on light ones.

## 0.9.5

- Background and popup contexts no longer expire when they don't poll again within 8 s (Prism's own host view decides their life: stop, crash, reload, close). A busy background (uBO/AdNauseam handling webRequest for every request on YouTube, or compiling lists) was dropped, and stayed dropped: every op then failed with "Say hello first" and the poll retried every 2 s without saying hello again. Popups got "Could not establish connection. Receiving end does not exist." (uBO, AdNauseam) or "Attempting to use a disconnected port object" (Bitwarden).
- `ui/extension-page.js`: a "Say hello first" answer (to poll or any op) makes the page say hello again and register its webRequest listeners again (listeners keep their options for this). Open ports of the old connection stay closed.

## 0.9.4

- Closing an extension popup: `WKPageClose` before its contexts are killed (ports disconnected), and messages from a just-closed popup page (10 s, unless it's the current popup's page) are refused without logging. Before, the page kept running a moment after its ports were disconnected and reported errors (Bitwarden: "Attempting to use a disconnected port object" when right-clicking its button with the popup open).

## 0.9.3

- **Shields stand-ins and $removeparam** for requests pages make: `prism_shields_check_request_ex` (prism_shields 0.5.0) returns 2 + a data: URL (adblock-rust's `redirect`, for `$redirect` / `$redirect-rule` filters) or 3 + the rewritten address (`rewritten_url`, `$removeparam`). `shields::checkRequest` → RequestVerdict {Allow, Block, StandIn, Rewrite}; RequestFilter answers `r` (not cached) so PrismBundle.dll loads the replacement. Stand-ins count as blocked. Prism.exe still works with an older DLL (block only).
- `shields/build-scriptlets.mjs` takes `<uBlock/src>` as a third argument and adds uBlock Origin's stand-ins (`src/web_accessible_resources`, listed in `src/js/redirect-resources.js`; entries with `params` skipped) as adblock-rust redirect resources: uBO 1.76.4 gives 153 scriptlets + 47 stand-ins. `scripts/build-shields.ps1` checks out `src/web_accessible_resources` too (an older cache is fetched again).
- **contextMenus / menus**: `create` (returns the id at once; numbers when none given; MV2 `onclick`), `update`, `remove` (with children), `removeAll`, `onClicked`. Items per extension in creation order, kept while Prism runs; Manifest V3 extensions' items are saved to `ExtensionData\<id>\menus.json`, MV2 extensions' are dropped when their background restarts (they add them again). Contexts: selection, link, editable, image, video, audio; `page` only when none of those apply; `all`; `action` / `browser_action` / `page_action` for the toolbar button's menu (max 6). `documentUrlPatterns` match the page, `targetUrlPatterns` the link or media address; `%s` shows the selection (60 characters); checkbox and radio items (radio siblings turn off). Several items → a submenu with the extension's name.
- Page menus: `WKPageContextMenuClientV4` with `getContextMenuFromProposedMenuAsync` (only when an extension has items; otherwise WebKit's menu as is). Prism asks the page for `getSelection()`, then appends a separator and the items after WebKit's own (`WKContextMenuItemCreateAsAction` / `…CheckableAction` / `…Submenu`, tags from `kWKContextMenuItemBaseApplicationTag`). `showContextMenu` stays empty so WebKit draws the menu.
- **commands**: shortcuts from `suggested_key` (`windows`, then `default`): Ctrl/Alt/Shift (Command = Ctrl) + A–Z, 0–9, F1–F12, Comma, Period, Home, End, PageUp, PageDown, Space, Insert, Delete, arrows; media keys alone. Other keys need Ctrl or Alt, like Chrome. `_execute_action` / `_execute_browser_action` / `_execute_page_action` open the popup; others send `commands.onCommand(name, tab)`. Prism's own shortcuts are checked first (`handleBrowserShortcut`). `commands.getAll` comes from Prism. The Extensions page lists shortcuts; `"global"` shortcuts are listed as unsupported.
- CI uploads webkit.lock only when it isn't committed yet or differs from the committed one.

## 0.9.2

- **webRequest** for extensions: `onBeforeRequest` (blocking listeners with `webRequestBlocking` can cancel or redirect; Firefox-style promise answers work), `onCompleted`, `onErrorOccurred`. Header events (`onBeforeSendHeaders`, `onHeadersReceived`...) accept listeners but never fire. Listeners need the `webRequest` permission and a host permission for the request's address; filters `urls`, `types` and `tabId` are honoured.
- Subresource requests come from PrismBundle.dll. The question now carries the resource id, frame id (0 = main frame, then numbered per frame in the process), parent frame id (`WKBundleFrameGetParentFrame`), method and the frame's address. The answer is `"<0|1|r>\t<flags>\t<request id>\t<redirect URL>"`; flag `n` = don't cache (any request an extension saw). Redirects are applied with `WKURLRequestCreateWithWKURL` (`chrome-extension://` targets become the extension's http address; http(s) and data: only). Finished / failed loads of reported requests come back as `prism:done "<request id>\t<1|0>\t<error>"` (`WKBundlePostMessage`).
- RequestFilter uses `WKContextInjectedBundleClientV2` with `didReceiveSynchronousMessageFromInjectedBundleWithListener` (and no plain sync callback, which WebKit would prefer), so answers can wait for extensions without blocking Prism's window. Prism Shields and extension rules decide first; then onBeforeRequest goes to matching listeners (through each page's poll) and Prism waits for blocking ones, at most 1 s (100 ms timer). Cancels count toward the shield badge.
- Main-frame loads send onBeforeRequest with type `main_frame` from decidePolicyForNavigationAction (not waited for), which uBlock-based extensions use to keep track of each tab's page.
- The Extensions page now lists only header changes as unsupported for webRequestBlocking extensions.

## 0.9.1

- Shields runtime (`ui/shields-runtime.js`): videos in hidden ads are stopped. Capture-phase `play` / `playing` listeners: a `<video>` inside an element matching one of Shields' hide selectors (the valid ones, joined for `closest`) is paused and muted at once; a connected `<video>` with no layout box or one under 2×2 px is checked again after 400 ms and stopped if it's still playing with no size. Skipped within 1.5 s of a trusted pointerdown/keydown. Videos already playing are checked when new hide rules arrive. Audio elements aren't touched. Tested in WebKitGTK 2.52 (hidden ad tile and zero-size player stopped; visible video and a player sized 150 ms late kept playing).

## 0.9.0

- **PrismBundle.dll** (`bundle/`), a WebKit injected bundle: set with `WKContextConfigurationSetInjectedBundlePath` when the DLL is next to Prism.exe. In each content process it looks up WebKit's C API in WebKit2.dll (`bundle/BundleApi.h`), sets a resource load client on every page and, in `willSendRequestForFrame`, asks Prism with `WKBundlePostSynchronousMessage("prism:check", "<page id>\t<kind>\t<first party URL>\t<URL>")`; "1" cancels the load (returns null). Not asked: main-frame documents and their redirects (checked in decidePolicy), non-http(s) and local addresses, requests whose first party isn't an http(s) site (Prism's own and extension pages). Answers are cached per kind + first party + URL (8192 entries), cleared when Prism posts `prism:filter "<1|0>\t<generation>"` with a new generation; "0" turns asking off. The page id is `WKBundlePageGetWebPageProxyIdentifierForTesting`, announced with `WKBundlePagePostMessage("prism:page")`.
- Request types are guessed from the file extension (`bundle/RequestKind.h`): script, stylesheet, image, font, media, xmlhttprequest (json/xml), subdocument (a frame's own document). Unknown types are checked as other, xmlhttprequest and script.
- `src/RequestFilter.*`: context injected bundle client (sync messages, initialization data), page injected bundle client for the page ids, decision: user-allowed domains first, then Prism Shields (`shields::blocksRequest`, respects Shields on/off and sites with Shields off; settings cached per change), then each blocking extension's engine. Per-page counts and hosts (reset on main-frame commit), also added to the global count and recent list.
- `src/DnrFilters.*`: declarativeNetRequest rules → adblock filters (urlFilter as is, `/x/` → `/x/*`, regexFilter → `/re/` with a trailing `$` written as `\z`, requestDomains → `||d^` lines, resourceTypes / excludedResourceTypes, initiatorDomains / excludedInitiatorDomains (and the old domains / excludedDomains) → `$domain=`, domainType → third-party / ~third-party, isUrlFilterCaseSensitive → match-case; allow / allowAllRequests → `@@`). Skipped: redirect, modifyHeaders, upgradeScheme, urlFilter or regexFilter together with requestDomains, excludedRequestDomains, requestMethods, tabIds, responseHeaders. On uBOL's default rulesets: 17,838 of 18,987 rules (the rest mostly redirects), ~0.2 s to convert and build, ~5 µs per check. Each extension gets its own adblock-rust engine (`shields::buildEngine`), built on the extension rebuild thread. Extensions page: "Blocks ads and trackers (N rules)".
- Toolbar: blocked-on-this-page badge on the shield (`prism.stats(total, here)`). Shields panel: "blocked on this page" and "Blocked on this page" list (command `page`), falling back to the all-tabs list when the bundle isn't running.
- Shortcuts: Alt+F4 closes the window (web views swallowed it); Ctrl+Shift+W closes the window.
- The filtering proxy stays for whole domains and for requests the bundle doesn't see (service workers, WebSockets).

## 0.8.2

- Extension storage (`storage-<area>.json`) is read with a 1 GB limit (`util::readFile` gained a `maxBytes` parameter, default still 16 MB). Files of 16 MB and over used to read as empty, so AdNauseam (unlimitedStorage, 16+ MB) looked freshly installed at every start, opened firstrun.html again and saved over its data.
- A storage file that exists but can't be read or parsed is renamed to `storage-<area>.unreadable.json` instead of being overwritten.
- Storage saves are coalesced: a change marks the store dirty, a 1 s thread timer serializes it and a background thread writes it (newest contents per file win). `extensions::flushStorage()` writes what's waiting at exit; removing an extension drops its pending writes.

## 0.8.1

- curl error 60 (peer certificate not verified) on an https page: once per site and session, Prism makes a WinHTTP HEAD request to the same address. WebKit's curl uses `CURLSSLOPT_NATIVE_CA` on Windows, which only reads root certificates already in the Windows store; Schannel's chain building downloads missing roots from Microsoft's trusted program (automatic root update). If WinHTTP succeeds, `engine::restartNetworkProcess()` (`WKWebsiteDataStoreTerminateNetworkProcess`, new optional binding) makes WebKit read the store again and the page loads again; otherwise the error page shows. HTTPS-first's fallback to http only happens after that.
- Error page for error 60: "Prism can't verify <site>'s certificate", with tips (date and time, Windows Update, inspecting networks).

## 0.8.0

- `src/Privacy.cpp`: per-site overrides in `site-privacy.json` (`{site: {https, params, fingerprinting}}`; a site's override also covers its subdomains; "www." is ignored). Settings `HttpsMode` (off | first | only, default first), `StripTrackingParams`, `Fingerprinting` (both default on).
- **HTTPS:** main-frame http(s) navigations go through privacy first (decidePolicyForNavigationAction): tracking parameters removed, then http:// upgraded to https:// (not for localhost, *.localhost, .local/.lan/.home/.internal/.test/.arpa, single-label names, IP addresses). The navigation is ignored and the new address loaded. If the upgraded load fails (didFailProvisionalNavigation): "first" opens the http address for this session with a toast; "only" shows error page `kind=https` with "Continue … (not secure)" (`prism:allowHttp`). An http navigation to the same site within 10 s of an upgrade (the site redirects back) stops upgrading it this session. Toolbar shows "Not secure" for http:// pages.
- **Tracking parameters:** utm_*, fbclid, gclid, gclsrc, dclid, gbraid, wbraid, msclkid, mc_eid, mc_cid, igshid, igsh, yclid, ysclid, twclid, ttclid, li_fat_id, epik, _hsenc, _hsmi, __hssc, __hstc, __hsfp, hsCtaTracking, mkt_tok, oly_anon_id, oly_enc_id, _openstat, rb_clickid, s_cid, vero_conv, vero_id, wickedid, ml_subscriber(_hash), __s, _kx, irclickid, srsltid; si only on Spotify and YouTube.
- **Fingerprinting protection:** a document-start user script (group "fingerprinting", all frames, http(s) only, not *.localhost, not sites with it off) seeded by a per-start random key and the site (last two labels): canvas getImageData/toDataURL/toBlob change ~1 in 128 pixels' lowest bit (stable per site), WebGL UNMASKED_VENDOR/RENDERER → "Apple Inc." / "Apple GPU" and readPixels changed the same way, AudioBuffer.getChannelData and AnalyserNode.getFloatFrequencyData ±1e-7 on some samples, navigator.hardwareConcurrency 4 or 8 (never more than the real value). Patched functions keep native-looking toString().
- Shields panel: "Privacy for this site" (HTTPS, Remove tracking from links, Fingerprinting protection; commands privacy / setPrivacy, reloads the tab); panel max height 800 px. Settings › Privacy: HTTPS mode, the two switches, and "Sites with their own settings" (privacy.list / privacy.remove).

## 0.7.3

- restoreSession(): extra prism:// pages (What's new after an update) that are already among the restored tabs select that tab instead of opening another one.

## 0.7.2

- tabs.* now report extension pages by the address `runtime.getURL()` gives (`http://<id>.localhost:<port>/<path>`) instead of `chrome-extension://<id>/<path>`, so extensions that look up their own tabs by URL find them. AdNauseam's first-run "Let's go!" sends `closeExtPage`, which looks for a tab whose URL starts with `vAPI.getURL('firstrun.html')`; it never matched, so the page stayed open.
- `displayURL()` maps the http extension addresses (not only old file:// ones) to `chrome-extension://<id>/<path>` for the address bar.

## 0.7.1

- Background tabs are not "in the window" (`WKViewSetIsInWindow(false)`; only the selected tab is). WebKit then doesn't let them start media (`Page::setCanStartMedia`), like Safari: YouTube opened in the background or restored tabs wait until shown; already playing media continues.
- Switching tabs posts `WM_MOUSELEAVE` to the hidden tab, and to the shown one when the cursor isn't over it, so pages don't act on a stale mouse position (YouTube's hover previews started on their own).
- Prism Shields at startup: navigations to websites wait (up to 5 s) until the blocker is built (`shields::isStarting`/`whenReady`, the policy listener is kept and used later), so the first pages get their scriptlets (YouTube showed an ad on the first load). The first build after starting uses the lists already downloaded and skips due updates (`cacheOnly`); a normal build with downloads follows right after.

## 0.7.0

- **Tab groups** (`TabGroup` in BrowserWindow: id, name, color, collapsed; `Tab::groupId`; a group's tabs are kept together by `normalizeGroups()`, empty groups go away). Tab right-click menu: new tab to the right, reload, duplicate, add to new/existing group, remove from group, close / close others / close to the right. Group menu: name, color (Chrome's 9 colors), collapse/expand, new tab in group, ungroup, close group. Links and popups from a grouped tab join its group. Saved in the session (`G` lines; `T` lines get an optional group number).
  - **One row** (setting `GroupStyle=chrome`): colored chip (name, or a dot) before the group's tabs, colored underline; click the chip to collapse (the selected tab moves out of a collapsed group).
  - **Two rows** (`GroupStyle=vivaldi`): top row = groups and single tabs ("units"), the selected one first, the rest in their home order; second row (32 px, 28 compact) = the selected group's tabs and a + for the group. Ctrl+1–9 pick the Nth unit as shown; on the selected unit they cycle its tabs. Selecting a group returns to its last used tab. Dragging a unit sets its new home place (`moveUnit`).
- **Dragging tabs** (toolbar.js, mouse-based): before/after another tab (joins that tab's group), onto a group chip or header (first in the group). `moveTab "<id>\n<index>\n<group>"`.
- **Vertical tabs** (`TabLayout=vertical`): a second WKView of toolbar.html?layout=sidebar on the left (240 px, 48 px narrowed with `VerticalTabsCollapsed`), same state and commands as the toolbar; the toolbar drops its tab strip. Groups are collapsible headers.
- **Site icons** (`src/Icons.cpp`, `%LOCALAPPDATA%\Prism\Icons\` + index.txt): after a page finishes loading, a script asks it for its best `<link rel=icon>` (SVG first, else up to 64 px; else /favicon.ico); Prism downloads it with `http::request` (no cookies, 512 KB max, only public host names, not localhost/IP literals), or decodes data: URLs; checked by magic bytes; per site, refreshed weekly. Shown in tabs, bookmarks bar, History, Bookmarks and new tab shortcuts. Cleared with "Browsing history".
- Tab widths are computed in toolbar.js (up to 220 px; narrow under 90, icon-only under 44) so the + button always fits (it was cut off with many tabs).
- **Color palettes** (`Palette`: default, midnight, nord, forest, rose, sepia; dark and light each) for the toolbar (`data-palette`) and Prism's pages (`?palette=`).
- **Custom CSS** (`src/UserCSS.cpp`): `interface.css` for the toolbar and sidebar (`prism.setUserCSS`), and `site-styles.json` rules (sites list, CSS, on/off) applied by one document-start user script (group `usercss`) on http(s) pages only. Settings › Custom CSS.
- **New tab page:** shortcuts (`NewTabShortcuts`: most visited from `history::topSites`, the bookmarks bar, or none; hide a site with ×, `NewTabHidden`), background picture (copied to the data folder, `NewTabBackground`). Home button (`HomeButton`).
- **Extensions menu** on the puzzle button: per extension Open / Show in toolbar / Options, and Manage extensions. Right-click on an extension button adds "Hide from toolbar" (`HiddenActions`).
- Settings: new Tabs, New tab page and Custom CSS sections; Colors under Appearance.

## 0.6.0

- **Bookmarks** (`src/Bookmarks.cpp`, `bookmarks.json`): bookmarks bar + Other bookmarks, folders. Star in the address bar and Ctrl+D (add; menu to rename/remove when already bookmarked). Bookmarks bar under the navigation bar (setting `BookmarksBar`, Ctrl+Shift+B): folders open as native menus (with "Open all"), overflow under », right-click menu (open, new/background tab, rename, delete, manage, hide bar), Ctrl/middle click opens in the background.
- **prism://bookmarks** (Ctrl+Shift+O): folder tree, search, inline editing, drag to reorder and move, add bookmark/folder. Import from Chrome-style `Bookmarks` files (Chrome, Edge, Brave, Vivaldi, Opera; their bar fills an empty Prism bar, the rest goes into "Imported from …") and from Netscape bookmark HTML files (Firefox's toolbar folder fills an empty bar). Export to Netscape HTML. File dialogs: `dialogs::openFile`/`saveFile`.
- **History** (`src/History.cpp`, `history.tsv`, append-only, rewritten when deleting or dropping old entries): http(s) and the user's own files, 90 days / 100,000 visits. Recorded on `didCommitNavigation` and history.pushState (`didSameDocumentNavigation`, push only); titles follow `didChangeTitle`. Not for Prism's pages, extension pages or error pages; nothing while `SaveHistory` is off. **prism://history** (Ctrl+H): by day, search, "Show more", remove single visits, clear last hour/24 h/7 days/4 weeks/everything. "Browsing history" added to Clear browsing data now.
- **Address-bar suggestions** (`src/Suggestions.cpp`): a native, non-activating popup under the address bar (GDI, Segoe MDL2 icons, rounded corners on Windows 11). First line is "search with …" or "go to …", then up to 3 bookmarks and history ranked by visits, recency (half-life 2 weeks), address-prefix and front-page bonuses; 8 lines at most. Local only. ↑/↓ select (the address bar shows the address), Esc restores the typed text, Enter opens. The toolbar's `suggest` command is answered synchronously.
- **Find in page** (Ctrl+F, F3/Shift+F3, Ctrl+G/Ctrl+Shift+G): find bar under the toolbar, `WKPageFindString` (case-insensitive, wrap-around, highlight + find indicator, up to 1000 matches), "N of M" (the index is tracked by Prism), red field for no matches, state per tab, reset on navigation.
- **Permission prompts:** location (`decidePolicyForGeolocationPermissionRequest`) and notifications (`decidePolicyForNotificationPermissionRequest`) ask in a bar under the toolbar (Allow / Block are saved per origin in `permissions.json`; ✕ denies once). Pending questions are denied when the page changes or the tab closes. Settings › Site permissions: Ask/Block defaults (`LocationDefault`, `NotificationsDefault`) and the saved answers (removable). Extension pages are never asked.
  - Location provider: Windows Location API (`ILocation`, lat/long report), read when a page starts watching and every 15 s; a clear error when Windows has location off for desktop apps.
  - Notification provider: shown as Windows notifications from a temporary notification-area icon; clicking focuses the page's tab (`WKNotificationManagerProviderDidClickNotification`). Saved answers are passed to WebKit (`notificationPermissions`, `DidUpdateNotificationPolicy`).
- **Printing** isn't possible with the stock Windows WebKit (`drawPagesToPDF`/`drawPagesForPrinting` are Cocoa/GTK only): Ctrl+P, the menu and `window.print()` show a message; moved to v0.9 (custom WebKit build). Camera and microphone: `ENABLE_MEDIA_STREAM` is off in the Windows port, so sites can't ask for them.
- Toolbar: rows under the navigation bar (bookmarks bar 30 px, permission bar 40 px, find bar 38 px; `toolbarCSSHeight()` adds them for the active tab). Menu: History, Bookmarks, Show bookmarks bar, Find in page, Print.
- WebKit API: find client, geolocation and notification providers and managers, `WKSecurityOrigin`/`WKMutableDictionary`/`WKBoolean` helpers, typed `printFrame`, permission and `didCommitNavigation`/`didSameDocumentNavigation` callbacks (all optional; 126 signatures and 7 client structs checked against WebKit's headers). `engine::context()`.

## 0.5.5

- `requestIdleCallback`/`cancelIdleCallback` for extensions (WebKit doesn't have them; Safari never shipped them). Extension pages always get them. In websites they're added only when an enabled extension's content scripts (or `scripting.executeScript` code) mention them, so pages don't gain a detectable feature otherwise. Simple version: runs after ~1 ms with a 50 ms `timeRemaining()`; `timeout` is ignored. AdNauseam's background threw `self.requestIdleCallback is not a function` on every `tabs.onUpdated`.

## 0.5.4

- Extension errors include the error's name and message: WebKit's `error.stack` leaves them out, so unhandled rejections only showed a stack.
- `webNavigation.getFrame` (frame 0) and `getAllFrames` return the tab's page instead of `null`.

## 0.5.3

- Extension pages get `chrome.privacy` (network, services and websites settings; `get` reports Prism's values as not controllable, `set`/`clear` do nothing), `webRequest.ResourceType` and the other webRequest option enums, `storage.sync` limits (QUOTA_BYTES_PER_ITEM, MAX_ITEMS, ...), and `dns` (Firefox extensions only; rejects). uBlock Origin–based extensions such as AdNauseam read `chrome.privacy.network...` while starting, so their background part stopped there and their popup stayed half-built.
- Errors from extension pages (uncaught errors, unhandled promise rejections, console.error) are reported to Prism (`log`) and listed per extension on the Extensions page (last 30 this session, repeats counted), with a red badge and "Clear errors".

## 0.5.2

- Each extension is served from its own address, `http://<id>.localhost:<port>/`, by a loopback-only server (`src/ExtensionServer.cpp`). Paths starting with `/` now mean the extension's folder (uBlock Origin Lite loads `/rulesets/...` that way; its background part failed, so its popup said "not a website" and its dashboard stayed empty). WebKit treats `*.localhost` as a secure origin and connects it to 127.0.0.1. The port is chosen once and saved (`ExtensionPort` in prism.ini), since it's part of every extension's origin.
- The server only gives the extension's own pages its files (Fetch Metadata, or Origin/Referer); other pages get just `web_accessible_resources`, with `Access-Control-Allow-Origin: *`. Host-header check against DNS rebinding; no `..` paths.
- File access from file URLs (added in 0.5.1) is off again. Toolbar icons still use file:// URLs.
- `fetch()` from extension pages to sites the extension has host permission for goes through Prism (`runtime.fetch`, `http::request`), skipping CORS like in Chrome; no cookies are sent or stored. Others are normal fetches.
- Extension tabs only show extension pages: links to websites open in a normal tab; websites can't open extension pages in a tab.
- Older file:// addresses of extension pages (from saved sessions) are mapped to the new ones.

## 0.5.1

- Fixed: extension popups (and background pages) built from JavaScript modules, such as uBlock Origin Lite's, stayed empty. WebKit only loads modules between file:// pages with file access allowed, so extension pages (background, popups, options tabs) now use their own configuration (`engine::extensionPageConfiguration()`) with file access and universal access from file URLs, like Chrome's extension origins.
- Options pages open in tabs created with that configuration (`Tab::extensionPages`); other files never load in such a tab, and extension pages never in a normal one (Prism opens a tab of the right kind instead).

## 0.5.0: Extensions, part 2

- Background pages run in hidden views (`src/ExtensionHost.cpp`): MV2 `background.page` and `background.scripts`, and MV3 service workers as a page (`_prism_background.html`) with `importScripts`, `registration`, `clients` and install/activate events. Hidden-page timer throttling is off for them. Crashed ones restart (up to 3 times).
- Toolbar buttons (`action`, `browser_action`, `page_action`) with icons, badges, titles and per-tab state. Popups open under the button, size themselves to their content (max 800 x 600) and close when focus leaves. Right-click menu: Options, Manage extension.
- Options pages open in a tab, shown as `chrome-extension://<id>/...` (also typeable). Websites can't open extension pages.
- Messaging between every part of an extension: `runtime.sendMessage`/`onMessage` (sendResponse, `return true`, promise replies), `tabs.sendMessage`, ports (`runtime.connect`, `tabs.connect`), and `storage.onChanged` everywhere. Each context keeps one request open to Prism ("poll"); contexts that stop asking are dropped.
- Extension page API (`ui/extension-page.js`): runtime, storage, tabs, windows, action/browserAction/pageAction, scripting, alarms, permissions (optional permissions with a confirmation), declarativeNetRequest rulesets, i18n, management, plus harmless stand-ins for contextMenus, commands, notifications, webRequest, webNavigation, idle and offscreen. `fetch()` of the extension's own files works.
- Tab addresses and titles are only shown to extensions with the `tabs` or a matching host permission, or `activeTab` after a toolbar click.
- Extensions page: Options button, and a list of what each extension needs that Prism can't do yet.
- Fixed: the shield's blocked counter stopped updating after the first session save. `PRISM_VERSION_MINOR` was stale.

## 0.4.0: Prism Shields (part 1)

- Prism Shields, a built-in ad, tracker and malware blocker, on by default. Powered by Brave's adblock-rust engine (`shields/`, built as `prism_shields.dll`) with uBlock Origin's default lists: uBlock filters (ads, privacy, badware, unbreak, quick fixes), EasyList, EasyPrivacy and Peter Lowe's list. Cookie-notice and annoyance lists are optional.
- Three layers: domain blocking through the local proxy, cosmetic filtering (per-site and generic hiding), and uBlock Origin scriptlets (bundled from uBO 1.76.4 into `shields/scriptlets.json`, GPLv3, shipped as a separate data file).
- Images, frames and media from a site's own servers that network filters match are hidden (first-party ads).
- Domains that lists block only as a third party aren't blocked while you're visiting that site.
- Shields panel from the address-bar shield: per-site on/off (reloads the tab), global switch, counters, recently blocked domains with "Allow". Grey shield where Shields is off.
- Settings → Shields: on/off, filter lists, update now (lists also update every 4 days), custom filters, sites with Shields off, allowed domains. `prism://settings#shields` opens the section.
- "Allow" button on the blocked-site page when Shields blocked it.
- Extensions: step-by-step install progress with download size, and a "Blocking on" badge.
- `scripts/build-shields.ps1` builds the DLL and the scriptlet bundle (CI and `build.cmd` run it; skipped with a warning without Rust or Node.js).

## 0.3.0: extensions (part 1)

- Extensions page (`prism://extensions`, Ctrl+Shift+E, toolbar button): install from Chrome Web Store / Firefox Add-ons links, `.crx`/`.xpi`/`.zip` files, or unpacked folders; enable, disable, remove.
- "Add to Prism" button in the address bar on store extension pages.
- Plain-language permissions check before installing, and a per-extension "what works in Prism" summary.
- Content scripts (`content_scripts`: matches, globs, run_at, all_frames, CSS) with `chrome.*`/`browser.*` storage (local/sync/session), i18n and runtime basics; callbacks and promises.
- Content scripts are checked to be valid, self-contained programs before installing, and talk to Prism with a per-extension secret token.
- Ad and tracker blocking from declarativeNetRequest rulesets (uBlock Origin Lite and similar) through a local filtering proxy, per domain. Individual filter lists can be turned on and off.
- Blocked-site page naming the extension; blocked count on the address-bar shield.
- Not yet: background scripts, popups, options pages, messaging (planned for 0.4); path-level blocking (planned for 0.8).

## 0.2.0: settings

- Settings page (`prism://settings`, Ctrl+, , the menu, or the address-bar shield) with General, Appearance, Privacy and About sections. Changes apply immediately.
- Search engine choice: DuckDuckGo, Startpage, Brave Search, Ecosia, Google, Bing, or a custom address.
- "Continue where you left off": windows and tabs are saved and restored.
- Choose the download folder.
- Theme (System/Dark/Light), accent color, compact tabs.
- Third-party cookie blocking: Standard or Strict.
- Clear browsing data now, or every time Prism closes.
- Opening Settings or What's new reuses an existing tab.

## 0.1.0: first build

- Windows app built on WebKit's C API, loading WebKit at runtime (no WebKit compile needed).
- Tabs: tab strip, new/close/switch, middle-click to close, reopen closed tabs, popups open as tabs.
- Address bar: web addresses or searches (DuckDuckGo by default, configurable in `prism.ini`).
- Privacy defaults: tracking prevention, third-party cookies blocked, no DNS prefetching, no hyperlink auditing, popups only on click, Safari user agent. No telemetry.
- Downloads to the Downloads folder, with a notification.
- Error and crash pages, JavaScript dialogs, external app links (always asks first).
- Dark and light themes that follow Windows, including a dark title bar.
- Keyboard shortcuts, mouse back/forward buttons, zoom, developer tools (F12).
- "What's new" page shown once after each update.
- Scripts to download the WebKit runtime and build Prism; GitHub Actions build with ready-to-run zips.
