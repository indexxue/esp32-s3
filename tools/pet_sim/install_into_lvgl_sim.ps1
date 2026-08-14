# Inject desktop_pet sources into the gitignored LVGL Windows simulator.
# Run after tools/setup_lvgl_sim.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\pet_sim\install_into_lvgl_sim.ps1

$ErrorActionPreference = "Stop"
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$vsDir = Join-Path $repoRoot "tools\lvgl_sim\lv_port_pc_visual_studio"
$simDir = Join-Path $vsDir "LvglWindowsSimulator"
$vcx = Join-Path $simDir "LvglWindowsSimulator.vcxproj"
$cppCandidates = @(
    (Join-Path $simDir "LvglWindowsSimulator.cpp"),
    (Join-Path $simDir "main.cpp")
)
$cpp = $cppCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not (Test-Path $vcx)) {
    Write-Error "Simulator project not found: $vcx`nRun tools\setup_lvgl_sim.ps1 first."
}

$relFromSim = "..\..\..\.."
$sources = @(
    "$relFromSim\desktop_pet\main\source\pet\pet_core\pet_core.c",
    "$relFromSim\desktop_pet\main\source\pet\pet_fs.c",
    "$relFromSim\desktop_pet\main\source\pet\sd_cfg.c",
    "$relFromSim\desktop_pet\main\source\pet\pet_res\pet_res.c",
    "$relFromSim\desktop_pet\main\source\pet\pet_view\pet_view.c",
    "$relFromSim\tools\pet_sim\pet_sim_app.c"
)
$includes = @(
    "$relFromSim\desktop_pet\main\source\pet",
    "$relFromSim\desktop_pet\main\source\pet\pet_core",
    "$relFromSim\desktop_pet\main\source\pet\pet_res",
    "$relFromSim\desktop_pet\main\source\pet\pet_view",
    "$relFromSim\tools\pet_sim"
)

$vcxText = Get-Content -Raw -Path $vcx
$marker = "PET_SIM_HOOK"
if ($vcxText -notmatch $marker) {
    $itemXml = "  <!-- $marker -->`r`n  <ItemGroup>`r`n"
    foreach ($s in $sources) {
        $itemXml += "    <ClCompile Include=`"$s`" />`r`n"
    }
    $itemXml += "    <ClInclude Include=`"$relFromSim\tools\pet_sim\pet_sim_app.h`" />`r`n"
    $itemXml += "  </ItemGroup>`r`n"
    if ($vcxText -match "</Project>") {
        $vcxText = $vcxText -replace "</Project>", ($itemXml + "</Project>")
    } else {
        Write-Error "vcxproj has no </Project>"
    }

    $incJoined = ($includes -join ";") + ";"
    if ($vcxText -match "<AdditionalIncludeDirectories>([^<]*)</AdditionalIncludeDirectories>") {
        $vcxText = [regex]::Replace(
            $vcxText,
            "<AdditionalIncludeDirectories>([^<]*)</AdditionalIncludeDirectories>",
            { param($m)
                $cur = $m.Groups[1].Value
                if ($cur -match "pet_core") { return $m.Value }
                return "<AdditionalIncludeDirectories>$incJoined$cur</AdditionalIncludeDirectories>"
            }
        )
    } else {
        Write-Warning "No AdditionalIncludeDirectories in vcxproj; add include dirs manually (see tools/pet_sim/README.md)."
    }
    Set-Content -Path $vcx -Value $vcxText -Encoding UTF8
    Write-Host "Patched $vcx"
} else {
    Write-Host "vcxproj already has $marker"
}

if ($null -ne $cpp) {
    $cppText = Get-Content -Raw -Path $cpp
    if ($cppText -notmatch "pet_sim_app_create") {
        $hook = @"

    /* $marker */
    {
        extern "C" void pet_sim_app_create(void);
        pet_sim_app_create();
    }

"@
        if ($cppText -match "lv_demo_widgets\s*\(\s*\)\s*;") {
            $cppText = [regex]::Replace($cppText, "lv_demo_widgets\s*\(\s*\)\s*;", $hook.Trim())
        } elseif ($cppText -match "lv_windows_create_display\s*\([^;]+\)\s*;") {
            $cppText = [regex]::Replace(
                $cppText,
                "(lv_windows_create_display\s*\([^;]+\)\s*;)",
                { param($m) $m.Value + "`r`n" + $hook }
            )
        } else {
            Write-Warning "Could not find insertion point in $cpp; add pet_sim_app_create() manually."
        }

        $cppText = [regex]::Replace(
            $cppText,
            "lv_windows_create_display\s*\(\s*([^,]+)\s*,\s*\d+\s*,\s*\d+",
            { param($m) "lv_windows_create_display(" + $m.Groups[1].Value + ", 240, 240" }
        )
        Set-Content -Path $cpp -Value $cppText -Encoding UTF8
        Write-Host "Patched $cpp (240x240 + pet_sim_app_create)"
    } else {
        Write-Host "cpp already calls pet_sim_app_create"
    }
} else {
    Write-Warning "No LvglWindowsSimulator.cpp found; see tools/pet_sim/README.md"
}

$userFile = Join-Path $simDir "LvglWindowsSimulator.vcxproj.user"
$userXml = @"
<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="Current" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Debug|x64'">
    <LocalDebuggerWorkingDirectory>$repoRoot</LocalDebuggerWorkingDirectory>
    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>
  </PropertyGroup>
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Release|x64'">
    <LocalDebuggerWorkingDirectory>$repoRoot</LocalDebuggerWorkingDirectory>
    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>
  </PropertyGroup>
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Debug|Win32'">
    <LocalDebuggerWorkingDirectory>$repoRoot</LocalDebuggerWorkingDirectory>
    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>
  </PropertyGroup>
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Release|Win32'">
    <LocalDebuggerWorkingDirectory>$repoRoot</LocalDebuggerWorkingDirectory>
    <DebuggerFlavor>WindowsLocalDebugger</DebuggerFlavor>
  </PropertyGroup>
</Project>
"@
Set-Content -Path $userFile -Value $userXml -Encoding UTF8
Write-Host "Wrote debugger working directory: $repoRoot"
Write-Host "Done. Open LVGL.sln and run LvglWindowsSimulator."
