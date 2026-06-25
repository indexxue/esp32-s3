param(
    [ValidateSet("project", "ble_demo", "factory", "ballot_guard")]
    [string]$Project = "project",
    [ValidateSet("build", "reconfigure", "clean", "fullclean", "size", "size-components", "size-files")]
    [string]$Action = "build",
    [switch]$ShowSize,
    [ValidateSet("summary", "components", "files")]
    [string]$ShowSizeLevel = "summary"
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "IdfEnv.ps1")
. (Join-Path $PSScriptRoot "IdfSizeSummary.ps1")

$idf = Initialize-IdfEnvironment
$projectPath = Resolve-IdfProjectPath -Project $Project -IdfEnv $idf

Write-Host "Using IDF_PATH: $($idf.IdfPath)"
Write-Host "Using Python : $($idf.IdfPython)"
Write-Host "Project    : $Project"
Write-Host "Action     : $Action"
if ($ShowSize -and $Action -eq "build") {
    Write-Host "ShowSize   : yes ($ShowSizeLevel after build)"
}

$sizeOnlyActions = @("size", "size-components", "size-files")
if ($sizeOnlyActions -contains $Action) {
    if ($Action -eq "size") {
        Write-IdfSizeHumanSummary -ProjectPath $projectPath -IdfPython $idf.IdfPython -IdfPath $idf.IdfPath
        exit 0
    }
    $exitCode = Invoke-IdfPy -IdfEnv $idf -ProjectPath $projectPath -Arguments @($Action)
    if ($exitCode -ne 0) {
        Write-Error "idf.py $Action failed with exit code $exitCode"
    }
    exit $exitCode
}

if (($Action -eq "build") -or ($Action -eq "reconfigure")) {
    Ensure-IdfTarget -IdfEnv $idf -ProjectPath $projectPath
}

$exitCode = Invoke-IdfPy -IdfEnv $idf -ProjectPath $projectPath -Arguments @($Action)
if ($exitCode -ne 0) {
    Write-Error "idf.py $Action failed with exit code $exitCode"
}

if ($ShowSize -and $Action -eq "build" -and $exitCode -eq 0) {
    if ($ShowSizeLevel -eq "summary") {
        Write-IdfSizeHumanSummary -ProjectPath $projectPath -IdfPython $idf.IdfPython -IdfPath $idf.IdfPath
    }
    else {
        $sizeCmd = switch ($ShowSizeLevel) {
            "components" { "size-components" }
            "files" { "size-files" }
            default { "size-components" }
        }
        Write-Host ""
        Write-Host "--- Firmware memory usage: idf.py $sizeCmd ---"
        $sizeExit = Invoke-IdfPy -IdfEnv $idf -ProjectPath $projectPath -Arguments @($sizeCmd)
        if ($sizeExit -ne 0) {
            Write-Error "idf.py $sizeCmd failed with exit code $sizeExit"
        }
    }
}
