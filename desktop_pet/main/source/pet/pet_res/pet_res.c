/**
 * @file pet_res.c
 * @brief Parse PETP pack.bin and RGBH frames. No JSON on device.
 */

#include "pet_res.h"

#include "pet_fs.h"

#include <string.h>

#define PET_PACK_HDR_SIZE (20U)
#define PET_RGBH_HDR_SIZE (12U)

typedef struct {
    uint8_t frame_count;
    uint8_t fps;
    char names[PET_RES_MAX_FRAMES][PET_RES_NAME_LEN];
    pet_res_face_t faces[PET_RES_MAX_FRAMES];
} pet_res_clip_t;

static bool s_loaded;
static uint16_t s_pack_ver;
static pet_needs_cfg_t s_cfg;
static pet_res_clip_t s_clips[PET_CLIP_COUNT];

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t rd_i16(const uint8_t *p)
{
    return (int16_t)rd_u16(p);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void parse_face_block(const uint8_t *blk, pet_res_face_t *out)
{
    out->eye_l.x = rd_i16(&blk[0]);
    out->eye_l.y = rd_i16(&blk[2]);
    out->eye_l.angle_deg = rd_i16(&blk[4]);
    out->eye_r.x = rd_i16(&blk[6]);
    out->eye_r.y = rd_i16(&blk[8]);
    out->eye_r.angle_deg = rd_i16(&blk[10]);
    out->mouth.x = rd_i16(&blk[12]);
    out->mouth.y = rd_i16(&blk[14]);
    out->mouth.angle_deg = rd_i16(&blk[16]);
    out->brow_l.x = rd_i16(&blk[18]);
    out->brow_l.y = rd_i16(&blk[20]);
    out->brow_l.angle_deg = rd_i16(&blk[22]);
    out->brow_r.x = rd_i16(&blk[24]);
    out->brow_r.y = rd_i16(&blk[26]);
    out->brow_r.angle_deg = rd_i16(&blk[28]);
    out->valid = true;
}

void pet_res_unload(void)
{
    (void)memset(s_clips, 0, sizeof(s_clips));
    s_pack_ver = 0U;
    s_loaded = false;
}

bool pet_res_is_loaded(void)
{
    return s_loaded;
}

uint16_t pet_res_pack_version(void)
{
    return s_pack_ver;
}

const pet_needs_cfg_t *pet_res_needs_cfg(void)
{
    return s_loaded ? &s_cfg : pet_core_default_cfg();
}

uint8_t pet_res_clip_frame_count(pet_clip_id_t clip)
{
    if (!s_loaded || (clip >= PET_CLIP_COUNT)) {
        return 0U;
    }
    return s_clips[clip].frame_count;
}

uint8_t pet_res_clip_fps(pet_clip_id_t clip)
{
    if (!s_loaded || (clip >= PET_CLIP_COUNT)) {
        return 0U;
    }
    return s_clips[clip].fps;
}

bool pet_res_get_face(pet_clip_id_t clip, uint8_t frame_index, pet_res_face_t *out)
{
    if ((out == NULL) || !s_loaded || (clip >= PET_CLIP_COUNT) ||
        (frame_index >= s_clips[clip].frame_count)) {
        return false;
    }
    *out = s_clips[clip].faces[frame_index];
    return out->valid;
}

bool pet_res_load(void)
{
    uint8_t hdr[PET_PACK_HDR_SIZE];
    FILE *fp;
    uint16_t ver;
    uint8_t clip_count;
    uint8_t i;
    uint8_t c;

    pet_res_unload();
    fp = pet_fs_open_read("pack.bin");
    if (fp == NULL) {
        return false;
    }
    if (fread(hdr, 1U, PET_PACK_HDR_SIZE, fp) != PET_PACK_HDR_SIZE) {
        (void)fclose(fp);
        return false;
    }
    if (memcmp(hdr, "PETP", 4) != 0) {
        (void)fclose(fp);
        return false;
    }
    ver = rd_u16(&hdr[4]);
    if ((ver != 1U) && (ver != 2U)) {
        (void)fclose(fp);
        return false;
    }
    s_pack_ver = ver;
    s_cfg.hunger_decay_s = rd_u16(&hdr[8]);
    s_cfg.mood_decay_s = rd_u16(&hdr[10]);
    s_cfg.energy_decay_s = rd_u16(&hdr[12]);
    s_cfg.energy_recover_s = rd_u16(&hdr[14]);
    s_cfg.feed_hunger = hdr[16];
    s_cfg.play_mood = hdr[17];
    s_cfg.tap_mood = hdr[18];
    clip_count = hdr[19];
    if (clip_count > PET_RES_MAX_CLIPS) {
        (void)fclose(fp);
        return false;
    }

    for (i = 0U; i < clip_count; i++) {
        uint8_t meta[4];
        uint8_t clip_id;
        uint8_t frames;
        uint8_t fps;
        uint8_t f;

        if (fread(meta, 1U, 4U, fp) != 4U) {
            (void)fclose(fp);
            pet_res_unload();
            return false;
        }
        clip_id = meta[0];
        frames = meta[1];
        fps = meta[2];
        if ((clip_id >= PET_CLIP_COUNT) || (frames > PET_RES_MAX_FRAMES) || (frames == 0U)) {
            (void)fclose(fp);
            pet_res_unload();
            return false;
        }
        s_clips[clip_id].frame_count = frames;
        s_clips[clip_id].fps = (fps == 0U) ? 4U : fps;
        for (f = 0U; f < frames; f++) {
            uint8_t face_blk[PET_RES_FACE_BLOCK_LEN];

            if (fread(s_clips[clip_id].names[f], 1U, PET_RES_NAME_LEN, fp) != PET_RES_NAME_LEN) {
                (void)fclose(fp);
                pet_res_unload();
                return false;
            }
            s_clips[clip_id].names[f][PET_RES_NAME_LEN - 1U] = '\0';
            s_clips[clip_id].faces[f].valid = false;
            if (ver >= 2U) {
                if (fread(face_blk, 1U, PET_RES_FACE_BLOCK_LEN, fp) != PET_RES_FACE_BLOCK_LEN) {
                    (void)fclose(fp);
                    pet_res_unload();
                    return false;
                }
                parse_face_block(face_blk, &s_clips[clip_id].faces[f]);
            }
        }
    }
    (void)fclose(fp);

    /* Need at least idle so the body layer can run. */
    if (s_clips[PET_CLIP_IDLE].frame_count == 0U) {
        pet_res_unload();
        return false;
    }
    for (c = 0U; c < (uint8_t)PET_CLIP_COUNT; c++) {
        if (s_clips[c].frame_count == 0U) {
            s_clips[c] = s_clips[PET_CLIP_IDLE];
        }
    }
    s_loaded = true;
    return true;
}

bool pet_res_load_rgbh(const char *rel, uint16_t *pixels, uint32_t pixel_cap,
                       uint16_t max_w, uint16_t max_h, uint16_t *w, uint16_t *h)
{
    uint8_t hdr[PET_RGBH_HDR_SIZE];
    FILE *fp;
    uint16_t fw;
    uint16_t fh;
    uint32_t flags;
    uint32_t need;
    size_t got;

    if ((rel == NULL) || (pixels == NULL) || (w == NULL) || (h == NULL)) {
        return false;
    }
    fp = pet_fs_open_read(rel);
    if (fp == NULL) {
        return false;
    }
    if (fread(hdr, 1U, PET_RGBH_HDR_SIZE, fp) != PET_RGBH_HDR_SIZE) {
        (void)fclose(fp);
        return false;
    }
    if (memcmp(hdr, "RGBH", 4) != 0) {
        (void)fclose(fp);
        return false;
    }
    fw = rd_u16(&hdr[4]);
    fh = rd_u16(&hdr[6]);
    flags = rd_u32(&hdr[8]);
    if ((fw == 0U) || (fh == 0U) || (fw > max_w) || (fh > max_h) || (flags != 0U)) {
        (void)fclose(fp);
        return false;
    }
    need = (uint32_t)fw * (uint32_t)fh;
    if (need > pixel_cap) {
        (void)fclose(fp);
        return false;
    }
    got = fread(pixels, 2U, (size_t)need, fp);
    (void)fclose(fp);
    if (got != (size_t)need) {
        return false;
    }
    *w = fw;
    *h = fh;
    return true;
}

bool pet_res_load_frame(pet_clip_id_t clip, uint8_t frame_index,
                        uint16_t *pixels, uint32_t pixel_cap,
                        uint16_t *w, uint16_t *h)
{
    if (!s_loaded || (clip >= PET_CLIP_COUNT) || (frame_index >= s_clips[clip].frame_count)) {
        return false;
    }
    return pet_res_load_rgbh(s_clips[clip].names[frame_index], pixels, pixel_cap,
                             PET_RES_FRAME_MAX_W, PET_RES_FRAME_MAX_H, w, h);
}

bool pet_res_load_splash(uint16_t *pixels, uint32_t pixel_cap,
                         uint16_t *w, uint16_t *h)
{
    return pet_res_load_rgbh(PET_RES_SPLASH_REL, pixels, pixel_cap,
                             PET_RES_SPLASH_MAX_W, PET_RES_SPLASH_MAX_H, w, h);
}

bool pet_res_load_ui_icon(pet_ui_icon_id_t id, uint16_t *pixels, uint32_t pixel_cap,
                          uint16_t *w, uint16_t *h)
{
    static const char *const rels[PET_UI_ICON_COUNT] = {
        PET_RES_UI_ICON_FEED_REL,
        PET_RES_UI_ICON_PLAY_REL,
        PET_RES_UI_ICON_SLEEP_REL,
        PET_RES_UI_ICON_CHAT_REL,
    };

    if (id >= PET_UI_ICON_COUNT) {
        return false;
    }
    return pet_res_load_rgbh(rels[id], pixels, pixel_cap,
                             PET_RES_UI_ICON_MAX_W, PET_RES_UI_ICON_MAX_H, w, h);
}
