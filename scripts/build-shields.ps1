# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

<#
.SYNOPSIS
    Builds Prism Shields into dist\: prism_shields.dll (Brave's adblock-rust
    engine, from shields\) and shields\scriptlets.json (uBlock Origin's
    scriptlets).

.DESCRIPTION
    Needs Rust (https://rustup.rs) for the DLL, and Node.js plus Git for the
    scriptlets. Whatever is missing is skipped with a warning: without the DLL
    Prism runs with Shields turned off; without the scriptlets Shields still
    blocks and hides ads, but can't fix sites like YouTube.

    uBlock Origin's scriptlets are GPLv3. They're shipped as a separate data
    file with their license next to them; Prism itself stays MPL-2.0.
#>
param(
    [switch]$SkipEngine,
    [switch]$SkipScriptlets
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Dist = Join-Path $Root 'dist'
$Cache = Join-Path $Root '.cache'
# The uBlock Origin release the scriptlets come from.
$UBlockTag = '1.76.4'

function Write-Step($text) { Write-Host "==> $text" -ForegroundColor Cyan }

New-Item -ItemType Directory -Force -Path $Dist, $Cache | Out-Null

# --- prism_shields.dll ----------------------------------------------------------
if (-not $SkipEngine) {
    if (Get-Command cargo -ErrorAction SilentlyContinue) {
        Write-Step 'Building the Shields engine (Rust; the first build takes a few minutes)'
        $manifest = Join-Path $Root 'shields\Cargo.toml'
        & cargo build --release --locked --manifest-path $manifest --target x86_64-pc-windows-msvc
        if ($LASTEXITCODE -ne 0) { throw 'Building prism_shields.dll failed.' }
        Copy-Item (Join-Path $Root 'shields\target\x86_64-pc-windows-msvc\release\prism_shields.dll') $Dist -Force
    } else {
        Write-Warning 'Rust (cargo) isn''t installed, so prism_shields.dll wasn''t built and Prism Shields will be off. Get Rust from https://rustup.rs and run this again.'
    }
}

# --- shields\scriptlets.json ------------------------------------------------------
if (-not $SkipScriptlets) {
    $node = Get-Command node -ErrorAction SilentlyContinue
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($node -and $git) {
        $ublock = Join-Path $Cache "uBlock-$UBlockTag"
        # Earlier builds only fetched src/js; the stand-ins need src/web_accessible_resources too.
        if (-not (Test-Path (Join-Path $ublock 'src\js\resources\scriptlets.js')) -or -not (Test-Path (Join-Path $ublock 'src\web_accessible_resources\noop.js'))) {
            Write-Step "Downloading uBlock Origin $UBlockTag's scriptlets and stand-ins"
            if (Test-Path $ublock) { Remove-Item -Recurse -Force $ublock }
            & git -c advice.detachedHead=false clone --quiet --depth 1 --branch $UBlockTag --filter=blob:none --sparse https://github.com/gorhill/uBlock.git $ublock
            if ($LASTEXITCODE -ne 0) { throw 'Downloading uBlock Origin failed.' }
            & git -C $ublock sparse-checkout set src/js src/web_accessible_resources
            if ($LASTEXITCODE -ne 0) { throw 'Downloading uBlock Origin failed.' }
        }

        Write-Step 'Bundling the scriptlets'
        $out = Join-Path $Dist 'shields'
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        & node (Join-Path $Root 'shields\build-scriptlets.mjs') (Join-Path $ublock 'src\js\resources') (Join-Path $out 'scriptlets.json') (Join-Path $ublock 'src')
        if ($LASTEXITCODE -ne 0) { throw 'Bundling the scriptlets failed.' }
        Copy-Item (Join-Path $ublock 'LICENSE.txt') (Join-Path $out 'LICENSE-uBlock-Origin.txt') -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path (Join-Path $out 'LICENSE-uBlock-Origin.txt'))) {
            # The sparse checkout only has src/js; fetch the license on its own.
            & git -C $ublock checkout HEAD -- LICENSE.txt 2>$null
            Copy-Item (Join-Path $ublock 'LICENSE.txt') (Join-Path $out 'LICENSE-uBlock-Origin.txt') -Force -ErrorAction SilentlyContinue
        }
        Set-Content -Path (Join-Path $out 'README.txt') -Encoding UTF8 -Value @"
scriptlets.json holds uBlock Origin's scriptlets and its stand-ins for blocked
files (from uBlock Origin $UBlockTag, https://github.com/gorhill/uBlock),
packed for Prism Shields.

They are Copyright (C) Raymond Hill and contributors and licensed under the
GNU General Public License v3 (see LICENSE-uBlock-Origin.txt). The source is at
https://github.com/gorhill/uBlock/tree/$UBlockTag/src/js/resources and
src/web_accessible_resources, and the
packing script at shields/build-scriptlets.mjs in Prism's repository.

Prism itself is licensed under the Mozilla Public License 2.0; this data file is
distributed alongside it, not as part of it.
"@
    } else {
        Write-Warning 'Node.js or Git isn''t installed, so uBlock Origin''s scriptlets weren''t bundled. Shields still blocks ads, but can''t fix sites like YouTube. Get Node.js from https://nodejs.org and run this again.'
    }
}

Write-Step 'Prism Shields is in dist\'
