# Thin wrapper: run from desktop_pet/ directory.
param(
    [ValidateSet("build", "reconfigure", "clean", "fullclean")]
    [string]$Action = "build"
)
& (Join-Path $PSScriptRoot "..\scripts\build_desktop_pet.ps1") @PSBoundParameters
