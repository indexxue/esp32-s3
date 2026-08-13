/**
 * @file pet_fs.h
 * @brief Portable stdio file access under a pack root (SD or PC folder).
 */

#ifndef PET_FS_H
#define PET_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PET_FS_PATH_MAX (256)

void pet_fs_set_root(const char *root);
const char *pet_fs_root(void);
bool pet_fs_join(char *out, size_t out_len, const char *rel);
FILE *pet_fs_open_read(const char *rel);
bool pet_fs_read_all(const char *rel, void *buf, size_t cap, size_t *got);
long pet_fs_file_size(const char *rel);

#ifdef __cplusplus
}
#endif

#endif /* PET_FS_H */
