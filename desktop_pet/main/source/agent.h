/**
 * @file agent.h
 * @brief 小智兼容 WebSocket 会话（OTA 登记 + hello + Opus）。
 */

#ifndef AGENT_H
#define AGENT_H

#include "type.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DESKTOP_PET_AGENT_STATE_IDLE = 0,
    DESKTOP_PET_AGENT_STATE_CONNECTING,
    DESKTOP_PET_AGENT_STATE_OPEN,
    DESKTOP_PET_AGENT_STATE_LISTENING,
    DESKTOP_PET_AGENT_STATE_SPEAKING,
    DESKTOP_PET_AGENT_STATE_ERROR,
} desktop_pet_agent_state_t;

typedef enum {
    DESKTOP_PET_AGENT_UI_STATE = 0, /* text unused; poll get_state */
    DESKTOP_PET_AGENT_UI_STT,
    DESKTOP_PET_AGENT_UI_TTS_TEXT,
    DESKTOP_PET_AGENT_UI_LLM,
    DESKTOP_PET_AGENT_UI_NET, /* 网络/会话失败提示，展示为字幕 */
} desktop_pet_agent_ui_evt_t;

/** May run off the LVGL thread — defer UI work. */
typedef void (*desktop_pet_agent_ui_cb_t)(desktop_pet_agent_ui_evt_t evt, const char *text);

status_t desktop_pet_agent_init(void);

/** 空闲则开会话（不自动听）；已打开/聆听则关闭。可在按键回调中调用（内部起任务）。 */
status_t desktop_pet_agent_session_toggle(void);

/** 仅开会话（hello），不 listen。对话页进页用；已打开则 no-op。 */
status_t desktop_pet_agent_session_open(void);

/** 关闭会话（离开对话页）。 */
status_t desktop_pet_agent_session_close(void);

/**
 * 开始听：无会话则先连接再 listen；已 OPEN 则直接 listen。
 * 可从 LVGL 回调调用（内部起任务，不阻塞 UI）。SPEAKING 时会先停 TTS。
 */
status_t desktop_pet_agent_listen_start(void);

/** 结束听：stop listen，保持 session OPEN。 */
status_t desktop_pet_agent_listen_stop(void);

/**
 * 单击切换听/停。进行中（已点 Talk 尚未进入 LISTENING）再点也会停。
 */
status_t desktop_pet_agent_listen_toggle(void);

desktop_pet_agent_state_t desktop_pet_agent_get_state(void);

/** 拷贝当前 session_id；未打开时返回空串。 */
status_t desktop_pet_agent_copy_session_id(char *buf, size_t buf_len);

bool desktop_pet_agent_is_open(void);

/** LISTENING，或已请求开始听但尚未进入 LISTENING。 */
bool desktop_pet_agent_is_listen_active(void);

void desktop_pet_agent_set_ui_cb(desktop_pet_agent_ui_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* AGENT_H */
