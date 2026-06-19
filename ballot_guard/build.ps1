# Thin wrapper: run from ballot_guard/ directory.
$ErrorActionPreference = "Stop"
$repoScript = Join-Path $PSScriptRoot "..\scripts\build_ballot_guard.ps1"
if (-not (Test-Path $repoScript)) {
    Write-Error "Missing repo build script: $repoScript"
}
& $repoScript @args
