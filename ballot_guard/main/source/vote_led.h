/**
 * @file vote_led.h
 * @brief ballot_guard RGB 灯效：按投票 phase / 业务事件驱动 led_scene。
 */

#ifndef VOTE_LED_H
#define VOTE_LED_H

#ifdef __cplusplus
extern "C" {
#endif

/** 自检 boot 动画结束，按当前 phase 恢复底色。 */
void vote_led_on_boot_ready(void);

/** phase 变化时同步底色（waiting/locked → 绿常亮，voting → 蓝慢闪）。 */
void vote_led_sync_phase(const char *phase);

/** 红外检测到人靠近（进入选人屏前）。 */
void vote_led_on_ir_approach(void);

/** 有效票登记成功。 */
void vote_led_on_valid_vote(void);

/** 废票登记（红闪 3 次）。 */
void vote_led_on_spoiled_vote(void);

/** 冷却期内重复触发红外（违规屏）。 */
void vote_led_on_violation(void);

/** 离开违规屏。 */
void vote_led_on_violation_end(void);

/** LCD/RTC 等关键外设初始化失败。 */
void vote_led_on_fault(void);

/** Wi-Fi STA 已连接 / 断开（黄色常亮提示离线）。 */
void vote_led_on_wifi_sta_connected(void);
void vote_led_on_wifi_sta_disconnected(void);

#ifdef __cplusplus
}
#endif

#endif /* VOTE_LED_H */
