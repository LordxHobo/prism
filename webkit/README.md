# Prism's own WebKit build

The WebKit builds that `scripts\fetch-webkit.ps1` downloads from
build.webkit.org play media through Windows' Media Foundation, which only
handles plain video files. They have no Media Source Extensions (MSE), the
streaming API that YouTube and most other video sites use, so those sites
say the browser can't play the video.

Prism builds its own WebKit with GitHub Actions, using WebKit's GStreamer
media player (the one WebKit uses on Linux and on smart TVs, with full MSE
support) built for Windows.

| | |
|---|---|
| `BASE` | The WebKit commit that's built. |
| `patches/` | Prism's changes to WebKit, applied in order with `git apply`. |
| `ci.ps1` | The build steps (used by `.github/workflows/webkit.yml`). |

## Building it

1. On GitHub, open **Actions** › **WebKit** › **Run workflow**.
2. Wait. A full build takes 8 to 12 hours in stages of about 5 hours (a
   GitHub job may run for at most 6), each carrying on where the previous
   one stopped. Later runs start from what the last run built, so after a
   fix only the changed files compile again (usually well under an hour).
3. When it finishes, the build is published as a Release named
   `webkit-<commit>-r<run>`.

If a stage fails because WebKit didn't compile, the run's **Summary** page
lists the errors, and the `webkit-errors-stage<n>` artifact has them in full
(`errors.txt`, plus the whole build log).

To start over without the saved progress, tick **Start from scratch** when
starting the run.

## Using it

```
powershell -ExecutionPolicy Bypass -File scripts\fetch-webkit.ps1 -Prism
```

This downloads the newest Prism WebKit build into `dist\` and pins it in
`webkit.lock` (commit that). From then on, `fetch-webkit.ps1 -Update` gets
the newest Prism build; `-Stock` goes back to WebKit's own builds.

The build includes a `gstreamer\` folder: GStreamer's libraries and the
plugins WebKit uses (demuxers, the H.264, VP8/VP9, AV1, AAC, Opus and Vorbis
decoders, and Windows audio output). Prism points GStreamer at it when it
starts; nothing needs installing. GStreamer is LGPL-licensed; its licenses
are in `gstreamer\share\licenses`.
