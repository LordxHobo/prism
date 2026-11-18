# Prism

A private, independent web browser for Windows, built on **WebKit** (the engine behind Safari). Prism isn't based on Chromium or Firefox.

- **No data collection.** No telemetry, analytics, crash reporting or accounts. Prism never contacts its developers.
- **Private by default.** WebKit's tracking prevention, third-party cookie blocking, no DNS prefetching, no link-click pings, and popups only when you click.
- **Prism Shields, a built-in ad and tracker blocker.** Brave's blocking engine with uBlock Origin's filter lists: ad and tracker domains, leftover ad boxes, and site-specific fixes (like YouTube's video ads). On everywhere, switchable per site.
- **Extensions from both stores**, with toolbar buttons, popups, background tasks and options pages. Install from the Chrome Web Store or Firefox Add-ons; the Extensions page shows what each one can and can't do in Prism yet.
- **Customizable.** The whole interface is HTML and CSS (see [`ui/`](ui)), so themes and layouts are easy to change.

> **Status: early development (v0.5).** Expect rough edges. Copy-protected (DRM) video such as Netflix or Disney+ can't play in any WebKit browser on Windows.

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
| `shields/` | Prism Shields' engine: a small Rust library (`prism_shields.dll`) around Brave's [adblock-rust](https://github.com/brave/adblock-rust), plus the script that bundles uBlock Origin's scriptlets. |
| `ui/` | The browser interface (tab strip, address bar) and Prism's own pages (`prism://newtab`, `prism://whatsnew`), drawn by WebKit itself. |
| `scripts/` | Downloads the WebKit runtime and builds Prism. |
| `webkit.lock` | The exact WebKit version Prism uses. |

## Roadmap

- [x] Tabs, address bar, downloads, privacy defaults (v0.1)
- [x] Settings, session restore, clear browsing data (v0.2)
- [x] Extensions, part 1: install, content scripts, storage, filter-list ad blocking (v0.3)
- [x] Prism Shields, part 1: built-in ad and tracker blocker (v0.4)
- [x] Extensions, part 2: popups, background tasks, options pages, messaging (v0.5)
- [ ] Bookmarks, history, find in page, printing, permission prompts (v0.6)
- [ ] Customization, tab groups and vertical tabs (v0.7)
- [ ] Privacy: fingerprinting protection, HTTPS-only, per-site settings, tracking-parameter removal (v0.8)
- [ ] Prism's own WebKit build: full network filtering, extensions part 3 (v0.9)
- [ ] Polish (v0.10), then 1.0 and Linux

## License

[Mozilla Public License 2.0](LICENSE). WebKit is licensed separately under LGPL and BSD licenses. adblock-rust is MPL-2.0. uBlock Origin's scriptlets, which Prism ships as a separate data file (`shields\scriptlets.json`), are GPLv3; filter lists are downloaded from their authors and keep their own licenses.
