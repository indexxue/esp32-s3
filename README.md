# ESP32-S3 Project

This repository tracks source code only. Local ESP-IDF toolchains and build outputs are not committed.

## Prerequisites

- Windows 10/11
- Espressif IDF Tools installed (default local path expected by this repo)

## First-time setup

From the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\setup_env.ps1
```

Then open a new terminal and load ESP-IDF environment:

```cmd
cmd /k ".\Espressif\frameworks\esp-idf-v5.5.4\export.bat"
```

## Build

```cmd
idf.py -C project build
```

## Notes

- `Espressif/` is intentionally ignored in Git. Each developer installs tools locally.
- If you use another ESP-IDF version or custom install path, update `scripts/setup_env.ps1`.
