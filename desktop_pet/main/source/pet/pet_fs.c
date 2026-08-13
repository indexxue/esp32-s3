/**
 * @file pet_fs.c
 * @brief fopen under pet_fs_set_root(); works on FatFS and Windows stdio.
 */

#include "pet_fs.h"

#include <string.h>

static char s_root[PET_FS_PATH_MAX] = "/sdcard/pet";

void pet_fs_set_root(const char *root)
{
    size_t n;

    if ((root == NULL) || (root[0] == '\0')) {
        (void)strncpy(s_root, "/sdcard/pet", sizeof(s_root) - 1U);
        s_root[sizeof(s_root) - 1U] = '\0';
        return;
    }
    n = strlen(root);
    if (n >= sizeof(s_root)) {
        n = sizeof(s_root) - 1U;
    }
    (void)memcpy(s_root, root, n);
    s_root[n] = '\0';
    while ((n > 1U) && ((s_root[n - 1U] == '/') || (s_root[n - 1U] == '\\'))) {
        n--;
        s_root[n] = '\0';
    }
}

const char *pet_fs_root(void)
{
    return s_root;
}

bool pet_fs_join(char *out, size_t out_len, const char *rel)
{
    int n;

    if ((out == NULL) || (out_len < 4U) || (rel == NULL) || (rel[0] == '\0')) {
        return false;
    }
    if ((rel[0] == '/') || (rel[0] == '\\') || (strstr(rel, "..") != NULL)) {
        return false;
    }
    n = snprintf(out, out_len, "%s/%s", s_root, rel);
    if ((n < 0) || ((size_t)n >= out_len)) {
        return false;
    }
    return true;
}

FILE *pet_fs_open_read(const char *rel)
{
    char path[PET_FS_PATH_MAX];

    if (!pet_fs_join(path, sizeof(path), rel)) {
        return NULL;
    }
    return fopen(path, "rb");
}

long pet_fs_file_size(const char *rel)
{
    FILE *fp = pet_fs_open_read(rel);
    long sz;

    if (fp == NULL) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        (void)fclose(fp);
        return -1;
    }
    sz = ftell(fp);
    (void)fclose(fp);
    return sz;
}

bool pet_fs_read_all(const char *rel, void *buf, size_t cap, size_t *got)
{
    FILE *fp;
    size_t n;

    if (got != NULL) {
        *got = 0U;
    }
    if ((buf == NULL) || (cap == 0U)) {
        return false;
    }
    fp = pet_fs_open_read(rel);
    if (fp == NULL) {
        return false;
    }
    n = fread(buf, 1U, cap, fp);
    (void)fclose(fp);
    if (got != NULL) {
        *got = n;
    }
    return n > 0U;
}
