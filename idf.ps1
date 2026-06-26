# Generic idf.py wrapper for all ESP-IDF projects in this repository.
#
# Examples:
#   .\idf.ps1 build
#   .\idf.ps1 -Project ble_demo build
#   .\idf.ps1 -Project project release 1.2.3
#   .\idf.ps1 -p COM13 flash monitor
#
# Note: use $args (no param block) so idf.py flags like -p / -D are not consumed by
# PowerShell parameter binding (including .\idf.ps1 -p COM13 … in PowerShell 5.1).

$ErrorActionPreference = "Stop"

$validProjects = @("project", "ble_demo", "factory", "ballot_guard")
$Project = "project"
$IdfArgs = @()
$RawArgs = @($args)
if ($RawArgs.Count -ge 1 -and $RawArgs[0] -eq "--") {
    $RawArgs = $RawArgs[1..($RawArgs.Count - 1)]
}

$i = 0
while ($i -lt $RawArgs.Count) {
    $arg = $RawArgs[$i]
    if ($arg -ieq "-Project") {
        if ($i + 1 -ge $RawArgs.Count) {
            Write-Error "-Project requires a value ($($validProjects -join ', '))"
        }
        $Project = $RawArgs[$i + 1]
        if ($validProjects -notcontains $Project) {
            Write-Error "Invalid -Project '$Project'. Use: $($validProjects -join ', ')"
        }
        $i += 2
        continue
    }
    $IdfArgs += $arg
    $i += 1
}

if ($IdfArgs.Count -eq 0) {
    Write-Error "Missing idf.py arguments. Example: .\idf.ps1 build"
}

function Get-IdfSerialPort {
    param([string[]]$Arguments)
    for ($i = 0; $i -lt $Arguments.Count; $i++) {
        $arg = $Arguments[$i]
        if ($arg -eq "-p" -or $arg -eq "--port") {
            if ($i + 1 -lt $Arguments.Count) {
                return $Arguments[$i + 1]
            }
        }
        if ($arg -like "-p*") {
            return $arg.Substring(2)
        }
        if ($arg -like "--port=*") {
            return $arg.Substring(8)
        }
    }
    return $null
}

function Test-SerialPortOpenable {
    param([string]$Port)
    $sp = $null
    try {
        $sp = New-Object System.IO.Ports.SerialPort $Port
        $sp.Open() | Out-Null
        return $true
    }
    catch {
        return $false
    }
    finally {
        if ($sp -and $sp.IsOpen) {
            $sp.Close()
        }
        if ($sp) {
            $sp.Dispose()
        }
    }
}

function Stop-StaleSerialToolProcesses {
    param([string]$Port)
    if (-not $Port) {
        return
    }
    $portPattern = [regex]::Escape($Port)
    Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            $cmd = $_.CommandLine
            if (-not $cmd) { return $false }
            $usesPort = ($cmd -match "\s-p\s+$portPattern\b") -or
                ($cmd -match "\s--port\s+$portPattern\b") -or
                ($cmd -match "\s--port=$portPattern\b") -or
                ($cmd -match "\s-p$portPattern\b")
            if (-not $usesPort) { return $false }
            return ($cmd -match 'idf_monitor|idf\.py') -or ($cmd -match 'esptool')
        } |
        ForEach-Object {
            Write-Host "Stopping stale serial tool PID $($_.ProcessId)"
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
    Start-Sleep -Milliseconds 500
}

function Ensure-SerialPortReady {
    param(
        [string]$Port,
        [int]$Retries = 5
    )
    if (-not $Port) {
        return
    }
    $availablePorts = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($availablePorts -notcontains $Port) {
        $listed = if ($availablePorts.Count -gt 0) { $availablePorts -join ", " } else { "(none)" }
        Write-Error "$Port not found. Available ports: $listed"
    }
    for ($attempt = 1; $attempt -le $Retries; $attempt++) {
        if (Test-SerialPortOpenable -Port $Port) {
            return
        }
        if ($attempt -eq 1) {
            Stop-StaleSerialToolProcesses -Port $Port
        }
        Start-Sleep -Milliseconds 800
    }
    Write-Error @"
$Port is busy or inaccessible.
- Exit an open idf monitor in this terminal with Ctrl+]
- Close Cursor/VS Code serial monitor or other apps using $Port
- Unplug/replug USB if the port is stuck after a crash
"@
}

function Wait-SerialPortReconnect {
    param(
        [string]$Port,
        [int]$TimeoutSec = 30
    )
    if (-not $Port) {
        Start-Sleep -Seconds 2
        return
    }
    Write-Host "Waiting for $Port to reconnect after flash..."
    Start-Sleep -Seconds 2
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $ports = [System.IO.Ports.SerialPort]::GetPortNames()
        if ($ports -contains $Port) {
            Start-Sleep -Seconds 1
            return
        }
        Start-Sleep -Milliseconds 500
    }
    Write-Warning "Port $Port not detected within ${TimeoutSec}s; starting monitor anyway."
}

. (Join-Path $PSScriptRoot "scripts\IdfEnv.ps1")
$idf = Initialize-IdfEnvironment
$projectPath = Resolve-IdfProjectPath -Project $Project -IdfEnv $idf

Write-Host "Project: $Project ($projectPath)"

$serialActions = @("flash", "erase-flash", "encrypted-flash", "monitor")
$needsSerialPort = ($IdfArgs | Where-Object { $serialActions -contains $_ }).Count -gt 0
$serialPort = Get-IdfSerialPort -Arguments $IdfArgs
if ($needsSerialPort -and $serialPort) {
    Ensure-SerialPortReady -Port $serialPort
}

$runFlashMonitorSplit = ($IdfArgs -contains "flash") -and ($IdfArgs -contains "monitor")
if (-not $runFlashMonitorSplit) {
    Write-Host "Command: idf.py $($IdfArgs -join ' ')"
}

$needsTarget = @("build", "reconfigure", "release", "release-all")
if ($needsTarget -contains $IdfArgs[0]) {
    Ensure-IdfTarget -IdfEnv $idf -ProjectPath $projectPath
}

if ($runFlashMonitorSplit) {
    $flashArgs = @($IdfArgs | Where-Object { $_ -ne "monitor" })
    $monitorArgs = @($IdfArgs | Where-Object { $_ -ne "flash" })
    $port = $serialPort

    Write-Host "Command: idf.py $($flashArgs -join ' ')"
    $exitCode = Invoke-IdfPy -IdfEnv $idf -ProjectPath $projectPath -Arguments $flashArgs
    if ($exitCode -ne 0) {
        Write-Error "idf.py failed with exit code $exitCode"
    }

    Wait-SerialPortReconnect -Port $port

    Write-Host "Command: idf.py $($monitorArgs -join ' ')"
    $exitCode = Invoke-IdfPy -IdfEnv $idf -ProjectPath $projectPath -Arguments $monitorArgs
    if ($exitCode -ne 0) {
        Write-Error "idf.py failed with exit code $exitCode"
    }
    exit $exitCode
}

$exitCode = Invoke-IdfPy -IdfEnv $idf -ProjectPath $projectPath -Arguments $IdfArgs
if ($exitCode -ne 0) {
    Write-Error "idf.py failed with exit code $exitCode"
}
exit $exitCode
