param(
    [ValidateSet("build", "reconfigure", "clean", "fullclean")]
    [string]$Action = "build"
)

$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$projectPath = Join-Path $repoRoot "ble_demo"
$idfPath = Join-Path $repoRoot "Espressif\frameworks\esp-idf-v5.5.4"
$idfPython = Join-Path $repoRoot "Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
$idfPy = Join-Path $idfPath "tools\idf.py"

$cmakeBin = Join-Path $repoRoot "Espressif\tools\cmake\3.30.2\bin"
$ninjaBin = Join-Path $repoRoot "Espressif\tools\ninja\1.12.1"

if (-not (Test-Path $projectPath)) { Write-Error "Missing ble_demo project: $projectPath" }
if (-not (Test-Path $idfPath)) { Write-Error "Missing ESP-IDF: $idfPath. Run scripts/setup_env.ps1 first." }
if (-not (Test-Path $idfPython)) { Write-Error "Missing IDF Python: $idfPython" }
if (-not (Test-Path $idfPy)) { Write-Error "Missing idf.py: $idfPy" }

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = Join-Path $repoRoot "Espressif"
$env:IDF_PYTHON_ENV_PATH = Split-Path $idfPython -Parent | Split-Path -Parent

Push-Location $idfPath
try {
    . (Join-Path $idfPath "export.ps1")
} finally {
    Pop-Location
}

$env:PATH = "$cmakeBin;$ninjaBin;$env:PATH"

Write-Host "IDF_PATH : $env:IDF_PATH"
Write-Host "Project  : $projectPath"
Write-Host "Action   : $Action"

$sdkconfig = Join-Path $projectPath "sdkconfig"
if ((-not (Test-Path $sdkconfig)) -and (($Action -eq "build") -or ($Action -eq "reconfigure"))) {
    Write-Host "No sdkconfig: idf.py set-target esp32s3 (once)"
    & $idfPython $idfPy -C $projectPath set-target esp32s3
    if ($LASTEXITCODE -ne 0) {
        Write-Error "set-target esp32s3 failed with exit code $LASTEXITCODE"
    }
}

& $idfPython $idfPy -C $projectPath $Action
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    Write-Error "idf.py $Action failed with exit code $exitCode"
}
exit $exitCode
