/**
 * @file web_ctrl_cmd.h
 * @brief 阶段 3：将 HTTP 侧「一行厂测命令」投递到独立任务执行，经 `cmd_process_line_for_web` 收集应答（与 USB 读行互斥）。
 */

#pragma once

#include <stddef.h>

#include "esp_err.h"

/** 与 `web_ctrl_cmd` 内部槽位缓冲一致；`POST /api/cmd` 应答拷贝上限。 */
#define WEB_CTRL_CMD_REPLY_MAX (384U)

esp_err_t web_ctrl_cmd_start(void);

void web_ctrl_cmd_stop(void);

/**
 * @brief 在 `web_ctrl_cmd` 任务中执行一行命令并等待完成。
 * @param line 与串口相同的命令行（不含 \\r\\n）。
 * @param reply_out 写入应答文本（与串口 `cmd:` 格式一致，可含多行）。
 * @param reply_cap `reply_out` 容量。
 * @param out_len 实际写入长度（不含结尾 NUL）。
 * @param timeout_ms 等待执行完成的最长时间。
 * @retval ESP_OK 成功
 * @retval ESP_ERR_INVALID_STATE 已有请求在执行（503）
 * @retval ESP_ERR_TIMEOUT 等待超时（504）；内部会排空本次执行后再返回。
 */
esp_err_t web_ctrl_cmd_execute_sync(const char *line, char *reply_out, size_t reply_cap, size_t *out_len,
                                    int timeout_ms);
