# Ballot Guard · 实现进度与待办

**版本**：v0.2  
**工程**：`ballot_guard/`（ESP32-S3 + ST7789 240×135 + DS3231 + IR + WS2812 + Web）  
**依据**：原始七大功能需求、`tmp/产品使用逻辑 + GPIO 硬连接表.md`、UI 设计稿  
**更新**：2026-06-22

> 使用逻辑与 GPIO 详见 [`ballot_guard_product_usage.md`](./ballot_guard_product_usage.md)。

---

## 1. 七大需求对照总览

| # | 原始需求 | 状态 | 说明 |
|:--|:---|:---|:---|
| （1） | 系统时钟与投票时间管理 | **已实现（核心）** | DS3231 驱动已接入；LCD 顶栏实时时钟；管理员可设投票起止时段 + **手动设 RTC 时分秒**；到达起止时间自动切换 phase（waiting / voting / locked），蜂鸣提示 |
| （2） | 废票自动判定 | **部分实现** | 按键模式：未切换候选人长按返回→空白废票；切换后长按返回→多选废票；LCD 冷却屏展示废票类型；蜂鸣 + RGB 报警。**无 OCR/图像识别**（产品为纯按键计票） |
| （3） | 投票防重复监管 | **已实现（核心）** | GPIO38/39 红外下降沿→进入选人屏；冷却期内再次触发→违规屏 + 蜂鸣报警；冷却秒数可配、LCD/Web 实时倒计时 |
| （4） | 自动计票与本地数据显示 | **已实现** | 短按确认有效票 +1；LCD 主界面/锁定屏/投票屏展示各候选人票数、有效票、废票、总票；计票后写 NVS |
| （5） | WiFi 数据上传 | **已实现** | SoftAP + HTTP；`GET /api/vote/status` 1 s 轮询；含 phase、票数、冷却剩余、外设状态、最近事件 |
| （6） | 基础参数设置与掉电存储 | **已实现** | NVS 命名空间 `ballot_guard`：投票时段、候选人数量、冷却时长、候选人名称、当前票数；上电自动加载 |
| （7） | 本地历史记录查看 | **已实现** | 最近 20 次会话归档（NVS）；主界面上键进入历史屏翻页；`GET /api/vote/history` |

---

## 2. 总览（工程维度）

| 维度 | 状态 |
|:---|:---|
| **平台壳层**（NVS / 板级 / 按键 / RGB / Web / 蜂鸣器） | 已实现 |
| **DS3231 硬件时钟** | 已实现（I2C GPIO4/5；读失败 → phase=fault） |
| **LCD 12+1 屏 UI** | 已实现（13 屏含系统时钟设置） |
| **管理员菜单**（时段 / 时钟 / 人数 / 冷却 / 重置） | 已实现 + NVS 持久化 |
| **投票业务闭环**（计票 / 红外 / 冷却 / 时段联动） | **已实现（本次补齐）** |
| **6 键 GPIO 映射** | 已在 `device_profile` 注册（开发板可用 2 键降级） |
| **RGB 与 phase 一一对应** | **部分**（计票/红外/违规有场景；待机/投票蓝闪等待专用场景） |
| **LCD 背光 PWM** | 未实现（GPIO46 待接） |
| **中文 LCD 字库** | 未实现（LCD 英文；Web UTF-8 中文） |

---

## 3. 已实现功能（按模块）

### 3.1 平台壳层（`ballot_guard/main/start.c`）

| 功能 | 关键文件 |
|:---|:---|
| 日志 / NVS / BoardInit | `start.c` |
| 按键 50 Hz 扫描 → `vote_menu_demo_on_button` | `button.c`, `start.c` |
| 红外轮询 → `vote_menu_demo_on_ir` | `board.c`, `start.c` |
| 蜂鸣器 GPIO47（短鸣/双短/三连/报警） | `start.c`, `buzzer` |
| RGB 上电 bootup；按键演示 trigger/success | `led_scene` |
| Web 自启 + 页面注册 | `web_ctrl`, `web_pages.c` |

**按键映射**

| 模式 | 说明 |
|:---|:---|
| **产品 6 键** | 上7 / 下16 / 左18 / 右8 / 确认17 / 返回15（见 `device_profile.c`） |
| **开发板 2 键** | 左=上/长按确认；右=下/长按返回（选人屏左右切换） |

> 产品 GPIO 表（8/15/16/17/18/21）与当前 `device_profile` 引脚编号不一致，联调前请按实际 PCB 核对。

---

### 3.2 时钟与投票时段（需求 1）

| 功能 | 关键文件 |
|:---|:---|
| DS3231 读写 | `cbb/ds3231.c`, `common/src/board.c` |
| LCD 顶栏 HH:MM:SS | `vote_status.c`, `vote_menu_lcd.c` |
| 投票起止时段（管理员 2.Vote Schedule） | `vote_menu_pages.c` → NVS |
| **系统时钟手动设置**（管理员 5.Set Clock） | `vote_menu_pages.c`, `vote_status_set_clock_hms()` |
| phase 计算 waiting / voting / locked / fault | `vote_status.c` |
| 到达 voting → 双短鸣 + 自动切投票屏；locked → 三连长鸣 + 归档 | `vote_menu_demo.c` |

---

### 3.3 计票与废票（需求 2、4）

| 操作 | 行为 |
|:---|:---|
| 红外触发（投票时段） | 进入选人屏 |
| 短按确认 | 有效票 +1 → 冷却 → RGB success |
| 长按返回（选人屏） | 废票 +1（空白/多选分类）→ 冷却 → RGB error + 蜂鸣 |
| 冷却结束 | 自动回到投票等待屏 |

| 文件 | 职责 |
|:---|:---|
| `vote_status.c` | 票数 RAM 镜像、JSON、废票类型、冷却剩余 |
| `vote_nvs.c` | 票数 / 配置持久化 |
| `vote_menu_demo.c` | 按键 / 红外 / 冷却状态机 |
| `vote_menu_lcd.c` | 各业务屏绘制 |

---

### 3.4 防重复与违规（需求 3）

| 条件 | 行为 |
|:---|:---|
| 冷却期内 IR 触发 | 违规屏 + `BUZZER_PATTERN_ALARM` + RGB alarm |
| 冷却剩余 | `vote_status_cooldown_remaining()`；JSON `cooldown_sec` 为**实时剩余** |

---

### 3.5 Web 看板（需求 5）

| API | 说明 |
|:---|:---|
| `GET /api/vote/status` | phase、倒计时、冷却、票数、外设、最近 5 事件 |
| `GET /api/vote/history` | 归档会话列表 |
| `GET /api/vote/settings` | 投票时段、人数、冷却、RTC 时钟 |
| `POST /api/vote/settings/*` | 保存时段 / 人数 / 冷却 / 时钟 / 重置数据 |
| `GET/POST /api/vote/candidates` | 候选人姓名 |
| `POST /api/vote/session/enter` | 退出 LCD 管理员菜单进入投票 |
| Wi‑Fi 配网 | 沿用 `web_ctrl` |

---

### 3.6 NVS 与历史（需求 6、7）

| 键前缀 | 内容 |
|:---|:---|
| `cfg_*` | 投票时段、人数、冷却 |
| `cand_name_*` | 候选人名称 |
| `vote_*` | 当前票数 |
| `hist_*` | 历史 FIFO（最多 20 条） |

---

## 4. 待办事项（按优先级）

### P1 — 体验与产品对齐

| # | 任务 | 说明 |
|:--|:---|:---|
| 1 | **RGB phase 专用场景** | 待机绿常亮、投票蓝慢闪、靠近蓝快闪等（见产品逻辑 §RGB） |
| 2 | **GPIO 与 PCB 对齐** | 统一 `device_profile` 与硬件连接表引脚 |
| 3 | **LCD 背光 PWM** | GPIO46 |
| 4 | **RTC 日期设置** | 当前仅时分秒；历史时间戳仍部分依赖编译日期 |
| 5 | **6 键长按进管理员** | 产品：确认键长按 3 s；当前为 home/locked 长按确认 |

### P2 — 硬化与文档

| # | 任务 |
|:--|:---|
| 6 | Web 鉴权 / 限流 |
| 7 | 厂测检查单与状态转换表单测 |
| 8 | 中文 LCD 字库（可选） |
| 9 | Wi‑Fi STA 离线 RGB 黄常亮 + Web 提示 |

---

## 5. 代码与原始需求差异说明

| 项目 | 原始/设计文档 | 当前实现 |
|:---|:---|:---|
| 废票判定 | 空白 / 多选 / 标记不规范 | **按键代理规则**（无选票图像识别） |
| 红外 | 必须 | 已实现；初始化失败可降级计票 |
| 历史条数 | 最近 10 次 | 实现为 **20 条** FIFO |
| 识别阈值 | 需求（6）提及 | **不适用**（无 OCR 模块） |
| 按键 GPIO | 8/15/16/17/18/21 | profile 中为 7/16/18/8/17/15 |

---

## 6. REST API 速查

| 方法 | 路径 | 说明 |
|:---|:---|:---|
| GET | `/` | 嵌入 `index.html` |
| GET | `/api/vote/status` | 投票看板 JSON |
| GET | `/api/vote/history` | 历史记录 |
| GET | `/api/vote/settings` | 参数设置（时段/人数/冷却/时钟） |
| POST | `/api/vote/settings/schedule` | 保存投票时段 |
| POST | `/api/vote/settings/count` | 保存候选人数量 |
| POST | `/api/vote/settings/cooldown` | 保存冷却时长 |
| POST | `/api/vote/settings/clock` | 设置 RTC 时钟 |
| POST | `/api/vote/settings/reset` | 重置全部投票数据 |
| GET/POST | `/api/vote/candidates` | 候选人姓名 |
| POST | `/api/vote/session/enter` | 进入投票界面 |
| GET/POST | `/api/wifi/*` | 配网（`web_ctrl`） |

---

## 7. 相关文档

| 文档 | 路径 |
|:---|:---|
| 产品使用逻辑 + GPIO | [`ballot_guard_product_usage.md`](./ballot_guard_product_usage.md) |
| UI 设计评审稿 | `tmp/ballot_guard_UI设计评审稿.md` |
| LCD 菜单 UI 设计 | `tmp/ballot_guard_LCD菜单UI设计.md` |
| Web 服务计划 | [`web_control_http_server_plan.md`](./web_control_http_server_plan.md) |
