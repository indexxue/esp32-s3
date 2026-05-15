# ESP32-S3 连接 ES8311：录音与播放开发计划

**版本**：1.3  
**角色定位**：基于 ESP-IDF 的 **硬件接线 + 驱动选型 + 分阶段 WBS**，与 [`doc/application_architecture.md`](application_architecture.md)（任务/分层）、[`doc/embedded_coding_standard.md`](embedded_coding_standard.md) 对齐。  
**Codec**：ES8311（I²S 数字音频 + I²C 控制；内置 ADC/DAC，可接麦克风与线路输出）。

---

## 1. 需求与边界

### 1.1 目标能力

| 能力 | 说明 |
|------|------|
| **播放** | ESP32-S3 经 I²S 向 ES8311 送 PCM，经内部 DAC 与外部功放（NS4150B）输出。 |
| **录音** | 麦克风或线路输入经 ES8311 ADC，经 I²S 回读至 ESP32-S3，可写入文件或上行网络（后续业务另文）。 |
| **电平与路由** | 通过 I²C 配置 ES8311 寄存器：音量、输入选择、ALC（若启用）、时钟与 I²S 格式。 |

### 1.2 非目标（首版可排除）

| 项 | 说明 |
|----|------|
| 多路混音、专业 DAW 级低延迟 | 以「稳定可用」为先；低延迟可单列阶段。 |
| 蓝牙 A2DP / USB Audio | 与本文 I²S 方案独立，需另立文档。 |
| 完整语音唤醒/降噪算法 | 可在录音数据稳定后再接第三方或自研算法。 |

---

## 2. 硬件连接（工程冻结表）

以下引脚号为 **ESP32-S3 GPIO**（与当前工程原理图一致；若改版以原理图为准）。

### 2.1 I²S（与 ES8311 数字音频）

| 信号 | GPIO | 说明 |
|------|------|------|
| **MCLK** | 16 | 主时钟（多数板级设计由 ESP32-S3 输出 MCLK 供 Codec）。 |
| **BCLK** | 45 | 位时钟。 |
| **LRCK** | 47 | 左右声道帧时钟 / WS。 |
| **DOUT** | 8 | 工程标注为「音频输出（播放）」——与 Codec 手册中 **SDIN/SDOUT** 命名易混淆，**务必以原理图网名为准**：通常 **MCU → Codec 播放数据** 接 Codec 的 **串行数据输入**，**Codec → MCU 录音数据** 接 Codec 的 **串行数据输出**。 |
| **DIN** | 21 | 工程标注为「音频输入（录音）」。 |

**核对项**：打开 ES8311 数据手册中 **Pin Description**，确认 `SDIN` / `SDOUT` 与 PCB 网络名 `DIN`/`DOUT` 的对应关系；软件里 `gpio_matrix` / `I2S` 的 `data_in` / `data_out` 必须与 **实际走线** 一致，否则表现为无声或噪声。

### 2.2 I²C（ES8311 控制）

| 信号 | GPIO | 说明 |
|------|------|------|
| **SCL** | 4 | I²C 时钟。 |
| **SDA** | 5 | I²C 数据。 |

**注意**：确认 ES8311 的 **I²C 地址**（硬件 AD0 引脚）与总线上其它器件无地址冲突；必要时在 `board` 层集中描述 `i2c_port`、上拉、速率（常用 100 kHz 调试 / 400 kHz 量产）。

### 2.3 功放使能（NS4150B）

| 信号 | GPIO | 说明 |
|------|------|------|
| **EN** | 7 | **高有效**，接 NS4150B 的 **SD** 脚；播放前拉高，待机/省电拉低（具体与 Pop 声抑制策略一起验证）。 |

---

## 3. 软件架构要点（与仓库约定对齐）

1. **单一所有者**：I²S 时钟与 DMA 建议由 `bsp_driver/i2s_std` 或上层 `esp_codec_dev` 独占；**Codec 寄存器** 由 `cbb/es8311` + 板级 `board.c` 桥接的 I²C 完成初始化与访问，避免多任务无锁写寄存器（I²C 总线互斥由 `bsp_driver` I²C 槽位与调用约定保证）。  
2. **任务上下文**：录音读数、文件写入、网络发送等 **长阻塞** 放在专用任务（参考 [`application_architecture.md`](application_architecture.md) 中 `app_mod` 或独立 `audio` 任务），不在按键回调里做。  
3. **看门狗**：DMA 双缓冲正常时 CPU 占用低；若在上层做重采样/编码，注意单次处理时长，避免饿死 IDLE。  
4. **与 LCD/SD 的 IO 冲突**：将本表 GPIO 与 `board`、LCD、SDMMC 引脚表 **交叉检查**，避免复用或 strap 引脚误用。

---

## 4. 技术方案选型（ESP-IDF）

| 路径 | 适用场景 |
|------|----------|
| **`esp_codec_dev` + 板级 `codec_dev` 配置`** | 官方推荐组合：I²S 驱动 + 已适配的 ES8311 控制；播放/录音 API 统一，维护成本低。 |
| **自研：直接 `i2s_std` / `i2s_tdm` + 手写寄存器表** | 需完全掌控寄存器、体积极致或对接非标准分支芯片时。 |

**建议**：优先 **`esp_codec_dev`**；仅在组件版本与芯片批次不兼容时再下沉寄存器级调试。

---

## 5. 分阶段开发计划（WBS）

### 阶段 0：环境与基线

- [x] 确认 ESP-IDF 版本与 `esp_codec_dev` / `es8311` 驱动是否已包含于当前 `idf.py` 工程。（本仓库当前走 **`bsp_driver` + 板级探测**；接入 `esp_codec_dev` 时在 `idf_component_register` 中增加依赖即可。）  
- [x] 在 `sdkconfig` 或 Kconfig 中启用所需 I²S / I²C 依赖。（`bsp_driver/CMakeLists.txt` 已 **`REQUIRES esp_driver_i2s`**；I²C 沿用既有 `esp_driver_i2c`。）  
- [x] 将 **§2 引脚表** 写入 `board` 或专用头文件，避免魔法数字散落在业务代码中。（见 `common/inc/board.h` 中 `BOARD_ES8311_*` / `BOARD_I2C_ES8311_*`。）

### 阶段 1：I²C 探测与 Codec 上电

- [x] I²C 总线初始化（端口、GPIO、上拉、时钟）。（沿用 `BoardInit` → `board_init_i2c` 中 I²C1。）  
- [x] 扫描或读取 ES8311 **芯片 ID 寄存器**，确认总线通、地址正确。（`cbb/es8311.c` + `board.c` 桥接 `I2cReadReg8`；`BoardEs8311()` 取句柄。）  
- [x] 配置基础电源/偏置相关寄存器（按数据手册与参考驱动顺序），确认无异常电流、无 I²C NAK 风暴。（`es8311_hw_open_default` + `es8311_analog_start`，寄存器顺序对齐 Espressif `esp_codec_dev`/`es8311_open` + `es8311_start`。）

**板级 I²S 封装（阶段 2 起用）**：`bsp_driver/src/i2s_std.c` 提供 `I2sStdDuplexInit` / `I2sStdDuplexDeinit`（Philips Stereo + Master），**默认不由 `BoardInit` 调用**，避免与后续 `esp_codec_dev` 自建 `i2s_chan` 重复占用同一 `I2S_NUM`。

### 阶段 2：时钟与 I²S 格式

- [x] 决定 **主从模式**：常见为 ESP32-S3 **I²S Master**，输出 MCLK/BCLK/LRCK；ES8311 作 Slave。（`bsp_driver/i2s_std` Master + `es8311_hw_open_default` / `es8311_analog_start` 中 Slave。）  
- [x] 计算 MCLK 与采样率关系（例如 256×Fs、384×Fs 等），与 ES8311 **PLL/Clock** 寄存器配置一致。（烟测默认 **256×Fs**，与 `I2S_STD_CLK_DEFAULT_CONFIG` 默认一致；系数表见 `cbb/es8311.c`。）  
- [x] 选定数据格式：**I²S Philips** vs **MSB/左对齐**；位深 **16 / 32 bit**；通道数 **单声道复用 LR** 或 **立体声**。（当前实现：Philips + 16-bit + 立体声槽位，两路相同采样数据。）  
- [ ] 用示波器或逻辑分析仪核对 BCLK、LRCK、MCLK 频率与占空比。

### 阶段 3：播放（DAC 路径）

- [x] GPIO7 **EN** 在播放前置高；建立「静音 → 建立 I²S → 缓升音量」序列以减轻 **上电 Pop**。（`main.c` 烟测：`dac_set_mute` → `I2sStdDuplexEnable` → 延时 → `BoardEs8311PaSet` → `es8311_analog_start(..., reset_digital=false)`（已 `set_format` 且 MCLK 已出，跳过软件复位以免清时钟）→ `dac_set_mute(false)`。）  
- [x] 播放固定 **正弦波 / WAV 测试缓冲**，确认耳机或喇叭端有稳定输出。（`application_start_modules_task` 拉起 `es8311_smoke`：48 kHz / 16-bit / 1 kHz 正弦约 2.5 s；编译前 `#define APP_ES8311_PLAYBACK_SMOKE 0` 可关闭。）  
- [x] 接入音量、静音、断电顺序（先静音或停 DMA，再拉低 EN）。（烟测结束：`dac_set_mute(true)`→延时→`BoardEs8311PaSet(FALSE)`→`I2sStdDuplexDisable`/`Deinit`。）

### 阶段 4：录音（ADC 路径）

- [ ] 确认麦克风偏置、增益寄存器（MIC PGA）；必要时先做 **ADC 原始波形** 日志或简易 FFT 烟测。  
- [ ] 配置 DMA **双缓冲** / `read` 周期，与上层环形缓冲对接。  
- [ ] 验证 **全双工**（若需要同时播 + 录）：注意 I²S 端口是否支持 full duplex 或采用 TDM/双引擎方案（依 IDF 与硬件绑定而定）。

### 阶段 5：业务集成与质量

- [ ] 封装 API：`audio_init`、`audio_play_start/stop`、`audio_record_start/stop`、`audio_set_volume`。  
- [ ] 栈与水线：在真实采样率与缓冲长度下测量 `uxTaskGetStackHighWaterMark`。  
- [ ] 异常处理：I²S 超时、I²C 失败、拔插（若有）时的安全降级与日志。  
- [ ] （可选）将录音数据写入 SD（与 [`doc/sdmmc_fat_io.md`](sdmmc_fat_io.md) 协调文件格式与写卡负载）。

---

## 6. 测试清单（手测 + 仪器）

| 序号 | 项目 | 通过标准 |
|------|------|----------|
| 1 | I²C 读 ID | 固定值与手册一致，100 次连续读无失败。 |
| 2 | MCLK/BCLK/LRCK | 频率与配置采样率匹配，无时钟丢失。 |
| 3 | 播放 1 kHz | 示波器/万用表 AC 档可见预期幅度；听感无断续。 |
| 4 | 录音静音底噪 | 静音环境下波形底噪在可接受范围，无 50 Hz 工频饱和（若有问题查模拟地与布局）。 |
| 5 | EN 开关 | EN 拉低时输出关闭；拉高恢复无异常爆音（可调软启动时序）。 |
| 6 | 长时间拷机 | ≥1 h 播放或环回，无内存泄漏、无 I²S 报错。 |

---

## 7. 风险与对策

| 风险 | 对策 |
|------|------|
| **DIN/DOUT 与手册 SDIN/SDOUT 接反** | 原理图核对；软件侧对调 `data_in`/`data_out` 做一次 A/B。 |
| **MCLK 与采样率不成整数比** | 查表计算 PLL；用示波器验证。 |
| **I²C 与 I²S  GPIO 冲突 / strap** | 启动日志检查 strap 警告；改板或改 GPIO。 |
| **全双工带宽** | 高采样率 + 32 bit 时关注 CPU 与内存；必要时降采样率或改 TDM。 |
| **多任务抢 I²C** | 单模块初始化 + `mutex`；避免在中断里写寄存器。 |

---

## 8. 关联文档

- [`doc/application_architecture.md`](application_architecture.md) — 任务与模块边界。  
- [`doc/embedded_coding_standard.md`](embedded_coding_standard.md) — 编码与审查约定。  
- [`doc/sdmmc_fat_io.md`](sdmmc_fat_io.md) — 若录音落盘。  
- Espressif：`esp_codec_dev`、`es8311` 组件说明与示例工程（以当前 IDF 文档为准）。

---

## 9. 变更记录

| 版本 | 日期 | 说明 |
|------|------|------|
| 1.0 | 2026-05-15 | 初版：录音/播放 WBS + 工程引脚表（MCLK 16、BCLK 45、LRCK 47、DOUT 8、DIN 21、I²C 4/5、功放 EN 7）。 |
| 1.2 | 2026-05-15 | ES8311 驱动迁入 **`cbb/es8311`**（I²C 回调式通用模块）；板级接线与 `bsp_driver` I²C 桥接保留在 `common/src/board.c`，`BoardEs8311()` 暴露句柄。 |
| 1.3 | 2026-05-15 | 阶段 1–3：`es8311_hw_open_default` / `set_format` / `analog_start` + MCLK 系数表；`I2sStdDuplexEnable/WriteTx`；`BoardEs8311AudioPathPrepared` / `BoardEs8311PaSet`；`main` 可选 1 kHz 播放烟测。 |
