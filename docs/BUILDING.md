# Building Prism

Prism has two parts:

1. **Prism itself**: a small Windows app (`src/`) plus its interface (`ui/`). It compiles in about a minute.
2. **The WebKit runtime**: WebKit's DLLs and helper programs. You **don't** compile these. WebKit's build servers publish a Windows build after every successful build, and `scripts/fetch-webkit.ps1` downloads one.

Everything ends up in `dist\`, which is a complete, portable copy of Prism.

## Option A: let GitHub build it

Push to `main` (or run the **Build** workflow by hand from the Actions tab). When it finishes, download the `Prism-…-windows-x64` artifact, unzip it and run `Prism.exe`.

## Option B: build on your PC

### What you need (one time)

- **Visual Studio 2022 or newer**, or the free **Build Tools for Visual Studio**, with the **Desktop development with C++** workload. It includes CMake and Ninja. Download: <https://visualstudio.microsoft.com/downloads/> (the Build Tools are under "Tools for Visual Studio").
- **Git**.
- About **1 GB** of free disk space.
- The [Microsoft Visual C++ Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe), which you probably already have.

### Steps

From the repository folder, in Command Prompt or PowerShell:

```bat
powershell -ExecutionPolicy Bypass -File scripts\fetch-webkit.ps1
scripts\build.cmd
dist\Prism.exe
```

- `fetch-webkit.ps1` downloads the WebKit version pinned in `webkit.lock` into `dist\`. The first time, when there's no `webkit.lock` yet, it picks the newest successful WebKit build and **creates** `webkit.lock`. Commit that file so every build uses the same WebKit.
- `build.cmd` finds Visual Studio, compiles `Prism.exe` into `dist\` and copies `ui\` next to it.

After changing Prism's code or anything in `ui\`, run `scripts\build.cmd` again and restart Prism.

### Updating WebKit

```bat
powershell -ExecutionPolicy Bypass -File scripts\fetch-webkit.ps1 -Update
```

This switches to the newest WebKit build and updates `webkit.lock`. Test it, then commit `webkit.lock`.

## If the WebKit download fails

WebKit's build infrastructure changes from time to time. If the script can't find a build:

1. Go to <https://build.webkit.org/#/builders> and open **Windows-64-bit-Release-Build**.
2. Click a recent green (successful) build number.
3. Under the **compile-webkit** step, click the **Archive** link to download the zip.
4. Run:

   ```bat
   powershell -ExecutionPolicy Bypass -File scripts\fetch-webkit.ps1 -Archive C:\path\to\the-download.zip
   ```

## Where Prism keeps your data

Everything goes in `%LOCALAPPDATA%\Prism`:

| Path | Contents |
| --- | --- |
| `Profile\` | Cookies, cache, site storage. Delete it to clear all browsing data. |
| `prism.ini` | Settings. |

| `session.txt` | Your open tabs, for "Continue where you left off". |

Change settings at `prism://settings` (or press Ctrl+,).

## Troubleshooting

- **"Prism couldn't load the WebKit engine"**: run `fetch-webkit.ps1` and install the Visual C++ Redistributable.
- **"This WebKit build is missing functions Prism needs"**: the WebKit build doesn't match this version of Prism. Run `fetch-webkit.ps1` without `-Update` to get the pinned version.
- **The window opens but stays blank**: WebKit's helper programs (`WebKitWebProcess.exe`, `WebKitNetworkProcess.exe`) must be in `dist\` next to `Prism.exe`, and your antivirus must allow them.
