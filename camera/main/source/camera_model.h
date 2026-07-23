/**
 * @file camera_model.h
 * @brief 端侧目标检测推理管道（ESP-DL + ESPDet-Pico）。
 *
 * 预处理：240×240 RGB565 → 缩放 224×224 → RGB888 → 归一化 INT8
 * 后处理：解析输出张量 → DIoU-NMS → 坐标映射回 240×240 像素
 * 输出：通过 printf（USB-Serial-JTAG）发送，同时可选 LCD 画框。
 */

#pragma once

#include "type.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 模型输入尺寸（ESPDet-Pico 标准）。 */
#define CAMERA_MODEL_INPUT_W (224U)
#define CAMERA_MODEL_INPUT_H (224U)
#define CAMERA_MODEL_INPUT_C (3U)

/** 摄像头原始帧尺寸。 */
#define CAMERA_MODEL_CAM_W (240U)
#define CAMERA_MODEL_CAM_H (240U)

/** NMS 最大候选框数。 */
#define CAMERA_MODEL_MAX_BOXES (100U)

/** 检测结果（单个目标）。坐标相对于摄像头原始帧（240×240）。 */
typedef struct {
    uint16_t x;          /**< 框左上角 x（像素） */
    uint16_t y;          /**< 框左上角 y（像素） */
    uint16_t w;          /**< 框宽（像素） */
    uint16_t h;          /**< 框高（像素） */
    uint8_t  class_id;   /**< 类别 ID */
    float    confidence;  /**< 置信度 [0.0, 1.0] */
} camera_detection_t;

/** 一帧检测结果集。 */
typedef struct {
    camera_detection_t boxes[CAMERA_MODEL_MAX_BOXES];
    uint8_t            count;       /**< 本帧检测到的目标数 */
    uint32_t           frame_id;    /**< 帧序号（调用方维护） */
    uint32_t           elapsed_ms;  /**< 本帧推理耗时（ms） */
} camera_detection_result_t;

/**
 * @brief 加载模型并初始化推理引擎。
 *
 * 模型通过 target_add_binary_data 嵌入 Flash，函数从符号表获取指针。
 * PSRAM 中分配输入张量 buffer。上电后仅调用一次。
 *
 * @return STATUS_OK 成功，STATUS_FAIL 模型加载/分配失败。
 */
status_t camera_model_init(void);

/**
 * @brief 对一帧 RGB565 图像执行推理。
 *
 * @param rgb565  帧数据指针（RGB565 big-endian，240×240）。
 * @param width   帧宽（须为 240）。
 * @param height  帧高（须为 240）。
 * @param frame_id 帧序号（仅记录到结果中）。
 * @return 指向内部静态结果 buffer 的指针；无检测时 count=0。
 *         返回值有效期至下一次 detect() 调用。
 */
const camera_detection_result_t *camera_model_detect(const uint8_t *rgb565,
                                                      uint16_t width,
                                                      uint16_t height,
                                                      uint32_t frame_id);

/**
 * @brief 释放模型资源（reboot 前调用，当前为 no-op）。
 */
void camera_model_deinit(void);

/**
 * @brief 模型是否已成功加载。
 */
bool_t camera_model_is_ready(void);

#ifdef __cplusplus
}
#endif
