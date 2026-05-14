# SD 卡图片 → ST7789 LCD 显示开发计划

本文与 `start.c` / `main.c` 中的板级初始化衔接：SD 卡在 `BoardInit` 中挂载（`BOARD_SDCARD_MOUNT_POINT`），LCD 为 **ST7789**，当前 `board.c` 固定为 **`ST7789_ROT_LANDSCAPE`（横屏）**。

**当前进度（2026-05）**：阶段 A、B（可选头）、**BMP 子集**、**多图图库**、**条带推屏**与 **`lcd_show_picture` 块写**均已落地；图库逻辑在 **`common/src/lcd_gallery.c`**，应用侧在 `main.c` 中调用 `lcd_gallery_*` 并配合 **GPIO0 单击** 切图。

---

## 1. 目标与约束

| 项目 | 本工程现状 |
|------|------------|
| 面板物理像素 | `ST7789_PANEL_W`×`ST7789_PANEL_H` = **240×135**（见 `cbb/st7789.h`） |
| 当前软件逻辑分辨率 | `st7789_display_width`×`st7789_display_height` = **240×135**（横屏旋转） |
| 挂载点 | `BOARD_SDCARD_MOUNT_POINT`（`/sdcard`） |
| 已有绘图 API | `lcd_show_picture`（`cbb/lcd.h`）；全屏推流亦用 `st7789_set_window` + `st7789_write_pixel_bytes`（图库与 `lcd_show_picture` 内部条带） |

**关键实现细节（避免花屏 / 色偏）**

1. **像素格式**：RGB565 **大端（高字节在前）** 与 `st7789_write_pixel_bytes` 一致（见 `cbb/lcd.c`）。
2. **参数含义**：`lcd_show_picture(lcd, x, y, length, width, pic)` 中 **`length` = 宽**，**`width` = 高**（命名易混，与窗口一致即可）。
3. **行优先**：与 `project/tools/lcd_rgb565_convert.py` 默认 **`--order row`** 一致；列优先 bin 可通过 RGBH 头 `flags` 标记，**图库会拒绝列优先载荷**（须用行优先重导）。
4. **颜色与 MADCTL_BGR**：PC 工具默认 **交换 R/B**；固件 BMP 路径对 BGR 源像素同样做与 bin 一致的通道处理，与实机对齐。
5. **RAM 与 DMA**：图库读 **`.bin`** 时按 **`BOARD_ST7789_SPI_MAX_TX`** 对齐行条带 **`DmaMalloc`** + `fread` + `st7789_write_pixel_bytes`；BMP 解码为 **`row_stride + strip_max`** 一块连续 `DmaMalloc`（见 `lcd_gallery.c`）。
6. **FAT 与路径缓冲**：图库在根目录枚举 **`.bin` / `.bmp`**（`strcasecmp`）；每条路径缓冲 **`LCD_GALLERY_PATH_MAX`（320）** 字节，避免 `snprintf("%s/%s", mount, d_name)` 在 **`-Wformat-truncation`** 下与长文件名（LFN）冲突。仍建议在仅 8.3 环境下使用短主名（见 `doc/sdmmc_fat_io.md` §4）。

---

## 2. 数据形态（分阶段）

### 阶段 A：裸 RGB565 二进制 — **已实现**

| 环节 | 说明 |
|------|------|
| PC | `project/tools/lcd_rgb565_convert.py`：默认 **240×135**、`--fit cover`、`--order row`、默认 **R/B 交换**；根目录 `tools/lcd_rgb565_convert.py` 转发；公共 **`fit_resize_image()`** 供 GUI 复用。 |
| 卡 | 根目录任意 **`.bin`**（内容须为全屏 **240×135×2** 字节或见阶段 B），由图库扫描排序后播放。 |
| 固件 | `common/src/lcd_gallery.c`：裸 bin **条带读取** + `st7789_write_pixel_bytes` 全屏推流。 |

### 阶段 B：带极简头的自定义格式 — **已实现（可选）**

| 环节 | 说明 |
|------|------|
| 格式 | 16 字节头：`RGBH` + 小端 `width/height` + `flags` + `reserved`，载荷为行优先 RGB565 大端（与脚本 **`--with-header`** 一致）。 |
| 固件 | `lcd_gallery` 根据文件长度识别裸载荷或 `16 + w*h*2`；`flags` 含列优先时拒绝。 |

### 阶段 C：标准 BMP — **子集已实现**

| 支持 | 说明 |
|------|------|
| 输入 | **BI_RGB**、**无压缩**、**24 位或 32 位**；正/负 `biHeight`（自下而上 / 自上向下）。 |
| 缩放 | 任意合法宽高 **最近邻映射** 到 **240×135** 全屏（与 PC `cover` 类逻辑不同，为设备端单通路缩放）。 |
| 不支持 | 调色板 8bit、RLE、**BI_BITFIELDS** 等；PNG 仍建议 PC 转 bin/BMP（未片上解码）。 |

**后续路线**：PNG 仅在需要「免 BMP 中间格式」时再评估；图库已覆盖 bin + BMP + 多图产品化路径。

---

## 3. 固件任务（对照实现）

| 任务 | 状态 | 说明 |
|------|------|------|
| 挂载后再读图 | 已做 | `BoardInit` 挂载；仅 `sdcard_get_card() != NULL` 时扫描/显示。 |
| 读文件 + 尺寸校验 | 已做 | `.bin`：裸或带头长度校验；`.bmp`：头字段与 `BI_RGB` 校验。 |
| 全屏显示 | 已做 | `lcd_gallery_show_index` → RGB565 条带或 BMP 行解码推流。 |
| 阶段 B 头 | 已做 | 见上表；与 `lcd_rgb565_convert.py --with-header` 对齐。 |
| 条带 DMA / 省内存 | 已做 | `.bin` 路径单条带缓冲，峰值约 **`~32 KiB` 量级**（与 `BOARD_ST7789_SPI_MAX_TX` 对齐）。 |
| **`lcd_show_picture` 块写 SPI** | 已做 | `cbb/lcd.c` 内按多行打包 `st7789_write_pixel_bytes`（见 `LCD_FAST_FILL_BLK` 思路）。 |
| 独立模块 | 已做 | **`lcd_gallery`**（`common/inc/lcd_gallery.h` + `common/src/lcd_gallery.c`）。 |
| 多图切换 | 已做 | 根目录 **`.bin`/`.bmp`** 排序列表；**GPIO0 单击** 下一张（与 `start.c` 灯效共存）；`main.c` 内 **~50 ms** 轮询按键、**~500 ms** 更新倾角条。 |
| 与 benchmark / 倾角 UI 共存 | 已注意 | 首帧图库在烟测后；循环内 QMI + 底部文字。 |
| 测试矩阵 | 部分 | 主路径 bin/BMP/多图；无卡、空目录、格式错误有日志。 |

---

## 4. PC 端工具

- **命令行**：`project/tools/lcd_rgb565_convert.py`（根目录 `tools/lcd_rgb565_convert.py` 转发）；**`--with-header`**、**`fit_resize_image()`** 等见脚本 `--help`。
- **可视化**：`tools/lcd_image_tool.py`（Tk）：选图、**旋转 0/90/180/270°**（逆时针，Pillow）、Fit、行/列序、R/B 交换、输出 **裸 bin / 带头 bin / BMP24**；默认输出到 **`tools/`** 目录，**短文件名**（如 `l{MMDDHHMMSS}_{slug}.bmp`，旋转非 0 时带 `_r{角度}`）。
- **依赖**：`tools/requirements-lcd-image.txt` / `project/tools/requirements-lcd-image.txt`（Pillow）；GUI 另需 **tkinter**。

**常用命令（在 `project` 目录）：**

```text
python tools\lcd_rgb565_convert.py tmp\icon.png -o img01.bin
```

**GUI（在仓库根目录）：**

```text
python tools\lcd_image_tool.py
```

相对路径在 cwd 下找不到时，命令行脚本会再尝试 **仓库根目录** 下的同名相对路径。

---

## 5. 与生命周期文档的关系

板级初始化顺序与模块职责见 `doc/application_architecture.md`；SD 读写与 DMA 习惯见 `doc/sdmmc_fat_io.md`。本计划描述「资源准备 → 图库枚举 → 像素推屏」闭环；**bin + RGBH + BMP 子集 + 多图** 已在当前仓库闭环。

---

## 6. 里程碑小结

| 里程碑 | 交付物 | 状态 |
|--------|--------|------|
| M0 | 本文档 + `lcd_rgb565_convert.py` 可生成 **240×135** 测试 bin | **完成** |
| M1 | 固件读 SD 根目录 **`.bin`** 并全屏显示 | **完成**（图库 + 条带） |
| M2 | 条带推流 + **RGBH 头**（阶段 B）+ **`lcd_show_picture` 块写** | **完成** |
| M3 | **BMP BI_RGB 24/32** + **按键多图** | **完成** |

**结论**：SD 卡资源在 **240×135 横屏** 下的主路径（RGB565 / 带头 bin / BMP、多图切换、PC 命令行 + GUI）已落地；后续可按需加强 **PNG**、**自动化测试** 或 **图库与灯效按键策略** 等产品细节。
