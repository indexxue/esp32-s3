/**
 * @file    cmd.h
 * @brief   串口/命令行厂测框架（语义对齐 STM32 工程 `tmp/serial_cmd`）：注册表、统一应答、按行解析。
 *
 * 用法概要：
 *   1. `cmd_init(write_fn, write_ctx)`；`write_fn` 为 NULL 时使用 `stdout` 写应答。
 *   2. `cmd_register_defaults()` 或 `cmd_register()` 注册自定义命令。
 *   3. 收到完整一行后调用 `cmd_process_line(line)`；或 `cmd_usb_line_service_start()` 启动 USB 读行任务。
 *
 * 应答：成功 `cmd:value\r\n`，失败 `ng\r\n`。输入 `help` 列出已注册命令。
 */

#ifndef COMMON_CMD_H
#define COMMON_CMD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "type.h"

#define CMD_TX_MUTEX_TIMEOUT_MS (500U)
#define CMD_LINE_MAX (200U)
#define CMD_NAME_MAX (16U)
#define CMD_HELP_MAX (48U)
#define CMD_MAX_COMMANDS (32U)
#define CMD_MAX_ARGC (8U)
#define CMD_STATUS_BUF_SIZE (128U)

/** 写串口/控制台；返回 true 表示本次写入已提交（与 STM32 `serial_cmd_send` 语义一致）。 */
typedef bool (*cmd_write_fn)(const void *data, size_t len, void *user_ctx);

typedef void (*cmd_handler_t)(int argc, const char *argv[]);

typedef struct {
    char name[CMD_NAME_MAX];
    cmd_handler_t handler;
    char help[CMD_HELP_MAX];
} cmd_entry_t;

void cmd_init(cmd_write_fn write_fn, void *write_ctx);

int cmd_register(const char *name, cmd_handler_t handler, const char *help);

void cmd_reply_ok(const char *cmd, const char *value);

void cmd_reply_ng(void);

bool cmd_send(const uint8_t *data, uint16_t len);

bool cmd_send_str(const char *str);

void cmd_process_line(const char *line);

/**
 * @brief 与 `cmd_process_line` 相同解析，但将 `cmd_reply_*` / `cmd_send` 输出写入 `cap` 缓冲（用于 HTTP 等侧路应答）。
 * @note 与 USB 读行任务互斥；`cap->buf` 与 `cap->cap` 须有效，成功时 `cap->len` 为写入长度且 `buf` 已 NUL 结尾。
 */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} cmd_web_capture_t;

void cmd_process_line_for_web(const char *line, cmd_web_capture_t *cap);

/** 若尚未注册任何命令，则 `cmd_init`（丢弃写）并 `cmd_register_defaults`（供仅 Web 路径使用）。 */
void cmd_embed_register_defaults_if_needed(void);

void cmd_register_defaults(void);

/** 应用层在 `cmd_register_defaults` 之后追加注册的回调（可 NULL，仅注册一次）。 */
void cmd_set_project_register_fn(void (*fn)(void));

void cmd_uart_lock(void);

void cmd_uart_unlock(void);

/**
 * @brief 安装 USB Serial/JTAG 驱动、用其作为应答通道，并创建读行任务（幂等）。
 * @note 依赖 `nvs_init` 与板级 I2C 已就绪后再调用，以便厂测命令可用。
 */
status_t cmd_usb_line_service_start(void);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_CMD_H */
