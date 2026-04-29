$ErrorActionPreference = "Stop"

Write-Host "Checking ESP-IDF environment..."

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$espRoot = Join-Path $repoRoot "Espressif"
$frameworkRoot = Join-Path $espRoot "frameworks"
$idfVersion = "v5.5.4"
$idfPath = Join-Path $PSScriptRoot "..\Espressif\frameworks\esp-idf-v5.5.4"
$idfPath = [System.IO.Path]::GetFullPath($idfPath)

if (-not (Test-Path $idfPath)) {
    Write-Host "ESP-IDF not found. Bootstrapping a fresh local install..."

    $gitCmd = Get-Command git -ErrorAction SilentlyContinue
    if ($null -eq $gitCmd) {
        Write-Error "Git is required for zero-install flow. Please install Git for Windows first."
    }

    if (-not (Test-Path $frameworkRoot)) {
        New-Item -ItemType Directory -Path $frameworkRoot -Force | Out-Null
    }

    Write-Host "Cloning ESP-IDF $idfVersion..."
    & git clone --branch $idfVersion --depth 1 https://github.com/espressif/esp-idf.git $idfPath
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to clone ESP-IDF repository."
    }
}

$installScript = Join-Path $idfPath "install.bat"
$exportScript = Join-Path $idfPath "export.bat"

if (-not (Test-Path $installScript)) {
    Write-Error "install.bat not found at: $installScript"
}

if (-not (Test-Path $exportScript)) {
    Write-Error "export.bat not found at: $exportScript"
}

Write-Host "Installing/refreshing toolchain (safe to run multiple times)..."
& cmd /c "`"$installScript`""
if ($LASTEXITCODE -ne 0) {
    Write-Error "ESP-IDF install.bat failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "Environment setup complete."
Write-Host "Run these commands in a new terminal:"
Write-Host "  cmd /k `"$exportScript`""
Write-Host "Then build:"
Write-Host "  (from repo root) idf.py -C project build"
Write-Host "  (from .\\project) idf.py build"
