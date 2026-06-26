param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("none", "signed_ota", "secure_boot")]
    [string]$Profile,
    [ValidateSet("project", "ble_demo", "factory", "ballot_guard")]
    [string]$Project = "project",
    [switch]$Build
)

$ErrorActionPreference = "Stop"
Write-Warning "Deprecated: use idf signing-profile instead."
Write-Host "  idf -Project $Project signing-profile $Profile$(if ($Build) { ' --build' })"

$repoRoot = Split-Path $PSScriptRoot -Parent
& (Join-Path $repoRoot "idf.cmd") -Project $Project signing-profile $Profile $(if ($Build) { '--build' })
exit $LASTEXITCODE
