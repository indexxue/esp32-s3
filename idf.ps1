# Generic idf.py wrapper for all ESP-IDF projects in this repository.
#
# Examples:
#   .\idf.ps1 build
#   .\idf.ps1 -Project ble_demo build
#   .\idf.ps1 -Project project release 1.2.3
#   .\idf.ps1 -Project factory -DPROJECT_VER=1.0.0 build

param(
    [ValidateSet("project", "ble_demo", "factory", "ballot_guard")]
    [string]$Project = "project",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = "Stop"

if ($IdfArgs.Count -eq 0) {
    Write-Error "Missing idf.py arguments. Example: .\idf.ps1 build"
}

. (Join-Path $PSScriptRoot "scripts\IdfEnv.ps1")
$idf = Initialize-IdfEnvironment
$projectPath = Resolve-IdfProjectPath -Project $Project -IdfEnv $idf

Write-Host "Project: $Project ($projectPath)"
Write-Host "Command: idf.py $($IdfArgs -join ' ')"

$needsTarget = @("build", "reconfigure", "release", "release-all")
if ($needsTarget -contains $IdfArgs[0]) {
    Ensure-IdfTarget -IdfEnv $idf -ProjectPath $projectPath
}

$exitCode = Invoke-IdfPy -IdfEnv $idf -ProjectPath $projectPath -Arguments $IdfArgs
if ($exitCode -ne 0) {
    Write-Error "idf.py failed with exit code $exitCode"
}
exit $exitCode
