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

static bool w_PollEvent(SDL_Event *event)
{
    REAL(SDL_PollEvent);
    bool got = real_SDL_PollEvent(event);
    if (got && event)
        scale_event(event);
    return got;
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
    if (g_scale == 1.0f)
        return NULL;
    for (size_t i = 0; i < SHIM_COUNT(k_wrappers); i++)
        if (strcmp(k_wrappers[i].name, symbol) == 0)
            return k_wrappers[i].fn;
    return NULL;
}
