# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

<#
.SYNOPSIS
    Downloads the WebKit engine that Prism runs on and puts it in dist\.

.DESCRIPTION
    WebKit's build servers (build.webkit.org) publish a ready-made Windows
    build of WebKit after every successful build. This script downloads one,
    so you never have to compile WebKit yourself.

    The exact WebKit version is pinned in webkit.lock at the top of the repo.
    Without -Update, the pinned version is downloaded. With -Update (or if
    there is no webkit.lock yet), the newest successful build is used and
    webkit.lock is rewritten. Commit webkit.lock afterwards so every build of
    Prism uses the same WebKit.

.PARAMETER Update
    Switch to the newest successful WebKit build and update webkit.lock.

.PARAMETER Archive
    Use a WebKit build zip you downloaded yourself instead (see docs\BUILDING.md).

.PARAMETER KeepSymbols
    Keep WebKit's debug symbol files (*.pdb). They're only needed to debug
    crashes inside WebKit and take up a lot of space.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\fetch-webkit.ps1
#>

[CmdletBinding()]
param(
    [switch]$Update,
    [string]$Archive,
    [switch]$KeepSymbols
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is very slow with its progress bar on

$Root = Split-Path -Parent $PSScriptRoot
$Dist = Join-Path $Root 'dist'
$Cache = Join-Path $Root '.cache'
$LockFile = Join-Path $Root 'webkit.lock'

$Builder = 'Windows-64-bit-Release-Build'
$BuildbotApi = 'https://build.webkit.org/api/v2'
$ArchiveBase = 'https://s3-us-west-2.amazonaws.com/archives.webkit.org/win-x86_64-release'

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }

function Get-ArchiveUrl([string]$Revision) {
    return "$ArchiveBase/$([uri]::EscapeDataString($Revision)).zip"
}

function Test-Url([string]$Url) {
    try {
        $response = Invoke-WebRequest -Uri $Url -Method Head -UseBasicParsing
        return $response.StatusCode -eq 200
    } catch {
        return $false
    }
}

# Asks build.webkit.org for recent successful Windows builds, newest first,
# and returns the first one whose archive can actually be downloaded.
function Find-LatestRevision {
    Write-Step "Looking up the newest WebKit build for Windows on build.webkit.org"
    $builders = Invoke-RestMethod -Uri "$BuildbotApi/builders?name=$Builder" -UseBasicParsing
    if (-not $builders.builders -or $builders.builders.Count -eq 0) {
        throw "build.webkit.org has no builder called '$Builder'. WebKit may have renamed it; see docs\BUILDING.md for the manual steps."
    }
    $builderId = $builders.builders[0].builderid

    $builds = Invoke-RestMethod -Uri "$BuildbotApi/builders/$builderId/builds?order=-number&limit=25&complete=true&property=archive_revision" -UseBasicParsing
    foreach ($build in $builds.builds) {
        # results: 0 = success, 1 = warnings.
        if ($build.results -gt 1) { continue }
        $property = $build.properties.archive_revision
        if (-not $property) { continue }
        $revision = [string]$property[0]
        if (-not $revision) { continue }
        $url = Get-ArchiveUrl $revision
        if (Test-Url $url) {
            Write-Host "    Found build $($build.number): WebKit $revision"
            return $revision
        }
    }
    throw "Couldn't find a downloadable WebKit build among the last 25 Windows builds. See docs\BUILDING.md for the manual steps."
}

function Read-Lock {
    if (-not (Test-Path $LockFile)) { return $null }
    return Get-Content $LockFile -Raw | ConvertFrom-Json
}

function Write-Lock([string]$Revision) {
    $lock = [ordered]@{
        revision = $Revision
        url = (Get-ArchiveUrl $Revision)
        builder = $Builder
        updated = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')
    }
    $json = $lock | ConvertTo-Json
    # Write UTF-8 without a byte order mark, with LF line endings.
    [System.IO.File]::WriteAllText($LockFile, ($json -replace "`r`n", "`n") + "`n", (New-Object System.Text.UTF8Encoding $false))
}

function Get-File([string]$Url, [string]$Destination) {
    $partial = "$Destination.partial"
    # curl.exe ships with Windows 10 and later, and shows real download progress.
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($curl) {
        & $curl.Source --location --fail --retry 3 --output $partial $Url
        if ($LASTEXITCODE -ne 0) { throw "Download failed (curl exit code $LASTEXITCODE): $Url" }
    } else {
        Invoke-WebRequest -Uri $Url -OutFile $partial -UseBasicParsing
    }
    Move-Item -Force $partial $Destination
}

# --- Pick the WebKit build ---------------------------------------------------

New-Item -ItemType Directory -Force -Path $Cache, $Dist | Out-Null

if ($Archive) {
    if (-not (Test-Path $Archive)) { throw "Archive not found: $Archive" }
    $zip = (Resolve-Path $Archive).Path
    $revision = [IO.Path]::GetFileNameWithoutExtension($zip)
    Write-Step "Using your WebKit archive: $zip"
} else {
    $lock = Read-Lock
    if ($Update -or -not $lock) {
        $revision = Find-LatestRevision
        Write-Lock $revision
        Write-Host "    Pinned WebKit $revision in webkit.lock (commit this file)."
    } else {
        $revision = $lock.revision
        Write-Step "Using WebKit $revision (pinned in webkit.lock)"
    }

    $zip = Join-Path $Cache ("webkit-" + ($revision -replace '[^\w.-]', '_') + ".zip")
    if (Test-Path $zip) {
        Write-Host "    Already downloaded."
    } else {
        $url = Get-ArchiveUrl $revision
        Write-Step "Downloading WebKit (a few hundred MB)"
        Write-Host "    $url"
        Get-File $url $zip
    }
}

# --- Unpack -------------------------------------------------------------------

$unpacked = Join-Path $Cache 'webkit-unpacked'
if (Test-Path $unpacked) { Remove-Item -Recurse -Force $unpacked }
Write-Step "Unpacking"
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $unpacked)

# The archive holds WebKit's "bin" folder, but don't rely on the exact layout:
# find the folder that contains WebKit2.dll.
$webkitDll = Get-ChildItem -Path $unpacked -Recurse -Filter 'WebKit2.dll' | Select-Object -First 1
if (-not $webkitDll) {
    throw "This archive doesn't contain WebKit2.dll, so it isn't a WebKit for Windows build."
}
$binDir = $webkitDll.DirectoryName

# Remove the WebKit runtime from a previous run (keep Prism.exe and ui\).
Write-Step "Installing the WebKit runtime into dist\"
$keep = @('Prism.exe', 'Prism.pdb', 'ui')
Get-ChildItem -Path $Dist | Where-Object { $keep -notcontains $_.Name } | Remove-Item -Recurse -Force

$exclude = @('*.ilk', '*.lib', '*.exp')
if (-not $KeepSymbols) { $exclude += '*.pdb' }
# WebKit's demo browser and test tools aren't part of Prism.
$exclude += @('MiniBrowser*', 'WebKitTestRunner*', 'DumpRenderTree*', 'TestWebKitAPI*', 'TestWTF*', 'TestWebCore*', 'ImageDiff*', 'jsc.exe', 'testapi*', 'testb3*', 'testair*', 'testmasm*', 'testdfg*', 'testRegExp*')

Get-ChildItem -Path $binDir -Force | ForEach-Object {
    $item = $_
    foreach ($pattern in $exclude) {
        if ($item.Name -like $pattern) { return }
    }
    Copy-Item -Recurse -Force -Path $item.FullName -Destination $Dist
}
Remove-Item -Recurse -Force $unpacked

# --- Checks -------------------------------------------------------------------

$missing = @('WebKit2.dll', 'WebKitWebProcess.exe', 'WebKitNetworkProcess.exe') | Where-Object { -not (Test-Path (Join-Path $Dist $_)) }
if ($missing) {
    Write-Warning "These WebKit files are missing from dist\: $($missing -join ', ')"
}

# WebKit is built with Microsoft's C++ compiler and needs its runtime.
$system32 = [Environment]::GetFolderPath('System')
if ($system32 -and -not (Test-Path (Join-Path $system32 'vcruntime140_1.dll')) -and -not (Test-Path (Join-Path $Dist 'vcruntime140_1.dll'))) {
    Write-Warning "The Microsoft Visual C++ Redistributable (x64) doesn't seem to be installed. Get it from https://aka.ms/vs/17/release/vc_redist.x64.exe"
}

$size = (Get-ChildItem -Path $Dist -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB
Write-Step ("Done: WebKit $revision is in dist\ ({0:N0} MB)" -f $size)
if (-not (Test-Path (Join-Path $Dist 'Prism.exe'))) {
    Write-Host "    Next: build Prism with scripts\build.cmd, then run dist\Prism.exe"
} else {
    Write-Host "    Run dist\Prism.exe"
}
