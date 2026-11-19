# Prism

A private, independent web browser for Windows, built on **WebKit** (the engine behind Safari). Prism isn't based on Chromium or Firefox.

- **No data collection.** No telemetry, analytics, crash reporting or accounts. Prism never contacts its developers.
- **Private by default.** WebKit's tracking prevention, third-party cookie blocking, HTTPS first, tracking parameters removed from links, fingerprinting protection, no DNS prefetching, no link-click pings, and popups only when you click. Each protection can be switched per site.
- **Prism Shields, a built-in ad and tracker blocker.** Brave's blocking engine with uBlock Origin's filter lists: ad and tracker domains, leftover ad boxes, and site-specific fixes (like YouTube's video ads). On everywhere, switchable per site.
- **Extensions from both stores**, with toolbar buttons, popups, background tasks and options pages. Install from the Chrome Web Store or Firefox Add-ons; the Extensions page shows what each one can and can't do in Prism yet.
- **The basics:** bookmarks (with a bookmarks bar, folders, and import from Chrome, Edge, Brave, Vivaldi, Opera or an HTML file), history with address-bar suggestions, find in page, and location and notification prompts.
- **Tabs your way:** tab groups in one row (like Chrome) or two rows (like Vivaldi), vertical tabs, drag to reorder, site icons.
- **Customizable:** color palettes, your own CSS for Prism's interface and for websites, a new tab page with shortcuts and your own background. The whole interface is HTML and CSS (see [`ui/`](ui)).

> **Status: early development (v0.8).** Expect rough edges. Copy-protected (DRM) video such as Netflix or Disney+ can't play in any WebKit browser on Windows.

## Download

Every push to `main` is built automatically. Open the **Actions** tab, pick the latest **Build** run, and download the `Prism-…-windows-x64` artifact. Unzip it and run `Prism.exe`.

You'll need the [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe), which most PCs already have.

## Build it yourself

You **don't** need to compile WebKit. Prism downloads a ready-made WebKit build from WebKit's own build servers. See [docs/BUILDING.md](docs/BUILDING.md) for details. In short:

```bat
powershell -ExecutionPolicy Bypass -File scripts\fetch-webkit.ps1
scripts\build.cmd
dist\Prism.exe
```

## How it works

| Part | What it does |
| --- | --- |
| `src/` | The Windows app (C++). It creates windows and tabs, and loads WebKit's `WebKit2.dll` at runtime through WebKit's C API. |
| `bundle/` | `PrismBundle.dll`, which WebKit loads into its page processes. It asks Prism about every request a page makes, so Prism Shields and blocking extensions can filter by full address. |
| `shields/` | Prism Shields' engine: a small Rust library (`prism_shields.dll`) around Brave's [adblock-rust](https://github.com/brave/adblock-rust), plus the script that bundles uBlock Origin's scriptlets. |
| `ui/` | The browser interface (tab strip, address bar, bookmarks bar) and Prism's own pages (`prism://newtab`, `prism://history`, `prism://bookmarks`, `prism://settings`, ...), drawn by WebKit itself. |
| `scripts/` | Downloads the WebKit runtime and builds Prism. |
| `webkit.lock` | The exact WebKit version Prism uses. |

## Roadmap

- [x] Tabs, address bar, downloads, privacy defaults (v0.1)
- [x] Settings, session restore, clear browsing data (v0.2)
- [x] Extensions, part 1: install, content scripts, storage, filter-list ad blocking (v0.3)
- [x] Prism Shields, part 1: built-in ad and tracker blocker (v0.4)
- [x] Extensions, part 2: popups, background tasks, options pages, messaging (v0.5)
- [x] Bookmarks, history, find in page, permission prompts (v0.6; printing moves to v0.9)
- [x] Customization, tab groups and vertical tabs (v0.7)
- [x] Privacy: fingerprinting protection, HTTPS first/only, per-site settings, tracking-parameter removal (v0.8)
- [ ] Full network filtering, extensions part 3, and Prism's own WebKit build for printing, camera and microphone (v0.9)
- [ ] Polish (v0.10), then 1.0 and Linux

## License

[Mozilla Public License 2.0](LICENSE). WebKit is licensed separately under LGPL and BSD licenses. adblock-rust is MPL-2.0. uBlock Origin's scriptlets, which Prism ships as a separate data file (`shields\scriptlets.json`), are GPLv3; filter lists are downloaded from their authors and keep their own licenses.
