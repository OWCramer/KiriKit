/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

/* The app loads ShioriKrkr.framework with dlopen when a game starts, not at launch: the
 * framework's SDL3 links Apple's OpenGLES, which must not load ahead of MetalANGLE (Ren'Py's
 * SDL2 looks GL functions up with dlsym(RTLD_DEFAULT)). This resolves shiorikrkr.h's
 * functions into a table. */

#ifndef SHIORIKRKR_LOADER_H
#define SHIORIKRKR_LOADER_H

#include "shiorikrkr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shiorikrkr_api {
    int (*start)(const shiorikrkr_config_t *config);
    int (*tick)(void);
    void *(*take_view)(void);
    void (*canvas_size)(int32_t *width, int32_t *height);
    void (*pointer)(int32_t phase, float x, float y, int32_t button);
    void (*wheel)(float dy, float x, float y);
    void (*key)(int32_t virtual_key, int32_t down);
    int (*eval)(const char *expression, char *out, uint32_t capacity);
    int (*capture)(const char *path);
    void (*stop)(void);
    int (*frame)(shiorikrkr_frame_t *frame);
    uint32_t (*cursor)(const uint8_t **rgba, int32_t *width, int32_t *height, int32_t *hot_x, int32_t *hot_y,
                       int32_t *kind);
} shiorikrkr_api_t;

/* Loads the framework binary at path (once per process) and returns its functions, or NULL
 * with the reason in error. */
const shiorikrkr_api_t *shiorikrkr_load(const char *path, char *error, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* SHIORIKRKR_LOADER_H */
