param(
    [ValidateSet("build", "reconfigure", "clean", "fullclean", "size", "size-components", "size-files")]
    [string]$Action = "build",
    # After a successful `build`, run idf.py size / size-components / size-files (firmware Flash/RAM usage).
    [switch]$ShowSize,
    [ValidateSet("summary", "components", "files")]
    [string]$ShowSizeLevel = "summary"
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

. (Join-Path $PSScriptRoot "IdfSizeSummary.ps1")

Write-Host "Using IDF_PATH: $env:IDF_PATH"
Write-Host "Using Python : $idfPython"
Write-Host "Action       : $Action"
if ($ShowSize -and $Action -eq "build") {
    Write-Host "ShowSize     : yes ($ShowSizeLevel after build)"
}

$sizeOnlyActions = @("size", "size-components", "size-files")
if ($sizeOnlyActions -contains $Action) {
    if ($Action -eq "size") {
        Write-IdfSizeHumanSummary -ProjectPath $projectPath -IdfPython $idfPython -IdfPath $idfPath
        exit 0
    }
    & $idfPython $idfPy -C $projectPath $Action
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        Write-Error "idf.py $Action failed with exit code $exitCode"
    }
    exit $exitCode
}

& $idfPython $idfPy -C $projectPath $Action
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    Write-Error "idf.py $Action failed with exit code $exitCode"
}

if ($ShowSize -and $Action -eq "build" -and $exitCode -eq 0) {
    if ($ShowSizeLevel -eq "summary") {
        Write-IdfSizeHumanSummary -ProjectPath $projectPath -IdfPython $idfPython -IdfPath $idfPath
    }
    else {
        $sizeCmd = switch ($ShowSizeLevel) {
            "components" { "size-components" }
            "files" { "size-files" }
            default { "size-components" }
        }
        Write-Host ""
        Write-Host "--- Firmware memory usage: idf.py $sizeCmd ---"
        & $idfPython $idfPy -C $projectPath $sizeCmd
        $sizeExit = $LASTEXITCODE
        if ($sizeExit -ne 0) {
            Write-Error "idf.py $sizeCmd failed with exit code $sizeExit"
        }
    }
}
