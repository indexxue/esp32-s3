# Download LVGL PC simulator into tools/lvgl_sim/ (gitignored — do not commit).
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\tools\setup_lvgl_sim.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\setup_lvgl_sim.ps1 -Update
#   powershell -ExecutionPolicy Bypass -File .\tools\setup_lvgl_sim.ps1 -InsecureSsl
#
# After install (Windows):
#   Open tools\lvgl_sim\lv_port_pc_visual_studio\LVGL.sln in Visual Studio
#   Set LvglWindowsSimulator as startup project → F5
# Desktop pet panel: set simulator resolution to 240x240 (round GC9A01).
#
# If clone fails with SSL verify errors (common on some Windows/corp nets):
#   pass -InsecureSsl  (sets GIT_SSL_NO_VERIFY=1 for this download only)

param(
    [switch]$Update,
    [switch]$InsecureSsl
)

$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$simRoot = Join-Path $repoRoot "tools\lvgl_sim"
$vsPortDir = Join-Path $simRoot "lv_port_pc_visual_studio"
$vsPortUrl = "https://github.com/lvgl/lv_port_pc_visual_studio.git"
# Align with LVGL 9.x used on ESP-IDF Component Manager
$vsPortBranch = "release/v9.2"

$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($null -eq $gitCmd) {
    Write-Error "Git is required. Install Git for Windows first."
}

if ($InsecureSsl -or ($env:GIT_SSL_NO_VERIFY -eq "1")) {
    $env:GIT_SSL_NO_VERIFY = "1"
    Write-Warning "GIT_SSL_NO_VERIFY=1 (TLS cert verify disabled for this download only)."
}

if (-not (Test-Path $simRoot)) {
    New-Item -ItemType Directory -Path $simRoot -Force | Out-Null
}

$marker = Join-Path $simRoot "INSTALLED.txt"

function Write-InstallMarker {
    $lines = @(
        "LVGL PC simulator (local only — tools/lvgl_sim is gitignored)",
        "Installed: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
        "Port: lv_port_pc_visual_studio @ $vsPortBranch (shallow)",
        "Path: $vsPortDir",
        "",
        "Open LVGL.sln in Visual Studio; run LvglWindowsSimulator.",
        "For desktop_pet: use 240x240 resolution to match GC9A01."
    )
    Set-Content -Path $marker -Value $lines -Encoding UTF8
}

function Invoke-ShallowSubmodules {
    param([string]$WorkDir)
    Push-Location $WorkDir
    try {
        # Depth-1 submodules: full history of lvgl/freetype is huge and not needed for UI sim.
        & git submodule update --init --recursive --depth 1
        if ($LASTEXITCODE -ne 0) {
            Write-Error "git submodule update --depth 1 failed."
        }
    } finally {
        Pop-Location
    }
}

if ((Test-Path $vsPortDir) -and -not $Update) {
    $lvglSrc = Join-Path $vsPortDir "LvglPlatform\lvgl\src"
    if (Test-Path $lvglSrc) {
        Write-Host "Already present: $vsPortDir"
        Write-Host "Pass -Update to re-fetch branch $vsPortBranch."
        if (-not (Test-Path $marker)) { Write-InstallMarker }
        Write-Host "Done."
        exit 0
    }
    Write-Warning "Incomplete install detected; finishing submodules..."
    Invoke-ShallowSubmodules -WorkDir $vsPortDir
    Write-InstallMarker
    Write-Host "Install complete (not tracked by git)."
    exit 0
}

if ((Test-Path $vsPortDir) -and $Update) {
    Write-Host "Updating $vsPortDir (branch $vsPortBranch)..."
    Push-Location $vsPortDir
    try {
        & git fetch --depth 1 origin $vsPortBranch
        if ($LASTEXITCODE -ne 0) { Write-Error "git fetch failed." }
        & git checkout -B $vsPortBranch FETCH_HEAD
        if ($LASTEXITCODE -ne 0) { Write-Error "git checkout failed." }
    } finally {
        Pop-Location
    }
    Invoke-ShallowSubmodules -WorkDir $vsPortDir
} else {
    if (Test-Path $vsPortDir) {
        Write-Host "Removing incomplete tree..."
        Remove-Item -Recurse -Force $vsPortDir
    }
    Write-Host "Cloning LVGL Windows simulator ($vsPortBranch, shallow)..."
    Write-Host "  -> $vsPortDir"
    # Clone port first without recursive (more reliable), then shallow submodules.
    & git clone --branch $vsPortBranch --depth 1 $vsPortUrl $vsPortDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to clone $vsPortUrl (try -InsecureSsl if SSL errors)."
    }
    Invoke-ShallowSubmodules -WorkDir $vsPortDir
}

Write-InstallMarker
Write-Host ""
Write-Host "Install complete (not tracked by git)."
Write-Host "  $vsPortDir"
Write-Host "Open LVGL.sln in Visual Studio to build/run the simulator."
