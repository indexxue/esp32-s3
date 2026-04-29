param(
    [ValidateSet("build", "reconfigure", "clean", "fullclean")]
    [string]$Action = "build"
)

$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$projectPath = Join-Path $repoRoot "project"
$idfPath = Join-Path $repoRoot "Espressif\frameworks\esp-idf-v5.5.4"
$idfPython = Join-Path $repoRoot "Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
$idfPy = Join-Path $idfPath "tools\idf.py"

$cmakeBin = Join-Path $repoRoot "Espressif\tools\cmake\3.30.2\bin"
$ninjaBin = Join-Path $repoRoot "Espressif\tools\ninja\1.12.1"

if (-not (Test-Path $projectPath)) { Write-Error "Missing project directory: $projectPath" }
if (-not (Test-Path $idfPath)) { Write-Error "Missing ESP-IDF path: $idfPath. Run scripts/setup_env.ps1 first." }
if (-not (Test-Path $idfPython)) { Write-Error "Missing ESP-IDF Python environment: $idfPython. Run scripts/setup_env.ps1 first." }
if (-not (Test-Path $idfPy)) { Write-Error "Missing idf.py: $idfPy" }
if (-not (Test-Path $cmakeBin)) { Write-Error "Missing CMake tools path: $cmakeBin" }
if (-not (Test-Path $ninjaBin)) { Write-Error "Missing Ninja tools path: $ninjaBin" }

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = Join-Path $repoRoot "Espressif"
$env:IDF_PYTHON_ENV_PATH = Split-Path $idfPython -Parent | Split-Path -Parent
$env:PATH = "$cmakeBin;$ninjaBin;$env:PATH"

Write-Host "Using IDF_PATH: $env:IDF_PATH"
Write-Host "Using Python : $idfPython"
Write-Host "Action       : $Action"

& $idfPython $idfPy -C $projectPath $Action
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    Write-Error "idf.py $Action failed with exit code $exitCode"
}
