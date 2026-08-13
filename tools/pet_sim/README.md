# Desktop pet LVGL simulator overlay

`tools/lvgl_sim/` is gitignored (clone via [`setup_lvgl_sim.ps1`](../setup_lvgl_sim.ps1)). This folder is the **in-repo** port: pack files + `pet_sim_app.c`.

## Run

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\setup_lvgl_sim.ps1
py -3 tools\pet_pack\pack_build.py
powershell -ExecutionPolicy Bypass -File .\tools\pet_sim\install_into_lvgl_sim.ps1
```

Open `tools\lvgl_sim\lv_port_pc_visual_studio\LVGL.sln`, set **LvglWindowsSimulator** as startup, F5.

Debugger working directory is the **repo root** so `tools/pet_sim/sdcard/pet` resolves. Optional: `PET_PACK_ROOT`.

Keys: `1` feed, `2` play, `3` sleep, `4` wake, `0` shake. On-screen Feed / Play / Sleep also work. Display 240×240.

## Manual VS hook (if the script cannot patch)

After `lv_windows_create_display(...)`:

```cpp
extern "C" void pet_sim_app_create(void);
pet_sim_app_create();
```

Do not call `lv_demo_widgets()`. Add compile units:

- `desktop_pet/main/source/pet/pet_core/pet_core.c`
- `desktop_pet/main/source/pet/pet_fs.c`
- `desktop_pet/main/source/pet/pet_res/pet_res.c`
- `desktop_pet/main/source/pet/pet_view/pet_view.c`
- `tools/pet_sim/pet_sim_app.c`

Include dirs: `desktop_pet/main/source/pet`, `.../pet_core`, `.../pet_res`, `.../pet_view`, `tools/pet_sim`.

## Smoke-test the brain (no LVGL)

```powershell
gcc -I desktop_pet/main/source/pet/pet_core desktop_pet/main/source/pet/pet_core/pet_core.c tools/pet_sim/pet_core_smoke.c -o tools/pet_sim/pet_core_smoke.exe
.\tools\pet_sim\pet_core_smoke.exe
```

Copy `tools/pet_sim/sdcard/pet/` onto the device as `/sdcard/pet/`.
