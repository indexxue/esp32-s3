desktop_pet — ESP32-S3 桌宠（LVGL UI + 板级驱动）

目录：可移植运行时在 main/source/pet/（pet_core / pet_fs / pet_res / pet_view）；
  板端业务在 main/source/{ui,audio,agent,pet_opus,web_pages}.c。本地 build/、managed_components/、
  sdkconfig、dependencies.lock 为生成物，勿提交。

引脚（common/inc/board.h → BOARD_PROFILE_DESKTOP_PET）：
  - I2C 唯一：SCL=18 SDA=8（ES8311 / QMI8685 / IT7259@0x46；无 INT/RST）
  - GC9A01：SCK=7 MOSI=16 MISO=15 CS=6 DC=17 BL=5；无 RST（靠重新上电）
  - I2S：DIN=9 LRCK=10 DOUT=11 BCLK=12 MCLK=13；SPE_EN=14
  - TB6612：PWMA=45 PWMB=38；M1_IN1=46 M1_IN2=3；M2_IN1=21 M2_IN2=47
  - SDMMC：CK=42 CMD=41 D0=2 D1=1 D2=39 D3=40
  - WS2812 RGB_DIN=48；按键=0；电池 ADC=GPIO4

构建 / 烧录：
  idf -Project desktop_pet build
  idf -Project desktop_pet -p PORT flash monitor

板端 LVGL UI（默认）：
  BoardInit 后启动 desktop_pet_ui：拓麻哥奇主界面（Needs 三点 + 身体 + 底弧四钮；五官预留、不显示）
  触摸：点身体 poke；长按喂食；Dock F/P/S/C = 喂/玩/睡/聊（聊→对话页，首版占位）
  IMU：晃一下玩、翻转约 1s 睡觉
  GPIO0 单击：默认无动作（debug 覆盖层已隔离；`DESKTOP_PET_ENABLE_DEBUG_UI=1` 可恢复 Rec/Play/Conn/Talk）
  GPIO0 双击：清除已存 STA，重启进入 SoftAP 配网（SSID ESP32-WebCtrl / esp32web1 → http://192.168.4.1/provision）
  DEBUG 页（可选）：Conn 小智 WS 开/关；Talk 开始/结束听（Opus 上行）
  成功日志：`hello ok` → `LISTENING` → 周期性 `uplink frames=`；说话后可能有 `STT "..."`
  资源包：`/sdcard/config` + `/sdcard/pet/`；网页 `GET /` 可上传 `pet.zip` 覆盖皮肤并重启
  开机：splash C（身体+环）→ Splash Gate（≥1s 且 pack 尝试结束）→ 主界面
  文档入口：doc/desktop_pet/README.md（framework / product / CONTEXT / cloud_asr）
  录音：ES8311 + I2S → PSRAM；Stop 后写 `/sdcard/record/rec_XXXX.wav`（仅 debug UI）
  需 SD 已挂载（FAT32）；无卡时仍可录音，仅保存失败
  依赖：main/idf_component.yml → lvgl/lvgl + esp_websocket_client（managed_components 不入库）
  若 `idf.py reconfigure` 后 WS 又报 Error create websocket task：对 stock
  managed 组件重新应用 patches/esp_websocket_client_psram_stack.patch
  （任务栈改到 PSRAM；见 patch 头注释）。当前树内 managed 文件已含该改动。
  若点击偏移：改 ui.c 里 it7259 panel/swap/invert 配置
  Agent URI：menuconfig「Desktop Pet Xiaozhi Agent」或 sdkconfig.defaults 中 CONFIG_DESKTOP_PET_AGENT_WS_URI

Web（AP/STA 同一 HTTP）：
  SoftAP 默认 http://192.168.4.1/ ；STA 用设备 DHCP IP
  `GET /` SD 文件管理页；`/api/sd/list|file|delete`；另有 `/provision` `/ota`
  需 DESKTOP_PET_ENABLE_WIFI_WEB=1 且 CONFIG_WEB_CTRL_AUTO_START

Bring-up 开关（board.h，改后重编）：
  DESKTOP_PET_ENABLE_LCD / IMU / MOTOR / SDCARD / AUDIO / TOUCH 默认 1
  DESKTOP_PET_ENABLE_DEBUG_UI 默认 0（Rec/Play debug 覆盖层）
  SD：需插卡；FatFS 报 (13)=卡已识别但无合法 FAT → 请格式化为 FAT32（勿用 exFAT）。

OTA：
  idf -Project desktop_pet release 1.0.0

LVGL PC 模拟器（仅本机下载，不入库）：
  powershell -ExecutionPolicy Bypass -File .\tools\setup_lvgl_sim.ps1
  py -3 tools\pet_skin\run_gui.py
  # 依赖：py -3 -m pip install -r tools\pet_skin\requirements.txt
  # 或 CLI：py -3 tools\pet_skin\cli.py --splash
  powershell -ExecutionPolicy Bypass -File .\tools\pet_sim\install_into_lvgl_sim.ps1
  产物目录 tools\lvgl_sim\（已在 .gitignore）
  用 Visual Studio 打开其中 LVGL.sln，跑 LvglWindowsSimulator（240×240）
  详见 tools/pet_sim/README.md ；把 tools/pet_sim/sdcard/ 拷到卡上 `/sdcard/`（含 config + pet/）
  皮肤工具索引：tools/pet_skin/README.md
