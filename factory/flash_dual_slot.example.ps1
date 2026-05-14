# Example: flash bootloader, partition table, and both OTA slots (optionally App_B).
# Adjust $Port, $RepoRoot, and image paths. Uses repo IDF Python: python -m esptool (same as flash_app_b.example.ps1).
# Offsets match flash_partition/partitions_16m_n16r8.md (16 MiB flash).

param(
    [string]$Port = "COM3",
    [string]$RepoRoot = ""
)

$ErrorActionPreference = "Stop"
if (-not $RepoRoot) {
    $RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
}

$idfPython = Join-Path $RepoRoot "Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
if (-not (Test-Path $idfPython)) {
    Write-Error "Missing $idfPython — run scripts/setup_env.ps1 or use an ESP-IDF shell with esptool on PATH."
}

$ProjectBuild = Join-Path $RepoRoot "project\build"
$BootloaderBin = Join-Path $ProjectBuild "bootloader\bootloader.bin"
$PartitionBin = Join-Path $ProjectBuild "partition_table\partition-table.bin"
$AppA = Join-Path $ProjectBuild "project.bin"
$AppB = Join-Path $RepoRoot "factory\build\factory.bin"

Write-Host "RepoRoot: $RepoRoot"
Write-Host "PORT: $Port"
Write-Host "Expect: bootloader.bin, partition-table.bin from project build; App_B from factory -> factory.bin"

# Linear offsets (SPI flash), same as flash_partition/partitions_16m_n16r8.md
$OFF_BOOT = 0x00000000
$OFF_PART = 0x00008000
$OFF_NVS = 0x00009000
$OFF_OTADATA = 0x00029000
$OFF_PHY = 0x0002B000
$OFF_APP_A = 0x00030000
$OFF_APP_B = 0x00810000

if (-not (Test-Path $BootloaderBin)) { Write-Error "Missing $BootloaderBin — run idf.py -C project build first." }
if (-not (Test-Path $PartitionBin)) { Write-Error "Missing $PartitionBin — run idf.py -C project build first." }
if (-not (Test-Path $AppA)) { Write-Error "Missing $AppA — run idf.py -C project build first." }

$esptoolArgs = @(
    "--chip", "esp32s3",
    "-p", $Port,
    "-b", "460800",
    "--before", "default_reset",
    "--after", "hard_reset",
    "write_flash",
    "--flash_mode", "dio",
    "--flash_freq", "80m",
    "--flash_size", "16MB",
    "0x$('{0:X}' -f $OFF_BOOT)", $BootloaderBin,
    "0x$('{0:X}' -f $OFF_PART)", $PartitionBin,
    "0x$('{0:X}' -f $OFF_APP_A)", $AppA
)

if (Test-Path $AppB) {
    $esptoolArgs += @("0x$('{0:X}' -f $OFF_APP_B)", $AppB)
    Write-Host "Including App_B from $AppB"
}
else {
    Write-Warning "Skip App_B (not found): $AppB — build with scripts/build_factory.ps1 first."
}

Write-Host "python -m esptool $($esptoolArgs -join ' ')"
& $idfPython -m esptool @esptoolArgs
