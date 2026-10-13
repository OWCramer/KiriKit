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

#define SHIORIKRKR_API_VERSION 3

/* One line of the engine's log (UTF-8, no newline). */
typedef void (*shiorikrkr_log_fn)(void *context, const char *line);

/* A message box the game shows (System.inform, a question): the host shows it its own way
 * and returns the index of the button chosen, or -1. Called on the engine's thread, inside a
 * tick; the host waits for the answer (a nested run loop) before returning. NULL: answered
 * with the first button. */
typedef int32_t (*shiorikrkr_message_fn)(void *context, const char *text, const char *caption,
                                         const char *const *buttons, int32_t button_count);

/* E-mote characters (NEKOPARA's): the engine keeps the game's script objects (D3DEmotePlayer
 * and the Motion classes) and hands the models, every command and every draw to the host's
 * own E-mote player, through this table. All calls on the engine's thread. */
typedef struct shiorikrkr_emote_host {
    uint32_t struct_size;            /* sizeof(shiorikrkr_emote_host_t) */
    void *context;
    /* A player for one character: its PSB files (read and decrypted from the archives in
     * memory, never written out), in the order the game names them. NULL when none can be
     * read; error then holds why (UTF-8, cut to capacity). */
    void *(*create)(void *context, const uint8_t *const *data, const uint64_t *sizes,
                    const char *const *names, int32_t count, char *error, uint32_t capacity);
    /* A second player with the same models and state (the game copies a layer for a
     * transition). */
    void *(*clone)(void *context, void *player);
    void (*destroy)(void *context, void *player);
    /* A command by name with a label (a variable or timeline, may be "") and numbers:
     * progress(frames), skip, pass, show, coord(x, y, frames, easing), rot(radians, frames,
     * easing), scale(s, frames, easing), color(rgba, frames, easing), variable(value, frames,
     * easing), play(flags), stop, fadeout(frames, easing), blend(ratio, frames, easing,
     * stop when 0), wind(start, goal, speed, min, max), windstop, controls(mesh division,
     * bust, hair, parts scale). */
    void (*command)(void *context, void *player, const char *verb, const char *label,
                    const double *numbers, int32_t count);
    /* A question: variable, playing, blend, animating, loop, count with label main / diff /
     * playing, flags(index), contains(x, y) with a layer label. Labels asked for by index
     * (label with label main / diff / playing and the index) come back in out. */
    double (*query)(void *context, void *player, const char *verb, const char *label,
                    const double *numbers, int32_t count, char *out, uint32_t capacity);
    /* Draws the character into rgba (R, G, B, A bytes, straight alpha, cleared by the host)
     * of width x height, through transform (a, b, c, d, tx, ty: x' = a x + c y + tx,
     * y' = b x + d y + ty, from the character's own space centred on the canvas). Returns 1
     * when it drew anything. */
    int32_t (*draw)(void *context, void *player, uint8_t *rgba, int32_t width, int32_t height,
                    int32_t pitch, const double *transform);
} shiorikrkr_emote_host_t;

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
    /* Since API version 2. */
    shiorikrkr_message_fn message;
    void *message_context;
    /* Non-zero: frames stay with the host (shiorikrkr_frame) and SDL draws nothing; zero:
     * SDL's own renderer draws into the engine view (the spike's path). */
    int32_t host_presents;
    /* Since API version 3. NULL: E-mote characters aren't drawn. */
    const shiorikrkr_emote_host_t *emote;
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

/* The game's composed frame, for the host to present: RGBA8, top row first. The pixels stay
 * valid until the next tick. serial changes when they do; blank is set while every sampled
 * pixel is black (the game's first frames, before its title draws). Returns 1 when there is a
 * frame. Only with host_presents. */
typedef struct shiorikrkr_frame {
    const uint8_t *pixels;
    int32_t width, height, pitch;
    uint64_t serial;
    int32_t blank;
} shiorikrkr_frame_t;
int shiorikrkr_frame(shiorikrkr_frame_t *frame);

/* The game's cursor, for the host to draw at the pointer: kind 0 the system pointer, 1 the
 * game's image (RGBA, straight alpha, top row first, valid until the cursor changes; hot is
 * its hotspot in the image's pixels), 2 hidden. Returns a serial that changes with it. */
uint32_t shiorikrkr_cursor(const uint8_t **rgba, int32_t *width, int32_t *height, int32_t *hot_x,
                           int32_t *hot_y, int32_t *kind);

/* The game's canvas size (the main window's inner size), 0x0 before the first window. */
void shiorikrkr_canvas_size(int32_t *width, int32_t *height);

/* Pointer input in the engine view's normalized coordinates (0...1 on each axis, the
 * engine letterboxes the canvas inside it). phase: 0 down, 1 moved, 2 up, 3 cancel (the press
 * ends without a click). Presses reach the game a tick after the pointer moves there and are
 * held at least a tick, as KAG's buttons need. button: 0 left,
 * 1 right, 2 middle. A tap is down + up at one point: a left click. */
enum { SHIORIKRKR_POINTER_DOWN = 0, SHIORIKRKR_POINTER_MOVED = 1, SHIORIKRKR_POINTER_UP = 2,
       SHIORIKRKR_POINTER_CANCEL = 3 /* a gesture took the touch: no click */ };
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
