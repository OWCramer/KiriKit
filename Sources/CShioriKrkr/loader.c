/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

#include "shiorikrkr_loader.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static shiorikrkr_api_t api;
static int loaded;

static void fail(char *error, uint32_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message ? message : "unknown error");
}

const shiorikrkr_api_t *shiorikrkr_load(const char *path, char *error, uint32_t capacity) {
    if (loaded) return &api;
    void *image = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!image) {
        fail(error, capacity, dlerror());
        return NULL;
    }
#define RESOLVE(field, name)                                   \
    *(void **)(&api.field) = dlsym(image, name);               \
    if (!api.field) {                                          \
        fail(error, capacity, "missing " name);                \
        return NULL;                                           \
    }
    RESOLVE(start, "shiorikrkr_start")
    RESOLVE(tick, "shiorikrkr_tick")
    RESOLVE(take_view, "shiorikrkr_take_view")
    RESOLVE(canvas_size, "shiorikrkr_canvas_size")
    RESOLVE(pointer, "shiorikrkr_pointer")
    RESOLVE(wheel, "shiorikrkr_wheel")
    RESOLVE(key, "shiorikrkr_key")
    RESOLVE(eval, "shiorikrkr_eval")
    RESOLVE(capture, "shiorikrkr_capture")
    RESOLVE(stop, "shiorikrkr_stop")
#undef RESOLVE
    loaded = 1;
    return &api;
}
