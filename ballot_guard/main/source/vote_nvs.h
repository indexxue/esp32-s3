/**
 * @file vote_nvs.h
 * @brief ballot_guard 专用 NVS 命名空间（cfg_* / cand_* / vote_*）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vote_status.h"

#define VOTE_NVS_CAND_NAME_MAX (20U)
#define VOTE_NVS_CAND_NAME_BUF (VOTE_NVS_CAND_NAME_MAX + 1U)

typedef struct {
    uint8_t start_h;
    uint8_t start_m;
    uint8_t end_h;
    uint8_t end_m;
    uint8_t candidate_count;
    uint8_t cooldown_sec;
} vote_nvs_cfg_t;

/** 上电调用一次；加载配置与票数到 RAM 镜像。 */
void vote_nvs_init(void);

bool vote_nvs_load_cfg(vote_nvs_cfg_t *out);
bool vote_nvs_save_cfg(const vote_nvs_cfg_t *cfg);

bool vote_nvs_get_candidate_name(uint8_t idx, char *out, size_t out_cap);
bool vote_nvs_set_candidate_name(uint8_t idx, const char *name);
bool vote_nvs_set_candidate_names(const char *const *names, uint8_t count);

/** 校验英文名：非空、≤20 字符、[A-Za-z0-9 -]。 */
bool vote_nvs_validate_candidate_name(const char *name);

bool vote_nvs_save_votes(void);

/** 从 NVS 重新加载 cfg 到 vote_menu_settings（丢弃 RAM 未保存修改）。 */
bool vote_nvs_reload_settings(void);

/** 恢复出厂默认 cfg（时段/人数/冷却）并写入 NVS。 */
bool vote_nvs_restore_default_cfg(void);

/** 将所有候选人姓名恢复为默认 Candidate N 并写入 NVS。 */
bool vote_nvs_restore_default_candidate_names(void);
