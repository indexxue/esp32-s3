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

/** 当前无外设初始化；后续接 GPIO/PWM/I2C 等时再在此实现。 */
bool_t BoardInit(void);

void BoardDeinit(void);

void BoardDeinitI2cBus(void);

#endif
