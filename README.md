# Prism

A private, independent web browser for Windows, built on **WebKit** (the engine behind Safari). Prism isn't based on Chromium or Firefox.

- **No data collection.** No telemetry, analytics, crash reporting or accounts. Prism never contacts its developers.
- **Private by default.** WebKit's tracking prevention, third-party cookie blocking, no DNS prefetching, no link-click pings, and popups only when you click.
- **Extensions from both stores** *(planned)*. Chrome Web Store and Firefox Add-ons extensions, starting with uBlock Origin and Bitwarden.
- **Customizable.** The whole interface is HTML and CSS (see [`ui/`](ui)), so themes and layouts are easy to change.

> **Status: early development (v0.2).** Expect rough edges. Copy-protected (DRM) video such as Netflix or Disney+ can't play in any WebKit browser on Windows.

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
| `ui/` | The browser interface (tab strip, address bar) and Prism's own pages (`prism://newtab`, `prism://whatsnew`), drawn by WebKit itself. |
| `scripts/` | Downloads the WebKit runtime and builds Prism. |
| `webkit.lock` | The exact WebKit version Prism uses. |

## Roadmap

- [x] Tabs, address bar, downloads, privacy defaults (v0.1)
- [x] Settings, session restore, clear browsing data (v0.2)
- [ ] Chrome and Firefox extensions (WebExtensions API)
- [ ] Bookmarks and history
- [ ] Themes, layouts and custom CSS
- [ ] Fingerprinting protection

## License

[Mozilla Public License 2.0](LICENSE). WebKit is licensed separately under LGPL and BSD licenses.
