<#
.SYNOPSIS
    Assemble a distributable YGO Nova release bundle (zip).

.DESCRIPTION
    Stages the built executable, game assets, the online relay tools, and the
    user docs into dist\YGONova-v<Version>\, then compresses that to
    dist\YGONova-v<Version>-win64.zip.

    The build is statically linked (vcpkg x64-windows-static-md), so no third-
    party DLLs are bundled. End users need the Microsoft Visual C++ 2015-2022
    Redistributable (x64) — noted in the README.

.PARAMETER Version
    Release version string. Default 1.0.0 (keep in sync with CMake project()).

.PARAMETER Config
    Build configuration to package. Default Release.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -Version 1.0.0
#>
param(
    [string]$Version = "1.0.4",
    [string]$Config  = "Release",
    # Card images (~300 MB, Konami IP) are NOT bundled by default — the app
    # downloads them on demand on first view. Pass this to ship them anyway.
    [switch]$IncludeCardImages,
    # Stage the bundle but skip the .zip (used by the installer build, which
    # only needs the staging directory).
    [switch]$SkipZip
)

$ErrorActionPreference = "Stop"

# Repo root = parent of this script's folder.
$root    = Split-Path -Parent $PSScriptRoot
$exePath = Join-Path $root "build\windows\$Config\YGONova.exe"

if (-not (Test-Path $exePath)) {
    Write-Error "Executable not found: $exePath`nBuild it first (build.bat or cmake --build build/windows --config $Config)."
}

$stageName = "YGONova-v$Version"
$distDir   = Join-Path $root "dist"
$stageDir  = Join-Path $distDir $stageName
$zipPath   = Join-Path $distDir "$stageName-win64.zip"

Write-Host "Packaging $stageName ($Config)..." -ForegroundColor Cyan

# Clean any previous staging for this version.
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
if (Test-Path $zipPath)  { Remove-Item $zipPath  -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

# 1) Executable (+ any DLLs that happen to sit next to it, for shared builds).
Copy-Item $exePath (Join-Path $stageDir "YGONova.exe") -Force
$exeDir = Split-Path -Parent $exePath
Get-ChildItem -Path $exeDir -Filter *.dll -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName $stageDir -Force }

# 1b) Bundle the VC++ 2015-2022 runtime DLLs app-local (next to the exe) so the
#     app starts on machines WITHOUT the redistributable installed. This is the
#     #1 cause of "crashes on start" on a clean PC for an MD-CRT build. The api-
#     ms-win-crt-* UCRT DLLs are a Windows 10+ component and need not be shipped.
$crtDlls = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")
$redistDir = Get-ChildItem `
    "C:\Program Files*\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" `
    -Directory -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
foreach ($d in $crtDlls) {
    $src = $null
    if ($redistDir) {
        $p = Join-Path $redistDir.FullName $d
        if (Test-Path $p) { $src = $p }
    }
    if (-not $src) {
        $p = Join-Path $env:WINDIR "System32\$d"
        if (Test-Path $p) { $src = $p }
    }
    if ($src) { Copy-Item $src $stageDir -Force; Write-Host "  bundled runtime $d" -ForegroundColor DarkGray }
    else      { Write-Host "  WARN: could not find $d to bundle" -ForegroundColor Yellow }
}

# 2) Game assets (canonical source tree). Copy top-level entries selectively so
#    the ~300 MB card-image folder is never even copied when excluded — far
#    faster than copy-everything-then-delete.
$assetsSrc = Join-Path $root "assets"
$assetsDst = Join-Path $stageDir "assets"
New-Item -ItemType Directory -Path $assetsDst -Force | Out-Null
Write-Host "  copying assets..." -ForegroundColor DarkGray
Get-ChildItem -Path $assetsSrc -Force | ForEach-Object {
    if (-not $IncludeCardImages -and $_.PSIsContainer -and $_.Name -eq "cards") {
        Write-Host "  excluded card images (on-demand download)" -ForegroundColor DarkGray
        return
    }
    # Dev script backups are not part of a release.
    if ($_.PSIsContainer -and $_.Name -like "scripts_backup_*") {
        Write-Host "  excluded $($_.Name) (dev backup)" -ForegroundColor DarkGray
        return
    }
    Copy-Item $_.FullName -Destination $assetsDst -Recurse -Force
}
# Generated/local cruft does not belong in the bundle.
$replays = Join-Path $assetsDst "replays"
if (Test-Path $replays) {
    Get-ChildItem $replays -Filter *.json -ErrorAction SilentlyContinue |
        Remove-Item -Force
}

# 3) Online relay tools (so players can self-host a relay).
$toolsDst = Join-Path $stageDir "tools"
New-Item -ItemType Directory -Path $toolsDst -Force | Out-Null
foreach ($t in @("relay_server.py", "run_relay_server.py")) {
    $src = Join-Path $root "tools\$t"
    if (Test-Path $src) { Copy-Item $src $toolsDst -Force }
}

# 4) Docs (whatever exists at the repo root).
foreach ($doc in @("README.md", "THIRD_PARTY_NOTICES.md",
                   "LICENSE", "NOTICE", "CHANGELOG.md")) {
    $src = Join-Path $root $doc
    if (Test-Path $src) { Copy-Item $src $stageDir -Force }
}

# 5) Zip it (unless the caller only wants the staging dir, e.g. the installer).
if ($SkipZip) {
    Write-Host "Staged (zip skipped): $stageDir" -ForegroundColor Green
} else {
    Write-Host "  compressing -> $zipPath" -ForegroundColor DarkGray
    Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $zipPath -Force
    $zipSize = "{0:N1} MB" -f ((Get-Item $zipPath).Length / 1MB)
    Write-Host "Done. $zipPath ($zipSize)" -ForegroundColor Green
}
