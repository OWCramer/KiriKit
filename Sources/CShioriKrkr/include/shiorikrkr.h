/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at
 * https://mozilla.org/MPL/2.0/. */

/* ShioriKrkr: Shiori's own KiriKiri engine (krkrsdl3-based, clean-room; see
 * Packages/ShioriKrkrKit/PROVENANCE.md). This is the whole interface the framework exports;
 * every krkrsdl3, TJS2, SDL3 and dependency symbol stays inside ShioriKrkr.framework, so it
 * can sit in the same process as AetherKrkr (ShioriKiriKiri.framework) and Ren'Py's SDL2.
 *
 * Threading: every call is made on the thread that called shiorikrkr_start (the main
 * thread in the app). One game per process lifetime for now: krkrsdl3's globals aren't
 * reset, so after shiorikrkr_stop the app returns to the library but a second game in the
 * same process isn't supported yet. */

#ifndef SHIORIKRKR_H
#define SHIORIKRKR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHIORIKRKR_API_VERSION 1

/* One line of the engine's log (UTF-8, no newline). */
typedef void (*shiorikrkr_log_fn)(void *context, const char *line);

typedef struct shiorikrkr_config {
    uint32_t struct_size;            /* sizeof(shiorikrkr_config_t) */
    /* The folder the engine opens: the game's base/ with patch/ laid over it. KiriKiri
     * looks for startup.tjs and data.xp3 there, and loose files there come before files in
     * the archives. savedata/ in it is where the game's saves go (the host links it to the
     * game's saves/ folder). */
    const char *game_root;
    /* The PC executable's name in game_root (e.g. "KARAKARA2.exe"), so System.exePath and
     * exeName read as the game expects. Never opened. NULL: "game.exe". */
    const char *exe_name;
    /* A CxDec scheme in Shiori's SHCX layout (GARbroDatabase.swift), or NULL. Archive reads
     * are then decrypted in memory, never written out. */
    const char *cx_scheme_path;
    /* Extra krkrsdl3 options, e.g. "-render=software", "-window=1280x720". */
    const char *const *arguments;
    uint32_t argument_count;
    shiorikrkr_log_fn log;
    void *log_context;
} shiorikrkr_config_t;

enum {
    SHIORIKRKR_OK = 0,
    SHIORIKRKR_ERROR_ARGUMENTS = 1,   /* config missing or game_root not found */
    SHIORIKRKR_ERROR_SCHEME = 2,      /* cx_scheme_path given but unreadable */
    SHIORIKRKR_ERROR_START = 3,       /* SDL, the window or the game's startup failed */
    SHIORIKRKR_ERROR_STATE = 4,       /* already started, or not started */
};

/* Starts the game: SDL, the engine's window and renderer, then the game's startup scripts
 * (they run inside this call up to their first wait). */
int shiorikrkr_start(const shiorikrkr_config_t *config);

/* One frame: delivers pending events, runs the engine's timers and scripts, composes and
 * presents. Returns 1 while the game runs, 0 once it has quit. */
int shiorikrkr_tick(void);

/* The platform view the engine draws into (UIView * on iOS, NSView * on macOS), for the
 * host to place in its own view hierarchy. NULL before start. The engine's own window is
 * hidden once the host has taken the view. */
void *shiorikrkr_take_view(void);

/* The game's canvas size (the main window's inner size), 0x0 before the first window. */
void shiorikrkr_canvas_size(int32_t *width, int32_t *height);

/* Pointer input in the engine view's normalized coordinates (0...1 on each axis, the
 * engine letterboxes the canvas inside it). phase: 0 down, 1 moved, 2 up. button: 0 left,
 * 1 right, 2 middle. A tap is down + up at one point: a left click. */
enum { SHIORIKRKR_POINTER_DOWN = 0, SHIORIKRKR_POINTER_MOVED = 1, SHIORIKRKR_POINTER_UP = 2 };
enum { SHIORIKRKR_BUTTON_LEFT = 0, SHIORIKRKR_BUTTON_RIGHT = 1, SHIORIKRKR_BUTTON_MIDDLE = 2 };
void shiorikrkr_pointer(int32_t phase, float x, float y, int32_t button);

/* The mouse wheel (positive dy scrolls up, KiriKiri's backlog), at a normalized point. */
void shiorikrkr_wheel(float dy, float x, float y);

/* A key by Windows virtual-key code (KiriKiri's VK_*, e.g. 0x0D Return, 0x11 Control). */
void shiorikrkr_key(int32_t virtual_key, int32_t down);

/* Evaluates TJS (UTF-8) in the global context between ticks. Copies the result as text,
 * NUL-terminated and cut to capacity, into out. Returns 0 when it ran, 1 when TJS threw
 * (out holds the message), 2 when no game is running. */
int shiorikrkr_eval(const char *expression, char *out, uint32_t capacity);

/* Saves the composed frame (the main window's primary layer and everything piled on it) as
 * a BMP at path. Returns 0 on success. For tests and the Mac harness. */
int shiorikrkr_capture(const char *path);

/* Ends the game: the engine's exit handlers, then SDL. */
void shiorikrkr_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* SHIORIKRKR_H */
