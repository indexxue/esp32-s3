/**
 * @file vote_history.h
 * @brief 投票历史记录（RAM 镜像 + NVS hist_*，FIFO 最多 20 条）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vote_status.h"
#include "vote_nvs.h"

#define VOTE_HISTORY_MAX_RECORDS (20U)

typedef struct {
    /** 打包结束时刻：高 32 位语义为 YYYYMMDD×10000 + HH×100 + MM（无 RTC 时用配置结束时刻）。 */
    uint32_t end_stamp;
    uint16_t valid;
    uint16_t spoiled;
    uint16_t cand_votes[VOTE_STATUS_MAX_CANDIDATES];
    char names_snapshot[VOTE_STATUS_MAX_CANDIDATES][VOTE_NVS_CAND_NAME_BUF];
} vote_history_entry_t;

/** 从 NVS 加载；由 vote_nvs_init 调用。 */
void vote_history_init(void);

uint8_t vote_history_count(void);

/** @a display_idx 0 = 最新一条，1 = 次新，依此类推。 */
bool vote_history_get_display(uint8_t display_idx, const vote_history_entry_t **out);

/** 将当前场次快照追加到历史（满 20 条则丢弃最旧）。 */
bool vote_history_append_current(void);

/**
 * 投票结束或重置前归档当前场次（有票且本场未归档时写入一次）。
 * @return true 表示写入了新记录。
 */
bool vote_history_archive_session_if_needed(void);

/** 重置票数后清除「本场已归档」标记。 */
void vote_history_on_session_reset(void);

/** 清空全部历史记录（RAM + NVS）。 */
bool vote_history_clear_all(void);

/** 格式化顶栏时间行，如 `2026-06-19 16:00 End`。 */
void vote_history_format_title(const vote_history_entry_t *e, char *buf, size_t cap);

/** 格式化摘要行，如 `V25 S2 Alice12 Bob8`。 */
void vote_history_format_summary(const vote_history_entry_t *e, char *buf, size_t cap);

/** 写入 JSON 数组到 @a out；返回长度，失败 0。 */
size_t vote_history_build_json(char *out, size_t out_cap);
