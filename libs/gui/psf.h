#pragma once

#include <parse/psf.h>
#include <stdbool.h>
#include <stddef.h>

typedef psf_blob_t psf_font_t;

bool psf_parse(const void *data, size_t len, psf_font_t *out);
bool psf_load_file(const char *path, void *storage, size_t storage_len, psf_font_t *out);
