/**
 * @file vote_history.h
 * @brief 投票历史记录（逐条计票事件，RAM 镜像 + NVS，FIFO 最多 20 条）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vote_status.h"
#include "vote_nvs.h"

#define VOTE_HISTORY_MAX_RECORDS (20U)

#define VOTE_HISTORY_KIND_VALID (0U)
#define VOTE_HISTORY_KIND_SPOILED (1U)

typedef struct {
    uint32_t date_ymd; /**< YYYYMMDD */
    uint32_t time_hms; /**< HHMMSS */
    uint8_t kind;      /**< VOTE_HISTORY_KIND_* */
    uint8_t spoiled_type;
    uint8_t cand_idx;
    char cand_name[VOTE_NVS_CAND_NAME_BUF];
} vote_history_entry_t;

void vote_history_init(void);

uint8_t vote_history_count(void);

/** @a display_idx 0 = 最新一条，1 = 次新，依此类推。 */
bool vote_history_get_display(uint8_t display_idx, const vote_history_entry_t **out);

/** 有效票写入历史（满 20 条丢弃最旧）。 */
bool vote_history_append_valid(uint8_t cand_idx);

/** 废票写入历史。 */
bool vote_history_append_spoiled(vote_spoiled_type_e type, uint8_t cand_idx);

/**
 * @deprecated 逐条记录已替代场次归档；保留 API 兼容，恒返回 false。
 */
bool vote_history_archive_session_if_needed(void);

/** @deprecated 保留兼容。 */
bool vote_history_append_current(void);

void vote_history_on_session_reset(void);

bool vote_history_clear_all(void);

/** 格式化时间行，如 `12:34:56`。 */
void vote_history_format_time(const vote_history_entry_t *e, char *buf, size_t cap);

/** 格式化详情行（GB2312），如 `有效 Alice`。 */
void vote_history_format_detail(const vote_history_entry_t *e, char *buf, size_t cap);

/** @deprecated 使用 format_time。 */
void vote_history_format_title(const vote_history_entry_t *e, char *buf, size_t cap);

/** @deprecated 使用 format_detail。 */
void vote_history_format_summary(const vote_history_entry_t *e, char *buf, size_t cap);

size_t vote_history_build_json(char *out, size_t out_cap);
