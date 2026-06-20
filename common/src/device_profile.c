#include "device_profile.h"

static const device_button_spec_t s_buttons_default[] = {
    {0, BTN_ID_UP, "上", 0, (uint16_t)(BTN_PERMISSION_UP | BTN_PERMISSION_RESET)},
    {3, BTN_ID_DOWN, "下", 0, (uint16_t)(BTN_PERMISSION_DOWN | BTN_PERMISSION_RESET | BTN_PERMISSION_PAIR)},
};

static const device_button_spec_t s_buttons_main[] = {
    {0, BTN_ID_UP, "上", 0, (uint16_t)(BTN_PERMISSION_UP | BTN_PERMISSION_RESET)},
    {3, BTN_ID_DOWN, "下", 0, (uint16_t)(BTN_PERMISSION_DOWN | BTN_PERMISSION_RESET | BTN_PERMISSION_PAIR)},
};

static const device_button_spec_t s_buttons_ballot_guard[] = {
    {0, BTN_ID_LEFT, "左", 0, (uint16_t)(BTN_PERMISSION_LEFT | BTN_PERMISSION_RESET)},
    {3, BTN_ID_RIGHT, "右", 0, (uint16_t)(BTN_PERMISSION_RIGHT | BTN_PERMISSION_RESET)},
};

static const device_product_profile_t s_product_profiles[] = {
    {
        .product_id         = NVS_DEVICE_ID_DEFAULT,
        .name               = "factory",
        .board_mask         = DEVICE_BOARD_MASK_FULL,
        .platform_mask      = DEVICE_PLATFORM_MASK_FULL,
        .buttons            = s_buttons_default,
        .button_count       = (uint8_t)(sizeof(s_buttons_default) / sizeof(s_buttons_default[0])),
        .lcd_smoke_title    = "ST7789 + lcd OK",
        .lcd_smoke_subtitle = "ESP32-S3 smoke test",
    },
    {
        .product_id         = NVS_PROJECT_ID_MAIN,
        .name               = "main",
        .board_mask         = DEVICE_BOARD_MASK_FULL,
        .platform_mask      = DEVICE_PLATFORM_MASK_FULL,
        .buttons            = s_buttons_main,
        .button_count       = (uint8_t)(sizeof(s_buttons_main) / sizeof(s_buttons_main[0])),
        .lcd_smoke_title    = "main LCD OK",
        .lcd_smoke_subtitle = "ESP32-S3 project",
    },
    {
        .product_id         = NVS_PROJECT_ID_BALLOT_GUARD,
        .name               = "ballot_guard",
        .board_mask         = DEVICE_BOARD_MASK_LCD,
        .platform_mask      = DEVICE_PLATFORM_MASK_BUTTON | DEVICE_PLATFORM_MASK_LED,
        .buttons            = s_buttons_ballot_guard,
        .button_count       = (uint8_t)(sizeof(s_buttons_ballot_guard) / sizeof(s_buttons_ballot_guard[0])),
        .lcd_smoke_title    = "ballot_guard LCD OK",
        .lcd_smoke_subtitle = "ballot_guard ready",
    },
};

typedef struct {
    uint32_t hardware_id;
    const char *name;
} device_hardware_entry_t;

static const device_hardware_entry_t s_hardware_table[] = {
    {NVS_HARDWARE_ID_TY_S3_REV_A, "ty_s3_rev_a"},
};

static const device_product_profile_t *product_profile_lookup(uint32_t product_id)
{
    size_t i;

    for (i = 0; i < (sizeof(s_product_profiles) / sizeof(s_product_profiles[0])); i++) {
        if (s_product_profiles[i].product_id == product_id) {
            return &s_product_profiles[i];
        }
    }
    for (i = 0; i < (sizeof(s_product_profiles) / sizeof(s_product_profiles[0])); i++) {
        if (s_product_profiles[i].product_id == (uint32_t)NVS_DEFAULT_DEVICE_ID) {
            return &s_product_profiles[i];
        }
    }
    return &s_product_profiles[0];
}

const device_product_profile_t *device_profile_product(void)
{
    return product_profile_lookup(nvs_product_id_active());
}

uint32_t device_profile_hardware_id(void)
{
    return nvs_hardware_id_get();
}

const char *device_profile_hardware_name(uint32_t hardware_id)
{
    size_t i;

    for (i = 0; i < (sizeof(s_hardware_table) / sizeof(s_hardware_table[0])); i++) {
        if (s_hardware_table[i].hardware_id == hardware_id) {
            return s_hardware_table[i].name;
        }
    }
    return NULL;
}

uint32_t device_profile_board_mask(void)
{
    return device_profile_product()->board_mask;
}

uint32_t device_profile_platform_mask(void)
{
    return device_profile_product()->platform_mask;
}

bool device_profile_board_wants(uint32_t mask)
{
    return (device_profile_board_mask() & mask) == mask;
}

bool device_profile_platform_wants(uint32_t mask)
{
    return (device_profile_platform_mask() & mask) == mask;
}

const char *device_profile_lcd_smoke_title(void)
{
    const device_product_profile_t *p = device_profile_product();

    return (p->lcd_smoke_title != NULL) ? p->lcd_smoke_title : "ST7789 + lcd OK";
}

const char *device_profile_lcd_smoke_subtitle(void)
{
    const device_product_profile_t *p = device_profile_product();

    return (p->lcd_smoke_subtitle != NULL) ? p->lcd_smoke_subtitle : "ESP32-S3";
}

uint8_t device_profile_button_count(void)
{
    return device_profile_product()->button_count;
}

const device_button_spec_t *device_profile_button_spec(uint8_t index)
{
    const device_product_profile_t *p = device_profile_product();

    if (index >= p->button_count) {
        return NULL;
    }
    return &p->buttons[index];
}
