/**
 * @file vote_status.h
 * @brief ballot_guard 投票状态快照（供 Web `/api/vote/status` 与后续状态机共享）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vote_menu_config.h"

#define VOTE_STATUS_MAX_CANDIDATES (6U)
#define VOTE_STATUS_MAX_EVENTS (5U)

/** 废票判定类型（按键模式：未选人/多选切换/标记不规范）。 */
typedef enum {
    VOTE_SPOILED_NONE = 0,
    VOTE_SPOILED_BLANK,
    VOTE_SPOILED_MULTIPLE,
    VOTE_SPOILED_IRREGULAR,
} vote_spoiled_type_e;

/** 投票时段内的交互 Phase（扁平 Phase 模型，见 doc/adr/0001）。 */
typedef enum {
    VOTE_INTERACTION_NONE = 0,
    VOTE_INTERACTION_SELECTING,
    VOTE_INTERACTION_COOLDOWN,
    VOTE_INTERACTION_VIOLATION,
} vote_interaction_phase_e;

/** 写入 JSON 到 `out`；返回写入长度，失败返回 0。 */
size_t vote_status_build_json(char *out, size_t out_cap);

/** 初始化默认候选人占位名（上电早期调用）。 */
void vote_status_init_defaults(void);

/** 记录一条最近操作（供后续计票流程调用）。 */
void vote_status_push_event(const char *time_hms, const char *text);

/** 重置票数与事件（管理员重置时调用）。 */
void vote_status_reset_counts(void);

/** Vote Reset 完成后：徽章显示 Idle，日程仍按时钟判断（见 CONTEXT.md）。 */
void vote_status_on_vote_reset(void);

/** Vote Reset 后是否仍显示 Idle 徽章（不影响红外/日程 voting 判定）。 */
bool vote_status_post_reset_idle(void);
void vote_status_clear_post_reset_idle(void);

/** 从 NVS 加载票数镜像。 */
void vote_status_load_counts(uint16_t valid, uint16_t spoiled, const uint32_t *cand_votes, uint8_t count);

uint8_t vote_status_candidate_count(void);
uint16_t vote_status_votes(uint8_t idx);
uint16_t vote_status_spoiled(void);
uint16_t vote_status_valid_total(void);
bool vote_status_has_any_votes(void);

const char *vote_status_candidate_name(uint8_t idx);
/** LCD 用英文名（≤20 字符 ASCII）。 */
const char *vote_status_candidate_lcd_name(uint8_t idx);
void vote_status_set_candidate_name(uint8_t idx, const char *name);

/** 有效票 +1 / 废票 +1；返回 false 若 idx 无效。 */
bool vote_status_add_valid(uint8_t idx);
bool vote_status_add_spoiled(void);
bool vote_status_add_spoiled_typed(vote_spoiled_type_e type, uint8_t cand_idx);

vote_spoiled_type_e vote_status_last_spoiled_type(void);
const char *vote_status_spoiled_type_label(vote_spoiled_type_e type);

/** 冷却剩余秒数（计票后递减；JSON 与 LCD 共用）。 */
uint8_t vote_status_cooldown_remaining(void);
void vote_status_set_cooldown_remaining(uint8_t sec);

/** 手动设置 DS3231 时分秒（保留当前日期）；失败返回 false。 */
bool vote_status_set_clock_hms(uint8_t hour, uint8_t minute, uint8_t second);

/** 设置/清除投票时段内交互 Phase（selecting/cooldown/violation）。 */
void vote_status_set_interaction_phase(vote_interaction_phase_e phase);
void vote_status_clear_interaction_phase(void);
vote_interaction_phase_e vote_status_interaction_phase(void);

/**
 * 当前扁平 Phase：booting | fault | idle | waiting | voting | selecting | cooldown | violation | locked
 */
const char *vote_status_current_phase(int *countdown_sec);

/** 仅日程驱动 Phase（不含交互 overlay）。 */
const char *vote_status_schedule_phase(int *countdown_sec);

/** 按 phase 映射默认业务 LCD 屏（不含 admin / history）。 */
vote_lcd_screen_id_t vote_status_lcd_screen_for_phase(const char *phase);
bool vote_status_is_voting_phase(const char *phase);

/** 当前是否处于 Vote Schedule 投票窗口内（日程 voting，不含交互态判断）。 */
bool vote_status_schedule_in_voting_window(void);

/** 格式化 DS3231 当前时间为 HH:MM:SS；失败写入 "--:--:--"。 */
bool vote_status_format_clock(char *buf, size_t cap);

/** 当日 0 点起的秒数 [0..86399]；DS3231 不可用时返回 -1。 */
int vote_status_seconds_of_day(void);
