/*
 * @Author: indexxue 2308039918@qq.com
 * @Date: 2026-04-30 21:58:22
 * @LastEditors: indexxue 2308039918@qq.com
 * @LastEditTime: 2026-05-02 10:16:34
 * @FilePath: \ESP32-S3\project\main\project.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"

#define LOG_INTERVAL_MS (2000U)

void app_main(void)
{
    if (BoardInit() == FALSE) {
        printf("BoardInit failed\n");
        return;
    }

    printf("app_main: peripherals idle, log only (interval %u ms)\n",
           (unsigned)LOG_INTERVAL_MS);

    for (unsigned n = 1U;; n++) {
        printf("alive #%u\n", n);
        vTaskDelay(pdMS_TO_TICKS(LOG_INTERVAL_MS));
    }
}
