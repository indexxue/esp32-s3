/*
 * @Author: indexxue 2308039918@qq.com
 * @Date: 2026-04-30 21:58:22
 * @LastEditors: indexxue 2308039918@qq.com
 * @LastEditTime: 2026-05-02 10:16:34
 * @FilePath: \ESP32-S3\project\main\project.c
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include <stdio.h>

#include "esp_attr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "gpio.h"
#include "i2c.h"
#include "pwm.h"
#include "timer.h"
#include "usb_serial_jtag.h"

/* -------------------------------------------------------------------------- */
/* 常量                                                                       */
/* -------------------------------------------------------------------------- */

/** 任务1：推挽输出，周期翻转 */
#define IO5_TOGGLE_PERIOD_MS (500U)

/** 任务2：IO4 PWM 呼吸灯（引脚见 board.h） */
#define PWM_BREATHE_DELAY_MS (4U)
#define PWM_BREATHE_STEPS (32U)

#define TASK_STACK_WORDS (3072U)
#define TASK_PRIO (5)

/** I2C 总线扫描周期（GPTimer） */
#define I2C_SCAN_PERIOD_MS (3000U)
#define I2C_SCAN_TIMER_RES_HZ (1000000U)

/** USB Serial/JTAG：周期上报、回显（打开 PC 串口工具可测） */
#define USB_USJ_TX_BUF (256U)
#define USB_USJ_RX_BUF (256U)
#define USB_USJ_STATUS_PERIOD_MS (2000U)

static bool_t s_usbSerialJtagReady = FALSE;

/* -------------------------------------------------------------------------- */
/* I2C 周期扫描（GPTimer + 任务通知）                                         */
/* -------------------------------------------------------------------------- */

static TaskHandle_t s_i2c_scan_task_handle = NULL;

static void i2c_scan_bus(void)
{
    u16_t addr;
    u8_t found = 0U;

    printf("[I2C] bus scan (0x08..0x77)\n");
    for (addr = 0x08U; addr <= 0x77U; addr++) {
        if (I2cProbe(addr) == TRUE) {
            printf("  found 7-bit addr 0x%02X\n", (unsigned)addr);
            found++;
        }
    }
    if (found == 0U) {
        printf("  (no device responded)\n");
    }
}

static void IRAM_ATTR i2c_scan_on_timer(GptimerHandle_t handle, void *userData)
{
    BaseType_t hpw = pdFALSE;
    (void)handle;
    (void)userData;
    if (s_i2c_scan_task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_i2c_scan_task_handle, &hpw);
    }
    portYIELD_FROM_ISR(hpw);
}

static void task_i2c_scan(void *arg)
{
    (void)arg;

    while (1) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        i2c_scan_bus();
    }
}

/* -------------------------------------------------------------------------- */
/* IO5 翻转                                                                   */
/* -------------------------------------------------------------------------- */

static void task_io5_toggle(void *arg)
{
    (void)arg;
    unsigned long flip_count = 0UL;

    while (1) {
        if (GpioTogglePin(BOARD_GPIO_IO5) == FALSE) {
            printf("[IO5] GpioTogglePin failed, err=%ld\n", (long)GpioGetLastError());
            vTaskDelay(pdMS_TO_TICKS(1000U));
            continue;
        }
        flip_count++;
        printf("[IO5] flip #%lu\n", flip_count);
        vTaskDelay(pdMS_TO_TICKS(IO5_TOGGLE_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* IO4 PWM 呼吸灯                                                             */
/* -------------------------------------------------------------------------- */

/** pdMS_TO_TICKS 小毫秒数在 100Hz tick 下可能为 0，vTaskDelay(0) 无法让 IDLE 喂狗 */
static TickType_t pwm_breathe_delay_ticks(void)
{
    TickType_t const t = pdMS_TO_TICKS(PWM_BREATHE_DELAY_MS);
    return (t < (TickType_t)1) ? (TickType_t)1 : t;
}

/* -------------------------------------------------------------------------- */
/* USB Serial/JTAG 测试（需 UsbSerialJtagDriverInit 成功；与控制台同占 USJ 时可能失败） */
/* -------------------------------------------------------------------------- */

static void task_usb_serial_jtag_test(void *arg)
{
    u8_t rxbuf[128];
    usize_t rlen;
    usize_t wlen;
    u32_t seq = 0U;

    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300U));

    while (1) {
        while (UsbSerialJtagRead(rxbuf, sizeof(rxbuf), 0, &rlen) == TRUE && rlen > 0U) {
            if (UsbSerialJtagWrite(rxbuf, rlen, 1000, &wlen) == FALSE) {
                break;
            }
        }

        seq++;
        {
            char line[96];
            int const n = snprintf(line,
                                   sizeof(line),
                                   "[USB Serial/JTAG] seq=%lu host=%s\r\n",
                                   (unsigned long)seq,
                                   UsbSerialJtagIsHostConnected() ? "yes" : "no");
            if ((n > 0) && (n < (int)sizeof(line))) {
                (void)UsbSerialJtagWrite((const u8_t *)line, (usize_t)n, 1000, &wlen);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(USB_USJ_STATUS_PERIOD_MS));
    }
}

static void task_io4_pwm_breathe(void *arg)
{
    (void)arg;
    s32_t maxDutyS = PwmGetMaxDuty();
    if (maxDutyS <= 0) {
        printf("[IO4 PWM] PwmGetMaxDuty invalid: %ld\n", (long)maxDutyS);
        vTaskDelete(NULL);
        return;
    }
    u32_t const maxDuty = (u32_t)maxDutyS;
    u32_t const step =
        (maxDuty / PWM_BREATHE_STEPS) != 0U ? (maxDuty / PWM_BREATHE_STEPS) : 1U;

    printf("[IO4 PWM] breathe, maxDuty=%lu step=%lu\n",
           (unsigned long)maxDuty, (unsigned long)step);

    while (1) {
        u32_t d;
        for (d = 0U; d <= maxDuty; d += step) {
            if (PwmSetDuty(PWM_CHANNEL_0_E, d) == FALSE) {
                printf("[IO4 PWM] PwmSetDuty failed, err=%ld\n", (long)PwmGetLastError());
                vTaskDelay(pdMS_TO_TICKS(1000U));
                continue;
            }
            vTaskDelay(pwm_breathe_delay_ticks());
        }
        for (d = maxDuty; d > 0U; d -= step) {
            if (PwmSetDuty(PWM_CHANNEL_0_E, d) == FALSE) {
                printf("[IO4 PWM] PwmSetDuty failed, err=%ld\n", (long)PwmGetLastError());
                vTaskDelay(pdMS_TO_TICKS(1000U));
                continue;
            }
            vTaskDelay(pwm_breathe_delay_ticks());
        }
    }
}

/* -------------------------------------------------------------------------- */
/* app_main：板级初始化、定时器、任务                                         */
/* -------------------------------------------------------------------------- */

/** 从 TimerDriverInit 到 TimerSetAlarm；失败时已按序回滚，不创建任务 */
static bool_t app_setup_i2c_scan_timer(GptimerHandle_t *out_timer)
{
    GptimerHandle_t scanTimer = GPTIMER_HANDLE_INVALID;

    if (TimerDriverInit() == FALSE) {
        printf("TimerDriverInit failed, err=%ld\n", (long)TimerGetLastError());
        return FALSE;
    }

    {
        TimerConfig_t tcfg = {
            .clockSource = TIMER_CLOCK_SRC_DEFAULT_E,
            .countDirection = TIMER_COUNT_UP_E,
            .resolutionHz = I2C_SCAN_TIMER_RES_HZ,
            .intrPriority = 0,
        };
        if (TimerCreate(&tcfg, &scanTimer) == FALSE) {
            printf("TimerCreate failed, err=%ld\n", (long)TimerGetLastError());
            (void)TimerDriverDeinit();
            return FALSE;
        }
    }

    if (TimerRegisterCallback(scanTimer, i2c_scan_on_timer, NULL) == FALSE) {
        printf("TimerRegisterCallback failed, err=%ld\n", (long)TimerGetLastError());
        (void)TimerDelete(scanTimer);
        (void)TimerDriverDeinit();
        return FALSE;
    }

    {
        u64_t const periodCounts = (u64_t)I2C_SCAN_PERIOD_MS * 1000ULL;
        TimerAlarmConfig_t alarm = {
            .alarmCount = periodCounts,
            .reloadCount = 0ULL,
            .autoReload = TRUE,
        };
        if (TimerSetAlarm(scanTimer, &alarm) == FALSE) {
            printf("TimerSetAlarm failed, err=%ld\n", (long)TimerGetLastError());
            (void)TimerDelete(scanTimer);
            (void)TimerDriverDeinit();
            return FALSE;
        }
    }

    *out_timer = scanTimer;
    return TRUE;
}

/**
 * 创建 IO5 / IO4 / I2C 扫描任务。若 io5 失败则全板回滚；若 io4 或 i2c 任务失败则仅释放 I2C
 *（io5 已跑，不可 BoardDeinit）。
 */
static bool_t app_create_tasks(GptimerHandle_t scan_timer)
{
    BaseType_t ok;

    ok = xTaskCreate(task_io5_toggle, "io5_toggle", TASK_STACK_WORDS, NULL, TASK_PRIO, NULL);
    if (ok != pdPASS) {
        printf("xTaskCreate io5_toggle failed\n");
        (void)TimerDelete(scan_timer);
        (void)TimerDriverDeinit();
        BoardDeinit();
        return FALSE;
    }

    ok = xTaskCreate(task_io4_pwm_breathe, "io4_pwm", TASK_STACK_WORDS, NULL, TASK_PRIO, NULL);
    if (ok != pdPASS) {
        printf("xTaskCreate io4_pwm failed\n");
        (void)TimerDelete(scan_timer);
        (void)TimerDriverDeinit();
        BoardDeinitI2cBus();
        return FALSE;
    }

    ok = xTaskCreate(task_i2c_scan, "i2c_scan", TASK_STACK_WORDS, NULL, TASK_PRIO,
                     &s_i2c_scan_task_handle);
    if (ok != pdPASS) {
        printf("xTaskCreate i2c_scan failed\n");
        (void)TimerDelete(scan_timer);
        (void)TimerDriverDeinit();
        BoardDeinitI2cBus();
        return FALSE;
    }

    if (s_usbSerialJtagReady != FALSE) {
        ok = xTaskCreate(task_usb_serial_jtag_test, "usb_usj", TASK_STACK_WORDS, NULL, TASK_PRIO, NULL);
        if (ok != pdPASS) {
            printf("xTaskCreate usb_usj failed\n");
            (void)UsbSerialJtagDriverDeinit();
            s_usbSerialJtagReady = FALSE;
        }
    }

    return TRUE;
}

void app_main(void)
{
    GptimerHandle_t scan_timer = GPTIMER_HANDLE_INVALID;

    if (BoardInit() == FALSE) {
        printf("BoardInit failed (gpio err=%ld pwm err=%ld i2c err=%ld)\n",
               (long)GpioGetLastError(),
               (long)PwmGetLastError(),
               (long)I2cGetLastError());
        return;
    }

    {
        UsbSerialJtagDriverConfig_t const usj = {
            .txBufferSize = USB_USJ_TX_BUF,
            .rxBufferSize = USB_USJ_RX_BUF,
        };
        if (UsbSerialJtagDriverInit(&usj) == TRUE) {
            s_usbSerialJtagReady = TRUE;
            printf("UsbSerialJtagDriverInit ok (task will echo RX + status every %u ms)\n",
                   (unsigned)USB_USJ_STATUS_PERIOD_MS);
        } else {
            printf("UsbSerialJtagDriverInit failed err=%ld (若 menuconfig 主控制台为 USB Serial/JTAG 会冲突)\n",
                   (long)UsbSerialJtagGetLastError());
        }
    }

    if (app_setup_i2c_scan_timer(&scan_timer) == FALSE) {
        if (s_usbSerialJtagReady != FALSE) {
            (void)UsbSerialJtagDriverDeinit();
            s_usbSerialJtagReady = FALSE;
        }
        BoardDeinit();
        return;
    }

    printf("Task1: IO5 toggle %u ms | Task2: IO4 PWM breathe %u Hz | I2C scan every %u ms on SDA=%d SCL=%d\n",
           (unsigned)IO5_TOGGLE_PERIOD_MS,
           (unsigned)BOARD_IO4_PWM_FREQ_HZ,
           (unsigned)I2C_SCAN_PERIOD_MS,
           BOARD_I2C_SDA_GPIO,
           BOARD_I2C_SCL_GPIO);

    if (app_create_tasks(scan_timer) == FALSE) {
        if (s_usbSerialJtagReady != FALSE) {
            (void)UsbSerialJtagDriverDeinit();
            s_usbSerialJtagReady = FALSE;
        }
        return;
    }

    if (TimerStart(scan_timer) == FALSE) {
        printf("TimerStart failed, err=%ld\n", (long)TimerGetLastError());
        return;
    }

    if (s_i2c_scan_task_handle != NULL) {
        xTaskNotifyGive(s_i2c_scan_task_handle);
    }

    vTaskDelete(NULL);
}
