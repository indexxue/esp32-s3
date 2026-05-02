#ifndef PROJECT_BOARD_H
#define PROJECT_BOARD_H

#include "type.h"

/** 本板硬件引脚与外设接线（换板主要改这里） */
#define BOARD_GPIO_IO5 (5)
#define BOARD_GPIO_IO4_PWM (4)

#define BOARD_I2C_PORT (0)
#define BOARD_I2C_SDA_GPIO (6)
#define BOARD_I2C_SCL_GPIO (7)
#define BOARD_I2C_MAX_DEVICE_SLOTS (1U)

/** IO4 呼吸灯 PWM 频率（与 BoardInit 中配置一致） */
#define BOARD_IO4_PWM_FREQ_HZ (5000U)

/**
 * 板级基础初始化：GPIO 驱动与 IO5、PWM 与 IO4 通道、I2C 总线。
 * @return 任一步失败返回 FALSE，且已尽力回滚此前已成功的步骤。
 */
bool_t BoardInit(void);

/** 与 BoardInit 对称，释放本模块初始化的外设（忽略未初始化状态） */
void BoardDeinit(void);

/**
 * 仅释放 I2C 总线（并同步内部状态）。在仍有任务使用 GPIO/PWM 时用于错误回滚，
 * 不可调用 BoardDeinit()。
 */
void BoardDeinitI2cBus(void);

#endif
