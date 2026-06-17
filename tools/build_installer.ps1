<#
.SYNOPSIS
    Build the YGO Nova Windows installer (Setup.exe) with Inno Setup.

.DESCRIPTION
    1. Stages the release bundle via package_release.ps1 (no zip).
    2. Compiles installer\YGONova.iss with Inno Setup's ISCC.exe.
    Output: dist\YGONova-v<Version>-Setup.exe

    Requires Inno Setup 6 (ISCC.exe). Install once with:
        winget install -e --id JRSoftware.InnoSetup

.PARAMETER Version
    Release version. Default 1.0.0 (keep in sync with CMake project()).

.PARAMETER Config
    Build configuration to package. Default Release.

.PARAMETER IncludeCardImages
    Bundle the ~300 MB card-image pack instead of relying on on-demand download.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\build_installer.ps1 -Version 1.0.0
#>
param(
    [string]$Version = "1.0.0",
    [string]$Config  = "Release",
    [switch]$IncludeCardImages
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

# 1) Locate ISCC.exe (covers machine-wide and per-user winget installs).
$iscc = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $iscc) {
    Write-Error ("Inno Setup (ISCC.exe) not found. Install it with:`n" +
                 "    winget install -e --id JRSoftware.InnoSetup")
}

# 2) Stage the bundle (no zip - the installer only needs the staging dir).
Write-Host "Staging release payload..." -ForegroundColor Cyan
$pkg = Join-Path $PSScriptRoot "package_release.ps1"
if ($IncludeCardImages) {
    & $pkg -Version $Version -Config $Config -SkipZip -IncludeCardImages
} else {
    & $pkg -Version $Version -Config $Config -SkipZip
}

# 3) Compile the installer.
Write-Host "Compiling installer with Inno Setup..." -ForegroundColor Cyan
$iss = Join-Path $root "installer\YGONova.iss"
& $iscc "/DMyAppVersion=$Version" $iss
if ($LASTEXITCODE -ne 0) { Write-Error "ISCC failed with exit code $LASTEXITCODE." }

$setup = Join-Path $root "dist\YGONova-v$Version-Setup.exe"
if (Test-Path $setup) {
    $sz = "{0:N1} MB" -f ((Get-Item $setup).Length / 1MB)
    Write-Host "Done. $setup ($sz)" -ForegroundColor Green
} else {
    Write-Error "Installer not produced - check ISCC output above."
}
