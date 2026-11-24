// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// GPU and memory tuning for the client's OpenGL calls (see gl_entry in gl.c).
//
//  - Shadow maps at a fraction of their size: a square depth texture of 1024 or more texels is the
//    sun's shadow map (the screen's depth buffer has the screen's aspect). It is allocated at
//    1/HYPRISM_SHADOW_DIV of its size (default 2; 1 turns it off), and while a framebuffer with such
//    an attachment is bound, glViewport and glScissor are scaled to match. Shaders sample it with
//    normalized coordinates, so they need no change.
//  - Large textures at half size (HYPRISM_TEXTURE_HALF=1, off by default): an RGBA8 texture of 2048
//    texels or more on a side is uploaded at half size. Level n of the game's mip chain becomes
//    level n-1, sub-image updates of level 0 are halved too, and max/base levels move with them.
//    A shader that reads texels by integer position (texelFetch) would see the wrong texels.
//  - GPU time per program (HYPRISM_GPU_PROFILE=1): a GL_TIME_ELAPSED query runs while each program
//    is in use; every five seconds the most expensive programs are logged with their share of the
//    frame. HYPRISM_DUMP_SHADERS names each program's shaders by number for a closer look.

#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "macho.h"

#define GL_TIME_ELAPSED_EXT 0x88BF
#define GL_TEXTURE_2D_ARRAY_ 0x8C1A
#define GL_TEXTURE_BINDING_2D_ARRAY_ 0x8C1D
#define GL_DRAW_FRAMEBUFFER_BINDING_ 0x8CA6
#define GL_DRAW_FRAMEBUFFER_ 0x8CA9
#define GL_FRAMEBUFFER_ 0x8D40
#define GL_TEXTURE_BASE_LEVEL_ 0x813C
#define GL_TEXTURE_MAX_LEVEL_ 0x813D

static int g_shadow_div = -1, g_texture_half = -1, g_profile = -1;

static void read_settings(void)
{
    if (g_shadow_div >= 0)
        return;
    const char *s = getenv("HYPRISM_SHADOW_DIV");
    g_shadow_div = s ? atoi(s) : 2;
    if (g_shadow_div < 1 || g_shadow_div > 8)
        g_shadow_div = 1;
    s = getenv("HYPRISM_TEXTURE_HALF");
    g_texture_half = s && s[0] == '1';
    s = getenv("HYPRISM_GPU_PROFILE");
    g_profile = s && s[0] == '1';
    if (g_shadow_div > 1)
        macho_log("gl", "shadow maps at 1/%d size (HYPRISM_SHADOW_DIV=1 turns this off)", g_shadow_div);
    if (g_texture_half)
        macho_log("gl", "large textures at half size (HYPRISM_TEXTURE_HALF)");
}

// ---------------------------------------------------------------------------
// Bookkeeping of the textures and framebuffers that were made smaller

#define MAX_SCALED 512
static struct { GLuint name; int div; } g_textures[MAX_SCALED], g_framebuffers[MAX_SCALED];
static int g_texture_count, g_framebuffer_count;

static int find_div(const void *table, int count, GLuint name)
{
    const struct { GLuint name; int div; } *t = table;
    for (int i = 0; i < count; i++)
        if (t[i].name == name)
            return t[i].div;
    return 1;
}

static void set_div(void *table, int *count, GLuint name, int div)
{
    struct { GLuint name; int div; } *t = table;
    for (int i = 0; i < *count; i++)
        if (t[i].name == name) {
            t[i].div = div;
            return;
        }
    if (*count < MAX_SCALED && div > 1) {
        t[*count].name = name;
        t[*count].div = div;
        (*count)++;
    }
}

static void (*gl_GetIntegerv_)(GLenum, GLint *);
static int g_bound_fb_div = 1;

static GLuint bound(GLenum binding)
{
    GLint v = 0;
    if (gl_GetIntegerv_)
        gl_GetIntegerv_(binding, &v);
    return (GLuint)v;
}

static GLenum binding_for(GLenum target)
{
    return target == GL_TEXTURE_2D_ARRAY_ ? GL_TEXTURE_BINDING_2D_ARRAY_ : GL_TEXTURE_BINDING_2D;
}

static int is_depth(GLenum internal)
{
    switch (internal) {
    case 0x1902: // GL_DEPTH_COMPONENT
    case 0x81A5: // GL_DEPTH_COMPONENT16
    case 0x81A6: // GL_DEPTH_COMPONENT24
    case 0x81A7: // GL_DEPTH_COMPONENT32
    case 0x8CAC: // GL_DEPTH_COMPONENT32F
    case 0x84F9: // GL_DEPTH_STENCIL
    case 0x88F0: // GL_DEPTH24_STENCIL8
    case 0x8CAD: // GL_DEPTH32F_STENCIL8
        return 1;
    default:
        return 0;
    }
}

// The divisor for a new texture of this shape, and records it against the bound texture.
static int shadow_div_for(GLenum target, GLenum internal, GLsizei w, GLsizei h, const void *pixels)
{
    read_settings();
    if (g_shadow_div <= 1 || pixels || !is_depth(internal) || w != h || w < 1024)
        return 1;
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_2D_ARRAY_)
        return 1;
    GLuint tex = bound(binding_for(target));
    if (find_div(g_textures, g_texture_count, tex) == 1) {
        static int logged;
        if (logged++ < 8)
            macho_log("gl", "shadow map %u: %dx%d -> %dx%d", tex, w, h, w / g_shadow_div, h / g_shadow_div);
    }
    set_div(g_textures, &g_texture_count, tex, g_shadow_div);
    return g_shadow_div;
}

// ---------------------------------------------------------------------------
// Half-size textures

static int pixels_in_memory(const void *pixels);

static int half_candidate(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLenum format,
                          GLenum type, const void *pixels)
{
    read_settings();
    return g_texture_half && target == GL_TEXTURE_2D && level == 0 && pixels_in_memory(pixels) && w >= 2048 &&
           h >= 2048 &&
           !(w & 1) && !(h & 1) && format == GL_RGBA && type == GL_UNSIGNED_BYTE &&
           (internal == GL_RGBA || internal == GL_RGBA8 || internal == GL_SRGB8_ALPHA8);
}

// Averages 2x2 blocks of RGBA8 texels; returns a malloc'd buffer of (w/2) x (h/2) texels.
static unsigned char *halve_rgba8(const unsigned char *src, GLsizei w, GLsizei h, GLint row_texels)
{
    GLsizei hw = w / 2, hh = h / 2;
    unsigned char *dst = malloc((size_t)hw * hh * 4);
    if (!dst)
        return NULL;
    size_t stride = (size_t)row_texels * 4;
    for (GLsizei y = 0; y < hh; y++) {
        const unsigned char *a = src + (size_t)(2 * y) * stride, *b = a + stride;
        unsigned char *d = dst + (size_t)y * hw * 4;
        for (GLsizei x = 0; x < hw; x++, a += 8, b += 8, d += 4)
            for (int c = 0; c < 4; c++)
                d[c] = (unsigned char)((a[c] + a[c + 4] + b[c] + b[c + 4] + 2) / 4);
    }
    return dst;
}

// GL_UNPACK_ROW_LENGTH, or `w` when it is 0 (rows as long as the image).
static GLint unpack_row_length(GLsizei w)
{
    GLint row = 0;
    if (gl_GetIntegerv_)
        gl_GetIntegerv_(GL_UNPACK_ROW_LENGTH, &row);
    return row > 0 ? row : w;
}

static void (*gl_PixelStorei_)(GLenum, GLint);

// Through MobileGlues, so its view of the unpack state stays right.
static void set_unpack_row_length(GLint row)
{
    if (gl_PixelStorei_)
        gl_PixelStorei_(GL_UNPACK_ROW_LENGTH, row);
}

// With a pixel unpack buffer bound, `pixels` is an offset into it, not memory the CPU can read.
static int pixels_in_memory(const void *pixels)
{
    return pixels && bound(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */) == 0;
}

static size_t g_saved_bytes;

static void (*real_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*real_TexParameteri)(GLenum, GLenum, GLint);

/// The loader's glTexImage2D (after the RGB10 fix in gl.c) passes through here.
void gl_tune_tex_image_2d(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                          GLenum format, GLenum type, const void *pixels,
                          void (*next)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                       const void *))
{
    int div = shadow_div_for(target, (GLenum)internal, w, h, pixels);
    if (div > 1) {
        next(target, level, internal, w / div, h / div, border, format, type, pixels);
        return;
    }
    if (g_texture_half && target == GL_TEXTURE_2D) {
        GLuint tex = bound(GL_TEXTURE_BINDING_2D);
        if (half_candidate(target, level, (GLenum)internal, w, h, format, type, pixels)) {
            unsigned char *half = halve_rgba8(pixels, w, h, unpack_row_length(w));
            if (half) {
                set_div(g_textures, &g_texture_count, tex, 2);
                g_saved_bytes += (size_t)w * h * 3;
                // Uploaded at level 0, tightly packed; the game's level 1 (same size) overwrites it
                // if it sends one.
                GLint row = unpack_row_length(0);
                set_unpack_row_length(0);
                next(target, 0, internal, w / 2, h / 2, border, format, type, half);
                set_unpack_row_length(row);
                free(half);
                static int logged;
                if (logged++ < 12)
                    macho_log("gl", "texture %u: %dx%d uploaded at half size (%zu MB saved so far)", tex, w, h,
                              g_saved_bytes >> 20);
                return;
            }
        } else if (level > 0 && find_div(g_textures, g_texture_count, tex) == 2) {
            next(target, level - 1, internal, w, h, border, format, type, pixels);
            return;
        } else if (level == 0 && find_div(g_textures, g_texture_count, tex) == 2) {
            set_div(g_textures, &g_texture_count, tex, 1); // reallocated by the game at another size
        }
    }
    next(target, level, internal, w, h, border, format, type, pixels);
}

static void w_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                            GLenum type, const void *pixels)
{
    if (g_texture_half && target == GL_TEXTURE_2D &&
        find_div(g_textures, g_texture_count, bound(GL_TEXTURE_BINDING_2D)) == 2) {
        if (level > 0) {
            real_TexSubImage2D(target, level - 1, x, y, w, h, format, type, pixels);
            return;
        }
        if (pixels_in_memory(pixels) && format == GL_RGBA && type == GL_UNSIGNED_BYTE && !(x & 1) && !(y & 1) &&
            !(w & 1) &&
            !(h & 1)) {
            unsigned char *half = halve_rgba8(pixels, w, h, unpack_row_length(w));
            if (half) {
                GLint row = unpack_row_length(0); // the halved block is tightly packed
                set_unpack_row_length(0);
                real_TexSubImage2D(target, 0, x / 2, y / 2, w / 2, h / 2, format, type, half);
                set_unpack_row_length(row);
                free(half);
                return;
            }
        }
    }
    real_TexSubImage2D(target, level, x, y, w, h, format, type, pixels);
}

static void w_TexParameteri(GLenum target, GLenum pname, GLint value)
{
    if (g_texture_half && target == GL_TEXTURE_2D && value > 0 &&
        (pname == GL_TEXTURE_MAX_LEVEL_ || pname == GL_TEXTURE_BASE_LEVEL_) &&
        find_div(g_textures, g_texture_count, bound(GL_TEXTURE_BINDING_2D)) == 2)
        value--;
    real_TexParameteri(target, pname, value);
}

// ---------------------------------------------------------------------------
// Shadow map allocation through glTexImage3D and glTexStorage*

static void (*real_TexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum,
                               const void *);
static void (*real_TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static void (*real_TexStorage3D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);

static void w_TexImage3D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLsizei d,
                         GLint border, GLenum format, GLenum type, const void *pixels)
{
    int div = shadow_div_for(target, (GLenum)internal, w, h, pixels);
    real_TexImage3D(target, level, internal, w / div, h / div, d, border, format, type, pixels);
}

static void w_TexStorage2D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h)
{
    int div = shadow_div_for(target, internal, w, h, NULL);
    real_TexStorage2D(target, levels, internal, w / div, h / div);
}

static void w_TexStorage3D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d)
{
    int div = shadow_div_for(target, internal, w, h, NULL);
    real_TexStorage3D(target, levels, internal, w / div, h / div, d);
}

// ---------------------------------------------------------------------------
// Framebuffers drawing into a smaller shadow map

static void (*real_FramebufferTexture)(GLenum, GLenum, GLuint, GLint);
static void (*real_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (*real_FramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint);
static void (*real_BindFramebuffer)(GLenum, GLuint);
static void (*real_Scissor)(GLint, GLint, GLsizei, GLsizei);

static void note_attachment(GLenum target, GLuint texture)
{
    if (g_texture_count == 0 || (target != GL_FRAMEBUFFER_ && target != GL_DRAW_FRAMEBUFFER_))
        return;
    int div = texture ? find_div(g_textures, g_texture_count, texture) : 1;
    GLuint fb = bound(GL_DRAW_FRAMEBUFFER_BINDING_);
    if (div > 1 || find_div(g_framebuffers, g_framebuffer_count, fb) > 1)
        set_div(g_framebuffers, &g_framebuffer_count, fb, div);
    g_bound_fb_div = find_div(g_framebuffers, g_framebuffer_count, fb);
}

static void w_FramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level)
{
    real_FramebufferTexture(target, attachment, texture, level);
    note_attachment(target, texture);
}

static void w_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                                   GLint level)
{
    real_FramebufferTexture2D(target, attachment, textarget, texture, level);
    note_attachment(target, texture);
}

static void w_FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                      GLint layer)
{
    real_FramebufferTextureLayer(target, attachment, texture, level, layer);
    note_attachment(target, texture);
}

// The last viewport and scissor the game asked for, in its own (full-size) units.
static GLint g_vp[4], g_sc[4];
static int g_have_vp, g_have_sc;
static void (*real_Viewport_)(GLint, GLint, GLsizei, GLsizei);

/// Called by gl.c's glViewport with the game's rectangle, before it is scaled.
void gl_tune_note_viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    g_vp[0] = x, g_vp[1] = y, g_vp[2] = w, g_vp[3] = h;
    g_have_vp = 1;
}

static void w_BindFramebuffer(GLenum target, GLuint fb)
{
    real_BindFramebuffer(target, fb);
    if (target != GL_FRAMEBUFFER_ && target != GL_DRAW_FRAMEBUFFER_)
        return;
    int div = g_framebuffer_count ? find_div(g_framebuffers, g_framebuffer_count, fb) : 1;
    if (div == g_bound_fb_div)
        return;
    g_bound_fb_div = div;
    // A viewport set before the bind belongs to the new target's scale.
    if (g_have_vp && real_Viewport_)
        real_Viewport_(g_vp[0] / div, g_vp[1] / div, g_vp[2] / div, g_vp[3] / div);
    if (g_have_sc && real_Scissor)
        real_Scissor(g_sc[0] / div, g_sc[1] / div, g_sc[2] / div, g_sc[3] / div);
}

/// The divisor for viewport and scissor rectangles on the bound draw framebuffer.
int gl_tune_viewport_div(void)
{
    return g_bound_fb_div;
}

static void w_Scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    g_sc[0] = x, g_sc[1] = y, g_sc[2] = w, g_sc[3] = h;
    g_have_sc = 1;
    int div = g_bound_fb_div;
    real_Scissor(x / div, y / div, w / div, h / div);
}

// ---------------------------------------------------------------------------
// GPU time per program

#define MAX_QUERIES 256
#define MAX_TIMED 256
static void (*q_GenQueries)(GLsizei, GLuint *);
static void (*q_BeginQuery)(GLenum, GLuint);
static void (*q_EndQuery)(GLenum);
static void (*q_GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
static GLuint g_query_pool[MAX_QUERIES];
static GLuint g_query_program[MAX_QUERIES];
static int g_query_head, g_query_tail, g_query_active = -1;
static struct { GLuint program; double ns; unsigned uses; } g_timed[MAX_TIMED];
static int g_timed_count;
static struct timespec g_profile_start;
static unsigned g_profile_frames;

static int profile_ready(void)
{
    static int state;
    if (state)
        return state > 0;
    // The device's own OpenGL ES, under MobileGlues, on the same context and thread.
    void *gles = dlopen("libGLESv3.so", RTLD_NOW);
    const GLubyte *(*get_string)(GLenum) = NULL;
    if (gles) {
        q_GenQueries = (void (*)(GLsizei, GLuint *))dlsym(gles, "glGenQueries");
        q_BeginQuery = (void (*)(GLenum, GLuint))dlsym(gles, "glBeginQuery");
        q_EndQuery = (void (*)(GLenum))dlsym(gles, "glEndQuery");
        q_GetQueryObjectuiv = (void (*)(GLuint, GLenum, GLuint *))dlsym(gles, "glGetQueryObjectuiv");
        get_string = (const GLubyte *(*)(GLenum))dlsym(gles, "glGetString");
    }
    const char *ext = get_string ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    state = q_GenQueries && q_BeginQuery && q_EndQuery && q_GetQueryObjectuiv && ext &&
                    strstr(ext, "GL_EXT_disjoint_timer_query")
                ? 1
                : -1;
    if (state > 0) {
        q_GenQueries(MAX_QUERIES, g_query_pool);
        clock_gettime(CLOCK_MONOTONIC, &g_profile_start);
    }
    macho_log("gl", "GPU profile %s", state > 0 ? "on" : "unavailable (no GL_EXT_disjoint_timer_query)");
    return state > 0;
}

static void add_time(GLuint program, double ns)
{
    for (int i = 0; i < g_timed_count; i++)
        if (g_timed[i].program == program) {
            g_timed[i].ns += ns;
            g_timed[i].uses++;
            return;
        }
    if (g_timed_count < MAX_TIMED)
        g_timed[g_timed_count++] = (typeof(g_timed[0])){ program, ns, 1 };
}

static void collect_results(void)
{
    while (g_query_tail != g_query_head && g_query_tail != g_query_active) {
        GLuint q = g_query_pool[g_query_tail], available = 0, ns = 0;
        q_GetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available)
            break;
        q_GetQueryObjectuiv(q, GL_QUERY_RESULT, &ns);
        add_time(g_query_program[g_query_tail], ns);
        g_query_tail = (g_query_tail + 1) % MAX_QUERIES;
    }
}

static int by_time(const void *a, const void *b)
{
    double x = ((const typeof(g_timed[0]) *)a)->ns, y = ((const typeof(g_timed[0]) *)b)->ns;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report_profile(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - g_profile_start.tv_sec) + (now.tv_nsec - g_profile_start.tv_nsec) / 1e9;
    if (elapsed < 5 || g_profile_frames == 0)
        return;
    qsort(g_timed, g_timed_count, sizeof g_timed[0], by_time);
    double total = 0;
    for (int i = 0; i < g_timed_count; i++)
        total += g_timed[i].ns;
    macho_log("gpu", "%u frames, %.2f ms of GPU time per frame in programs", g_profile_frames,
              total / 1e6 / g_profile_frames);
    for (int i = 0; i < g_timed_count && i < 10; i++)
        macho_log("gpu", "  program %u: %.2f ms/frame (%.0f%%), %u uses", g_timed[i].program,
                  g_timed[i].ns / 1e6 / g_profile_frames, total > 0 ? 100 * g_timed[i].ns / total : 0,
                  g_timed[i].uses / g_profile_frames);
    g_timed_count = 0;
    g_profile_frames = 0;
    g_profile_start = now;
}

/// Called from glUseProgram: times the GPU work done with each program.
void gl_tune_use_program(GLuint program)
{
    read_settings();
    if (!g_profile || !profile_ready())
        return;
    if (g_query_active >= 0) {
        q_EndQuery(GL_TIME_ELAPSED_EXT);
        g_query_active = -1;
    }
    collect_results();
    int next = (g_query_head + 1) % MAX_QUERIES;
    if (!program || next == g_query_tail)
        return; // pool full until results come back
    g_query_program[g_query_head] = program;
    q_BeginQuery(GL_TIME_ELAPSED_EXT, g_query_pool[g_query_head]);
    g_query_active = g_query_head;
    g_query_head = next;
}

/// Called once per presented frame.
void gl_tune_frame(void)
{
    if (g_profile <= 0)
        return;
    g_profile_frames++;
    report_profile();
}

// ---------------------------------------------------------------------------

static const struct { const char *name; void **real; void *entry; } k_entries[] = {
    { "glTexImage3D", (void **)&real_TexImage3D, (void *)w_TexImage3D },
    { "glTexStorage2D", (void **)&real_TexStorage2D, (void *)w_TexStorage2D },
    { "glTexStorage3D", (void **)&real_TexStorage3D, (void *)w_TexStorage3D },
    { "glFramebufferTexture", (void **)&real_FramebufferTexture, (void *)w_FramebufferTexture },
    { "glFramebufferTexture2D", (void **)&real_FramebufferTexture2D, (void *)w_FramebufferTexture2D },
    { "glFramebufferTextureLayer", (void **)&real_FramebufferTextureLayer, (void *)w_FramebufferTextureLayer },
    { "glBindFramebuffer", (void **)&real_BindFramebuffer, (void *)w_BindFramebuffer },
    { "glScissor", (void **)&real_Scissor, (void *)w_Scissor },
    { "glTexSubImage2D", (void **)&real_TexSubImage2D, (void *)w_TexSubImage2D },
    { "glTexParameteri", (void **)&real_TexParameteri, (void *)w_TexParameteri },
};

/// Wraps the GL entry points this file tunes; returns `fn` for the others.
void *gl_tune_entry(const char *name, void *fn, void *(*resolve)(const char *))
{
    read_settings();
    if (!gl_GetIntegerv_)
        gl_GetIntegerv_ = (void (*)(GLenum, GLint *))resolve("glGetIntegerv");
    if (!real_Viewport_)
        real_Viewport_ = (void (*)(GLint, GLint, GLsizei, GLsizei))resolve("glViewport");
    if (!gl_PixelStorei_)
        gl_PixelStorei_ = (void (*)(GLenum, GLint))resolve("glPixelStorei");
    int shadows = g_shadow_div > 1, textures = g_texture_half;
    for (size_t i = 0; i < sizeof k_entries / sizeof k_entries[0]; i++) {
        if (strcmp(k_entries[i].name, name) != 0)
            continue;
        int needed = (shadows && i < 8) || (textures && (i == 8 || i == 9));
        if (!needed)
            return fn;
        *k_entries[i].real = fn;
        return k_entries[i].entry;
    }
    return fn;
}
