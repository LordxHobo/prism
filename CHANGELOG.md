# Changelog

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
