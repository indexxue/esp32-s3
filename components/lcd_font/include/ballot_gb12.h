/**
 * @file ballot_gb12.h
 * @brief ballot_guard GB2312 12x12 字库
 */
#ifndef BALLOT_GB12_H
#define BALLOT_GB12_H

#include <stdint.h>

typedef struct {
    uint8_t Index[2];
    uint8_t Msk[24];
} ballot_gb12_glyph_t;

#define BALLOT_GB12_COUNT (119U)

extern const ballot_gb12_glyph_t ballot_gb12[BALLOT_GB12_COUNT];

#endif
