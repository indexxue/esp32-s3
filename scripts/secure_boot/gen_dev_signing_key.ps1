param([switch]$Force)

$ErrorActionPreference = "Stop"
Write-Warning "Deprecated: use idf signing-key-gen instead."
Write-Host "  idf signing-key-gen$(if ($Force) { ' --force' })"

$repoRoot = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
& (Join-Path $repoRoot "idf.cmd") signing-key-gen $(if ($Force) { '--force' })
exit $LASTEXITCODE
