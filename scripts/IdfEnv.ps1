# Shared ESP-IDF environment bootstrap for this repository.
# Sets IDF_EXTRA_ACTIONS_PATH so idf.py loads scripts/idf_py_actions/release_ext.py
# (no per-project idf_ext.py required).

$ErrorActionPreference = "Stop"

function Get-IdfRepoRoot {
    return [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
}

function Initialize-IdfEnvironment {
    [CmdletBinding()]
    param()

    $repoRoot = Get-IdfRepoRoot
    $idfPath = Join-Path $repoRoot "Espressif\frameworks\esp-idf-v5.5.4"
    $idfPython = Join-Path $repoRoot "Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
    $idfPy = Join-Path $idfPath "tools\idf.py"
    $cmakeBin = Join-Path $repoRoot "Espressif\tools\cmake\3.30.2\bin"
    $ninjaBin = Join-Path $repoRoot "Espressif\tools\ninja\1.12.1"
    $ninjaExe = Join-Path $ninjaBin "ninja.exe"
    $extraActions = Join-Path $repoRoot "scripts\idf_py_actions"

    if (-not (Test-Path $idfPath)) {
        Write-Error "Missing ESP-IDF path: $idfPath. Run scripts/setup_env.ps1 first."
    }
    if (-not (Test-Path $idfPython)) {
        Write-Error "Missing ESP-IDF Python: $idfPython. Run scripts/setup_env.ps1 first."
    }
    if (-not (Test-Path $idfPy)) {
        Write-Error "Missing idf.py: $idfPy"
    }
    if (-not (Test-Path $extraActions)) {
        Write-Error "Missing idf.py extensions directory: $extraActions"
    }
    if (-not (Test-Path $ninjaExe)) {
        Write-Error "Missing ninja: $ninjaExe. Run scripts/setup_env.ps1 first."
    }

    $env:IDF_PATH = $idfPath
    $env:IDF_TOOLS_PATH = Join-Path $repoRoot "Espressif"
    $env:IDF_PYTHON_ENV_PATH = Split-Path $idfPython -Parent | Split-Path -Parent
    $env:IDF_EXTRA_ACTIONS_PATH = $extraActions

    Push-Location $idfPath
    try {
        . (Join-Path $idfPath "export.ps1") | Out-Null
    }
    finally {
        Pop-Location
    }

    $env:PATH = "$cmakeBin;$ninjaBin;$env:PATH"
    $env:CMAKE_MAKE_PROGRAM = $ninjaExe

    return @{
        RepoRoot = $repoRoot
        IdfPath = $idfPath
        IdfPython = $idfPython
        IdfPy = $idfPy
        ExtraActionsPath = $extraActions
    }
}

function Resolve-IdfProjectPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("project", "ble_demo", "factory", "ballot_guard")]
        [string]$Project,
        [Parameter(Mandatory = $true)]
        [hashtable]$IdfEnv
    )

    $projectPath = Join-Path $IdfEnv.RepoRoot $Project
    if (-not (Test-Path $projectPath)) {
        Write-Error "Missing project directory: $projectPath"
    }
    return $projectPath
}

function Ensure-IdfTarget {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$IdfEnv,
        [Parameter(Mandatory = $true)]
        [string]$ProjectPath,
        [string]$Target = "esp32s3"
    )

    $sdkconfig = Join-Path $ProjectPath "sdkconfig"
    if (-not (Test-Path $sdkconfig)) {
        Write-Host "No sdkconfig: idf.py set-target $Target (once)"
        & $IdfEnv.IdfPython $IdfEnv.IdfPy -C $ProjectPath set-target $Target
        if ($LASTEXITCODE -ne 0) {
            Write-Error "set-target $Target failed with exit code $LASTEXITCODE"
        }
    }
}

function Invoke-IdfPy {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$IdfEnv,
        [Parameter(Mandatory = $true)]
        [string]$ProjectPath,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]]$Arguments
    )

    & $IdfEnv.IdfPython $IdfEnv.IdfPy -C $ProjectPath @Arguments | Out-Host
    return $LASTEXITCODE
}
