# desktop_pet 术语

桌宠产品域：圆屏 LVGL 表演、SD 皮肤、板端输入与云端会话边界。

**文档**：[`README.md`](README.md) · [`framework.md`](framework.md) · [`product.md`](product.md) · [`cloud_asr.md`](cloud_asr.md)

## Language

**卡根配置 (Card Config)**：
SD 挂载根上的 `config`（现路径 `/sdcard/config`）。UTF-8 `KEY=VALUE`，上电挂载后读取；可声明 `content_root` / `record_root`，未知键保留。凡 `*_root` 键都会被扫描（文件名+大小）。不属于皮肤包，换肤不覆盖。缺文件用默认根并仍尝试枚举文件。
_Avoid_：把设备配置写进 `/sdcard/pet/`、卡上 JSON（首版不解析 JSON）、无文件时拒绝启动

**皮肤包 (Skin Pack)**：
作者侧生成、可整包替换的只读内容树；含身体 clip、开机画面、以及后续主题/音效索引。设备挂载根现为 `/sdcard/pet/`。可用 zip（根为 `pack.bin` 或 `pet/pack.bin`）经网页 `POST /api/pet/skin` 覆盖后重启；不覆盖卡根 `config`。
_Avoid_：资源包（仅指 `pack.bin` 时）、主题包（未定前勿混用）、固件资源

**运行时数据 (Runtime Data)**：
设备运行中写入的内容，不随皮肤包替换而覆盖；现例：`/sdcard/record/` 录音。
_Avoid_：用户文件（过宽）、缓存（未定义淘汰策略前勿用）

**内容根 (Content Root)**：
皮肤包在 SD 上的唯一挂载目录；固件经 `pet_fs_set_root()` 访问。当前路径：`/sdcard/pet`。
_Avoid_：多根散落（`/sdcard/boot` + `/sdcard/ui` 等）

**开机画面 (Boot Splash)**：
上电后、主界面前展示的皮肤资源；固定相对路径 `boot/splash.bin`，**静态单帧**静图，与 `pack.bin` 解耦；缺文件则固件 fallback。**视觉框架 C**：身体 + 环形进度；LVGL 可叠进度弧，位图仍为单帧 `splash.bin`。
_Avoid_：启动动画文件序列（`boot/anim` 首版不做）、固件内嵌图（非默认策略）

**合法路径 (Declared Paths)**：
皮肤包内固件会读或文档已承诺的相对路径集合；首版皮肤：`pack.bin`、`body/*`、`boot/splash.bin`。卡根另承诺 `/sdcard/config`。未实现能力只在文档标为 reserved，不在卡上预建空目录。
_Avoid_：空目录占位、未文档化的随意路径

**设备像素帧 (RGBH Frame)**：
卡上唯一位图容器：`RGBH` 头 + RGB565 载荷（与身体帧相同）。作者源图（PNG/JPEG 等）只存在于 PC 工具链，不进 SD、不进固件解码器。
_Avoid_：卡上 PNG/JPEG、开机专用魔数容器

**皮肤打包工具 (Skin Pack Tool / `pet_skin`)**：
作者侧入口，落在 [`tools/pet_skin/`](../../tools/pet_skin/)；以 CLI/库生成合法路径下的二进制。GUI 为 Qt（PySide6）壳，按 FeatureModule 扩展，写盘只经 `skin_core`。开机 splash **仅静态单帧**。
_Avoid_：与 `skin_core` 平行的第二套写盘逻辑、让用户手写 RGB565、本阶段实现 `boot/anim`

**开机图适配 (Splash Fit)**：
工具默认将源图 **contain** 进 240×240，外侧填背景色（默认 `#202020`）；可用参数改 cover。变形拉伸不是默认。
_Avoid_：默认 cover 裁切、默认拉伸

**开机门闩 (Splash Gate)**：
离开开机画面的条件：已满足最短展示时间（默认 1000 ms），且皮肤包加载尝试已结束（成功或确认缺失）。不因 WiFi/agent 阻塞。
_Avoid_：纯固定时长无门闩、等到网络全就绪

**主界面构图 (Home Composition)**：
圆形主界面以宠物身体为视觉主体（约 ø140–160）；needs / 照料控件贴边且克制，不与身体抢权重。
_Avoid_：仪表盘式大 HUD、无铬沉浸（首版仍保留可发现的照料入口）

**Needs 指示 (Needs Dots)**：
主界面常显顶中三颗小圆点表示饥饿/心情/精力（色区分维度，填充/亮度表高低）；不以细条为默认。
_Avoid_：常显三细条、默认完全隐藏 needs

**产品主界面 (Product Home)**：
用户日常看到的圆形宠物界面。GPIO0 单击切换的 Rec/Play/Conn/Talk **debug 覆盖层不作为产品方向**（后续移除；研发改走串口 / `web_ctrl`）。
_Avoid_：把 debug 页当正式功能入口

**对话页 (Chat Surface)**：
独立界面，主界面「聊」进入。**框架 D（语音优先）**：大宠脸 + 当前一轮字幕 + 波形。**进入即听**；**离开即停**；**可打断 + 中断重讲**（语音 barge-in 或点按宠脸 → 停播并立即回听）；字幕只展当前一轮；**无活动约 45s** 自动回主界面并离开即停。
_Avoid_：无返回、禁止打断、离开挂 WS、主路径滚长历史、超时仅静音仍留在对话页

**照料 Dock (Care Dock)**：
主界面底边常显照料圆钮（喂 / 玩 / 睡），贴安全圆内；点按仍只投递既有 `PET_EVT_CARE_*`。
_Avoid_：现网大矩形文字键为默认、无常显照料入口

**照料反馈 (Care Feedback)**：
照料动作可伴随可感知演出（移动、跟随、睡姿等），由意图消费（clip / LED / 电机等）实现；具体动效后续专项定，不改「输入只产事件」边界。
_Avoid_：在按钮回调里直接驱电机/改 LVGL 动画抢 `pet_core`

**主界面文案 (Home Chrome Text)**：
产品主界面默认不常显状态句（如 `idle`）；仅异常短暂提示（如 `NO PACK`）后淡出。
_Avoid_：常显调试用英文状态行

**对话入口 (Chat Entry)**：
主界面 Dock「聊」进入对话页；首版可占位。
_Avoid_：用身体点按替代对话入口、把聊天框嵌进主界面

**主 Dock (Home Dock)**：
底弧一排四枚圆钮：喂 / 玩 / 睡 / 聊；「聊」仅样式区分。命中区保持在安全圆内。
_Avoid_：聊与照料拆到对角导致双热区体系

**五官叠加 (Face Overlay)**：
眼/嘴/眉由 LVGL 叠在身体 `RGBH` 之上，随 `PET_FACE_*` 与本地眨眼变化；位置/旋转来自皮肤包的**五官锚点**。**本阶段 reserved**：固件不创建、不显示五官；`pet_core` 仍发 `PET_INTENT_FACE`，`pet_view` 只消费不画。
_Avoid_：把五官烘焙进每一身体帧、未约定就加「自带五官」pack 开关、本阶段在屏上叠眼嘴眉

**五官锚点 (Face Anchor)**：
每身体帧一组作者侧坐标：左眼 / 右眼 / 嘴 / 左眉 / 右眉，各含相对身体图左上角的 `(x, y)` 与 **独立旋转角 `angle`（度，有符号）**。字段与 Design 标定保留，供以后叠加；本阶段设备忽略。缺省则退回历史中心对齐、`angle=0`。
_Avoid_：让出图模型猜坐标、仅整脸一个角度却要求五官各自倾斜、在设备上现场标定
