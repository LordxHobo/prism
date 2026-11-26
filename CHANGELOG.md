# Changelog

## 0.9.38

- **Translator** (`src/Translate.cpp`, `ui/translate-page.js`, `ui/translate/`). Pages are translated on this PC with Bergamot, the engine of Firefox Translations: Firefox's `bergamot-translator.js` (v0.6.0, MPL-2.0) and CLD2 (`cld-worker.js`, Apache-2.0) are in `ui/translate`; the WebAssembly engine and the language models come from Mozilla's Remote Settings (`translations-wasm`, `translations-models`), like in Firefox, and are kept in `<data>\translations\files` by SHA-256 (checked after download). Versions: engine 4.x with models 3.x (zstd, decompressed in the worker by `fzstd.js`, MIT), or engine 3.x with models 1.x/2.x if there's no 4.x; `filter_expression` is read as a desktop release Firefox would; no lexical shortlist (Firefox's default, more accurate). The record list is cached and refreshed daily.
  - The engine runs in a hidden page, `http://prism-translate.localhost:<port>/engine.html` (the extension server now has Prism sites with `mounts`, here `files/` → the downloads), with two workers: CLD2 and `translator-worker.js` (Bergamot's `BlockingService`, `translateViaPivoting` through English when there's no direct model, up to 4 models loaded). Prism calls `prismEngine.handle()`; answers come back by `prompt()`. The translator's memory is freed after 3 minutes without translating, the page closes after 10 idle minutes or if it crashes.
  - The page part is a PrismBundle content script in its own world (`prism-translate`, added to the payload by `engine::contentScriptsPayload()`, in private windows too). It sends a text sample once there's enough (or at DOMContentLoaded/load) for detection; Prism decides (`TranslateMode` auto/offer/off, `TranslateTo`, never/always languages and never sites in `translate.json`; private windows' "never" sites last until Prism closes). Translation goes block by block (like Firefox: elements whose children are all inline, sent as HTML with an id per tag and placeholders for code, images and the like), on-screen first, 24 pieces or 5,000 characters per request, two at a time; the result is merged into the page's own nodes. A MutationObserver translates what the page adds or changes later (at most 4 times per element), plus `title`, `alt`, `placeholder`, `aria-label` and the document title.
  - Address bar: a translate button (status in the toolbar state as `translate`) with a menu: Show original (reloads without translating), translate into another language, never translate this language / this site (or always, with "When I ask"), Translation settings. Settings › Translation lists the languages with sizes, download and delete.

## 0.9.37

- Extensions: `importScripts()` in background service workers ran each file with indirect `eval`, where top-level `const`/`let`/`class` stay inside that eval. A worker using them from an imported file (Enhancer for YouTube: `ReferenceError: Can't find variable: config` in service-worker.js) failed. Each file now runs as an inline classic `<script>` (synchronous on insertion, shared global lexical scope); its errors are rethrown, and `eval` remains the fallback if inline scripts can't run.

## 0.9.36

- Fix: 0.9.35 listed the new full screen functions (`WKViewSetFullScreenClient`, `WKViewExitFullScreen`, `WKPreferencesSetFullScreenEnabled`) as required, so Prism refused to start on a WebKit without patches/0003 ("This WebKit build is missing functions"). They're optional now: without them, full screen stays off.
- **Split view** (up to 4 tabs, `kMaxSplit`). `BrowserWindow::m_split` holds the tab ids; `shownTabs()` is the split when the selected tab is in it (and the window isn't in full screen), else the selected tab. `layout()` places them with `splitRects()` (2: columns; 3: left plus two stacked right; 4: grid; `m_splitX`/`m_splitY` set the lines, `splitGap()` apart) and shows split tabs only while the split is on screen. The lines between them are dragged in the main window (`WM_SETCURSOR`, `WM_LBUTTONDOWN`/`MOUSEMOVE`/`UP`, double-click resets; the class has `CS_DBLCLKS`), and the selected tab gets an accent frame (`WM_ERASEBKGND`). A click in a split tab selects it (`handleShortcut`); `selectTab` keeps other split tabs on screen. Tab menu: "Split view with" (a new tab or another tab) / "Add to split view", "Remove from split view", "Close split view". Toolbar: dragging a tab below the toolbar (or right of the sidebar) sends `splitTab`; state has `split`, `splitShown`, `splitFull`; split tabs get a line along their top. Closing a tab takes it out of the split; fewer than two ends it. Not saved in the session.

## 0.9.35

- **Full screen video** (the Fullscreen API). `webkit/patches/0003`: WebKit's Windows `PageClientImpl` had every full screen method as `notImplemented`; now `WKViewSetFullScreenClient` tells the app to show the view across the screen (or put it back) before returning, `WKViewIsFullScreen` and `WKViewExitFullScreen` complete it, and without a client full screen is refused. Prism: `FullScreenEnabled` is turned on only when that WebKit API exists; `BrowserWindow::setFullScreen` makes the window a borderless popup covering its monitor with only that tab (toolbar and sidebar hidden) and restores style and placement after. Esc or F11 leaves (`exitFullScreen`, which asks the page and puts the window back regardless); so do switching tabs, closing the tab and a crashed page.

## 0.9.34

- Plain media files didn't play: `MediaPlayerPrivateGStreamer::configureDownloadBuffer` set the download buffer's `temp-template` to `/var/tmp/WebKit-Media-XXXXXX`, and `downloadbuffer` failed to create it ("Could not create temp file", seen in a GStreamer log of mov_bbb.mp4). `webkit/patches/0001`: on Windows the template is in `g_get_tmp_dir()`. The first try still failed ("Invalid argument"): the template was built with a leading separator meant for `/var/tmp`, giving `\C:\Users\...`; on Windows it is now built without one.
- Settings › General › "Compatible page drawing" (`CompatibleDrawing`, off): new tabs call `WKViewSetUsesOffscreenRendering`, so the GPU process renders into a texture, reads it back and the UI process paints it, instead of presenting into the view's window. A test for pages whose display stops updating (input and scrolling still work; switching tabs shows the current state), which points at presentation into the window.

- CI (`webkit/ci.ps1`, Build): a stage failed with 50+ WebKit steps and no compiler message at all (clang-cl stopping quietly, most likely out of memory with the newer Clang). When steps fail without a compiler error and 15+ minutes are left, ninja runs once more with half as many jobs (`-j`, half the cores); real errors just fail again. The error summary now also recognizes `LLVM ERROR`, out-of-memory and crash messages, and lists failed steps that printed nothing as "[no compiler error]".

- CI (`webkit/ci.ps1`): the next failure was 56 WebKit files refusing their precompiled header: `xfacet` (MSVC) "has been modified since the precompiled header ... was built" (mtime 1795473283 expected, 1794868496 found). Each runner image installs Visual Studio, the Windows SDK and LLVM at another time, and a precompiled header from the saved state records the old times; ninja doesn't rebuild it because the new headers are older. Tools: `Set-SystemHeaderTimes` gives the MSVC, ATL, Windows SDK and Clang resource headers the same fixed time as the sources (2001-01-01). Build: `Remove-StalePrecompiledHeaders` deletes the precompiled headers clang refused (and their objects) and ninja runs again, once. `fatal error:` at the start of a line now counts as an error in the summary.

## 0.9.33

- **"Extensions in private windows"** (Settings › Privacy, `ExtensionsInPrivate`, off by default). `BrowserWindow::extensionsHere()` (normal window, or private with the setting on) gates extension events, action buttons, context menu items, shortcuts and webRequest main-frame notes; `tabs::` includes private windows with it. The private user content controller now has the extension message handlers too, refusing messages from private pages while the setting is off (`extensionMessageAllowed`), and registers the "extensions" user-script group when it's on. PrismBundle in private processes gets the content scripts at start (`prism:contentScripts?`) and on changes (`Extensions.cpp`), and `engine::privateExtensionsChanged()` (from `settingsChanged`) sends the scripts or none when the setting flips; it applies to documents loaded afterwards. RequestFilter passes private requests to webRequest listeners only with the setting on. Toolbar (`extensionsHere` in the state) and the private new tab page say which applies.
- Toolbar: every part of `prism.update` runs on its own (`safely`), so one that throws doesn't leave the others (the tabs) stale; errors, uncaught exceptions and rejected promises go to Prism (`toolbarError`) and into the performance log (`perf::note`, once a minute per message). `runToolbarScript` notes scripts WebKit couldn't run (syntax errors, exceptions). If the toolbar has no tabs 1.5 s after loading, it sends `ready` again.
- Extension buttons whose icon is missing or doesn't load show the extension's first letter.

## 0.9.32

- **Private windows** (`BrowserWindow::openPrivate`, menu, Ctrl+Shift+N, bookmark menu "Open in private window").
  - `engine::privatePageConfiguration()`: a `WKWebsiteDataStoreCreateNonPersistentDataStore()` store (ITP on, cookie policy and blocking proxy applied like the main store), shared by all private windows until the last one closes (`engine::releasePrivateData()`, from `onDestroy`). Its own `WKContext` (with PrismBundle), so private pages run in separate web processes; `RequestFilter` keeps a list of contexts, answers `prism:contentScripts?` with no scripts for the private one, and never passes its requests to extensions' webRequest. Its own user content controller: Prism's script message handlers without the extension ones, and every user-script group except "extensions".
  - Not written from private windows: history (visits, titles), the session file (private windows are skipped; with only private windows open, the saved session is kept), site icons (`requestIcon`), permission decisions (location and notifications are denied).
  - Hidden from extensions: no window/tab events, no context menu items, no keyboard shortcuts, no action buttons; `tabs::` skips private windows (except `findByPageAnyWindow`, `activate`, `focusWindow` and `windowHandle`, used by Prism itself).
  - Closing the last normal window while a private one is open saves the normal tabs, as if it were the last window.
  - Toolbar: `private` in the state; purple frame and a "Private" label; the extensions button is hidden. New tab page: no most visited sites, and a note on what private windows keep.
- Toolbar: `.icon[hidden]` is really hidden (`.icon` set `display: grid`, which beat the `hidden` attribute).

## 0.9.31

- No picture in video, real cause: with 0.9.30 `paint()` ran for every frame (about 6 times per frame, once per tile), so frames reached the page's tiles. But `DrawingAreaWC` paints tiles into GPU-process image buffers, through `RemoteGraphicsContextProxy`, whose `drawVideoFrame()` only does anything on Cocoa (shared video frames). Elsewhere it dropped the frame. Plain MP4 files were black too; canvas copies (YouTube's ambient mode) worked because they're drawn in the web process.
- `webkit/patches/0001`: without coordinated graphics, `MediaPlayerPrivateGStreamer::paint()` converts each new sample to a `NativeImage` once (`m_paintedSample`/`m_paintedImage`) and paints it with `drawNativeImage()`, so the GPU process gets one image per frame rather than one per tile. `RemoteGraphicsContextProxy::drawVideoFrame()` falls back to `GraphicsContext::drawVideoFrame()` (frame → image) off Cocoa.

- CI (`webkit/ci.ps1`, Tools): Chocolatey's LLVM feed timed out (504) and LLVM's newest release (23.1.6) has no `LLVM-*-win64.exe` any more (LLVM moved to MSI installers). Chocolatey is asked a second time; then `Install-LLVMFromGitHub` takes the newest release of the needed major version or later that has a Windows x64 archive (`LLVM-*-Windows-X64.tar.xz` or `clang+llvm-*-x86_64-pc-windows-msvc.tar.xz`, unpacked into `C:\llvm`) or the old `.exe` installer, looking back through recent releases. Any failure there falls back to `_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` instead of stopping the build.
- CI (`webkit/ci.ps1`, Tools): the runner's Visual Studio C++ library (MSVC 14.52) refuses Clang older than 22 (`STL1000`), and Chocolatey's LLVM is 20.1.8. The Tools step reads the needed version from `yvals_core.h`; if `clang-cl` is older, it installs LLVM's newest release from GitHub into `C:\llvm` (first on the PATH). If that's still too old, `_CL_=/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH` is set for the rest of the job. A new compiler and C++ library mean WebKit is compiled from scratch once.
- CI (`webkit/ci.ps1`, Tools): gperf no longer comes only from Chocolatey. A newer runner image left no gperf on the PATH, so CMake stopped with "Could NOT find Gperf". The step now looks for `gperf.exe` in Chocolatey's package, builds it with vcpkg (`gperf:x64-windows`) if it isn't there, and prints which gperf it uses.

## 0.9.30

- No picture in video, cause: `MediaPlayerPrivateGStreamer::paint()` returned early while `m_pageIsVisible` was false. It defaults to false, and `MediaPlayer` only forwards the flag to a new engine when it's true, so the MSE player could keep it false even on a visible page. YouTube's ambient mode still worked because canvas copies go through `videoFrameForCurrentTime()`, not `paint()`. Without coordinated graphics (`webkit/patches/0001`), `paint()` no longer skips on that flag (the media element only asks for paint while on screen); it still skips while suspended.
- Logging for the next round if it's still black: `paint()` logs why it didn't paint (suspended, no frame yet) at `webkit*:5`, each painted frame at `:6`, and failed frame conversions in `VideoFrameGStreamer` as `GST_PERFORMANCE` warnings.

## 0.9.29

- **Crash recovery** (`src/Recovery.cpp/.h`): each run writes `running-<pid>.txt` (its process start time) in the data folder and removes it when the message loop ends. At startup, a note whose process is gone (or whose PID now belongs to another process) means that run didn't close normally; `session.txt` is then copied to `session-recovery.txt` before this run saves over it. Unless the session reopens anyway ("Continue where you left off"), the first window's info bar offers to reopen those tabs (`BrowserWindow::offerRecovery`, prompt kind `restore`): Restore opens them with `restoreSession({}, recoveryFile)` and closes the starting window if it only has the new tab; "Not now" discards them. An unanswered offer is made again at the next start.
- Crashed background tabs: `webProcessDidTerminate` sets `reloadWhenShown` for tabs that aren't active; `selectTab` navigates to it. The active tab still shows the crash page.
- Crash reports: `SetUnhandledExceptionFilter` writes a minidump (`MiniDumpNormal | WithThreadInfo | WithUnloadedModules`, through dbghelp.dll from System32) to `crashes\prism-<date>-<time>-<pid>.dmp`; the newest five are kept.
- `BrowserWindow::hasInfobar()` decides the info bar row (permissions, password offers, recovery). `restoreSession` takes an optional session file.

## 0.9.28

- First working Prism WebKit build: YouTube streams through MSE (all qualities offered), but no picture. `MediaPlayerPrivateGStreamer` reported `supportsAcceleratedRendering() == true` and took `m_canRenderingBeAccelerated` from accelerated compositing, so the video got a composited layer and `triggerRepaint()` skipped repainting; frames would have gone to `pushTextureToCompositor()`, which only exists with coordinated graphics. Without coordinated graphics (`webkit/patches/0001`), it reports no accelerated rendering and always repaints, so frames are painted through `paint()` (Skia).
- Known: full screen. WebKit's Windows `PageClientImpl` doesn't implement `WebFullScreenManagerProxyClient` (every method is `notImplemented()`), so sites see no Fullscreen API. Needs a WebKit patch plus Prism handling the window.

## 0.9.27

- `webkit/patches/0001`: `Source/WebKit/PlatformWin.cmake` adds GStreamer's (and GLib's) include folders to `WebKit_SYSTEM_INCLUDE_DIRECTORIES`, which WebProcess, NetworkProcess and GPUProcess inherit, and its libraries to `WebKit_PRIVATE_LIBRARIES`. WebCore's `SharedBuffer.h` includes `GStreamerCommon.h`, so every WebKit file sees `<gst/gst.h>`. WebCore itself now compiles completely.

## 0.9.26

- `webkit/ci.ps1`: state format `links-3`. Progress saved in format `links-2` left every hard-linked file out of the archive, including vcpkg's installed libraries (`vcpkg_installed/.../icu-i18n.pc` missing on restore, so `vcpkg install` failed); it's ignored now, and the next build starts from scratch.

## 0.9.25

- `webkit/ci.ps1`: only symbolic links and junctions go in `links.txt` (PowerShell also reports hard links through `LinkType`, with no target, and `CreateSymbolicLink` refused the empty target on restore). Restoring skips such entries from earlier saves; those files weren't archived and ninja makes them again.

## 0.9.24

- `webkit/patches/0001`: `MediaPlayerPrivateGStreamer::IsDuplicateSample` is declared outside `USE(COORDINATED_GRAPHICS)`; `triggerRepaint()` uses it on every port. This was the only file in WebCore that still failed to compile.

## 0.9.23

- `webkit/ci.ps1`: symbolic links in the build folder (WebKit's Headers/PrivateHeaders, Skia's header folders) are no longer archived; `links.txt` lists each one (path, target, file or folder, its own last-write time), 7-Zip leaves them out (`-x@`), and restoring recreates them with `CreateSymbolicLink` and sets the link's own time (`CreateFileW` with `FILE_FLAG_OPEN_REPARSE_POINT` + `SetFileTime`), so ninja doesn't recompile against them. 7-Zip's `-snl` round trip had left `JavaScriptCore/AbstractSlotVisitor.h` unresolvable for WebCore's precompiled header. Restoring no longer deletes `Skia/Headers`, which was only made again when CMake reconfigured. State format `links-2`: earlier saved progress is ignored.

## 0.9.22

- `webkit/patches/0001`, fixes from the first compile round: `MediaSourcePrivate::computeBufferedRanges` loses its own `WEBCORE_EXPORT` (the class is exported; clang-cl rejects both); `MediaSource.cpp` uses `intptr_t` for its reverse track loops (no `ssize_t` on Windows); `AppendPipeline` default-initializes its `std::atomic_flag` (MSVC's has no `bool` constructor); `GStreamerCommon.cpp` only uses `ApplicationGLib.h`/`getApplicationName()` with GLib (else client-name "WebKit"); `GStreamerVideoFrameConverter` only sets timer priority with the GLib event loop; `MediaPlayer.cpp` includes `CoordinatedPlatformLayerBufferProxy.h` only with coordinated graphics and `MediaPlayerPrivateGStreamer.cpp` includes `GStreamerCaptureDeviceManager.h` only with media streams; `platform/audio/gstreamer` is on the Windows include path (`AudioDecoder.cpp` includes `AudioDecoderGStreamer.h`).

## 0.9.21

- `webkit/ci.ps1`: WebKit's PrivateHeaders/Headers are symbolic links to the source headers (`WEBKIT_SYMLINK_FILES`, `cmake -E create_symlink`); the first run's archive (without `-snl`) had turned them into copies, so `#pragma once` saw each header twice (`redefinition of 'CodeBlock'` etc. in LLIntOffsetsExtractor). Archives are now extracted with `-snl` too, and carry `format.txt` (`links-1`); progress without it is ignored and the build starts from scratch.

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
