# SD 卡视频 → ST7789 LCD 播放开发计划

**版本**：1.1  
**依据**：`doc/sd_lcd_bitmap_plan.md`（图库已闭环）、`doc/sdmmc_fat_io.md`、`doc/application_architecture.md`、`flash_partition/partitions_16m_n16r8.md`、`components/web_ctrl`  
**目标**：在 **不破坏现有静态图库** 的前提下，从 SD 卡读取视频文件，在 **240×135 横屏** ST7789 上连续播放。

**当前进度（2026-06）**：

| 项 | 状态 |
|----|------|
| N16R8 平台配置（PSRAM + CPU 240MHz） | **已完成，实机已验证** |
| M0 单帧 JPEG 硬解 + FPS 基准 | **未开始** |
| M1+ 播放器模块与业务集成 | **未开始** |

---

## 0. 当前工程基线（可复用能力）

| 能力 | 现状 | 视频播放可复用点 |
|------|------|------------------|
| SD 卡 | `sdcard_mount("/sdcard")`，4 线 SDMMC 40MHz，FAT/VFS | `fopen`/`fread` 顺序读；`DmaMalloc` 大块缓冲 |
| LCD | ST7789 240×135，`st7789_set_window` + `st7789_write_pixel_bytes` | 图库 **条带推屏**（`lcd_gallery.c` → `stream_rgb565_payload`） |
| 显示内容 | `lcd_gallery`：`.bin`/`.bmp` 静态图，GPIO0 单击切图 | 枚举/路径缓冲/排序逻辑可借鉴 |
| 任务模型 | `app_mod`（prio 4）独占 LCD+SD；按键 prio 5 | 视频播放宜为 **同一所有者** 下的子状态机 |
| 远程控制 | `cmd.c` + `web_ctrl` 队列执行 | 新增 `video play/stop` 命令与 HTTP API |
| 芯片 / 内存 | ESP32-S3 **N16R8**：16MB Flash + **8MB OPI PSRAM 已启用**（见 §0.1） | JPEG 输入/ RGB565 全帧 / 双缓冲放 PSRAM；DMA 条带仍用内部池 |
| CPU | **240MHz**（`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`） | AVI 解析、调度、与 WiFi 并存 |
| 编译优化 | **Performance**（`CONFIG_COMPILER_OPTIMIZATION_PERF`） | SD/LCD 路径吞吐；`-Wstringop-truncation` 已在 `web_ctrl` 中处理 |

**图库计划结论**（`sd_lcd_bitmap_plan.md` §6）：静态图路径已闭环。视频是 **时间维度的扩展**，不建议塞进 `lcd_gallery` 做 BMP 逐帧解码（太慢、RAM 不够），应 **独立播放器模块** `lcd_video`。

### 0.1 N16R8 平台配置（已落地）

配置写入 **`project/sdkconfig.defaults`**（`factory/sdkconfig.defaults` 同步 PSRAM/CPU 项）。**注意**：仅改 defaults **不会**覆盖已有 `project/sdkconfig` 中的旧符号；若 menuconfig 曾保存过「PSRAM 关闭」，需 **删除或重生成 `sdkconfig` 后 `idf.py reconfigure`**，再全量编译烧录。

**`project/sdkconfig.defaults` 中与视频相关片段：**

```properties
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_TYPE_AUTO=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_COMPILER_OPTIMIZATION_PERF=y
```

Flash 仍为 **DIO @ 80MHz**（与 WROOM-1 N16R8 四线 Flash 模组匹配）；未开 Oct Flash 120MHz（对 MJPEG 读卡/推屏路径收益有限）。

**实机启动日志验收（2026-06-14 已通过）** — 复位后串口应出现：

```text
I (...) octal_psram: ...
I (...) esp_psram: Found 8MB PSRAM device
I (...) esp_psram: Speed: 80MHz
I (...) esp_psram: SPI SRAM memory test OK
I (...) esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator
I (...) cpu_start: cpu freq: 240000000 Hz
I (...) esp_psram: Reserving pool of 32K of internal memory for DMA/internal allocations
```

`heap_init` 在 PSRAM 入堆前后会打印内部 **RAM/DRAM** 区；**不必**要求出现字面 `SPIRAM` 行，以 **`Adding pool of 8192K`** 为准。

工程内 **尚无** 应用层 `mem` 命令或 `BoardInit` 堆统计；日常以 **上电 ESP-IDF 启动日志** 核对。可选后续在 `cmd.c` 增加 `mem` 打印 `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`。

---

## 1. 目标与硬约束

### 1.1 产品目标（首版）

- SD 卡根目录（或子目录 `videos/`）放置视频文件，上电或按键/Web 触发播放。
- 全屏 **240×135**，与当前横屏 MADCTL 一致。
- 支持 **循环播放**、**停止回到图库**、**播放中 GPIO0 停止**（避免与「切下一张图」冲突，需定义模式）。
- 顶栏网络/电量：播放时可 **暂停刷新** 或 **每 N 帧刷新一次**（避免叠影与 SPI 争抢）。

### 1.2 非目标（首版明确不做）

- H.264/MP4 软解（ESP32-S3 无硬件 H.264，软解不可实时）。
- 带音轨同步（板级暂无 ES8311/I2S 代码；见可选阶段 F）。
- 网页边下边播、Range 流式（首版只做 **整文件本地顺序读**）。
- 任意分辨率源视频的 **实时高质量缩放**（首版要求 **PC 预缩放到 240×135**）。

### 1.3 性能预算（用于选型）

| 环节 | 量级 | 说明 |
|------|------|------|
| 全帧 RGB565 | 240×135×2 ≈ **63.3 KiB/帧** | 与图库一致 |
| SPI 推屏 @40MHz | 理论 ~2–4 MiB/s 有效 | 条带写，与图库相同路径；M3 可试 60–80MHz |
| FAT 顺序读 | 烟测/基准已有（128KiB chunk） | MJPEG 压缩后读带宽通常不是瓶颈 |
| JPEG 硬解 240×135 | 约 **5–20 ms/帧**（质量相关） | 依赖 `esp_driver_jpeg`（IDF 5.5.4 已编入工程组件树） |
| **合理首版帧率** | **8–15 FPS**（目标 12 FPS） | PSRAM/240MHz 已就绪；待 M0 实测校准 |

**平台提速对帧率的预期**（在 M0 实测前）：PSRAM 使大帧缓冲与 WiFi 共存成为可能；240MHz + PERF 对解析/调度约有 **10–20%** 余量；**SPI/SD 与 MJPEG 码率** 仍为上限，勿指望仅靠提频翻倍 FPS。

---

## 2. 视频格式选型（推荐路径）

### 2.1 主路径：**MJPEG in AVI**（推荐 M1 交付）

| 项 | 选择理由 |
|----|----------|
| 容器 | AVI + `movi` 内连续 JPEG 帧；结构简单，可 **流式解析**（无需索引全载入 RAM） |
| 编码 | Motion JPEG，每帧独立 JPEG → 可用 **ESP32-S3 JPEG 硬件解码** |
| 音频 | 首版 **`-an` 无音轨**，降低解析复杂度 |
| PC 转码 | FFmpeg 一行命令，可脚本化 |

**推荐转码命令（写入 `project/tools/` 脚本）：**

```bash
ffmpeg -i input.mp4 -vf "scale=240:135:force_original_aspect_ratio=decrease,pad=240:135:(ow-iw)/2:(oh-ih)/2" \
  -c:v mjpeg -q:v 8 -r 12 -an output.avi
```

- 分辨率：**240×135**（与面板一致，解码后 **无需缩放**）。
- 帧率：**12 FPS** 为首版目标；文件过大则 `-q:v 10` 或降 `-r`。

### 2.2 备选路径 A：**自定义 MJPEG 裸流**（`MJPG` 头 + 帧长 + JPEG 载荷）

- 比 AVI 解析更简单，适合 **自研 PC 工具**。
- 缺点：非标准，需自写 `lcd_mjpg_convert.py`。
- 建议在 M1 若 AVI 解析耗时超预期时再并行。

### 2.3 备选路径 B：**RGB565 帧序列**（`.bin` 动画 / 自定义 `RGBV` 容器）

- 直接复用 `stream_rgb565_payload` 逻辑，**零解码**。
- 缺点：体积巨大（63 KiB×帧数×时长），仅适合 **短循环动画**（<5s）。
- 可作为 **M5 可选**：与图库共用条带推屏，容器头类似 RGBH。

**结论**：首版以 **MJPEG AVI** 为主；RGB565 动画作为低复杂度备选。

---

## 3. 软件架构设计

### 3.1 模块划分

```text
common/inc/lcd_video.h
common/src/lcd_video.c      ← 新建：AVI 解析、帧调度、JPEG 解码、推屏
common/inc/lcd_display.h    ← 可选：LCD 互斥 +「当前模式」枚举
project/tools/avi_mjpeg_convert.ps1
doc/sd_lcd_video_playback_plan.md
```

**不要**把视频循环塞进 `lcd_gallery.c`；通过 **显示模式** 互斥协作：

```c
typedef enum {
    LCD_UI_MODE_GALLERY,
    LCD_UI_MODE_VIDEO,
} lcd_ui_mode_t;
```

### 3.2 数据通路

```text
SD: fopen(avi) → AVI 解析器读 movi 块
    → 每帧 JPEG 字节块（优先 PSRAM：heap_caps_malloc(..., MALLOC_CAP_SPIRAM)）
    → esp_driver_jpeg 硬解 → RGB565 大端（输出缓冲宜 PSRAM）
    → 与图库相同的 rgb565_strip → st7789_write_pixel_bytes（条带缓冲用 DmaMalloc / 内部 DMA 池）
    → esp_timer 节拍 / 帧耗时补偿 → 下一帧
```

与 `sdmmc_fat_io.md` 一致：播放期间 **仅 FAT 读**，不与 `sdmmc_read_sectors` 裸扇区并发。

**缓冲归属约定**：

| 数据 | 推荐分配能力 | 原因 |
|------|----------------|------|
| JPEG 压缩帧、解码后全帧 RGB565 | `MALLOC_CAP_SPIRAM` | 体积大，数量多 |
| SPI 推屏条带 | `MALLOC_CAP_DMA`（`DmaMalloc`） | ST7789 与 SDMMC DMA 路径 |
| WiFi/LwIP | 已配置 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | 减轻内部 RAM 压力 |

### 3.3 任务与优先级（对齐 `application_architecture.md`）

| 方案 | 说明 | 推荐 |
|------|------|------|
| A. 在 `app_mod` 内状态机 | 与图库同任务，切换 `mode` | **首版推荐** — 天然满足 LCD/SD 单一所有者 |
| B. 独立 `lcd_video` 任务 | 需 `xSemaphore` 保护 SPI + FAT | 帧率稳定后再拆 |

**播放循环伪逻辑**（在 `application_modules_task` 中）：

1. 非播放：`vTaskDelay(50ms)` + 图库按键逻辑（保持现状）。
2. 收到 `PLAY` 命令：切 `LCD_UI_MODE_VIDEO`，`lcd_gallery` 不再刷屏。
3. `lcd_video_play(path)` **阻塞循环**直至结束/停止；内部每帧 `taskYIELD()` 避免饿死 IDLE/WDT。
4. 结束：切回 `LCD_UI_MODE_GALLERY`，恢复当前索引静态图 + 顶栏。

### 3.4 内存与 sdkconfig（N16R8）

| 缓冲 | 建议 | 说明 |
|------|------|------|
| JPEG 输入 | 1× 最大帧大小（PSRAM，如 32–64 KiB） | AVI 帧长不一，按帧扩容或上限截断 |
| RGB565 输出 | 1× 全帧 63 KiB（PSRAM） | 硬解输出；推屏仍 **条带** 拷入 DMA 缓冲 |
| 双缓冲 | M3 优化再加 | 读下一 JPEG ∥ 解当前帧 |

**平台 sdkconfig 清单：**

| 配置项 | 状态 | 备注 |
|--------|------|------|
| OPI PSRAM 8MB @ 80MHz | **已完成** | `sdkconfig.defaults` + 实机日志 |
| `SPIRAM_USE_MALLOC` | **已完成** | 8192K 入堆 |
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | **已完成** | 与 `web_ctrl` 并存 |
| CPU 240MHz | **已完成** | 启动日志 `240000000 Hz` |
| `COMPILER_OPTIMIZATION_PERF` | **已完成** | 构建需通过 `-Wstringop-truncation`（`web_ctrl` 已修） |
| `common/CMakeLists.txt` 链接 `esp_driver_jpeg` | **待 M0** | IDF 组件树已含驱动，应用模块待接 |
| JPEG 调试日志关闭 | **待 M0** | menuconfig 保持默认即可 |

---

## 4. API 与交互设计

### 4.1 C API（`lcd_video.h` 草案）

```c
status_t lcd_video_scan(void);                    /* 枚举 /sdcard/videos/*.avi */
uint8_t  lcd_video_count(void);
status_t lcd_video_play_path(st7789_t *lcd, const char *path);  /* 阻塞至结束 */
status_t lcd_video_play_index(st7789_t *lcd, uint8_t idx);
void     lcd_video_request_stop(void);            /* 异步停止，播放循环内轮询 */
bool     lcd_video_is_playing(void);
```

### 4.2 按键策略（避免与图库冲突）

| 模式 | GPIO0 单击 | 建议 |
|------|------------|------|
| 图库 | 下一张图（现状） | 保持 |
| 视频播放中 | **停止** 并回图库 | 替代切图 |
| 图库 | GPIO0 **双击** → 播放当前槽位视频 | 可选 M2 |

### 4.3 串口命令（扩展 `cmd.c`）

| 命令 | 行为 |
|------|------|
| `video list` | 列出可播放文件 |
| `video play <name>` | 投递到 `app_mod` 或设标志位启动 |
| `video stop` | `lcd_video_request_stop()` |
| `video play_i <n>` | 按索引播放 |
| `mem`（可选） | 打印 internal / SPIRAM 剩余堆，便于现场核对 |

实现方式与现有 `lcd_show` 命令一致：在 `cmd.c` 解析，**实际播放在 `app_mod`**（可用 `volatile` 标志 + 路径缓冲，或 **FreeRTOS 队列** 传递 `lcd_video_cmd_t`）。

### 4.4 Web API（阶段 M4，对齐 `web_bmp_upload` 模式）

| 端点 | 方法 | 说明 |
|------|------|------|
| `/api/video/list` | GET | JSON 文件列表 |
| `/api/video/play` | POST | `{"name":"demo.avi"}` → 队列，短超时应答 |
| `/api/video/stop` | POST | 停止播放 |

HTTP handler **只入队**，禁止在回调里解码/刷屏（`web_control_http_server_plan.md` §1.2）。

---

## 5. 分阶段实施（WBS）

### 阶段 M0：可行性验证（1–2 天）

| 任务 | 交付 | 状态 |
|------|------|------|
| `sdkconfig.defaults` 开启 OPI PSRAM + 240MHz + PERF | 见 §0.1；`factory/sdkconfig.defaults` 同步 | **完成** |
| 实机启动日志核对 PSRAM/CPU | `Found 8MB` + `8192K` + `240000000 Hz` | **完成** |
| `common/CMakeLists.txt` 增加 `esp_driver_jpeg` 依赖 | 链接 JPEG 硬解 | 未开始 |
| 单帧基准 | SD 上 JPEG 或 AVI 首帧 → 硬解 → 全屏 | 未开始 |
| 连续帧 FPS | 100 帧 MJPEG（可无 AVI 壳）统计 `avg_fps` | 未开始 |
| 分段耗时日志 | 读 SD / 解码 / 推屏 各段 ms | 未开始 |

**验收**：串口 `avg_fps >= 8`（240×135，q≈8）。平台项已满足；**剩余为应用侧 JPEG 管线**。

### 阶段 M1：最小 AVI 播放器（3–5 天）

| 任务 | 交付 |
|------|------|
| `lcd_video.c` AVI 子集解析 | 支持 `hdrl` + `movi`，**idx1 可选**；仅 **MJPEG** FourCC |
| 播放循环 | `lcd_video_play_path` 阻塞播放 + `request_stop` |
| 集成 `main.c` | 图库与视频模式切换；播放时跳过图库按键 |
| PC 脚本 | `project/tools/avi_mjpeg_convert.ps1` + 样例 `demo.avi` 说明 |

**验收**：卡上 `demo.avi` 循环播放 ≥30s 无花屏、无 WDT；停止后回到静态图。

### 阶段 M2：产品化播放器（3–4 天）

| 任务 | 交付 |
|------|------|
| 多文件枚举 + 排序 | `/sdcard/videos/` 或根目录 `.avi` |
| `cmd.c` 命令 | `video list/play/stop` |
| 错误处理 | 文件不存在、非 MJPEG、解码失败 → 日志 + 回图库 |
| 顶栏策略 | 播放中每 60 帧或停止后刷新网络/电量 |

**验收**：串口 `video play demo.avi` 与按键停止均可用。

### 阶段 M3：性能优化（可选，2–3 天）

| 任务 | 说明 |
|------|------|
| 双缓冲流水线 | 读帧 ∥ 解码（PSRAM 双块） |
| 条带推屏优化 | 复用 `rgb565_strip_bytes()` |
| SPI 40→60MHz 探测 | 参考 `board.h` 注释，花屏则回退 |
| FAT 读缓冲放大 | 单次 `fread` 对齐 16–32 KiB |
| PSRAM 120MHz（可选） | 仅 WROOM-2 等硬件确认后；Oct PSRAM 120M 为实验特性 |

**目标**：稳定 **12–15 FPS**。

### 阶段 M4：Web 与上传（可选，3 天）

| 任务 | 说明 |
|------|------|
| `web_video.c` | 仿 `web_bmp_upload.c` 列表/播放/停止 |
| 上传 AVI | multipart 上传至 `/sdcard/videos/`（注意文件大小与超时） |

### 阶段 M5：RGB565 动画容器（可选）

- 自定义 `RGBV` 头 + 帧表 + 裸 RGB565 载荷。
- 复用图库条带逻辑，适合 **无 JPEG 解码开销** 的短动画。

### 阶段 F：音视频同步（远期）

- 需先落地 ES8311/I2S 录音回放方案（板级驱动与文档）。
- AVI 含 PCM 时需 **独立 I2S 任务** + 时间戳；首版不做。

---

## 6. PC 端工具链

| 工具 | 路径 | 作用 | 状态 |
|------|------|------|------|
| 转码脚本 | `project/tools/avi_mjpeg_convert.ps1` | 批量生成 240×135 MJPEG AVI | 未开始 |
| 依赖 | 系统安装 **FFmpeg** | 文档写明版本与命令 | — |
| 可选 GUI | 扩展 `tools/lcd_image_tool.py` | 增加「导出预览 AVI」标签页 | 未开始 |

**卡上目录约定（建议）**：

```text
/sdcard/
  img01.bmp          ← 现有图库
  videos/
    demo.avi         ← 视频
```

图库继续扫根目录 `.bmp`/`.bin`；视频扫 `videos/*.avi`，避免枚举混淆。

**常用转码（在 PC 上）：**

```text
ffmpeg -i input.mp4 -vf "scale=240:135:force_original_aspect_ratio=decrease,pad=240:135:(ow-iw)/2:(oh-ih)/2" -c:v mjpeg -q:v 8 -r 12 -an demo.avi
```

---

## 7. 风险与对策

| 风险 | 对策 |
|------|------|
| PSRAM 未开 → 解码 OOM | **已缓解**：defaults + 启动日志验收；新环境复制 §0.1 |
| `sdkconfig.defaults` 不覆盖旧 `sdkconfig` | 改 defaults 后 **reconfigure**；必要时删 `sdkconfig` 再生成；保留 `sdkconfig.bak` 对照 |
| WiFi + 视频争抢 CPU/总线 | `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 已开；播放时顶栏少刷新、限制大文件 Web 上传 |
| PERF 构建 `-Werror=stringop-truncation` | WiFi/图库路径用限宽 `snprintf`（`web_ctrl` 已处理）；新代码遵循同模式 |
| AVI 变种多 | 首版只支持 FFmpeg 默认 MJPEG AVI；文档写明 **不支持** OpenDML 超大索引 |
| GPIO0 语义冲突 | 模式化按键；文档与 Web UI 标注 |
| 长文件名 / LFN | 路径缓冲 ≥320 字节（与 `LCD_GALLERY_PATH_MAX` 一致） |
| 看门狗 | 播放循环每帧 `vTaskDelay(1)` 或 `esp_task_wdt_reset`（若订阅 WDT） |

---

## 8. 测试矩阵

| 用例 | 预期 |
|------|------|
| 上电 PSRAM/CPU 日志 | §0.1 关键行齐全 |
| 无 SD 卡 | `video play` 返回错误，保持图库 |
| 空 `videos/` | 列表为空，日志 WARN |
| 12 FPS / 30s AVI | 无明显卡顿（无音轨） |
| 播放中 `video stop` | <500ms 停并回图库 |
| 播放中拔卡 | 读失败优雅退出，不挂死 |
| Web `POST play` + 串口 `video play` | 互斥：后到的排队或拒绝（503） |
| 与 LED 场景 / 按键 | 灯效任务不受影响（prio 5） |

---

## 9. 里程碑小结

| 里程碑 | 交付物 | 状态 |
|--------|--------|------|
| **M0a** | N16R8：`sdkconfig.defaults` + 实机 PSRAM/240MHz 日志 | **完成** |
| **M0b** | 单帧 JPEG 硬解全屏 + FPS 分段日志 | 未开始 |
| **M1** | `lcd_video` + AVI 循环播放 + PC 转码脚本 | 未开始 |
| **M2** | cmd 控制 + 多文件 + 模式切换 | 未开始 |
| **M3** | 12+ FPS 优化 | 未开始 |
| **M4** | Web list/play/stop | 未开始 |
| **M5** | RGB565 动画（可选） | 未开始 |

---

## 10. 建议的下一个 PR 范围

平台配置已合入；**下一 PR 建议仅 M0b + M1**：

1. `common/CMakeLists.txt`：`esp_driver_jpeg`（或封装模块）依赖  
2. `common/src/lcd_video.c` + `lcd_video.h`（含 M0b 单帧/FPS 基准入口，可用 `cmd` 或编译宏触发）  
3. `main.c`：最小 `LCD_UI_MODE` 切换  
4. `project/tools/avi_mjpeg_convert.ps1`  
5. **不改** 图库枚举规则、**不改** Web（留 M4）

（**勿重复**提交已完成的 `sdkconfig.defaults` / `factory/sdkconfig.defaults`，除非再调 PSRAM 频率或 CPU。）

---

## 11. 与现有文档关系

| 文档 | 关系 |
|------|------|
| `sd_lcd_bitmap_plan.md` | 静态图已闭环；视频为并行能力 |
| `sdmmc_fat_io.md` | 顺序读、DMA 缓冲、禁止裸扇区并发 |
| `application_architecture.md` | 单一所有者、队列、任务优先级 |
| `web_control_http_server_plan.md` | HTTP 只入队，长任务在 `app_mod` |
| `flash_partition/partitions_16m_n16r8.md` | N16 Flash 分区；R8 PSRAM 由 sdkconfig 管理 |

---

## 12. 变更记录

| 版本 | 日期 | 说明 |
|------|------|------|
| 1.0 | 2026-06 | 初版计划 |
| 1.1 | 2026-06-14 | N16R8 平台配置落地并实机验证；M0 拆为 M0a/M0b；补充启动日志验收、缓冲归属、sdkconfig 合并说明、下一 PR 范围 |

---

**结论**：**SD + ST7789 图库 + Web/命令行 + N16R8（PSRAM/240MHz）** 基础已就绪。下一工作重点是 **M0b JPEG 硬解基准** 与 **M1 MJPEG AVI 最小播放器**；路线仍为 **PC 预转 240×135 AVI + 固件硬解 + 条带推屏**，首版目标 **8–12 FPS**、无音轨。
