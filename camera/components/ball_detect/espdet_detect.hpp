#pragma once

#include "dl_detect_base.hpp"
#include "dl_detect_espdet_postprocessor.hpp"

namespace ball_detect {
class ESPDet : public dl::detect::DetectImpl {
public:
    static inline constexpr float default_score_thr = 0.25f;
    static inline constexpr float default_nms_thr = 0.7f;
    ESPDet(const char *model_name, float score_thr, float nms_thr);
};
} // namespace ball_detect

class BallDetect : public dl::detect::DetectWrapper {
public:
    typedef enum {
        ESPDET_PICO_224_224_BALL = 0,
    } model_type_t;

    BallDetect(model_type_t model_type = static_cast<model_type_t>(CONFIG_DEFAULT_BALL_DETECT_MODEL),
               bool lazy_load = true);

private:
    void load_model() override;
    model_type_t m_model_type;
};
