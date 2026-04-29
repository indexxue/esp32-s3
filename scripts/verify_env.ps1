param(
    [switch]$RunInstall,
    [switch]$RunBuildTest,
    [switch]$RequireIdfInPath
)

$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$projectPath = Join-Path $repoRoot "project"
$idfPath = Join-Path $repoRoot "Espressif\frameworks\esp-idf-v5.5.4"
$setupScript = Join-Path $repoRoot "scripts\setup_env.ps1"
$exportScript = Join-Path $idfPath "export.bat"
$installScript = Join-Path $idfPath "install.bat"

Write-Host "Verifying ESP32-S3 environment..."
Write-Host "Repository root: $repoRoot"

if (-not (Test-Path $setupScript)) { Write-Error "Missing setup script: $setupScript" }
if (-not (Test-Path $projectPath)) { Write-Error "Missing project directory: $projectPath" }
if (-not (Test-Path (Join-Path $projectPath "CMakeLists.txt"))) { Write-Error "Missing project/CMakeLists.txt" }
if (-not (Test-Path $idfPath)) { Write-Error "Missing ESP-IDF path: $idfPath" }
if (-not (Test-Path $installScript)) { Write-Error "Missing install script: $installScript" }
if (-not (Test-Path $exportScript)) { Write-Error "Missing export script: $exportScript" }

if ($RunInstall) {
    Write-Host "Running setup script..."
    & powershell -ExecutionPolicy Bypass -File $setupScript
    if ($LASTEXITCODE -ne 0) {
        Write-Error "setup_env.ps1 failed with exit code $LASTEXITCODE"
    }
}

Write-Host "Checking idf.py in current terminal..."
$idfCmd = Get-Command idf.py -ErrorAction SilentlyContinue
if ($null -eq $idfCmd) {
    if ($RequireIdfInPath) {
        Write-Error "idf.py not found in current PATH. Please run: cmd /k `"$exportScript`""
    }
    Write-Warning "idf.py is not available in current PATH."
    Write-Host "Open a new terminal and run:"
    Write-Host "  cmd /k `"$exportScript`""
    Write-Host "Then rerun this check with -RequireIdfInPath if needed."
}
else {
    & idf.py --version
    if ($LASTEXITCODE -ne 0) {
        Write-Error "idf.py exists in PATH but failed to run."
    }
}

if ($RunBuildTest) {
    if ($null -eq $idfCmd) {
        Write-Error "RunBuildTest requires idf.py in PATH. Run export.bat first."
    }
    Write-Host "Running lightweight project configure test..."
    & idf.py -C $projectPath reconfigure
    $buildExit = $LASTEXITCODE
    if ($buildExit -ne 0) {
        Write-Error "Project configure test failed with exit code $buildExit"
    }
}

Write-Host ""
Write-Host "Verification passed."
Write-Host "Build commands:"
Write-Host "  From repo root: idf.py -C project build"
Write-Host "  From project dir: idf.py build"
