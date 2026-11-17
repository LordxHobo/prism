# Changelog

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
