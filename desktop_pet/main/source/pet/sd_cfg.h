/**
 * @file sd_cfg.h
 * @brief SD mount-root config (`config`) + file inventory. No JSON.
 */

#ifndef SD_CFG_H
#define SD_CFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_CFG_NAME "config"
#define SD_CFG_KEY_LEN (24)
#define SD_CFG_VAL_LEN (64)
#define SD_CFG_REL_LEN (96)
#define SD_CFG_PATH_MAX (256)
#define SD_CFG_MAX_KEYS (16U)
#define SD_CFG_MAX_FILES (48U)

bool sd_cfg_load(const char *mount);
bool sd_cfg_is_loaded(void);
const char *sd_cfg_mount(void);
const char *sd_cfg_get(const char *key);
const char *sd_cfg_content_path(void);
const char *sd_cfg_record_path(void);
uint8_t sd_cfg_file_count(void);
bool sd_cfg_file_at(uint8_t index, char *rel, size_t rel_len, uint32_t *size);

#ifdef __cplusplus
}
#endif

#endif /* SD_CFG_H */
