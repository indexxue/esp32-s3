/**
 * @file pet_sfx.h
 * @brief Care SFX: play one random WAV from pet/sfx/<clip>/ per clip intent.
 */

#ifndef PET_SFX_H
#define PET_SFX_H

#include "pet_core.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** When true (chat listen/speak), Care SFX is ignored. */
void pet_sfx_set_chat_busy(bool busy);

/** Pick one WAV under sfx/<clip>/ and play; missing pool → silent. */
void pet_sfx_play_for_clip(pet_clip_id_t clip);

#ifdef __cplusplus
}
#endif

#endif /* PET_SFX_H */
