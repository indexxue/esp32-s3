param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [ValidateSet("project", "ble_demo", "factory", "ballot_guard")]
    [string]$Project = "project",
    [switch]$AllProjects,
    [switch]$FlashBundle,
    [switch]$WithDebug,
    [ValidateSet("signed_ota", "secure_boot")]
    [string]$SigningProfile,
    [string]$SigningKeyId
)

$ErrorActionPreference = "Stop"
Write-Warning "Deprecated: use idf release instead."

$repoRoot = Split-Path $PSScriptRoot -Parent
$idfArgs = @('-Project', $Project)
if ($AllProjects) {
    $idfArgs += @('release-all', $Version)
} else {
    $idfArgs += @('release', $Version)
}
if ($FlashBundle) { $idfArgs += '--flash-bundle' }
if ($WithDebug) { $idfArgs += '--debug' }
if ($SigningProfile) { $idfArgs += @('--signing-profile', $SigningProfile) }
if ($SigningKeyId) { $idfArgs += @('--signing-key-id', $SigningKeyId) }

Write-Host "  idf $($idfArgs -join ' ')"
& (Join-Path $repoRoot "idf.cmd") @idfArgs
exit $LASTEXITCODE
