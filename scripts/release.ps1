param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [ValidateSet("project", "ble_demo", "factory", "ballot_guard")]
    [string]$Project = "project",
    [switch]$AllProjects,
    [switch]$FlashBundle,
    [switch]$WithDebug
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "IdfEnv.ps1")

$idf = Initialize-IdfEnvironment

$releaseArgs = @()
if ($FlashBundle) { $releaseArgs += '--flash-bundle' }
if ($WithDebug) { $releaseArgs += '--debug' }

if ($AllProjects) {
    Write-Host "Release-all version: $Version"
    $projectPath = Resolve-IdfProjectPath -Project project -IdfEnv $idf
    Ensure-IdfTarget -IdfEnv $idf -ProjectPath $projectPath
    $allArgs = @('release-all', $Version) + $releaseArgs
    $exitCode = Invoke-IdfPy -IdfEnv $idf -ProjectPath $projectPath -Arguments $allArgs
    if ($exitCode -ne 0) {
        Write-Error "idf.py release-all failed with exit code $exitCode"
    }
    Write-Host "Done. See firmware\$Version\"
    exit 0
}

$projectPath = Resolve-IdfProjectPath -Project $Project -IdfEnv $idf
Ensure-IdfTarget -IdfEnv $idf -ProjectPath $projectPath

Write-Host "Release $Project version: $Version → firmware\$Version\"
$singleArgs = @('release', $Version) + $releaseArgs

$exitCode = Invoke-IdfPy -IdfEnv $idf -ProjectPath $projectPath -Arguments $singleArgs
if ($exitCode -ne 0) {
    Write-Error "idf.py release failed with exit code $exitCode"
}

Write-Host "Done. See firmware\$Version\"
