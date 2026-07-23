# Thin wrapper: run from camera/ directory.
param(
    [ValidateSet("build", "reconfigure", "clean", "fullclean")]
    [string]$Action = "build"
)
& (Join-Path $PSScriptRoot "..\scripts\build_camera.ps1") @PSBoundParameters
