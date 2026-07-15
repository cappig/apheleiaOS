#include "image.h"

#include <limits.h>
#include <parse/ppm.h>
#include <parse/qoi.h>
#include <stdlib.h>
#include <string.h>

#include "file.h"

static bool _image_alloc(u32 width, u32 height, size_t max_pixels, pixel_t **out) {
    size_t count = (size_t)width * (size_t)height;
    if (!width || !height || count / height != width || count > max_pixels || count > SIZE_MAX / sizeof(pixel_t)) {
        return false;
    }

    *out = malloc(count * sizeof(pixel_t));
    return *out != NULL;
}

static bool _load_qoi(const u8 *data, size_t size, size_t max_pixels, wm_image_t *out) {
    qoi_info_t info = { 0 };
    if (!qoi_read_info(data, size, &info) || !_image_alloc(info.width, info.height, max_pixels, &out->pixels)) {
        return false;
    }

    size_t count = (size_t)info.width * (size_t)info.height;
    if (!qoi_decode(data, size, out->pixels, count)) {
        wm_image_release(out);
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        out->pixels[i] &= 0x00ffffffU;
    }

    out->width = info.width;
    out->height = info.height;
    return true;
}

static bool _load_ppm(const u8 *data, size_t size, size_t max_pixels, wm_image_t *out) {
    ppm_p6_blob_t ppm = { 0 };
    if (!ppm_parse_p6_blob(data, size, &ppm) || !_image_alloc(ppm.width, ppm.height, max_pixels, &out->pixels)) {
        return false;
    }

    size_t count = (size_t)ppm.width * (size_t)ppm.height;
    for (size_t i = 0; i < count; i++) {
        u8 r = ppm.raster[i * 3];
        u8 g = ppm.raster[i * 3 + 1];
        u8 b = ppm.raster[i * 3 + 2];
        out->pixels[i] = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
    }

    out->width = ppm.width;
    out->height = ppm.height;
    return true;
}

bool wm_image_load(const char *path, size_t max_bytes, size_t max_pixels, wm_image_t *out) {
    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (!path || !path[0]) {
        return false;
    }

    u8 *data = NULL;
    size_t size = 0;
    if (!wm_file_read_all(path, max_bytes, &data, &size)) {
        return false;
    }

    bool loaded = _load_qoi(data, size, max_pixels, out) || _load_ppm(data, size, max_pixels, out);
    free(data);
    return loaded;
}

void wm_image_release(wm_image_t *image) {
    if (!image) {
        return;
    }

    if (image->pixels) {
        free(image->pixels);
    }

    memset(image, 0, sizeof(*image));
}
