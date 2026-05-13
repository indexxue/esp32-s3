# Thin wrapper: run from this directory (project/) so paths stay correct.
# Example: .\build.ps1 -ShowSize
$ErrorActionPreference = "Stop"
$repoScript = Join-Path $PSScriptRoot "..\scripts\build.ps1"
if (-not (Test-Path $repoScript)) {
    Write-Error "Missing repo build script: $repoScript"
}
& $repoScript @args
