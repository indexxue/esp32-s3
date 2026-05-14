# 将 factory 工程编译产物 factory.bin 烧写到 app_b（ota_1）起始地址 0x810000。
# 量产 app_a 仍用 project 工程 idf.py flash 或原有脚本；本脚本只写 B 槽。
param(
    [string]$Port = "COM13"
)

$ErrorActionPreference = "Stop"
$RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$Bin = Join-Path $RepoRoot "factory\build\factory.bin"
$idfPython = Join-Path $RepoRoot "Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"

if (-not (Test-Path $Bin)) {
    Write-Error "Missing $Bin — run scripts/build_factory.ps1 (or idf.py -C factory build) first."
}
if (-not (Test-Path $idfPython)) {
    Write-Error "Missing $idfPython — install repo ESP-IDF env (scripts/setup_env.ps1) or run from an ESP-IDF shell: python -m esptool ..."
}

$OFF_APP_B = 0x00810000

Write-Host "Flash $Bin -> 0x$('{0:X}' -f $OFF_APP_B) PORT=$Port (via $idfPython -m esptool)"

& $idfPython -m esptool --chip esp32s3 -p $Port -b 460800 --before default_reset --after hard_reset write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 16MB `
    "0x$('{0:X}' -f $OFF_APP_B)" $Bin
