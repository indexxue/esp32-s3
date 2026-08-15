# Setup pet_tts: venv + deps + Piper voice models (zh/en).
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

$VenvPython = Join-Path $Root ".venv\Scripts\python.exe"
if (-not (Test-Path $VenvPython)) {
    Write-Host "==> create venv"
    py -3 -m venv .venv
}

Write-Host "==> pip install"
& $VenvPython -m pip install --upgrade pip
& $VenvPython -m pip install -r requirements.txt

$Voices = Join-Path $Root "voices"
New-Item -ItemType Directory -Force -Path $Voices | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $Root "out") | Out-Null

# Prefer hf-mirror (CN); fall back to huggingface.co
$Bases = @(
    "https://hf-mirror.com/rhasspy/piper-voices/resolve/main",
    "https://huggingface.co/rhasspy/piper-voices/resolve/main"
)
$Models = @(
    @{
        Stem = "zh_CN-huayan-medium"
        Rel  = "zh/zh_CN/huayan/medium/zh_CN-huayan-medium"
    },
    @{
        Stem = "en_US-lessac-medium"
        Rel  = "en/en_US/lessac/medium/en_US-lessac-medium"
    }
)

function Download-VoiceFile([string]$RelPath, [string]$OutFile) {
    $lastErr = $null
    foreach ($base in $Bases) {
        $uri = "$base/$RelPath"
        try {
            Write-Host "==> download $RelPath"
            Write-Host "    from $base"
            Invoke-WebRequest -Uri $uri -OutFile $OutFile -UseBasicParsing -TimeoutSec 600
            return
        } catch {
            $lastErr = $_
            Write-Host "    failed: $($_.Exception.Message)"
            if (Test-Path $OutFile) { Remove-Item -Force $OutFile }
        }
    }
    throw "download failed for $RelPath : $lastErr"
}

foreach ($m in $Models) {
    $onnx = Join-Path $Voices ($m.Stem + ".onnx")
    $json = Join-Path $Voices ($m.Stem + ".onnx.json")
    if (-not (Test-Path $onnx)) {
        Download-VoiceFile ($m.Rel + ".onnx") $onnx
    } else {
        Write-Host "==> skip $($m.Stem).onnx (exists)"
    }
    if (-not (Test-Path $json)) {
        Download-VoiceFile ($m.Rel + ".onnx.json") $json
    } else {
        Write-Host "==> skip $($m.Stem).onnx.json (exists)"
    }
}

Write-Host ""
Write-Host "Done. Examples:"
Write-Host "  .\.venv\Scripts\python.exe tts.py synth `"你好`" -e piper -o out\hi_piper.wav"
Write-Host "  .\.venv\Scripts\python.exe tts.py synth `"Hello`" -e edge --voice-key en-f -o out\hi_edge.wav"
Write-Host "  .\.venv\Scripts\python.exe tts.py list-edge"
