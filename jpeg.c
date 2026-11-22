/* Copyright (C) 2026 Sandro Miisamm.  GPL-3.0-or-later, see LICENSE. */
/* Cover decoding for RetroSqueeze: LMS delivers every cover and radio logo as a
   baseline JPEG in the size asked for, so stb_image is cut down to JPEG only. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

/* JPEG bytes -> malloc'd 24-bit BGR rows, bottom-up (ready for a DIB); NULL on error */
unsigned char *jpeg_to_dib(const unsigned char *jpg, int len, int *w, int *h)
{
    int x, y, n, stride;
    unsigned char *rgb = stbi_load_from_memory(jpg, len, w, h, &n, 3), *dib;
    if (!rgb) return 0;
    stride = (*w * 3 + 3) & ~3;
    dib = (unsigned char *)malloc(stride * *h);
    if (dib)
        for (y = 0; y < *h; y++) {
            unsigned char *s = rgb + (size_t)y * *w * 3, *d = dib + (size_t)(*h - 1 - y) * stride;
            for (x = 0; x < *w; x++, s += 3, d += 3) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
        }
    stbi_image_free(rgb);
    return dib;
}
