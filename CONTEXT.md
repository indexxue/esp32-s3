# Ballot Guard

基于 ESP32-S3 的端边协同智能选票识别与全流程监管系统。本 glossary 描述 ballot_guard 产品域语言，不含实现细节。

## Language

**Business Screen**:
LCD 上展示投票业务状态的独立画面（主界面、投票流程、历史记录等），不属于管理员菜单层级。
_Avoid_: 业务页, 主屏

**Admin Menu**:
管理员用于配置 Vote Schedule、候选人数量、Cooldown Duration 及重置票数的 LCD 菜单层级；根列表固定 4 项、不滚动。
_Avoid_: 设置菜单, 配置模式, settings mode, 六项菜单

**Admin Menu Root**:
Admin Menu 的顶层列表，固定 4 项：Vote Schedule、候选人数量、Cooldown Duration、重置投票。不含状态跳转（如「进入投票」）或硬件维护（如 RTC 校时）；后者通过网页看板完成。
_Avoid_: 进入投票, 设置时钟, SCR_ADMIN_ENTER, SCR_ADMIN_CLOCK

**Web Admin Panel**:
浏览器端管理员界面，能力边界与 Admin Menu Root 对齐（Schedule / 人数 / 冷却 / Vote Reset），并额外提供 Vote History 查看与清空、RTC 校时；不含手动切换 Phase 的「进入投票」入口。Phase 仅由 Vote Schedule 与系统时钟驱动。
_Avoid_: 网页进入投票, 手动 override voting

**Admin Menu Entry**:
在允许进入的 Business Screen 上，按住确认键满 3 秒（含进度环反馈）进入 Admin Menu；进入时短鸣一声，RGB 保持进入前的业务色。被拒绝时显示 Toast「投票进行中不可设置」，不打断当前流程。
_Avoid_: 进设置, 长按返回进菜单, 10 秒长按

**Admin Menu Exit**:
从 Admin Menu 根按返回退出时，回到 Admin Menu Entry 触发时所在的 Business Screen（Home / Locked / History），而非一律跳转到 Phase 默认屏。
_Avoid_: 退出回主界面, leave_to_app 按 Phase 跳转

**Admin Entry Gate**:
规定哪些 Business Screen 允许 Admin Menu Entry 的策略。仅 **主界面（Home）**、**结果锁定（Locked）**、**历史记录（History）** 三屏允许进入；投票流程中的画面（投票等待、选人、冷却、违规）一律拒绝。
_Avoid_: 菜单白名单, 进菜单条件

**Candidate**:
投票计票对象；数量由 Admin Menu 配置（2–6 人），每名 Candidate 独立累计得票。
_Avoid_: 选项, 选手

**Candidate Placeholder Name**:
Candidate 在 LCD 与网页看板上显示的固定标识名（A、B、C…），不可通过 Admin Menu 修改；仅随 Candidate 数量增减而扩展或收缩。
_Avoid_: 候选人姓名, 自定义名称

**Valid Vote**:
投票者在选人屏（Select Screen）上选定 Candidate 后，短按确认键完成的一次有效计票。
_Avoid_: 有效票登记, 加票

**Spoiled Vote**:
不计入任何 Candidate 得票的选票；在 Select Screen 上通过返回键长按（≥2 秒）主动登记。
_Avoid_: 废票登记, 无效票, 超时自动废票

**Select Screen**:
投票流程中供投票者选择 Candidate 的 Business Screen；确认键短按登记 Valid Vote，返回键短按取消回到投票等待屏，返回键长按登记 Spoiled Vote。
_Avoid_: 选人屏, SCR_SELECT, 选择候选人

**Select Session Timeout**:
Select Screen 上连续无操作达到 15 秒后，系统自动退回 Voting Phase（投票等待屏），不登记任何票；红外可再次触发新的选人流程。
_Avoid_: 2 秒超时, 30 秒自动废票, 弃权废票

**Vote Schedule**:
一次投票活动的开始与结束时刻（同日、开始早于结束）；由 Admin Menu 配置并持久化，驱动系统 Phase 切换。
_Avoid_: 投票时间, 时段设置

**Schedule Editor**:
Admin Menu 中编辑 Vote Schedule 的子屏；单屏数字块布局，焦点落在时/分字段上时可直接用导航键增减，确认键在字段间跳转并最终保存。
_Avoid_: 时间列表页, 两步式编辑

**Cooldown Duration**:
单次有效票或废票登记后，系统拒绝下一次计票的锁定时长（3–10 秒）；由 Admin Menu 配置。
_Avoid_: 冷却时间, 锁定秒数

**Numeric Parameter Editor**:
Admin Menu 中编辑单个整数参数的通用子屏模式：居中数字框 + 导航键增减 + 保存按钮；Vote Schedule 以外的数值项（候选人数量、Cooldown Duration）均采用此模式。
_Avoid_: 刻度尺, 括号行, 列表式参数页

**Vote History**:
系统归档的历次投票会话摘要（时间戳、有效票/废票、各 Candidate 得票）。LCD 只读浏览；网页管理员 Tab 可查看并与 LCD 同步，且提供「清空全部历史」操作（LCD 无清除入口）。
_Avoid_: 历史数据, 记录列表, 仅 LCD 查看

**History Screen**:
浏览 Vote History 的 Business Screen；仅从主界面（Home）经导航快捷手势进入，不纳入 Admin Menu。
_Avoid_: 历史菜单项, SCR_ADMIN_HISTORY

**Vote Reset**:
Admin Menu 中的危险操作：先将当前会话归档至 Vote History，再清零有效票、废票与各 Candidate 得票；Vote Schedule、候选人数量、Cooldown Duration 等配置保持不变。完成后退出 Admin Menu，回到 Home。
_Avoid_: 恢复出厂, 重置配置, restore_default_cfg, 留在菜单

## System state

**Phase**:
系统对外可观测的全局状态，驱动 LCD 顶栏徽章、RGB 灯效与 Web `/api/vote/status`；采用扁平枚举，投票时段内的交互态与日程态同级。
_Avoid_: 系统状态, mode, screen

**Phase** 取值：`booting` · `fault` · `idle` · `waiting` · `voting` · `selecting` · `cooldown` · `violation` · `locked`

**Idle Phase**:
Vote Schedule 有效，且系统处于 boot 后或 Vote Reset 后的中性待机；徽章「待机」，RGB 绿常亮。
_Avoid_: 空闲, 非投票时段（泛指）

**Fault Phase**:
RTC 不可用，或 Vote Schedule 无效（开始时间不早于结束时间）；徽章「系统故障」。
_Avoid_: idle（用于日程无效）, 错误态

**Waiting Phase**:
当前时刻早于 Vote Schedule 开始时刻；徽章「待开启」，Home 显示倒计时。
_Avoid_: 待投票, SCR_WAITING

**Voting Phase**:
处于 Vote Schedule 窗口内，且无进行中的选人/冷却/违规交互；对应投票等待 Business Screen。
_Avoid_: 投票时段（泛指）

**Selecting Phase**:
红外触发后，投票者正在 Select Screen 上选择 Candidate。
_Avoid_: 选人态, phase=voting（当屏为 SELECT 时）

**Cooldown Phase**:
刚完成 Valid Vote 或 Spoiled Vote 登记，系统按 Cooldown Duration 拒绝下一次计票；徽章「请稍候」。
_Avoid_: 冷却屏, 锁定中

**Violation Phase**:
Cooldown Phase 内红外再次触发；徽章「违规重复」，RGB 与蜂鸣器报警。
_Avoid_: 重复投票, 违规重复投票

**Locked Phase**:
当前时刻已达 Vote Schedule 结束时刻；结果锁定，徽章「投票已锁定」。
_Avoid_: 已结束, 投票结束
