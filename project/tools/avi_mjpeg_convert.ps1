#Requires -Version 5.1
<#
.SYNOPSIS
  将视频转为 ESP32-S3 ST7789 240x135 MJPEG AVI（无音轨）。

.DESCRIPTION
  依赖系统 PATH 中的 ffmpeg。输出适合 lcd_video 模块硬解/软解播放。
  复制到 SD 卡：/sdcard/videos/<name>.avi

.PARAMETER Input
  源视频路径（mp4/mkv/...）

.PARAMETER Output
  输出 AVI 路径（默认 demo.avi）

.PARAMETER Fps
  帧率（默认 12）

.PARAMETER Quality
  MJPEG 质量 1-31，越小越好（默认 8，对应 ffmpeg -q:v 8）

.EXAMPLE
  .\avi_mjpeg_convert.ps1 -Input clip.mp4 -Output demo.avi
#>
param(
    [Parameter(Mandatory = $true)]
    [string] $Input,

    [string] $Output = "demo.avi",

    [int] $Fps = 12,

    [int] $Quality = 8
)

$ErrorActionPreference = "Stop"

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Error "ffmpeg not found in PATH. Install FFmpeg and retry."
}

if (-not (Test-Path -LiteralPath $Input)) {
    Write-Error "Input not found: $Input"
}

$vf = "scale=240:135:force_original_aspect_ratio=decrease,pad=240:135:(ow-iw)/2:(oh-ih)/2"

Write-Host "Converting -> $Output (${Fps}fps, q:v $Quality) ..."

& ffmpeg -y -i $Input `
    -vf $vf `
    -c:v mjpeg -q:v $Quality -r $Fps -an `
    $Output

if ($LASTEXITCODE -ne 0) {
    Write-Error "ffmpeg failed with exit code $LASTEXITCODE"
}

Write-Host "Done. Copy to SD: /sdcard/videos/$(Split-Path -Leaf $Output)"
