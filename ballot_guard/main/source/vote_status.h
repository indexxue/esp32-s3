/**
 * @file vote_status.h
 * @brief ballot_guard 投票状态快照（供 Web `/api/vote/status` 与后续状态机共享）。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define VOTE_STATUS_MAX_CANDIDATES (6U)
#define VOTE_STATUS_MAX_EVENTS (5U)

/** 写入 JSON 到 `out`；返回写入长度，失败返回 0。 */
size_t vote_status_build_json(char *out, size_t out_cap);

/** 记录一条最近操作（供后续计票流程调用）。 */
void vote_status_push_event(const char *time_hms, const char *text);

/** 重置票数与事件（管理员重置时调用）。 */
void vote_status_reset_counts(void);

uint8_t vote_status_candidate_count(void);
uint16_t vote_status_votes(uint8_t idx);
uint16_t vote_status_spoiled(void);
const char *vote_status_candidate_name(uint8_t idx);
/** LCD 用 ASCII 简称（字库仅 ASCII/GB2312，勿直接显示 UTF-8 中文）。 */
const char *vote_status_candidate_lcd_name(uint8_t idx);
