/**
 * @file wake.h
 * @brief Main-screen WakeNet (ESP-SR); SD models; wake → chat + reply + listen.
 *
 * Memory phases (internal SRAM):
 *   HOME  — WakeNet + httpd
 *   CHAT  — httpd suspended (STA kept); WakeNet kept, detect paused
 *   Leave — resume httpd (no model recreate)
 *   Blank — detect armed even if chat UI open (voice lights + γ)
 */

#ifndef WAKE_H
#define WAKE_H

#include "type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

status_t desktop_pet_wake_init(void);

bool desktop_pet_wake_is_available(void);

/** True during detect→reply→listen (inhibit screen blank). */
bool desktop_pet_wake_is_busy(void);

void desktop_pet_wake_set_home_active(bool active);

/** Enter chat: suspend httpd + pause detect (keep WakeNet). Idempotent. */
void desktop_pet_wake_enter_chat_mode(void);

/** Leave chat: re-arm detect + deferred httpd resume. Idempotent. */
void desktop_pet_wake_leave_chat_mode(void);

/**
 * Screen blanked while chat may have paused detect — allow WakeNet again
 * without session_close / httpd changes.
 */
void desktop_pet_wake_arm_for_blank(void);

void desktop_pet_wake_ui_poll(void);

void desktop_pet_wake_yield_for_playback(void);

#ifdef __cplusplus
}
#endif

#endif /* WAKE_H */
