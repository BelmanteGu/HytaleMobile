// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// dlopen and dlsym for the Mach-O program. Darwin system libraries resolve to
// virtual handles backed by the shim tables; other libraries are reported so
// a native replacement can be wired in.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "macho.h"

#define LOG(...) macho_log("dyld", __VA_ARGS__)

// Darwin special handles.
#define APPLE_RTLD_NEXT ((void *)-1)
#define APPLE_RTLD_DEFAULT ((void *)-2)
#define APPLE_RTLD_SELF ((void *)-3)
#define APPLE_RTLD_MAIN_ONLY ((void *)-5)

struct virtual_library {
    char *path;
    void *host; // host ELF replacement, if any
};

static struct virtual_library g_libraries[64];
static size_t g_library_count;
static __thread const char *t_dlerror;

static int is_darwin_system_library(const char *path)
{
    if (strstr(path, "libicucore"))
        return 0; // served by the Android ICU replacement below
    return strncmp(path, "/usr/lib/", 9) == 0 || strncmp(path, "/System/Library/", 16) == 0 ||
           strstr(path, "libSystem") != NULL;
}

// Bundled Mac dylibs that depend on Apple frameworks are replaced by Android
// builds exporting the same C API. Matching uses the bare library name, so the
// .NET probing variants (openal, libopenal, libopenal.dylib, ...) all resolve.
static const struct { const char *name; const char *host; } k_replacements[] = {
    { "icucore", "libicu.so" },
    { "SDL3", "libSDL3.so" },
    { "openal", "libopenal.so" },
};

static const char *replacement_for(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strncmp(base, "lib", 3) == 0)
        base += 3;
    size_t len = strcspn(base, ".");
    for (size_t i = 0; i < SHIM_COUNT(k_replacements); i++)
        if (strlen(k_replacements[i].name) == len && strncmp(base, k_replacements[i].name, len) == 0)
            return k_replacements[i].host;
    return NULL;
}

extern const char *g_executable_path;

static const char *executable_dir(void)
{
    static char dir[PATH_MAX];
    if (!dir[0]) {
        snprintf(dir, sizeof dir, "%s", g_executable_path);
        char *slash = strrchr(dir, '/');
        if (slash)
            *slash = 0;
    }
    return dir;
}

static void *apple_dlopen(const char *path, int mode)
{
    if (!path)
        return APPLE_RTLD_MAIN_ONLY;
    LOG("dlopen(%s, %#x)", path, mode);
    for (size_t i = 0; i < g_library_count; i++)
        if (strcmp(g_libraries[i].path, path) == 0)
            return &g_libraries[i];

    void *host = NULL;
    if (!is_darwin_system_library(path)) {
        const char *replacement = replacement_for(path);
        if (replacement)
            host = dlopen(replacement, RTLD_NOW | RTLD_LOCAL);
        char bundled[PATH_MAX];
        if (!replacement && access(path, R_OK) != 0 && !strchr(path, '/')) {
            snprintf(bundled, sizeof bundled, "%s/%s%s%s", executable_dir(),
                     strncmp(path, "lib", 3) ? "lib" : "", path, strstr(path, ".dylib") ? "" : ".dylib");
            path = bundled;
        }
        if (!host && !replacement && access(path, R_OK) == 0) {
            // A bundled macOS dylib: load it natively with the Mach-O loader.
            void *image = macho_dlopen_image(path);
            if (image)
                return image;
        }
        if (!host) {
            LOG("  no native replacement for %s%s%s", path, replacement ? ": " : "", replacement ? dlerror() : "");
            t_dlerror = "library not available on this platform";
            return NULL;
        }
    }
    if (g_library_count == SHIM_COUNT(g_libraries))
        return NULL;
    g_libraries[g_library_count] = (struct virtual_library){ strdup(path), host };
    return &g_libraries[g_library_count++];
}

void *sdl_scale_lookup(void *sdl_handle, const char *symbol);

/// Binds an import of a bundled dylib from another bundled library that has a
/// native replacement (libSDL3_image links libSDL3), so both use the same host
/// library. Returns 1 when install_name is replaced, with the symbol or NULL in *out.
int dyld_replacement_symbol(const char *install_name, const char *name, void **out)
{
    if (is_darwin_system_library(install_name))
        return 0;
    const char *replacement = replacement_for(install_name);
    if (!replacement)
        return 0;
    *out = NULL;
    void *host = dlopen(replacement, RTLD_NOW | RTLD_LOCAL);
    if (!host)
        return 1;
    const char *symbol = name[0] == '_' ? name + 1 : name;
    if (strcmp(replacement, "libSDL3.so") == 0)
        *out = sdl_scale_lookup(host, symbol);
    if (!*out)
        *out = dlsym(host, symbol);
    return 1;
}

static void *apple_dlsym(void *handle, const char *symbol)
{
    char mangled[512];
    snprintf(mangled, sizeof mangled, "_%s", symbol);
    struct virtual_library *lib = NULL;
    if (handle >= (void *)g_libraries && handle < (void *)(g_libraries + SHIM_COUNT(g_libraries)))
        lib = handle;

    void *address = NULL;
    if (macho_is_image(handle))
        address = macho_image_symbol(handle, mangled);
    else if (lib && lib->host && strstr(lib->path, "SDL3") && !strstr(lib->path, "SDL3_image") &&
             (address = sdl_scale_lookup(lib->host, symbol)) != NULL)
        ; // point-space wrapper (see sdl_scale.c)
    else if (lib && lib->host)
        address = dlsym(lib->host, symbol);
    else
        address = shim_lookup(mangled, lib ? lib->path : NULL);
    if (!address) {
        LOG("dlsym(%s, %s) not found", lib ? lib->path : "<special>", symbol);
        t_dlerror = "symbol not found";
    }
    return address;
}

static int apple_dlclose(void *handle)
{
    (void)handle;
    return 0;
}

static char *apple_dlerror(void)
{
    const char *e = t_dlerror;
    t_dlerror = NULL;
    return (char *)e;
}

const struct shim dyld_shims[] = {
    { "_dlopen", (void *)apple_dlopen },
    { "_dlsym", (void *)apple_dlsym },
    { "_dlclose", (void *)apple_dlclose },
    { "_dlerror", (void *)apple_dlerror },
};
const size_t dyld_shims_count = SHIM_COUNT(dyld_shims);
