# Changelog

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
