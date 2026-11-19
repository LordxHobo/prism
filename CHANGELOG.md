# Changelog

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
