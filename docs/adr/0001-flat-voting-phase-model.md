# 扁平 Phase 模型（LCD / RGB / Web 共用）

Ballot Guard 的系统状态使用单一 `phase` 字段，取值包括日程态（`idle`、`waiting`、`voting`、`locked`）与投票时段内的交互态（`selecting`、`cooldown`、`violation`），以及生命周期态（`booting`、`fault`）。LCD 顶栏徽章、RGB 灯效与 `/api/vote/status` 的 `phase` / `phase_label` 均读取同一来源，不再在 JSON 中长期保持 `voting` 而 LCD 已切到选人/冷却/违规屏。

曾考虑 `phase`（日程）+ `session`（交互）双字段，但会增加网页轮询与状态机的映射成本；在 1s 轮询的首版 Web 看板下，扁平枚举更直接。`idle` 表示日程有效时的中性待机；RTC 不可用或 Vote Schedule 无效（开始 ≥ 结束）时使用 `fault`，避免把配置错误误标为「待机」。
