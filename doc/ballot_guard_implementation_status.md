# Ballot Guard · 实现进度与待办

**版本**：v0.1  
**工程**：`ballot_guard/`（ESP32-S3 + ST7789 240×135 + WS2812 + Web）  
**依据**：`tmp/产品使用逻辑 + GPIO 硬连接表.md`、`tmp/ballot_guard_UI设计评审稿.md`、`tmp/ballot_guard_LCD菜单UI设计.md`  
**更新**：2026-06-20

> 使用逻辑与 GPIO 详见同目录 [`ballot_guard_product_usage.md`](./ballot_guard_product_usage.md)。

---

## 1. 总览

| 维度 | 状态 |
|:---|:---|
| **平台壳层**（NVS / 板级 / 按键 / RGB / Web 自启） | 已实现 |
| **LCD 画面绘制**（12 屏 UI 骨架） | 已实现（静态/调试切换为主） |
| **管理员菜单**（时段 / 人数 / 冷却 / 重置） | 已实现（RAM 配置，未持久化） |
| **Web 实时看板** | 已实现（1s 轮询，只读） |
| **Web LCD 调试** | 已实现 |
| **Web Wi‑Fi 配网** | 已实现（沿用 `web_ctrl`） |
| **投票核心业务**（计票 / 红外 / 冷却 / 状态机） | **未实现** |
| **DS3231 硬件时钟** | **未接入** |
| **6 键 / 蜂鸣器 / 红外** | **红外 GPIO 已接入**；6 键映射已在 profile；蜂鸣器未接入 |

---

## 2. 已实现功能

### 2.1 平台壳层（`ballot_guard/main/start.c`）

| 功能 | 说明 | 关键文件 |
|:---|:---|:---|
| 日志与 NVS 初始化 | 上电 `log_init` → `nvs_init` → `BoardInit` | `start.c` |
| 产品画像 | `ballot_guard` 启用 LCD + 按键 + LED + Web 掩码 | `common/src/device_profile.c` |
| 按键扫描任务 | `btn_scan`，50 Hz；回调优先交给 LCD 菜单 | `start.c`, `common/src/button.c` |
| RGB 灯效 | 上电 `LED_SCENE_ID_BOOTUP`；左/右单击可触发 trigger/success 场景 | `led_scene` |
| Web 自启 | `CONFIG_WEB_CTRL_AUTO_START` 时创建 `web_boot` 任务 | `start.c`, `components/web_ctrl` |
| OTA 槽切换 | 左键长按（菜单未接管时）切换下次启动分区并复位 | `start.c` |

**当前按键映射（开发板 2 键，非最终 6 键产品）**

| 物理键 | GPIO | 短按 | 长按 |
|:---|:---|:---|:---|
| 左 | 0 | 上 / 菜单内上一项 | 进入管理员菜单 / 菜单确认 |
| 右 | 3 | 下 / 菜单内下一项 | 返回 / 退出菜单 |

> 菜单逻辑用 `NAV_PREV` / `NAV_NEXT` / `CONFIRM` / `BACK` 抽象，后续可换绑 6 键而不改菜单引擎。

---

### 2.2 LCD 菜单与绘制

| 功能 | 说明 | 关键文件 |
|:---|:---|:---|
| 通用 menu 引擎 | 子菜单、参数步进、危险项、Toast | `components/menu` |
| 12 个 LCD 画面 ID | 业务屏 7 + 管理员菜单 5 | `vote_menu_config.h` |
| 管理员菜单页表 | 投票时段、候选人数量(2~6)、冷却(3~10s)、重置确认 | `vote_menu_pages.c` |
| LCD 绘制 | 240×135 三区布局、深色主题、ASCII 候选人简称 A~F | `vote_menu_lcd.c` |
| UI 任务 | `vote_menu` 任务：200 ms tick、1 s 时钟局部刷新、互斥锁保护 SPI | `vote_menu_demo.c` |
| 业务屏手动切换 | 主界面 → 历史（上键）；选人屏内上下切换焦点 | `vote_menu_demo.c` |
| 管理员入口 | 长按左键进入菜单；重置后清空 `vote_status` 计数 | `vote_menu_demo.c` |
| Web 远程切屏 | `GET/POST /api/lcd/menu` 切换画面或注入 up/down/enter/back | `web_pages.c` |

**已可预览的 LCD 画面**

- 业务：`home` / `voting` / `select` / `cooldown` / `violation` / `locked` / `history`
- 管理：`admin` / `admin_time` / `admin_count` / `admin_cooldown` / `admin_reset`

---

### 2.3 投票状态快照（Web 数据源）

| 功能 | 说明 | 关键文件 |
|:---|:---|:---|
| JSON 构建 | `GET /api/vote/status` 返回 phase、票数、时段、外设、最近事件 | `vote_status.c`, `web_pages.c` |
| 时段阶段计算 | 根据 `vote_menu_settings` 起止时间与当前时钟比较 → `waiting` / `voting` / `locked` / `idle` | `vote_status.c` |
| 候选人默认名 | Web 用 UTF-8 中文；LCD 用 A~F | `vote_status.c` |
| 重置 API | 管理员菜单「Confirm Reset」调用 `vote_status_reset_counts()` | `vote_menu_pages.c` |
| 事件队列 | `vote_status_push_event()` 已实现，尚无计票流程写入 | `vote_status.c` |

**当前限制**

- 时钟来源为 `esp_timer_get_time()` 上电时长，**非 DS3231 真实 RTC**。
- 票数字段存在但**无加票/废票写入路径**，看板默认全 0。
- `cooldown_sec` 在 JSON 中固定返回 0。
- `ds3231_ok` 读 `BoardDs3231()`；`ir_ok` 读 `BoardPeriphReady(DEVICE_BOARD_MASK_IR)`。

---

### 2.4 Web 前端（`ballot_guard/main/source/www/index.html`）

| Tab | 功能 |
|:---|:---|
| **实时看板** | 状态徽章、倒计时、有效/废/总票、候选人柱状图、外设芯片、最近 5 条操作；1 s 轮询 `/api/vote/status` |
| **LCD 调试** | 12 屏一键切换；模拟 up/down/enter/back；1 s 轮询当前页 |
| **网络设置** | Wi‑Fi 状态、扫描、保存 SSID/密码、清除 STA 并重启（沿用 `web_ctrl` REST） |

页面嵌入固件（`EMBED_FILES`），深色主题对齐 LCD 设计 Token。

---

### 2.6 红外传感器（GPIO38 / GPIO39）

| 功能 | 说明 | 关键文件 |
|:---|:---|:---|
| 板级 GPIO 初始化 | 输入上拉、下降沿中断、ISR 入队 | `common/src/board.c`, `common/inc/board.h` |
| 产品掩码 | `ballot_guard` 启用 `DEVICE_BOARD_MASK_IR`（**不含 SD 卡**） | `device_profile.c` |
| 事件轮询 | `ir_poll` 任务取队列并打日志 | `start.c` |
| Web 外设状态 | `peripherals.ir` 读真实初始化结果 | `vote_status.c` |

> **待办**：将红外触发绑定投票状态机（VOTING→SELECT、COOLDOWN→VIOLATION）。

---

### 2.7 构建与集成

| 项 | 说明 |
|:---|:---|
| CMake 源文件 | `vote_status.c`, `vote_menu_*.c`, `web_pages.c`, `start.c` | `ballot_guard/main/CMakeLists.txt` |
| 依赖组件 | `cbb`, `common`, `bsp_driver`, `web_ctrl`, `menu`, `esp_timer` |
| 产品 ID | `nvs_project_use_device_id(BALLOT_GUARD)` | `ballot_guard/CMakeLists.txt` |

---

## 3. 待办事项

按推荐实施顺序排列；同一优先级内可并行。

### P0 — 核心业务闭环

| # | 任务 | 说明 | 建议落点 |
|:--|:---|:---|:---|
| 1 | **投票状态机** | 统一管理 `idle → waiting → voting → selecting → cooldown → locked → fault`；驱动 LCD 自动切屏、Web phase、RGB | 新建 `vote_fsm.c` 或扩展 `vote_menu_demo.c` |
| 2 | **有效票 / 废票计票** | 短按确认加票、长按返回登记废票；更新 `s_votes[]` / `s_spoiled` 并 `vote_status_push_event` | `vote_status.c` + 状态机 |
| 3 | **冷却锁定** | 计票后 N 秒拒绝重复；LCD/Web 显示剩余秒数；JSON `cooldown_sec` 实时化 | 状态机 + `vote_status.c` |
| 4 | **时段自动切换** | 到达开始/结束时间自动切屏、蜂鸣提示（见产品逻辑） | 状态机 + 1 s 定时检查 |
| 5 | **LCD 与 phase 联动** | 主界面 Phase 文案、倒计时、投票屏剩余时间改为读状态机而非硬编码 | `vote_menu_lcd.c` |

### P1 — 外设与持久化

| # | 任务 | 说明 | 建议落点 |
|:--|:---|:---|:---|
| 6 | **DS3231 驱动与 RTC** | I2C GPIO4/5；替换 `esp_timer` 伪时钟；初始化失败 → fault 屏 + 红灯 | `bsp_driver` / `board` |
| 7 | **NVS 持久化投票配置** | 时段、候选人数量、冷却时长；上电加载 | `vote_menu_pages.c` + NVS 键 |
| 8 | **NVS 持久化票数** | 可选：断电保留或仅会话内 RAM（产品文档倾向可重置） | `vote_status.c` |
| 9 | **6 键 GPIO 映射** | 按产品表注册 上/下/左/右/确认/返回；更新 `device_profile` | `device_profile.c`, `button.c` |
| 10 | **红外防重复** | GPIO38/39 下降沿中断；靠近 → 选人屏；冷却期触发 → 违规屏 + 报警 | `board.c` **已初始化**；`start.c` 轮询；状态机待接 |
| 11 | **蜂鸣器** | GPIO47 PWM；开始两短鸣、结束长鸣、违规报警 | `bsp_driver` + 状态机 |
| 12 | **RGB 场景与状态绑定** | 黄呼吸/绿常亮/蓝慢闪/快闪/红报警等，替换通用 trigger/success | `led_scene` 扩展或投票专用 wrapper |

### P2 — 数据与体验完善

| # | 任务 | 说明 | 建议落点 |
|:--|:---|:---|:---|
| 13 | **历史记录** | 最近 10 次投票摘要；LCD 历史屏 + `GET /api/vote/history`（当前返回空数组） | `vote_status.c`, `web_pages.c` |
| 14 | **外设状态真实上报** | `peripherals.ds3231` / `ir` / `rgb` 读实际初始化结果 | `vote_status.c` |
| 15 | **LCD 背光 PWM** | GPIO46 亮度控制 | `board` / ST7789 初始化 |
| 16 | **候选人名称可配置** | 管理员菜单或 NVS 写入（LCD 仍用 ASCII 简称） | `vote_menu_pages.c` |
| 17 | **中文 LCD 字库** | 若需屏上显示中文姓名，扩展字库或 GB2312 渲染 | `vote_menu_lcd.c` |
| 18 | **Wi‑Fi STA 离线指示** | STA 失败时 RGB 黄常亮 + Web 顶部提示 | 状态机 + `index.html` |
| 19 | **单元 / 联调测试** | 状态转换表、JSON 快照、菜单保存边界 | `test/` 或厂测文档 |

### P3 — 文档与硬化

| # | 任务 | 说明 |
|:--|:---|:---|
| 20 | 厂测检查单 | 上电自检、各屏截图、Web 看板、计票一次全流程 |
| 21 | Web 鉴权 / 限流 | 参考 `doc/web_control_http_server_plan.md` 阶段 5 |
| 22 | 设计稿同步 | `tmp/` 评审稿与实现差异定期回写本文 |

---

## 4. 代码与产品设计差异（评审对照）

| 项目 | 产品设计（`tmp/`） | 当前代码 |
|:---|:---|:---|
| 物理按键 | 6 键（GPIO 8/15/16/17/18/21） | 2 键（GPIO 0/3），逻辑动作已抽象 |
| 时钟 | DS3231 I2C | 上电计时 `esp_timer` |
| 计票 | 红外触发 + 确认/长按废票 | UI 仅有选人屏，无写票 |
| 冷却 / 违规 | 完整流程 + 声光报警 | 仅有冷却屏 UI 占位 |
| RGB | 与投票 phase 一一对应 | 通用 `led_scene` 演示场景 |
| 网页管理 | 只读看板 | 已实现；配置仅在 LCD |
| 历史 | 最近 10 条 | API 空实现；LCD 屏可翻页无数据 |
| 配置存储 | NVS 全部参数 | 菜单默认值在 RAM，Wi‑Fi 走 NVS |

---

## 5. 相关文档

| 文档 | 路径 |
|:---|:---|
| 产品使用逻辑 + GPIO | [`ballot_guard_product_usage.md`](./ballot_guard_product_usage.md) |
| UI 设计评审稿 | `tmp/ballot_guard_UI设计评审稿.md` |
| LCD 菜单 UI 设计 | `tmp/ballot_guard_LCD菜单UI设计.md` |
| Web 服务计划 | [`web_control_http_server_plan.md`](./web_control_http_server_plan.md) |
| 应用架构 | [`application_architecture.md`](./application_architecture.md) |

---

## 6. REST API 速查（已实现）

| 方法 | 路径 | 说明 |
|:---|:---|:---|
| GET | `/` | 嵌入 `index.html` |
| GET | `/api/vote/status` | 投票看板 JSON |
| GET | `/api/vote/history` | 历史记录（当前恒为空） |
| GET | `/api/lcd/menu` | LCD 当前页与屏列表 |
| POST | `/api/lcd/menu` | `{"page":"select"}` 或 `{"event":"up"}` |
| GET | `/api/wifi/status` | Wi‑Fi / IP / 电量（`web_ctrl`） |
| POST | `/api/wifi/scan` | 扫描 AP |
| GET | `/api/wifi/scan_result` | 扫描结果 |
| POST | `/api/wifi/save` | 保存 STA 凭据 |
| POST | `/api/wifi/sta_disconnect` | 清除 STA 并重启 |
