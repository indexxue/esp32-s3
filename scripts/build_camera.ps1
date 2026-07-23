param(
    [ValidateSet("build", "reconfigure", "clean", "fullclean")]
    [string]$Action = "build"
)

& (Join-Path $PSScriptRoot "build.ps1") -Project camera @PSBoundParameters
