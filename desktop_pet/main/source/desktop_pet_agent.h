/**
 * @file desktop_pet_agent.h
 * @brief 小智兼容 WebSocket 会话（hello + Opus 上行）。
 */

#ifndef DESKTOP_PET_AGENT_H
#define DESKTOP_PET_AGENT_H

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

status_t desktop_pet_agent_init(void);

/** 空闲则开会话（不自动听）；已打开/聆听则关闭。可在按键回调中调用（内部起任务）。 */
status_t desktop_pet_agent_session_toggle(void);

/**
 * 开始听：无会话则先连接再 listen；已 OPEN 则直接 listen。
 * 可从 LVGL 回调调用（内部起任务，不阻塞 UI）。
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

#ifdef __cplusplus
}
#endif

#endif /* DESKTOP_PET_AGENT_H */
