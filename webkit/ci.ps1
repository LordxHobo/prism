# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

<#
.SYNOPSIS
    The steps of Prism's WebKit build on GitHub Actions (see
    .github/workflows/webkit.yml and webkit/README.md).

.DESCRIPTION
    Tools      Build tools (LLVM, gperf, ninja, vcpkg) and GStreamer's MSVC
               development files.
    Source     The WebKit revision in webkit/BASE, with webkit/patches applied.
    Restore    The build progress an earlier stage or run saved.
    Configure  CMake (vcpkg builds WebKit's libraries the first time).
    Build      ninja, until it finishes or the stage runs out of time.
    Archive    Saves the build progress for the next stage or run.
    Prune      Removes older saved progress.
    Package    The runtime zip (WebKit + the GStreamer parts it uses),
               published as a Release.

    Only meant to run on GitHub's Windows runners.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Tools', 'Source', 'Restore', 'Configure', 'Build', 'Archive', 'Prune', 'Package')]
    [string]$Step
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Prism = Split-Path -Parent $PSScriptRoot
$Src = 'C:\wk'
$BuildRoot = 'C:\wk\WebKitBuild'
$Build = 'C:\wk\WebKitBuild\Release'
$Meta = 'C:\wk-meta'
$StateDir = 'C:\wk-state'
$LogDir = 'C:\wk-logs'
$GstRoot = 'C:\gstreamer'
$GstVersion = '1.28.9'
$VcpkgRoot = 'C:\vcpkg-wk'
$VcpkgCache = 'C:\vcpkg-cache'
$OldTime = '2001-01-01T00:00:00'
# Changes when saved progress from older versions of this script can't be used.
$StateFormat = 'links-1'

$Base = (Get-Content (Join-Path $PSScriptRoot 'BASE') -Raw).Trim()
$StatePrefix = 'webkit-state-' + $Base.Substring(0, 12)
$Stage = [int]$env:PRISM_STAGE
$Repo = $env:GITHUB_REPOSITORY

# Stop building this long after the job started, leaving time to save the
# progress (GitHub stops a job after 6 hours; the workflow allows 355 min).
$BuildBudgetMinutes = 305

$CMakeArgs = @(
    '-DUSE_GSTREAMER=ON'
    '-DENABLE_MEDIA_SOURCE=ON'
    "-DGSTREAMER_ROOT=$($GstRoot -replace '\\', '/')"
    '-DWEBKIT_MSVC_LINE_TABLES_ONLY=ON'
    '-DENABLE_API_TESTS=OFF'
    '-DENABLE_LAYOUT_TESTS=OFF'
    '-DENABLE_MINIBROWSER=OFF'
    '-DDEVELOPER_MODE=OFF'
)

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }

function Add-Env([string]$Name, [string]$Value) {
    Set-Item -Path "env:$Name" -Value $Value
    "$Name=$Value" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
}

function Add-Path([string]$Dir) {
    $env:PATH = "$Dir;$env:PATH"
    $Dir | Out-File -FilePath $env:GITHUB_PATH -Append -Encoding utf8
}

function Add-Output([string]$Name, [string]$Value) {
    "$Name=$Value" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
}

function Add-Summary([string]$Text) {
    if ($env:GITHUB_STEP_SUMMARY) { $Text | Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Append -Encoding utf8 }
}

function Invoke-Checked([string]$File, [string[]]$Arguments) {
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$File failed (exit code $LASTEXITCODE)" }
}

function Get-File([string]$Url, [string]$Destination) {
    & curl.exe --location --fail --retry 3 --silent --show-error --output $Destination $Url
    if ($LASTEXITCODE -ne 0) { throw "Download failed: $Url" }
}

# Strawberry Perl's MinGW folder has its own zlib, libxml2, pkg-config...
# CMake must not find those.
function Remove-MinGWFromPath {
    $env:PATH = ($env:PATH -split ';' | Where-Object { $_ -and $_ -notmatch '\\Strawberry\\c\\' }) -join ';'
}

function Get-SavedStates {
    $all = @()
    $page = 1
    while ($page -le 5) {
        $json = gh api "repos/$Repo/actions/artifacts?per_page=100&page=$page" | ConvertFrom-Json
        if (-not $json.artifacts) { break }
        $all += $json.artifacts | Where-Object { $_.name.StartsWith($StatePrefix) -and -not $_.expired }
        if ($json.artifacts.Count -lt 100) { break }
        $page++
    }
    return $all | Sort-Object -Property created_at -Descending
}

function Get-FileHashes([string[]]$Paths) {
    $map = @{}
    foreach ($path in $Paths) {
        $full = Join-Path $Src $path
        if (Test-Path -LiteralPath $full -PathType Leaf) { $map[$path] = (Get-FileHash -Algorithm SHA256 -LiteralPath $full).Hash }
    }
    return $map
}

switch ($Step) {

'Tools' {
    git config --global core.longpaths true

    Write-Step 'gperf and ninja'
    Invoke-Checked choco @('install', 'gperf', 'ninja', '-y', '--no-progress')

    Write-Step 'LLVM (clang-cl)'
    Invoke-Checked choco @('upgrade', 'llvm', '-y', '--no-progress')
    Add-Path 'C:\Program Files\LLVM\bin'
    & clang-cl --version

    Write-Step 'vcpkg'
    if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
        Invoke-Checked git @('clone', '--quiet', 'https://github.com/microsoft/vcpkg.git', $VcpkgRoot)
        Invoke-Checked "$VcpkgRoot\bootstrap-vcpkg.bat" @('-disableMetrics')
    }
    New-Item -ItemType Directory -Force -Path $VcpkgCache | Out-Null
    Add-Env 'VCPKG_ROOT' $VcpkgRoot
    Add-Env 'VCPKG_DEFAULT_BINARY_CACHE' $VcpkgCache
    Add-Env 'VCPKG_DISABLE_METRICS' '1'

    Write-Step "GStreamer $GstVersion (MSVC x86_64, runtime and development files)"
    $installer = "$env:RUNNER_TEMP\gstreamer-$GstVersion.exe"
    $url = "https://gstreamer.freedesktop.org/data/pkg/windows/$GstVersion/msvc/gstreamer-1.0-msvc-x86_64-$GstVersion.exe"
    Get-File $url $installer
    Get-File "$url.sha256sum" "$installer.sha256sum"
    $expected = ((Get-Content "$installer.sha256sum" -Raw).Trim() -split '\s+')[0].ToUpperInvariant()
    $actual = (Get-FileHash -Algorithm SHA256 $installer).Hash
    if ($expected -ne $actual) { throw "GStreamer installer checksum mismatch: expected $expected, got $actual" }
    Write-Host "    SHA-256 $actual"
    $process = Start-Process -FilePath $installer -Wait -PassThru -ArgumentList @(
        '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/ALLUSERS', '/TYPE=devel', "/DIR=$GstRoot")
    if ($process.ExitCode -ne 0) { throw "The GStreamer installer failed (exit code $($process.ExitCode))" }
    if (-not (Test-Path "$GstRoot\include\gstreamer-1.0\gst\gst.h")) {
        Get-ChildItem $GstRoot -ErrorAction SilentlyContinue | Format-Table -AutoSize | Out-String | Write-Host
        throw "GStreamer's development files aren't in $GstRoot"
    }
    Add-Env 'GSTREAMER_ROOT' $GstRoot
    Add-Env 'WEBKIT_IGNORE_PATH' 'C:/Strawberry/c/bin;C:/Strawberry/c/lib;C:/Strawberry/c/include'
}

'Source' {
    Write-Step "WebKit $Base"
    New-Item -ItemType Directory -Force -Path $Src, $Meta | Out-Null
    Push-Location $Src
    try {
        Invoke-Checked git @('init', '--quiet')
        Invoke-Checked git @('config', 'core.longpaths', 'true')
        Invoke-Checked git @('config', 'core.autocrlf', 'false')
        Invoke-Checked git @('remote', 'add', 'origin', 'https://github.com/WebKit/WebKit.git')
        # The tests and websites are most of WebKit's repository; the build
        # doesn't need them.
        Invoke-Checked git @('sparse-checkout', 'init', '--no-cone')
        @('/*', '!/LayoutTests/', '!/JSTests/', '!/PerformanceTests/', '!/Websites/', '!/ManualTests/', '!/WebDriverTests/') |
            Set-Content -Path '.git\info\sparse-checkout' -Encoding ascii
        Invoke-Checked git @('fetch', '--quiet', '--depth', '1', '--filter=blob:none', 'origin', $Base)
        Invoke-Checked git @('checkout', '--quiet', 'FETCH_HEAD')

        # Give every file the same old time, so a build saved by an earlier
        # run only recompiles what the patches actually change.
        Write-Step 'Normalizing file times'
        & 'C:\Program Files\Git\usr\bin\bash.exe' -lc "cd /c/wk && git ls-files -z | xargs -0 touch -c -d '$OldTime'"
        if ($LASTEXITCODE -ne 0) { throw 'touch failed' }

        Write-Step 'Applying Prism''s patches'
        $patches = Get-ChildItem (Join-Path $PSScriptRoot 'patches') -Filter '*.patch' | Sort-Object Name
        foreach ($patch in $patches) {
            Write-Host "    $($patch.Name)"
            Invoke-Checked git @('apply', '--whitespace=nowarn', $patch.FullName)
        }
        $changed = @(git diff --name-only) + @(git ls-files --others --exclude-standard)
        $hashes = Get-FileHashes $changed
        $hashes.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)`t$($_.Value)" } |
            Set-Content -Path "$Meta\patched.txt" -Encoding utf8
        Write-Host "    $($hashes.Count) files changed by the patches"
    } finally {
        Pop-Location
    }
}

'Restore' {
    if ($Stage -eq 1 -and $env:PRISM_FRESH -eq 'true') {
        Write-Step 'Starting from scratch (fresh build requested)'
        return
    }
    $states = @(Get-SavedStates)
    if ($states.Count -eq 0) {
        Write-Step 'No saved build progress for this WebKit revision: building from scratch'
        return
    }
    $state = $states[0]
    Write-Step "Restoring $($state.name) ($([math]::Round($state.size_in_bytes / 1GB, 1)) GB, saved $($state.created_at))"
    $zip = "$env:RUNNER_TEMP\state.zip"
    Invoke-Checked curl.exe @('--location', '--fail', '--retry', '3', '--silent', '--show-error',
        '-H', "Authorization: Bearer $env:GH_TOKEN", '-H', 'Accept: application/vnd.github+json',
        '--output', $zip, "https://api.github.com/repos/$Repo/actions/artifacts/$($state.id)/zip")
    $unpacked = "$env:RUNNER_TEMP\state"
    Invoke-Checked 7z @('x', '-y', '-bso0', '-bsp0', "-o$unpacked", $zip)
    Remove-Item $zip
    # WebKit's PrivateHeaders are symbolic links to the source headers.
    # Progress saved before links were kept as links has them as copies,
    # and a header then gets included twice ("redefinition of ..."), so
    # that progress can't be used.
    if ((Get-Content "$unpacked\format.txt" -ErrorAction SilentlyContinue) -ne $StateFormat) {
        Write-Step 'That progress was saved by an older version of this script: building from scratch'
        Remove-Item -Recurse -Force $unpacked
        return
    }
    New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
    Invoke-Checked 7z @('x', '-y', '-snl', '-bso0', '-bsp0', "-o$BuildRoot", "$unpacked\build.7z")

    # CMake makes Skia's header folders as symbolic links when it configures,
    # and can't replace them if they came back as real folders (archives
    # from before links were stored as links). Configuring makes them again.
    if (Test-Path "$Build\Skia\Headers") {
        & cmd.exe /c rmdir /s /q "$Build\Skia\Headers"
    }

    # Files the patches change but that are the same as in the saved build
    # get the old time back, so they aren't compiled again.
    if (Test-Path "$unpacked\patched.txt") {
        $previous = @{}
        foreach ($line in Get-Content "$unpacked\patched.txt") {
            $parts = $line -split "`t"
            if ($parts.Count -eq 2) { $previous[$parts[0]] = $parts[1] }
        }
        $same = 0
        foreach ($line in Get-Content "$Meta\patched.txt") {
            $parts = $line -split "`t"
            if ($parts.Count -eq 2 -and $previous[$parts[0]] -eq $parts[1]) {
                (Get-Item -LiteralPath (Join-Path $Src $parts[0])).LastWriteTimeUtc = [datetime]::Parse($OldTime)
                $same++
            }
        }
        Write-Host "    $same patched files are unchanged since that build"
    }
    Remove-Item -Recurse -Force $unpacked
}

'Configure' {
    Remove-MinGWFromPath
    $env:CC = 'clang-cl'
    $env:CXX = 'clang-cl'
    Push-Location $Src
    try {
        Write-Step 'Generating the build files (the first run also builds WebKit''s libraries with vcpkg, which takes a while)'
        $cmakeArgs = '--cmakeargs=' + ($CMakeArgs -join ' ')
        Invoke-Checked perl @('Tools\Scripts\build-webkit', '--win', '--release', '--generate-project-only', $cmakeArgs)
    } finally {
        Pop-Location
    }
    if (-not (Test-Path "$Build\build.ninja")) { throw "No build.ninja in $Build" }
    Add-Summary "Configured WebKit $Base with: ``$($CMakeArgs -join ' ')``"
}

'Build' {
    Remove-MinGWFromPath
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    $log = "$LogDir\ninja-stage$Stage.log"
    $start = [DateTimeOffset]::FromUnixTimeSeconds([long]$env:PRISM_JOB_START)
    $deadline = $start.AddMinutes($BuildBudgetMinutes)
    $seconds = [int]($deadline - [DateTimeOffset]::UtcNow).TotalSeconds
    Add-Output 'state' "$StatePrefix-run$($env:GITHUB_RUN_ID)-stage$Stage"
    if ($seconds -lt 600) {
        Write-Step 'Too little time left in this stage to build; saving and continuing in the next stage'
        Add-Output 'more' 'true'
        return
    }

    Write-Step "Building with ninja for up to $([int]($seconds / 60)) minutes"
    # -k 0: keep going after errors, so one run reports all of them.
    $ninja = Start-Process -FilePath ninja -ArgumentList @('-C', $Build, '-k', '0') -NoNewWindow -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $handle = $ninja.Handle # keeps the exit code available
    $finished = $false
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        if ($ninja.WaitForExit(600000)) { $finished = $true; break }
        $last = Get-Content $log -Tail 1 -ErrorAction SilentlyContinue
        Write-Host "    $([DateTime]::UtcNow.ToString('HH:mm')) $last"
    }
    if (-not $finished) {
        if ($ninja.WaitForExit([math]::Max(0, [int]($deadline - [DateTimeOffset]::UtcNow).TotalMilliseconds))) { $finished = $true }
    }
    if (-not $finished) {
        Write-Step 'Out of time for this stage: stopping ninja'
        & taskkill.exe /PID $ninja.Id /T /F | Out-Null
        Start-Sleep -Seconds 10
    }
    Get-Content "$log.err" -ErrorAction SilentlyContinue | Add-Content $log

    # Collect what failed, with the compiler's messages.
    # ninja prints "FAILED: <output>", then the command and the compiler's
    # messages, until the next "[n/total]" progress line. Keep the errors
    # (with the lines after each, which show the code and any notes) and
    # leave out the warnings, which can be thousands of lines.
    $errors = New-Object System.Collections.Generic.List[string]
    $messages = @{}
    $inBlock = $false
    $keep = 0
    $blockLines = 0
    foreach ($line in [IO.File]::ReadLines($log)) {
        if ($line.StartsWith('FAILED: ')) {
            if ($errors.Count) { $errors.Add('') }
            $errors.Add($line)
            $inBlock = $true
            $keep = 0
            $blockLines = 0
            continue
        }
        if (-not $inBlock) { continue }
        if ($line -match '^\[\d+/\d+\] ') { $inBlock = $false; continue }
        $isError = $line -match '(: (fatal )?error|error LNK\d+|^error: |: undefined symbol|lld-link: error)'
        if ($isError) {
            $keep = 6
            $message = ($line -replace '^.*?: (fatal )?error: ', '') -replace '\s+', ' '
            if ($message.Length -gt 220) { $message = $message.Substring(0, 220) }
            $messages[$message] = 1 + [int]$messages[$message]
        } elseif ($line -match ': warning: ') {
            $keep = 0
        }
        if ($keep -gt 0 -and $blockLines -lt 80) {
            $errors.Add($line)
            $blockLines++
            $keep--
        }
        if ($errors.Count -ge 8000) { break }
    }
    if ($messages.Count) {
        $distinct = $messages.GetEnumerator() | Sort-Object -Property Value -Descending | ForEach-Object { '{0,5}x  {1}' -f $_.Value, $_.Key }
        $errors.InsertRange(0, [string[]](@('Distinct errors (count, message):') + $distinct + @('', '----', '')))
    }
    $hasErrors = $errors.Count -gt 0
    if ($hasErrors) {
        $errors | Set-Content -Path "$LogDir\errors.txt" -Encoding utf8
        $failed = @(Select-String -Path $log -Pattern '^FAILED: ').Count
        Add-Summary "### Stage $Stage`: $failed build step(s) failed"
        Add-Summary '```'
        Add-Summary (($errors | Select-Object -First 400) -join "`n")
        Add-Summary '```'
        Add-Output 'errors' 'true'
    }

    $progress = (Select-String -Path $log -Pattern '^\[\d+/\d+\]' | Select-Object -Last 1).Line
    Write-Host "    Last step: $progress"
    if ($finished -and $ninja.ExitCode -eq 0) {
        Write-Step 'WebKit is built'
        Add-Summary "### Stage $Stage`: WebKit built"
        Add-Output 'done' 'true'
    } elseif (-not $finished -and -not $hasErrors) {
        Add-Summary "### Stage $Stage`: out of time at $progress, the next stage continues"
        Add-Output 'more' 'true'
    } elseif (-not $hasErrors) {
        # ninja stopped without a recognizable error (e.g. it crashed).
        Get-Content $log -Tail 60 | Set-Content -Path "$LogDir\errors.txt" -Encoding utf8
        Add-Summary "### Stage $Stage`: ninja stopped with exit code $($ninja.ExitCode)"
        Add-Output 'errors' 'true'
    }
}

'Archive' {
    Write-Step 'Saving the build progress'
    New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
    # 7-Zip keeps the file times, which ninja relies on.
    # -snl: store symbolic links (Skia's header folders) as links, not as
    # copies of what they point to.
    Invoke-Checked 7z @('a', '-t7z', '-mx=1', '-mmt=on', '-mtm=on', '-snl', '-bso0', '-bsp0', "$StateDir\build.7z", $Build)
    Copy-Item "$Meta\patched.txt" "$StateDir\patched.txt"
    Set-Content -Path "$StateDir\format.txt" -Value $StateFormat -Encoding ascii
    $size = (Get-Item "$StateDir\build.7z").Length / 1GB
    Write-Host ("    {0:N1} GB" -f $size)
}

'Prune' {
    $current = "$StatePrefix-run$($env:GITHUB_RUN_ID)-stage$Stage"
    # Everything saved before this stage, for any WebKit revision.
    $page = 1
    while ($page -le 5) {
        $json = gh api "repos/$Repo/actions/artifacts?per_page=100&page=$page" | ConvertFrom-Json
        if (-not $json.artifacts) { break }
        foreach ($artifact in $json.artifacts) {
            if ($artifact.name.StartsWith('webkit-state-') -and $artifact.name -ne $current -and -not $artifact.expired) {
                Write-Host "    Removing $($artifact.name)"
                gh api -X DELETE "repos/$Repo/actions/artifacts/$($artifact.id)" | Out-Null
            }
        }
        if ($json.artifacts.Count -lt 100) { break }
        $page++
    }
}

'Package' {
    $bin = "$Build\bin"
    $out = "$env:RUNNER_TEMP\package"
    $runtime = "$out\bin"
    New-Item -ItemType Directory -Force -Path $runtime | Out-Null

    Write-Step 'WebKit runtime'
    $skip = @('MiniBrowser*', 'WebKitTestRunner*', 'DumpRenderTree*', 'TestWebKitAPI*', 'TestWTF*', 'TestWebCore*', 'TestJavaScriptCore*',
        'ImageDiff*', 'jsc.exe', 'testapi*', 'testb3*', 'testair*', 'testmasm*', 'testdfg*', 'testRegExp*', '*.ilk', '*.exp', '*.lib', '*.pdb')
    Get-ChildItem $bin | Where-Object {
        $name = $_.Name
        -not ($skip | Where-Object { $name -like $_ })
    } | ForEach-Object { Copy-Item -Recurse -Force $_.FullName $runtime }
    foreach ($required in @('WebKit2.dll', 'WebKitWebProcess.exe', 'WebKitNetworkProcess.exe')) {
        if (-not (Test-Path "$runtime\$required")) { throw "$required wasn't built" }
    }

    Write-Step 'GStreamer runtime (only what WebKit uses)'
    $gstOut = "$runtime\gstreamer"
    New-Item -ItemType Directory -Force -Path "$gstOut\bin", "$gstOut\lib\gstreamer-1.0", "$gstOut\libexec\gstreamer-1.0" | Out-Null
    # Elements WebKit's player and its Media Source code build pipelines
    # from, plus decoders for the formats sites stream (H.264, VP8/9, AV1,
    # AAC, Opus, Vorbis, MP3) and Windows audio output.
    $plugins = @(
        'gstcoreelements', 'gstcoretracers', 'gstapp', 'gstplayback', 'gsttypefindfunctions', 'gstaudioconvert', 'gstaudioresample',
        'gstaudiorate', 'gstvideoconvertscale', 'gstvideorate', 'gstvolume', 'gstautodetect', 'gstaudiofx', 'gstaudioparsers',
        'gstvideoparsersbad', 'gstisomp4', 'gstmatroska', 'gstogg', 'gstid3demux', 'gstopus', 'gstopusparse', 'gstvorbis', 'gstvpx',
        'gstdav1d', 'gstlibav', 'gstd3d11', 'gstd3d12', 'gstmediafoundation', 'gstwasapi', 'gstwasapi2', 'gstdirectsound',
        'gstdeinterlace', 'gstvideofilter', 'gstsubparse', 'gstrawparse', 'gstwavparse', 'gstflac', 'gstmpg123', 'gstlame')
    $missing = @()
    foreach ($plugin in $plugins) {
        $file = "$GstRoot\lib\gstreamer-1.0\$plugin.dll"
        if (Test-Path $file) { Copy-Item $file "$gstOut\lib\gstreamer-1.0\" } else { $missing += $plugin }
    }
    if ($missing) { Write-Warning "Plugins not in this GStreamer: $($missing -join ', ')" }
    Copy-Item "$GstRoot\libexec\gstreamer-1.0\gst-plugin-scanner.exe" "$gstOut\libexec\gstreamer-1.0\"

    # The DLLs from GStreamer's bin folder that the plugins, the scanner and
    # WebKit itself load, found by following their imports.
    $available = @{}
    Get-ChildItem "$GstRoot\bin" -Filter '*.dll' | ForEach-Object { $available[$_.Name.ToLowerInvariant()] = $_.FullName }
    $queue = New-Object System.Collections.Generic.Queue[string]
    Get-ChildItem "$gstOut\lib\gstreamer-1.0\*.dll", "$gstOut\libexec\gstreamer-1.0\*.exe", "$runtime\WebKit2.dll", "$runtime\WebKitWebProcess.exe" |
        ForEach-Object { $queue.Enqueue($_.FullName) }
    $copied = @{}
    while ($queue.Count -gt 0) {
        $file = $queue.Dequeue()
        $imports = & dumpbin.exe /nologo /dependents $file | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '\.dll$' }
        foreach ($import in $imports) {
            $key = $import.ToLowerInvariant()
            if ($available.ContainsKey($key) -and -not $copied.ContainsKey($key)) {
                $copied[$key] = $true
                Copy-Item $available[$key] "$gstOut\bin\"
                $queue.Enqueue($available[$key])
            }
        }
    }
    Write-Host "    $(@(Get-ChildItem "$gstOut\lib\gstreamer-1.0").Count) plugins, $($copied.Count) libraries"
    if (Test-Path "$GstRoot\share\licenses") {
        New-Item -ItemType Directory -Force -Path "$gstOut\share" | Out-Null
        Copy-Item -Recurse -Force "$GstRoot\share\licenses" "$gstOut\share\licenses"
    }

    $short = $Base.Substring(0, 12)
    $tag = "webkit-$short-r$($env:GITHUB_RUN_NUMBER)"
    [ordered]@{
        webkit = $Base
        gstreamer = $GstVersion
        patches = @(Get-ChildItem (Join-Path $PSScriptRoot 'patches') -Filter '*.patch' | Sort-Object Name | ForEach-Object { $_.Name })
        run = "$env:GITHUB_SERVER_URL/$Repo/actions/runs/$env:GITHUB_RUN_ID"
        built = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')
    } | ConvertTo-Json | Set-Content -Path "$runtime\prism-webkit.json" -Encoding utf8

    $zip = "$env:RUNNER_TEMP\webkit-win64.zip"
    Invoke-Checked 7z @('a', '-tzip', '-mx=7', '-mmt=on', '-bso0', '-bsp0', $zip, "$out\bin")
    $size = (Get-Item $zip).Length / 1MB

    Write-Step "Publishing Release $tag ($([int]$size) MB)"
    $notes = @"
Prism's own WebKit build for Windows: WebKit $Base with Prism's patches (webkit/patches), media through GStreamer $GstVersion.

Install it with ``scripts\fetch-webkit.ps1 -Prism`` (then commit webkit.lock).
"@
    Invoke-Checked gh @('release', 'create', $tag, $zip, '--repo', $Repo, '--title', "Prism WebKit $short (run $env:GITHUB_RUN_NUMBER)",
        '--notes', $notes, '--latest=false', '--target', $env:GITHUB_SHA)
    Add-Summary "### Published Release ``$tag`` ($([int]$size) MB)"
}

}
