#include "espdet_detect.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

#if CONFIG_BALL_DETECT_MODEL_IN_FLASH_RODATA
extern const uint8_t ball_detect_espdl[] asm("_binary_ball_detect_espdl_start");
static const char *s_model_rodata = (const char *)ball_detect_espdl;
#elif CONFIG_BALL_DETECT_MODEL_IN_FLASH_PARTITION
static const char *s_model_rodata = "ball_det";
#endif

namespace ball_detect {

ESPDet::ESPDet(const char *model_name, float score_thr, float nms_thr)
{
#if CONFIG_BALL_DETECT_MODEL_IN_SDCARD
#error "ball_detect SDCARD model path is not supported in camera firmware"
#else
    /*
     * 强制权重进 PSRAM。Flash 直读 + 并发 JPEG 时 infer ~500ms+，叠框严重滞后。
     * 若 largest block 太小仍尝试 true（loader 内会按对齐等再判）。
     */
    const size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t spiram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    const bool param_copy = true;
    ESP_LOGI("ball_detect",
             "param_copy=%d spiram_free=%u largest=%u",
             (int)param_copy,
             (unsigned)spiram_free,
             (unsigned)spiram_largest);
    m_model = new dl::Model(s_model_rodata,
                            model_name,
                            static_cast<fbs::model_location_type_t>(CONFIG_BALL_DETECT_MODEL_LOCATION),
                            0,
                            dl::MEMORY_MANAGER_GREEDY,
                            nullptr,
                            param_copy);
#endif
    m_model->minimize();
    m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});
    m_image_preprocessor->enable_letterbox({114, 114, 114});
    m_postprocessor = new dl::detect::ESPDetPostProcessor(
        m_model, m_image_preprocessor, score_thr, nms_thr, 10, {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});
}

} // namespace ball_detect

BallDetect::BallDetect(model_type_t model_type, bool lazy_load) : m_model_type(model_type)
{
    switch (model_type) {
    case model_type_t::ESPDET_PICO_224_224_BALL:
        m_score_thr[0] = ball_detect::ESPDet::default_score_thr;
        m_nms_thr[0] = ball_detect::ESPDet::default_nms_thr;
        break;
    }
    if (lazy_load) {
        m_model = nullptr;
    } else {
        load_model();
    }
}

void BallDetect::load_model()
{
    switch (m_model_type) {
    case model_type_t::ESPDET_PICO_224_224_BALL:
#if CONFIG_FLASH_ESPDET_PICO_224_224_BALL || CONFIG_BALL_DETECT_MODEL_IN_SDCARD
        m_model = new ball_detect::ESPDet("espdet_pico_224_224_ball.espdl", m_score_thr[0], m_nms_thr[0]);
#else
        ESP_LOGE("ball_detect", "espdet_pico_224_224_ball is not selected in menuconfig.");
#endif
        break;
    }
}
