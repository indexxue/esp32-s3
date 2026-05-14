# Human-readable firmware size (KiB/MiB) + app partition free space.
# Uses esp_idf_size JSON (no idf.py size / no extra ninja all).

function script:Format-IdfByteSize([long]$bytes) {
    if ($bytes -lt 0) { return 'N/A' }
    if ($bytes -ge 1048576) {
        $m = [double]$bytes / 1048576.0
        $k = [double]$bytes / 1024.0
        return ('{0:N2} MiB ({1:N0} KiB)' -f $m, $k)
    }
    if ($bytes -ge 1024) {
        return ('{0:N2} KiB' -f ([double]$bytes / 1024.0))
    }
    return ('{0} B' -f $bytes)
}

function script:Parse-IdfPartitionSizeToken([string]$token) {
    $t = $token.Trim().TrimEnd(',')
    if ($t -match '^0x[0-9a-fA-F]+$') {
        return [long][System.Convert]::ToInt64($t, 16)
    }
    if ($t -match '^(\d+)\s*M') {
        return [long]$matches[1] * 1048576L
    }
    if ($t -match '^(\d+)\s*K') {
        return [long]$matches[1] * 1024L
    }
    return [long]$t
}

function script:Get-IdfAppBinFlashInfo {
    param([string]$BuildDir)
    $flashArgsPath = Join-Path $BuildDir 'flash_app_args'
    if (-not (Test-Path $flashArgsPath)) { return $null }
    $lines = Get-Content $flashArgsPath | Where-Object { $_ -match '\S' -and $_ -notmatch '^\s*#' }
    foreach ($line in $lines) {
        if ($line -match '^\s*(0x[0-9a-fA-F]+)\s+(\S+)\s*$') {
            $off = [long][System.Convert]::ToInt64($matches[1], 16)
            $name = $matches[2].Trim()
            $binPath = Join-Path $BuildDir $name
            if (Test-Path $binPath) {
                return @{
                    Offset = $off
                    BinPath = $binPath
                    BinSize = (Get-Item $binPath).Length
                }
            }
        }
    }
    return $null
}

function script:Invoke-PythonCaptured {
    param(
        [string]$PythonExe,
        [string[]]$ArgumentList
    )
    $stdoutF = Join-Path $env:TEMP ('idfcap_out_{0}.txt' -f [Guid]::NewGuid().ToString('N'))
    $stderrF = Join-Path $env:TEMP ('idfcap_err_{0}.txt' -f [Guid]::NewGuid().ToString('N'))
    New-Item -Path $stdoutF -ItemType File -Force | Out-Null
    New-Item -Path $stderrF -ItemType File -Force | Out-Null
    try {
        $p = Start-Process -FilePath $PythonExe -ArgumentList $ArgumentList -Wait -PassThru -NoNewWindow `
            -RedirectStandardOutput $stdoutF -RedirectStandardError $stderrF
        $code = if ($p) { $p.ExitCode } else { -1 }
        $outText = if (Test-Path $stdoutF) { Get-Content $stdoutF -Raw -Encoding UTF8 } else { '' }
        return @{ ExitCode = $code; Stdout = $outText }
    }
    finally {
        Remove-Item $stdoutF, $stderrF -Force -ErrorAction SilentlyContinue
    }
}

function script:Get-IdfAppPartitionForOffset {
    param(
        [string]$IdfPython,
        [string]$GenEsp32PartPy,
        [string]$PartitionBin,
        [long]$ImageOffset
    )
    if (-not (Test-Path $GenEsp32PartPy) -or -not (Test-Path $PartitionBin)) { return $null }
    $cap = Invoke-PythonCaptured -PythonExe $IdfPython -ArgumentList @($GenEsp32PartPy, $PartitionBin)
    if ($cap.ExitCode -ne 0) { return $null }
    $text = [string]$cap.Stdout
    foreach ($line in $text -split "`n") {
        $l = $line.Trim()
        if ($l.StartsWith('#') -or $l.Length -eq 0) { continue }
        $p = $l -split ','
        if ($p.Count -lt 5) { continue }
        if ($p[1].Trim() -ne 'app') { continue }
        $off = [long][System.Convert]::ToInt64(($p[3].Trim()), 16)
        $sz = Parse-IdfPartitionSizeToken $p[4]
        $end = $off + $sz
        if ($ImageOffset -ge $off -and $ImageOffset -lt $end) {
            return @{
                Name = $p[0].Trim()
                SubType = $p[2].Trim()
                Offset = $off
                Size = $sz
            }
        }
    }
    return $null
}

function Write-IdfSizeHumanSummary {
    param(
        [Parameter(Mandatory)][string]$ProjectPath,
        [Parameter(Mandatory)][string]$IdfPython,
        [Parameter(Mandatory)][string]$IdfPath
    )
    $buildDir = Join-Path $ProjectPath 'build'
    if (-not (Test-Path $buildDir)) {
        Write-Warning "No build directory: $buildDir"
        return
    }

    $pdPath = Join-Path $buildDir 'project_description.json'
    if (-not (Test-Path $pdPath)) {
        Write-Warning 'Missing project_description.json (build first).'
        return
    }
    $pd = Get-Content $pdPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $elfName = [string]$pd.app_elf
    if (-not $elfName) {
        Write-Warning 'project_description.json has no app_elf.'
        return
    }
    $mapName = [System.IO.Path]::ChangeExtension($elfName, '.map')
    $mapPath = Join-Path $buildDir $mapName
    if (-not (Test-Path $mapPath)) {
        Write-Warning "Missing linker map: $mapPath"
        return
    }

    $tmpJson = Join-Path $env:TEMP ('idf_size_summary_{0}.json' -f [Guid]::NewGuid().ToString('N'))
    try {
        $env:IDF_PATH = $IdfPath
        $env:ESP_IDF_SIZE_NG = '1'
        $cap = Invoke-PythonCaptured -PythonExe $IdfPython -ArgumentList @(
            '-m', 'esp_idf_size', $mapPath, '--format', 'json2', '--use-flash-size', '-o', $tmpJson
        )
        if ($cap.ExitCode -ne 0 -or -not (Test-Path $tmpJson)) {
            Write-Warning 'esp_idf_size failed; try: idf.py -C project size'
            return
        }
        $data = Get-Content $tmpJson -Raw -Encoding UTF8 | ConvertFrom-Json
    }
    finally {
        if (Test-Path $tmpJson) { Remove-Item $tmpJson -Force -ErrorAction SilentlyContinue }
    }

    Write-Host ''
    Write-Host '======== Firmware size (KiB / MiB) ========'

    $binInfo = Get-IdfAppBinFlashInfo $buildDir
    $genPart = Join-Path $IdfPath 'components\partition_table\gen_esp32part.py'
    $partBin = Join-Path $buildDir 'partition_table\partition-table.bin'
    $part = $null
    if ($binInfo) {
        $part = Get-IdfAppPartitionForOffset -IdfPython $IdfPython -GenEsp32PartPy $genPart -PartitionBin $partBin -ImageOffset $binInfo.Offset
        Write-Host ('Flash image file: {0}' -f (Split-Path $binInfo.BinPath -Leaf))
        Write-Host ('  On-disk size:   {0}' -f (Format-IdfByteSize $binInfo.BinSize))
        if ($part) {
            $remain = $part.Size - $binInfo.BinSize
            $pct = if ($part.Size -gt 0) { 100.0 * $binInfo.BinSize / $part.Size } else { 0 }
            $offStr = '0x{0:X}' -f $part.Offset
            Write-Host ('  App partition:  {0} ({1}), offset {2}, capacity {3}' -f $part.Name, $part.SubType, $offStr, (Format-IdfByteSize $part.Size))
            Write-Host ('  Used in part.:  {0:N1} %' -f $pct)
            if ($remain -ge 0) {
                Write-Host ('  Free in part.:  {0} (approx., before padding rules)' -f (Format-IdfByteSize $remain))
            }
            else {
                Write-Host '  WARNING: image larger than partition; fix partition table or shrink firmware.'
            }
        }
        else {
            Write-Host '  (Partition table missing or no app partition matches flash offset.)'
        }
    }
    else {
        Write-Host '(No build/flash_app_args or .bin; skipped partition comparison.)'
    }

    Write-Host ''
    Write-Host '--- Static usage by memory type (same source as idf.py size) ---'
    foreach ($row in $data.layout) {
        $name = [string]$row.name
        $used = [long]$row.used
        $free = [long]$row.free
        $total = [long]$row.total
        if ($total -gt 0) {
            $usedPct = 100.0 * $used / $total
            $pctStr = '{0:N1} %' -f $usedPct
            Write-Host ('{0,-14} used {1,-22}  free {2,-22}  capacity {3}  ({4})' -f $name, (Format-IdfByteSize $used), (Format-IdfByteSize $free), (Format-IdfByteSize $total), $pctStr)
        }
        else {
            Write-Host ('{0,-14} used {1}' -f $name, (Format-IdfByteSize $used))
        }
    }
    Write-Host ''
    Write-Host 'Note: Flash Code / Flash Data capacity here follows linker+flash config; app partition capacity is separate (see above).'
}
