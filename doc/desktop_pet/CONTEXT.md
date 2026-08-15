# desktop_pet 术语

桌宠产品域：圆屏 LVGL 表演、SD 皮肤、板端输入与云端会话边界。

**文档**：[`README.md`](README.md) · [`framework.md`](framework.md) · [`product.md`](product.md) · [`cloud_asr.md`](cloud_asr.md)

## Language

**卡根配置 (Card Config)**：
SD 挂载根上的 `config`（现路径 `/sdcard/config`）。UTF-8 `KEY=VALUE`，上电挂载后读取；可声明 `content_root` / `record_root`，未知键保留。凡 `*_root` 键都会被扫描（文件名+大小）。不属于皮肤包，换肤不覆盖。缺文件用默认根并仍尝试枚举文件。
_Avoid_：把设备配置写进 `/sdcard/pet/`、卡上 JSON（首版不解析 JSON）、无文件时拒绝启动

**皮肤包 (Skin Pack)**：
作者侧生成、可整包替换的只读内容树；含身体 clip、开机画面、可选 `font/` 字幕字库、`lang/` UI 铬文案、`sfx/` 照料提示音、以及后续主题索引。设备挂载根现为 `/sdcard/pet/`。可用 zip（根为 `pack.bin` 或 `pet/pack.bin`）经网页 `POST /api/pet/skin` 覆盖后重启；不覆盖卡根 `config`。若 zip **不含** `font/caption.bin`，提交时保留卡上原有 `pet/font/`；若 zip **不含** `lang/`，保留 `pet/lang/`；若 zip **不含**有效 `sfx/**/*.wav`，保留 `pet/sfx/`。
_Avoid_：资源包（仅指 `pack.bin` 时）、主题包（未定前勿混用）、固件资源

**运行时数据 (Runtime Data)**：
设备运行中写入的内容，不随皮肤包替换而覆盖；现例：`/sdcard/record/` 录音。
_Avoid_：用户文件（过宽）、缓存（未定义淘汰策略前勿用）

**内容根 (Content Root)**：
皮肤包在 SD 上的唯一挂载目录；固件经 `pet_fs_set_root()` 访问。当前路径：`/sdcard/pet`。
_Avoid_：多根散落（`/sdcard/boot` + `/sdcard/ui` 等）

**开机画面 (Boot Splash)**：
上电后、主界面前展示的皮肤资源；固定相对路径 `boot/splash.bin`，**静态单帧**静图，与 `pack.bin` 解耦；缺文件则固件 fallback。**视觉框架 C**：身体 + 环形进度；LVGL 淡入/呼吸/淡出 + 进度弧，位图仍为单帧 `splash.bin`。
_Avoid_：启动动画文件序列（`boot/anim` 首版不做）、固件内嵌图（非默认策略）

**合法路径 (Declared Paths)**：
皮肤包内固件会读或文档已承诺的相对路径集合；首版皮肤：`pack.bin`、`body/*`、`boot/splash.bin`。卡根另承诺 `/sdcard/config`。未实现能力只在文档标为 reserved，不在卡上预建空目录。
_Avoid_：空目录占位、未文档化的随意路径

**设备像素帧 (RGBH Frame)**：
卡上唯一位图容器：`RGBH` 头 + RGB565 载荷（与身体帧相同）。作者源图（PNG/JPEG 等）只存在于 PC 工具链，不进 SD、不进固件解码器。
_Avoid_：卡上 PNG/JPEG、开机专用魔数容器

**皮肤打包工具 (Skin Pack Tool / `pet_skin`)**：
作者侧入口，落在 [`tools/pet_tool/skin/`](../../tools/pet_tool/skin/)（伞目录 [`pet_tool`](../../tools/pet_tool/)）；以 CLI/库生成合法路径下的二进制。GUI 为 Qt（PySide6）壳，按 FeatureModule 扩展，写盘只经 `skin_core`。开机 splash **仅静态单帧**。网页预览：读 `skin/pack.json` + PNG；画面仅产品态（开机 C / 左弧主界面 / 对话 D）。
_Avoid_：与 `skin_core` 平行的第二套写盘逻辑、让用户手写 RGB565、本阶段实现 `boot/anim`、在预览里保留已废弃备选构图

**开机图适配 (Splash Fit)**：
工具默认将源图 **contain** 进 240×240，外侧填背景色（默认 `#202020`）；可用参数改 cover。变形拉伸不是默认。
_Avoid_：默认 cover 裁切、默认拉伸

**开机门闩 (Splash Gate)**：
离开开机画面的条件：已满足最短展示时间（默认 1000 ms），且皮肤包加载尝试已结束（成功或确认缺失）。不因 WiFi/agent 阻塞。
_Avoid_：纯固定时长无门闩、等到网络全就绪

**主界面构图 (Home Composition)**：
圆形主界面以宠物身体为视觉主体（约 ø140–160）；needs / 照料控件贴边且克制，不与身体抢权重。
_Avoid_：仪表盘式大 HUD、无铬沉浸（首版仍保留可发现的照料入口）

**Needs 指示 (Needs Arcs)**：
主界面底部三段弧条（圆屏内缩），绿/蓝/黄 = 饥饿/心情/精力（0–100%）；值存 NVS；约每 30 s 消耗一拍（默认十数分钟掉 1%），照料即时刷新并落盘。
_Avoid_：顶中三点、过密每秒衰减、常显数字百分比

**网络状态 (Net Status)**：
主界面顶部偏右常显网络图形（落在 ø200 安全圆内，避免圆屏切角）；STA 已获 IPv4 为在线。可选皮肤 `theme/ui/wifi_on.bin` / `wifi_off.bin`，缺则固件绘制扇形（在线青 / 离线灰）。单击：在线断连、离线重连（STA；不改凭据）。进对话页 / 设置页随主界面铬隐藏。
_Avoid_：贴矩形屏角（圆屏不可见）、常显 IP 字符串、用文字替代图标为默认产品态

**电量状态 (Battery Status)**：
主界面顶铬：WiFi **左侧偏上**同弧小电池图形（安全圆内）；ADC 分压读 `battery_*`（GPIO4）；绿/黄/红按电量，充电时青色。进对话页 / 设置页随铬隐藏。
_Avoid_：常显大号百分比、贴圆屏切角、阻塞 UI 线程做频繁 ADC（硬件采样已节流）

**设置入口 (Settings Entry)**：
主界面顶铬：WiFi 右侧偏下同弧小钮；可选 `theme/ui/settings.bin`，缺则字母 S。进入可滑动设置页（圆屏安全区内滚动）。
_Avoid_：贴矩形角、与身体热区抢点、把设置嵌进主界面仪表盘

**设置页 (Settings Surface)**：
独立层，返回钮 + 竖直滚动列表。首版：固件/皮肤包版本、MAC、网络/SSID/IP、电量%；有 CJK 字库时可切 EN/中文（默认 EN，**NVS 持久化**）；文案为 **UI 铬语言包**（固件底表 + 可选皮肤 `lang/`）；触摸校准入口。换肤切换 reserved（首版显示 soon，仍走网页换肤）。
_Avoid_：首版塞清 WiFi/重启等危险动作、无字库时强切中文、把网页换肤做成未完成的屏上流程

**产品主界面 (Product Home)**：
用户日常看到的圆形宠物界面。GPIO0 单击切换的 Rec/Play/Conn/Talk **debug 覆盖层不作为产品方向**（后续移除；研发改走串口 / `web_ctrl`）。
_Avoid_：把 debug 页当正式功能入口

**对话页 (Chat Surface)**：
独立界面，主界面「聊」进入。**框架 D（单击切换听/等答）**：大宠脸 + 当前一轮字幕 + 波形。**进页只连会话**；**单击开听 / 再单击停听等答**；说中可再点打断；字幕只展当前一轮；**无活动约 45s** 自动回主界面并离开即停。字幕字体优先 `/sdcard/pet/font/caption.bin`（LVGL bin），缺则内置 CJK 子集。
_Avoid_：无返回、禁止打断、离开挂 WS、主路径滚长历史、超时仅静音仍留在对话页

**照料 Dock (Care Dock) / 护理弧**：
主界面左侧弧形常显照料圆钮（喂 / 玩 / 睡），贴安全圆内；点按仍只投递既有 `PET_EVT_CARE_*`。
可选皮肤 `theme/ui/{feed,play,sleep}.bin`，缺则字母 F/P/S。
_Avoid_：现网大矩形文字键为默认、无常显照料入口、与身体热区严重重叠

**照料反馈 (Care Feedback)**：
照料动作可伴随可感知演出（移动、跟随、睡姿等），由意图消费（clip / LED / 电机等）实现；具体动效后续专项定，不改「输入只产事件」边界。
_Avoid_：在按钮回调里直接驱电机/改 LVGL 动画抢 `pet_core`

**照料分档 (Care Tier)**：
喂/戳等按 Needs 规则选档（爽/平/拒或冷），再映射 clip；首版喂+戳，共享 one-shot `refuse`。档内随机与玩分档后置。详见 [`framework.md`](framework.md) §5.1。
_Avoid_：按钮里写死单一动画、把分档逻辑放进 `pet_view`

**照料提示音 (Care SFX)**：
随身体 clip 触发的本地短语音/音效；皮肤 `pet/sfx/<clip>/*.wav`（16 kHz mono 16-bit），每池多句、每次随机播一句（尽量不连播同一文件）。可打断上一句 Care SFX；对话听/说中不播。缺资源静默。由 `PET_INTENT_SFX`（`arg0`=clip）驱动，不经云端 TTS。
_Avoid_：语言包（UI 铬）、云端播报、idle/sleepy/sad 提示音（首版不做）

**主界面文案 (Home Chrome Text)**：
产品主界面默认不常显状态句（如 `idle`）；仅异常短暂提示（如 `NO PACK`）后淡出。
_Avoid_：常显调试用英文状态行

**UI 铬语言包 (UI Chrome Locale)**：
设备屏上**系统铬**可切换语种的文案集合：设置页标签、对话模式字（听/说/连接中等）、短提示（如 `NO PACK` / `tap to talk`）。可选挂在皮肤 `lang/{en,zh}.txt`（UTF-8 `KEY=VALUE`，按 key 覆盖固件底表）；换肤保留策略同字库。不含角色口吻、不含云端 STT/TTS/LLM 语种。用户当前语种偏好属设备态（NVS），不属于本包内容。
_Avoid_：语言包（过宽）、皮肤口吻文案、会话语种、独立第三包（未定前勿另起根）

**对话入口 (Chat Entry)**：
主界面**右侧**独立「聊」钮进入对话页；首版可占位。可选 `theme/ui/chat.bin`，缺则字母 C。
_Avoid_：用身体点按替代对话入口、把聊天框嵌进主界面

**主 Dock (Home Dock)**：
历史用语；现布局为 **左弧照料三钮 + 右侧 Chat**（不再底弧四钮一排）。命中区保持在安全圆内。
_Avoid_：仍按「底中四钮」实现或文档描述

**触摸校准 (Touch Calib)**：
IT7259 → LVGL 线性变换存 NVS；上电应用。向导：确认 Needs↑/下边方向后，三轮四向采样。
_Avoid_：正方向已确认时再改 swap/invert；用坏校准硬拉按钮命中

**五官叠加 (Face Overlay)**：
眼/嘴/眉由 LVGL 叠在身体 `RGBH` 之上，随 `PET_FACE_*` 与本地眨眼变化；位置/旋转来自皮肤包的**五官锚点**。**本阶段 reserved**：固件不创建、不显示五官；`pet_core` 仍发 `PET_INTENT_FACE`，`pet_view` 只消费不画。
_Avoid_：把五官烘焙进每一身体帧、未约定就加「自带五官」pack 开关、本阶段在屏上叠眼嘴眉

**五官锚点 (Face Anchor)**：
每身体帧一组作者侧坐标：左眼 / 右眼 / 嘴 / 左眉 / 右眉，各含相对身体图左上角的 `(x, y)` 与 **独立旋转角 `angle`（度，有符号）**。字段与 Design 标定保留，供以后叠加；本阶段设备忽略。缺省则退回历史中心对齐、`angle=0`。
_Avoid_：让出图模型猜坐标、仅整脸一个角度却要求五官各自倾斜、在设备上现场标定
