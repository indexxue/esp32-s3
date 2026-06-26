---
name: esp32-s3-coding-standard
description: >-
  Apply ESP32-S3 + ESP-IDF v5.5.4 coding, layering, and build conventions for
  this repository. Use when writing, reviewing, or refactoring C/C++ firmware,
  adding ESP-IDF components, changing bsp_driver/cbb/common/project layout,
  or running idf.py/build.ps1 in this repo.
---

# ESP32-S3 Coding Standard

## Authoritative reference

Read and follow `doc/embedded_coding_standard.md` before making firmware changes. For OTA/partition topics, also check `doc/partition_switch_development_plan.md`.

## Quick checklist

### Layering (dependency direction)

- `project/main` → `common`, `ota`, `bsp_driver`, `cbb`, IDF components
- `common` → `bsp_driver`, `ota` (never reverse)
- `ota` → IDF only (`app_update`, `freertos`；不依赖 `common`，避免循环)
- `bsp_driver` → IDF only (no `common`, avoid `cbb`)
- `cbb` → `bsp_driver` or IDF (no `common`)
- **Forbidden**: `bsp_driver` → `common`, `cbb` → `common`, `ota` → `common`

### Where new code goes

| Content | Location |
|---------|----------|
| Board GPIO/UART/I2C/Timer | `bsp_driver/inc`, `bsp_driver/src` |
| Chip/display/sensor drivers | `cbb/` |
| Hardware-agnostic logic | `common/inc`, `common/src` |
| OTA 写入 / 双槽切换 | `ota/inc`, `ota/src` |
| Product flow / `app_main` | `project/main/` |

New components must be registered via `EXTRA_COMPONENT_DIRS` or equivalent in `project/CMakeLists.txt`.

### Coding essentials

1. Check every `esp_err_t`; no silent failures on init paths.
2. Fixed `TAG` per module; avoid heavy logging in ISR/hot paths.
3. First include in `.c` is the module's own `.h`; then IDF/FreeRTOS, then project, then stdlib.
4. Naming: `snake_case` functions, `_t` typedefs, `s_`/`g_` statics/globals, `UPPER_SNAKE` macros.
5. ISR: minimal work; use FromISR APIs; `IRAM_ATTR` where required; DMA/cache rules per IDF docs.
6. No new compiler warnings; verify with `.\scripts\build.ps1` or `idf.py -C project build`.

### Build commands

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
# or: .\idf.ps1 build
```

Target: `esp32s3`. CMake root: `project/`.
