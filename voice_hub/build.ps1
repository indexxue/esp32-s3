# Thin wrapper: run from voice_hub/ directory.
param(
    [ValidateSet("build", "reconfigure", "clean", "fullclean")]
    [string]$Action = "build"
)
& (Join-Path $PSScriptRoot "..\scripts\build_voice_hub.ps1") @PSBoundParameters
