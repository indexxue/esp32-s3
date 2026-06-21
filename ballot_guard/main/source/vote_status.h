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

/** 写入 JSON 到 `out`；返回写入长度，失败返回 0。 */
size_t vote_status_build_json(char *out, size_t out_cap);

/** 初始化默认候选人占位名（上电早期调用）。 */
void vote_status_init_defaults(void);

/** 记录一条最近操作（供后续计票流程调用）。 */
void vote_status_push_event(const char *time_hms, const char *text);

/** 重置票数与事件（管理员重置时调用）。 */
void vote_status_reset_counts(void);

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

/** phase: idle / waiting / voting / locked / booting / fault */
const char *vote_status_current_phase(int *countdown_sec);
/** 按 phase 映射默认业务 LCD 屏（不含 admin / history）。 */
vote_lcd_screen_id_t vote_status_lcd_screen_for_phase(const char *phase);
bool vote_status_is_voting_phase(const char *phase);

/** 格式化 DS3231 当前时间为 HH:MM:SS；失败写入 "--:--:--"。 */
bool vote_status_format_clock(char *buf, size_t cap);

/** 当日 0 点起的秒数 [0..86399]；DS3231 不可用时返回 -1。 */
int vote_status_seconds_of_day(void);
