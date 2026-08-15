/*
 * @Author: indexxue 2308039918@qq.com
 * @Date: 2026-08-15 14:45:45
 * @LastEditors: indexxue 2308039918@qq.com
 * @LastEditTime: 2026-08-15 14:49:41
 * @FilePath: \ESP32-S3\desktop_pet\main\source\pet\pet_i18n\pet_i18n.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
/**
 * @file pet_i18n.h
 * @brief UI chrome locale: firmware base table + optional pet/lang TXT overlay.
 */

#ifndef PET_I18N_H
#define PET_I18N_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PET_I18N_KEY_LEN (32U)
#define PET_I18N_VAL_LEN (48U)
#define PET_I18N_OVERLAY_MAX (48U)

#define PET_I18N_REL_EN "lang/en.txt"
#define PET_I18N_REL_ZH "lang/zh.txt"

typedef enum {
    PET_I18N_LANG_EN = 0,
    PET_I18N_LANG_ZH,
} pet_i18n_lang_t;

/** Load SD overlay for lang (clears prior overlay). Missing file is OK. */
void pet_i18n_load(pet_i18n_lang_t lang);

pet_i18n_lang_t pet_i18n_lang(void);

/**
 * Resolve UI chrome string by key.
 * Overlay → firmware[lang] → firmware[EN] → key.
 * When prefer_zh is false, ZH firmware/overlay values are skipped (use EN).
 */
const char *pet_i18n_tr(const char *key);

/**
 * Like pet_i18n_tr, but if use_zh is false forces English firmware/overlay path
 * (used when CJK font unavailable).
 */
const char *pet_i18n_tr_ex(const char *key, bool use_zh);

#ifdef __cplusplus
}
#endif

#endif /* PET_I18N_H */
