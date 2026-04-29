# ESP32-S3 Project

This repository tracks source code only. Local ESP-IDF toolchains and build outputs are not committed.

## Prerequisites

- Windows 10/11
- Git for Windows (required for zero-install bootstrap)

## First-time setup

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_env.ps1
```

The setup script supports zero-install flow:

- If ESP-IDF is missing, it auto-clones `v5.5.4` to `.\Espressif\frameworks\esp-idf-v5.5.4`
- Then it runs `install.bat` to install/update toolchain dependencies

Then open a new terminal and load ESP-IDF environment:

```cmd
cmd /k ".\Espressif\frameworks\esp-idf-v5.5.4\export.bat"
```

## Build

```cmd
idf.py -C project build
```

If your current directory is `project`, use:

```cmd
idf.py build
```

## Verify setup script

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1
```

Optional full checks:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_env.ps1 -RunInstall -RequireIdfInPath -RunBuildTest
```

## Notes

- `Espressif/` is intentionally ignored in Git. Each developer installs tools locally.
- If you use another ESP-IDF version or custom install path, update `scripts/setup_env.ps1`.
