// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// SDL entry points the client resolves through the loader.
//  - HYPRISM_UI_SCALE > 1 reports window and mouse coordinates in points (pixels /
//    scale). The client's game interface scale is viewport height / 1080, so this
//    only matters for the Asset Editor; the default is 1 (off).
//  - SDL_GL_GetProcAddress results pass through gl_entry (gl.c).

#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "macho.h"

static void *g_sdl;
static float g_scale = 1.0f;

#define REAL(name) static __typeof__(&name) real_##name; if (!real_##name) real_##name = (__typeof__(&name))dlsym(g_sdl, #name)

static bool w_GetWindowSize(SDL_Window *window, int *w, int *h)
{
    REAL(SDL_GetWindowSize);
    bool ok = real_SDL_GetWindowSize(window, w, h);
    if (ok && w)
        *w = (int)(*w / g_scale);
    if (ok && h)
        *h = (int)(*h / g_scale);
    return ok;
}

static bool w_SetWindowSize(SDL_Window *window, int w, int h)
{
    REAL(SDL_SetWindowSize);
    return real_SDL_SetWindowSize(window, (int)(w * g_scale), (int)(h * g_scale));
}

static bool w_SetWindowMinimumSize(SDL_Window *window, int w, int h)
{
    REAL(SDL_SetWindowMinimumSize);
    return real_SDL_SetWindowMinimumSize(window, (int)(w * g_scale), (int)(h * g_scale));
}

static bool w_GetWindowPosition(SDL_Window *window, int *x, int *y)
{
    REAL(SDL_GetWindowPosition);
    bool ok = real_SDL_GetWindowPosition(window, x, y);
    if (ok && x)
        *x = (int)(*x / g_scale);
    if (ok && y)
        *y = (int)(*y / g_scale);
    return ok;
}

static SDL_MouseButtonFlags w_GetMouseState(float *x, float *y)
{
    REAL(SDL_GetMouseState);
    SDL_MouseButtonFlags flags = real_SDL_GetMouseState(x, y);
    if (x)
        *x /= g_scale;
    if (y)
        *y /= g_scale;
    return flags;
}

static SDL_MouseButtonFlags w_GetGlobalMouseState(float *x, float *y)
{
    REAL(SDL_GetGlobalMouseState);
    SDL_MouseButtonFlags flags = real_SDL_GetGlobalMouseState(x, y);
    if (x)
        *x /= g_scale;
    if (y)
        *y /= g_scale;
    return flags;
}

static void w_WarpMouseInWindow(SDL_Window *window, float x, float y)
{
    REAL(SDL_WarpMouseInWindow);
    real_SDL_WarpMouseInWindow(window, x * g_scale, y * g_scale);
}

static bool w_WarpMouseGlobal(float x, float y)
{
    REAL(SDL_WarpMouseGlobal);
    return real_SDL_WarpMouseGlobal(x * g_scale, y * g_scale);
}

static bool w_GetDisplayBounds(SDL_DisplayID display, SDL_Rect *rect)
{
    REAL(SDL_GetDisplayBounds);
    bool ok = real_SDL_GetDisplayBounds(display, rect);
    if (ok && rect) {
        rect->x = (int)(rect->x / g_scale);
        rect->y = (int)(rect->y / g_scale);
        rect->w = (int)(rect->w / g_scale);
        rect->h = (int)(rect->h / g_scale);
    }
    return ok;
}

// Display modes are reported in points with a pixel density, as on macOS.
static const SDL_DisplayMode *w_GetCurrentDisplayMode(SDL_DisplayID display)
{
    REAL(SDL_GetCurrentDisplayMode);
    const SDL_DisplayMode *mode = real_SDL_GetCurrentDisplayMode(display);
    if (!mode)
        return NULL;
    static __thread SDL_DisplayMode scaled;
    scaled = *mode;
    scaled.w = (int)(mode->w / g_scale);
    scaled.h = (int)(mode->h / g_scale);
    scaled.pixel_density = mode->pixel_density * g_scale;
    return &scaled;
}

static void scale_event(SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        e->motion.x /= g_scale;
        e->motion.y /= g_scale;
        e->motion.xrel /= g_scale;
        e->motion.yrel /= g_scale;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        e->button.x /= g_scale;
        e->button.y /= g_scale;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        e->wheel.mouse_x /= g_scale;
        e->wheel.mouse_y /= g_scale;
        break;
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
        e->window.data1 = (Sint32)(e->window.data1 / g_scale);
        e->window.data2 = (Sint32)(e->window.data2 / g_scale);
        break;
    default:
        break;
    }
}

// SDL turns every touch into mouse input, and a list cannot be scrolled with that. While two fingers
// are down, their motion becomes mouse wheel events and the synthetic mouse events are dropped.
// Fingers only reach SDL in menus: GamepadOverlay consumes the touches during play.
static SDL_FingerID g_fingers[8];
static int g_finger_count;
// The first finger's touch already pressed the left mouse button; when a second finger lands (a scroll
// begins) that press is released, so the game does not start dragging an item or a slider.
static bool g_release_press;

static void track_fingers(const SDL_Event *e)
{
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        g_finger_count = 0; // the lifts of fingers that were down are not delivered
    } else if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (g_finger_count < 8)
            g_fingers[g_finger_count++] = e->tfinger.fingerID;
        if (g_finger_count == 2)
            g_release_press = true;
    } else if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) {
        for (int i = 0; i < g_finger_count; i++) {
            if (g_fingers[i] == e->tfinger.fingerID) {
                g_fingers[i] = g_fingers[--g_finger_count];
                break;
            }
        }
    }
}

static bool w_PollEvent(SDL_Event *event)
{
    REAL(SDL_PollEvent);
    REAL(SDL_GetMouseState);
    REAL(SDL_GetTicksNS);
    if (g_release_press && event) {
        REAL(SDL_GetKeyboardFocus);
        REAL(SDL_GetWindowID);
        g_release_press = false;
        SDL_Window *focus = real_SDL_GetKeyboardFocus();
        float mx = 0, my = 0;
        real_SDL_GetMouseState(&mx, &my);
        memset(event, 0, sizeof(*event));
        event->type = SDL_EVENT_MOUSE_BUTTON_UP;
        event->button.timestamp = real_SDL_GetTicksNS();
        event->button.windowID = focus ? real_SDL_GetWindowID(focus) : 0;
        event->button.which = SDL_TOUCH_MOUSEID;
        event->button.button = SDL_BUTTON_LEFT;
        event->button.down = false;
        event->button.clicks = 1;
        event->button.x = mx;
        event->button.y = my;
        scale_event(event);
        return true;
    }
    while (real_SDL_PollEvent(event)) {
        if (!event)
            return true;
        track_fingers(event);
        if (g_finger_count >= 2) {
            bool mouse = event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN
                || event->type == SDL_EVENT_MOUSE_BUTTON_UP;
            if (mouse && event->motion.which == SDL_TOUCH_MOUSEID)
                continue;
            if (event->type == SDL_EVENT_FINGER_MOTION) {
                float mx = 0, my = 0, dy = event->tfinger.dy;
                SDL_WindowID window = event->tfinger.windowID;
                real_SDL_GetMouseState(&mx, &my);
                memset(event, 0, sizeof(*event));
                event->type = SDL_EVENT_MOUSE_WHEEL;
                event->wheel.timestamp = real_SDL_GetTicksNS();
                event->wheel.windowID = window;
                event->wheel.which = SDL_TOUCH_MOUSEID;
                event->wheel.y = dy * 6.0f; // two fingers move, so a swipe over half the screen is 6 notches
                event->wheel.direction = SDL_MOUSEWHEEL_NORMAL;
                event->wheel.mouse_x = mx;
                event->wheel.mouse_y = my;
            }
        }
        scale_event(event);
        return true;
    }
    return false;
}

static const struct { const char *name; void *fn; } k_wrappers[] = {
    { "SDL_GetWindowSize", (void *)w_GetWindowSize },
    { "SDL_SetWindowSize", (void *)w_SetWindowSize },
    { "SDL_SetWindowMinimumSize", (void *)w_SetWindowMinimumSize },
    { "SDL_GetWindowPosition", (void *)w_GetWindowPosition },
    { "SDL_GetMouseState", (void *)w_GetMouseState },
    { "SDL_GetGlobalMouseState", (void *)w_GetGlobalMouseState },
    { "SDL_WarpMouseInWindow", (void *)w_WarpMouseInWindow },
    { "SDL_WarpMouseGlobal", (void *)w_WarpMouseGlobal },
    { "SDL_GetDisplayBounds", (void *)w_GetDisplayBounds },
    { "SDL_GetCurrentDisplayMode", (void *)w_GetCurrentDisplayMode },
    { "SDL_PollEvent", (void *)w_PollEvent },
};

void *gl_entry(const char *name, void *fn, void *(*resolve)(const char *));

static void *gl_resolve(const char *proc)
{
    REAL(SDL_GL_GetProcAddress);
    return (void *)real_SDL_GL_GetProcAddress(proc);
}

// GL functions go through gl_entry (see gl.c) for Darwin stack argument layouts.
static SDL_FunctionPointer w_GL_GetProcAddress(const char *proc)
{
    return (SDL_FunctionPointer)gl_entry(proc, gl_resolve(proc), gl_resolve);
}

/// Returns a wrapper for an SDL entry point, or NULL to use SDL directly.
// HYPRISM_PERF=1 logs the frame rate every two seconds: frames per second, the average and worst
// interval between buffer swaps, and how long SDL_GL_SwapWindow blocked (waiting for the GPU or vsync).
void gl_tune_frame(void);

static bool w_GL_SwapWindow(SDL_Window *window)
{
    REAL(SDL_GL_SwapWindow);
    gl_tune_frame();
    static int perf = -1;
    if (perf < 0)
        perf = getenv("HYPRISM_PERF") != NULL;
    if (!perf)
        return real_SDL_GL_SwapWindow(window);
    static struct timespec last, window_start;
    static unsigned frames;
    static double sum_interval, max_interval, sum_swap;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    bool ok = real_SDL_GL_SwapWindow(window);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double swap_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (last.tv_sec) {
        double dt = (t1.tv_sec - last.tv_sec) * 1e3 + (t1.tv_nsec - last.tv_nsec) / 1e6;
        sum_interval += dt;
        if (dt > max_interval)
            max_interval = dt;
        sum_swap += swap_ms;
        frames++;
    }
    last = t1;
    if (!window_start.tv_sec)
        window_start = t1;
    double elapsed = (t1.tv_sec - window_start.tv_sec) + (t1.tv_nsec - window_start.tv_nsec) / 1e9;
    if (elapsed >= 2.0 && frames) {
        macho_log("perf", "fps %.1f  frame avg %.1f ms max %.1f ms  swap wait avg %.1f ms", frames / elapsed,
                  sum_interval / frames, max_interval, sum_swap / frames);
        frames = 0;
        sum_interval = max_interval = sum_swap = 0;
        window_start = t1;
    }
    return ok;
}

// The client runs with VSync off and an uncapped frame rate, so on a 60 Hz phone display it renders
// 70 to 100 frames a second that are never shown. That only heats the SoC, which then throttles the
// GPU (a measured 220 MHz out of 719) and makes the frame pacing worse. Present at the display's
// rate instead. HYPRISM_VSYNC=0 restores the client's own setting.
static bool w_GL_SetSwapInterval(int interval)
{
    REAL(SDL_GL_SetSwapInterval);
    const char *env = getenv("HYPRISM_VSYNC");
    if (!(env && env[0] == '0') && interval == 0) {
        static int logged;
        if (!logged++)
            macho_log("sdl", "forcing VSync on (the client asked for an uncapped frame rate)");
        interval = 1;
    }
    // HYPRISM_SWAP_INTERVAL=2 presents every other refresh (30 FPS on a 60 Hz panel): the battery
    // mode on a display without a mode near 45 Hz.
    const char *every = getenv("HYPRISM_SWAP_INTERVAL");
    if (every && atoi(every) >= 1 && interval != 0) {
        static int logged;
        if (!logged++)
            macho_log("sdl", "presenting every %d display refreshes (HYPRISM_SWAP_INTERVAL)", atoi(every));
        interval = atoi(every);
    }
    return real_SDL_GL_SetSwapInterval(interval);
}

// HYPRISM_CONTENT_SCALE=<x> makes SDL_GetDisplayContentScale answer x. The client imports it and keeps
// a UiScale of its own; this is a way to see whether the interface follows the display's content scale
// (an experiment for a larger HUD, off by default).
static float w_GetDisplayContentScale(SDL_DisplayID display)
{
    REAL(SDL_GetDisplayContentScale);
    const char *env = getenv("HYPRISM_CONTENT_SCALE");
    float real = real_SDL_GetDisplayContentScale(display);
    if (env && atof(env) > 0) {
        static int logged;
        if (!logged++)
            macho_log("sdl", "display content scale %.2f reported as %.2f (HYPRISM_CONTENT_SCALE)", real, atof(env));
        return (float)atof(env);
    }
    return real;
}

void *sdl_scale_lookup(void *sdl_handle, const char *symbol)
{
    if (!g_sdl) {
        g_sdl = sdl_handle;
        const char *env = getenv("HYPRISM_UI_SCALE");
        if (env && atof(env) > 0)
            g_scale = (float)atof(env);
        if (g_scale != 1.0f)
            macho_log("sdl", "presenting the window at %.2fx pixel density", g_scale);
    }
    if (strcmp(symbol, "SDL_GL_GetProcAddress") == 0)
        return (void *)w_GL_GetProcAddress;
    if (strcmp(symbol, "SDL_GL_SetSwapInterval") == 0)
        return (void *)w_GL_SetSwapInterval;
    if (strcmp(symbol, "SDL_GL_SwapWindow") == 0 && (getenv("HYPRISM_PERF") || getenv("HYPRISM_GPU_PROFILE")))
        return (void *)w_GL_SwapWindow;
    if (strcmp(symbol, "SDL_GetDisplayContentScale") == 0 && getenv("HYPRISM_CONTENT_SCALE"))
        return (void *)w_GetDisplayContentScale;
    if (g_scale == 1.0f)
        return NULL;
    for (size_t i = 0; i < SHIM_COUNT(k_wrappers); i++)
        if (strcmp(k_wrappers[i].name, symbol) == 0)
            return k_wrappers[i].fn;
    return NULL;
}
