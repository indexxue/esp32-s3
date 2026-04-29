$ErrorActionPreference = "Stop"

Write-Host "Checking ESP-IDF environment..."

$idfPath = Join-Path $PSScriptRoot "..\Espressif\frameworks\esp-idf-v5.5.4"
$idfPath = [System.IO.Path]::GetFullPath($idfPath)

if (-not (Test-Path $idfPath)) {
    Write-Error "ESP-IDF not found at: $idfPath`nPlease install ESP-IDF (Espressif IDF Tools) first."
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
Write-Host "  idf.py -C project build"
