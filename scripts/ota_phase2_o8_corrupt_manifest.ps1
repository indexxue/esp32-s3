# O8 acceptance helper: copy a release manifest and corrupt sha256 for pull-path rejection test.
# Usage:
#   .\scripts\ota_phase2_o8_corrupt_manifest.ps1 -Version 1.0.7 -Product project
# Then serve the corrupt dir and POST /api/ota/pull with manifest URL pointing at manifest_o8_bad.json.
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [ValidateSet("project", "ballot_guard")]
    [string]$Product = "project",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$srcDir = Join-Path $repoRoot "firmware\$Version"
if (-not (Test-Path $srcDir)) {
    throw "Release dir not found: $srcDir (run idf -Project $Product release $Version first)"
}

$manifestPath = Join-Path $srcDir "manifest.json"
if (-not (Test-Path $manifestPath)) {
    throw "manifest.json not found in $srcDir"
}

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $repoRoot "firmware\_o8_test\$Version"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Copy-Item -Path (Join-Path $srcDir "*.bin") -Destination $OutDir -Force -ErrorAction SilentlyContinue
$json = Get-Content -Raw -Encoding UTF8 $manifestPath | ConvertFrom-Json
$ota = $json.products.$Product.ota
if ($null -eq $ota) {
    throw "products.$Product.ota missing in manifest"
}

$badSha = "0000000000000000000000000000000000000000000000000000000000000000"
$ota.sha256 = $badSha
if ($json.products.$Product.artifacts -and $json.products.$Product.artifacts.Count -gt 0) {
    foreach ($a in $json.products.$Product.artifacts) {
        if ($a.role -eq "app" -and $a.format -ne "intel_hex") {
            $a.sha256 = $badSha
        }
    }
}

$outManifest = Join-Path $OutDir "manifest_o8_bad.json"
$json | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 $outManifest

Write-Host "O8 test artifacts in: $OutDir"
Write-Host "Serve:  python -m http.server 8090 --bind <PC_LAN_IP>  (cd to OutDir)"
Write-Host "Pull:   manifest_url=http://<PC_IP>:8090/manifest_o8_bad.json  product=$Product"
Write-Host "Expect: pull fails, state=idle, run_ver unchanged, serial log: SHA256 mismatch"
