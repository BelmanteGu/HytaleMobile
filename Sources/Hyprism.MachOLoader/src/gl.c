// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// OpenGL entry points the client and Noesis resolve, through SDL_GL_GetProcAddress
// or OpenGL.framework imports, pass through gl_entry before reaching MobileGlues.
//
// Darwin arm64 packs stack arguments at their natural size and alignment, while
// AAPCS64 gives each one an 8-byte slot. Functions with more than eight integer
// arguments and two or more 4-byte values on the stack therefore read the wrong
// values on Android; glTexSubImage3D received the low half of the pixel pointer
// as its type, so the interface atlas was never uploaded. The STACK_ARGS entries
// in trampolines.S pass the caller's stack argument area to the readers below.
//
// Shaders are adjusted for what GLSL ES cannot express: an upper-left gl_FragCoord
// origin and uniform initializers (see "GLSL that OpenGL ES does not express").
//
// HYPRISM_GL_TRACE=1 also logs texture uploads and the GL errors they raise.

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macho.h"

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;

static uint32_t arg32(const uint8_t *stack, size_t offset)
{
    uint32_t v;
    memcpy(&v, stack + offset, sizeof v);
    return v;
}

static const void *argptr(const uint8_t *stack, size_t offset)
{
    const void *v;
    memcpy(&v, stack + offset, sizeof v);
    return v;
}

// ---------------------------------------------------------------------------
// Darwin stack argument layouts

static void (*real_TexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum,
                                  const void *);
static void (*real_CompressedTexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum,
                                            GLsizei, const void *);
static void (*real_ClearTexSubImage)(GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum,
                                     const void *);
static void (*real_BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
static void (*real_CopyImageSubData)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint,
                                     GLint, GLsizei, GLsizei, GLsizei);

// Stack: format @0, type @4, pixels @8.
void gl_tex_sub_image_3d_stack(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                               GLsizei d, const uint8_t *stack)
{
    real_TexSubImage3D(target, level, x, y, z, w, h, d, arg32(stack, 0), arg32(stack, 4), argptr(stack, 8));
}

// Stack: format @0, type @4, data @8.
void gl_clear_tex_sub_image_stack(GLuint texture, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h,
                                  GLsizei d, const uint8_t *stack)
{
    real_ClearTexSubImage(texture, level, x, y, z, w, h, d, arg32(stack, 0), arg32(stack, 4), argptr(stack, 8));
}

// Stack: format @0, imageSize @4, data @8.
void gl_compressed_tex_sub_image_3d_stack(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w,
                                          GLsizei h, GLsizei d, const uint8_t *stack)
{
    real_CompressedTexSubImage3D(target, level, x, y, z, w, h, d, arg32(stack, 0), (GLsizei)arg32(stack, 4),
                                 argptr(stack, 8));
}

// Stack: mask @0, filter @4.
void gl_blit_framebuffer_stack(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1,
                               GLint dy1, const uint8_t *stack)
{
    real_BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, arg32(stack, 0), arg32(stack, 4));
}

// Stack: dstLevel @0, dstX @4, dstY @8, dstZ @12, srcWidth @16, srcHeight @20, srcDepth @24.
void gl_copy_image_sub_data_stack(GLuint src, GLenum src_target, GLint src_level, GLint sx, GLint sy, GLint sz,
                                  GLuint dst, GLenum dst_target, const uint8_t *stack)
{
    real_CopyImageSubData(src, src_target, src_level, sx, sy, sz, dst, dst_target, (GLint)arg32(stack, 0),
                          (GLint)arg32(stack, 4), (GLint)arg32(stack, 8), (GLint)arg32(stack, 12),
                          (GLsizei)arg32(stack, 16), (GLsizei)arg32(stack, 20), (GLsizei)arg32(stack, 24));
}

void apple_glTexSubImage3D(void);
void apple_glCompressedTexSubImage3D(void);
void apple_glClearTexSubImage(void);
void apple_glBlitFramebuffer(void);
void apple_glCopyImageSubData(void);

static const struct { const char *name; void **real; void (*entry)(void); } k_stack_entries[] = {
    { "glTexSubImage3D", (void **)&real_TexSubImage3D, apple_glTexSubImage3D },
    { "glCompressedTexSubImage3D", (void **)&real_CompressedTexSubImage3D, apple_glCompressedTexSubImage3D },
    { "glClearTexSubImage", (void **)&real_ClearTexSubImage, apple_glClearTexSubImage },
    { "glBlitFramebuffer", (void **)&real_BlitFramebuffer, apple_glBlitFramebuffer },
    { "glCopyImageSubData", (void **)&real_CopyImageSubData, apple_glCopyImageSubData },
};

// ---------------------------------------------------------------------------
// GLSL that OpenGL ES does not express
//
// Two desktop features the client relies on are lost in translation:
//  - layout(origin_upper_left) for gl_FragCoord. The declaration is replaced by a
//    macro that flips y against the viewport height, kept in a uniform.
//  - Uniform initializers (uniform float uOpacity = 1.0;). They read as 0 on
//    OpenGL ES, which made the avatar preview fully transparent, so the values are
//    parsed from the source and set after every glLinkProgram.

#define MAX_SHADERS 1024
#define MAX_PROGRAMS 512
#define MAX_INITS 16
#define MAX_ATTACHED 4

typedef char GLchar;

struct uniform_init {
    char name[48];
    int is_int;
    int components;
    float value[4];
};

struct shader_info {
    GLuint shader;
    int flip;
    int init_count;
    struct uniform_init inits[MAX_INITS];
};

struct program_info {
    GLuint program;
    GLuint shaders[MAX_ATTACHED];
    int shader_count;
    GLint flip_location; // -1 when the program does not flip gl_FragCoord
};

static void (*real_ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static void (*real_AttachShader)(GLuint, GLuint);
static void (*real_LinkProgram)(GLuint);
static void (*real_UseProgram)(GLuint);
static void (*real_Viewport)(GLint, GLint, GLsizei, GLsizei);
static GLint (*gl_GetUniformLocation)(GLuint, const GLchar *);
static void (*gl_GetShaderiv)(GLuint, GLenum, GLint *);
static void (*gl_GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static void (*gl_GetProgramiv)(GLuint, GLenum, GLint *);
static void (*gl_GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static void (*real_CompileShader)(GLuint);
static void (*gl_Uniform1f)(GLint, float);
static void (*gl_Uniform2f)(GLint, float, float);
static void (*gl_Uniform3f)(GLint, float, float, float);
static void (*gl_Uniform4f)(GLint, float, float, float, float);
static void (*gl_Uniform1i)(GLint, GLint);
static void (*gl_Uniform2i)(GLint, GLint, GLint);
static void (*gl_Uniform3i)(GLint, GLint, GLint, GLint);
static void (*gl_Uniform4i)(GLint, GLint, GLint, GLint, GLint);

static struct shader_info g_shaders[MAX_SHADERS];
static struct program_info g_programs[MAX_PROGRAMS];
static size_t g_shader_count, g_program_count;
static GLuint g_current_program;
static float g_viewport_height;
static GLsizei g_viewport_width;

static struct shader_info *shader_info(GLuint shader, int create)
{
    for (size_t i = 0; i < g_shader_count; i++)
        if (g_shaders[i].shader == shader)
            return &g_shaders[i];
    if (!create || g_shader_count == MAX_SHADERS)
        return NULL;
    struct shader_info *s = &g_shaders[g_shader_count++];
    s->shader = shader;
    return s;
}

static struct program_info *program_info(GLuint program, int create)
{
    for (size_t i = 0; i < g_program_count; i++)
        if (g_programs[i].program == program)
            return &g_programs[i];
    if (!create || g_program_count == MAX_PROGRAMS)
        return NULL;
    struct program_info *p = &g_programs[g_program_count++];
    p->program = program;
    p->flip_location = -1;
    return p;
}

// Constant expressions of uniform initializers: numbers, + - * /, parentheses and
// scalar or vector constructors such as vec3(0) or ivec2(1, 2).
static int is_ident(char c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

struct value {
    int n;
    float v[4];
};

static const char *skip_space(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static const char *parse_sum(const char *p, struct value *out);

static const char *parse_factor(const char *p, struct value *out)
{
    p = skip_space(p);
    if (*p == '(') {
        p = parse_sum(p + 1, out);
        if (!p || *(p = skip_space(p)) != ')')
            return NULL;
        return p + 1;
    }
    if (*p == '-') {
        p = parse_factor(p + 1, out);
        for (int i = 0; p && i < out->n; i++)
            out->v[i] = -out->v[i];
        return p;
    }
    if ((*p >= '0' && *p <= '9') || *p == '.') {
        char *end;
        out->n = 1;
        out->v[0] = strtof(p, &end);
        if (*end == 'f' || *end == 'F' || *end == 'u' || *end == 'U')
            end++;
        return end;
    }
    const char *name = p;
    while (is_ident(*p))
        p++;
    if (p == name || *(p = skip_space(p)) != '(')
        return NULL;
    int width = 1;
    const char *digit = p - 1;
    while (digit > name && (*digit == ' ' || *digit == '\t'))
        digit--;
    if (*digit >= '2' && *digit <= '4')
        width = *digit - '0';
    struct value args = { 0 };
    p++;
    for (;;) {
        struct value arg;
        if (!(p = parse_sum(p, &arg)))
            return NULL;
        for (int i = 0; i < arg.n && args.n < 4; i++)
            args.v[args.n++] = arg.v[i];
        p = skip_space(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != ')')
            return NULL;
        p++;
        break;
    }
    out->n = width;
    for (int i = 0; i < width; i++)
        out->v[i] = args.n == 1 ? args.v[0] : i < args.n ? args.v[i] : 0;
    return p;
}

static const char *parse_product(const char *p, struct value *out)
{
    if (!(p = parse_factor(p, out)))
        return NULL;
    for (;;) {
        p = skip_space(p);
        if (*p != '*' && *p != '/')
            return p;
        char op = *p;
        struct value rhs;
        if (!(p = parse_factor(p + 1, &rhs)))
            return NULL;
        for (int i = 0; i < out->n; i++) {
            float r = rhs.v[rhs.n == 1 ? 0 : i];
            out->v[i] = op == '*' ? out->v[i] * r : out->v[i] / r;
        }
    }
}

static const char *parse_sum(const char *p, struct value *out)
{
    if (!(p = parse_product(p, out)))
        return NULL;
    for (;;) {
        p = skip_space(p);
        if (*p != '+' && *p != '-')
            return p;
        char op = *p;
        struct value rhs;
        if (!(p = parse_product(p + 1, &rhs)))
            return NULL;
        for (int i = 0; i < out->n; i++) {
            float r = rhs.v[rhs.n == 1 ? 0 : i];
            out->v[i] = op == '+' ? out->v[i] + r : out->v[i] - r;
        }
    }
}

static void collect_uniform_inits(struct shader_info *info, const char *src)
{
    static const struct { const char *type; int is_int, components; } k_types[] = {
        { "float", 0, 1 }, { "vec2", 0, 2 }, { "vec3", 0, 3 }, { "vec4", 0, 4 },
        { "int", 1, 1 },   { "ivec2", 1, 2 }, { "ivec3", 1, 3 }, { "ivec4", 1, 4 }, { "bool", 1, 1 },
    };
    info->init_count = 0;
    for (const char *line = src; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        const char *p = skip_space(line);
        if (strncmp(p, "uniform", 7) != 0 || is_ident(p[7]))
            continue;
        p = skip_space(p + 7);
        size_t t = 0;
        for (; t < SHIM_COUNT(k_types); t++) {
            size_t len = strlen(k_types[t].type);
            if (strncmp(p, k_types[t].type, len) == 0 && !is_ident(p[len])) {
                p = skip_space(p + len);
                break;
            }
        }
        if (t == SHIM_COUNT(k_types))
            continue;
        const char *name = p;
        while (is_ident(*p))
            p++;
        size_t name_len = (size_t)(p - name);
        p = skip_space(p);
        if (*p != '=' || name_len == 0 || name_len >= sizeof info->inits[0].name || info->init_count == MAX_INITS)
            continue;
        struct value v;
        if (!parse_sum(p + 1, &v))
            continue;
        struct uniform_init *init = &info->inits[info->init_count++];
        memcpy(init->name, name, name_len);
        init->name[name_len] = 0;
        init->is_int = k_types[t].is_int;
        init->components = k_types[t].components;
        for (int i = 0; i < 4; i++)
            init->value[i] = v.n == 1 ? v.v[0] : i < v.n ? v.v[i] : 0;
    }
}

static const char k_flip_decl[] =
    "uniform float mg_FramebufferHeight;\n"
    "#define mg_FragCoordUL vec4(gl_FragCoord.x, mg_FramebufferHeight - gl_FragCoord.y, gl_FragCoord.zw)\n";

// Replaces the origin_upper_left redeclaration with the flip uniform and macro, and
// every other gl_FragCoord token with the macro. Returns NULL when not needed.
static char *rewrite_upper_left(const char *src)
{
    const char *layout = strstr(src, "origin_upper_left");
    if (!layout)
        return NULL;
    const char *line = layout;
    while (line > src && line[-1] != '\n')
        line--;
    const char *end = strchr(layout, '\n');
    end = end ? end + 1 : layout + strlen(layout);
    size_t cap = strlen(src) * 2 + sizeof k_flip_decl;
    char *out = malloc(cap), *o = out;
    memcpy(o, src, (size_t)(line - src));
    o += line - src;
    memcpy(o, k_flip_decl, sizeof k_flip_decl - 1);
    o += sizeof k_flip_decl - 1;
    static const char token[] = "gl_FragCoord", macro[] = "mg_FragCoordUL";
    for (const char *p = end; *p;) {
        if (strncmp(p, token, sizeof token - 1) == 0 && !is_ident(p[-1]) && !is_ident(p[sizeof token - 1])) {
            memcpy(o, macro, sizeof macro - 1);
            o += sizeof macro - 1;
            p += sizeof token - 1;
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
    return out;
}

static void set_flip_uniform(void)
{
    struct program_info *p = program_info(g_current_program, 0);
    if (p && p->flip_location >= 0)
        gl_Uniform1f(p->flip_location, g_viewport_height);
}

static void apply_uniform_init(GLint loc, const struct uniform_init *u)
{
    const float *v = u->value;
    if (u->is_int) {
        switch (u->components) {
        case 1: gl_Uniform1i(loc, (GLint)v[0]); break;
        case 2: gl_Uniform2i(loc, (GLint)v[0], (GLint)v[1]); break;
        case 3: gl_Uniform3i(loc, (GLint)v[0], (GLint)v[1], (GLint)v[2]); break;
        default: gl_Uniform4i(loc, (GLint)v[0], (GLint)v[1], (GLint)v[2], (GLint)v[3]); break;
        }
    } else {
        switch (u->components) {
        case 1: gl_Uniform1f(loc, v[0]); break;
        case 2: gl_Uniform2f(loc, v[0], v[1]); break;
        case 3: gl_Uniform3f(loc, v[0], v[1], v[2]); break;
        default: gl_Uniform4f(loc, v[0], v[1], v[2], v[3]); break;
        }
    }
}

static void w_ShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++)
        total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    char *joined = malloc(total + 1), *j = joined;
    for (GLsizei i = 0; i < count; i++) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(j, strings[i], n);
        j += n;
    }
    *j = 0;
    // HYPRISM_DUMP_SHADERS=<directory> writes every shader as it arrives, for debugging.
    const char *dump_dir = getenv("HYPRISM_DUMP_SHADERS");
    if (dump_dir) {
        static _Atomic int dumped;
        char path[512];
        snprintf(path, sizeof path, "%s/shader_%04d_%u.glsl", dump_dir, atomic_fetch_add(&dumped, 1), shader);
        FILE *f = fopen(path, "w");
        if (f) {
            fwrite(joined, 1, total, f);
            fclose(f);
        }
    }
    // HYPRISM_SHADER_REPLACE="<text the shader contains>|<old>|<new>" replaces
    // every <old> with <new> in matching shaders; "\\n" in <new> is a newline (diagnosis).
    const char *rep = getenv("HYPRISM_SHADER_REPLACE");
    const char *b1 = rep ? strchr(rep, '|') : NULL, *b2 = b1 ? strchr(b1 + 1, '|') : NULL;
    if (b2) {
        char needle[256], old[256], repl[512];
        snprintf(needle, sizeof needle, "%.*s", (int)(b1 - rep), rep);
        snprintf(old, sizeof old, "%.*s", (int)(b2 - b1 - 1), b1 + 1);
        size_t r = 0;
        for (const char *c = b2 + 1; *c && r < sizeof repl - 1; c++) {
            if (c[0] == '\\' && c[1] == 'n') { repl[r++] = '\n'; c++; }
            else repl[r++] = *c;
        }
        repl[r] = 0;
        if (needle[0] && old[0] && strstr(joined, needle) && strstr(joined, old)) {
            size_t ol = strlen(old), hits = 0;
            for (char *h = joined; (h = strstr(h, old)); h += ol)
                hits++;
            char *grown = malloc(total + hits * (r + 1) + 1), *g = grown;
            const char *cur = joined;
            for (char *h; (h = strstr(cur, old)); cur = h + ol) {
                memcpy(g, cur, (size_t)(h - cur));
                g += h - cur;
                memcpy(g, repl, r);
                g += r;
            }
            strcpy(g, cur);
            free(joined);
            joined = grown;
            total = strlen(joined);
            macho_log("gl", "shader %u: replaced %zu x '%s'", shader, hits, old);
        }
    }
    struct shader_info *info = shader_info(shader, 1);
    char *rewritten = rewrite_upper_left(joined);
    if (info) {
        info->flip = rewritten != NULL;
        collect_uniform_inits(info, joined);
    }
    if (rewritten)
        macho_log("gl", "shader %u: emulating an upper-left gl_FragCoord origin", shader);
    const GLchar *source = rewritten ? rewritten : joined;
    real_ShaderSource(shader, 1, &source, NULL);
    free(rewritten);
    free(joined);
}

// Reports shaders that fail to compile and programs that fail to link, with the driver's log.
static void w_CompileShader(GLuint shader)
{
    static _Atomic int n;
    real_CompileShader(shader);
    GLint ok = 1;
    gl_GetShaderiv(shader, 0x8B81, &ok); // GL_COMPILE_STATUS
    if (!ok && atomic_fetch_add(&n, 1) < 40) {
        char log[1024] = "";
        gl_GetShaderInfoLog(shader, sizeof log, NULL, log);
        macho_log("gl", "shader %u failed to compile: %s", shader, log);
    }
}

static void w_AttachShader(GLuint program, GLuint shader)
{
    real_AttachShader(program, shader);
    struct program_info *p = program_info(program, 1);
    if (p && p->shader_count < MAX_ATTACHED)
        p->shaders[p->shader_count++] = shader;
}

static void w_LinkProgram(GLuint program)
{
    real_LinkProgram(program);
    GLint linked = 1;
    gl_GetProgramiv(program, 0x8B82, &linked); // GL_LINK_STATUS
    if (!linked) {
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 40) {
            char log[1024] = "";
            gl_GetProgramInfoLog(program, sizeof log, NULL, log);
            macho_log("gl", "program %u failed to link: %s", program, log);
        }
    }
    struct program_info *p = program_info(program, 0);
    if (!p)
        return;
    int any_init = 0, flip = 0;
    for (int i = 0; i < p->shader_count; i++) {
        struct shader_info *s = shader_info(p->shaders[i], 0);
        any_init |= s && s->init_count;
        flip |= s && s->flip;
    }
    p->flip_location = flip ? gl_GetUniformLocation(program, "mg_FramebufferHeight") : -1;
    if (!any_init)
        return;
    real_UseProgram(program);
    for (int i = 0; i < p->shader_count; i++) {
        struct shader_info *s = shader_info(p->shaders[i], 0);
        for (int k = 0; s && k < s->init_count; k++) {
            GLint loc = gl_GetUniformLocation(program, s->inits[k].name);
            if (loc >= 0)
                apply_uniform_init(loc, &s->inits[k]);
        }
    }
    real_UseProgram(g_current_program);
}

static void w_UseProgram(GLuint program)
{
    real_UseProgram(program);
    g_current_program = program;
    set_flip_uniform();
}

static void w_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    real_Viewport(x, y, w, h);
    g_viewport_height = (float)(y + h);
    g_viewport_width = w;
    set_flip_uniform();
}

static const struct { const char *name; void **real; void *entry; } k_glsl_entries[] = {
    { "glShaderSource", (void **)&real_ShaderSource, (void *)w_ShaderSource },
    { "glCompileShader", (void **)&real_CompileShader, (void *)w_CompileShader },
    { "glAttachShader", (void **)&real_AttachShader, (void *)w_AttachShader },
    { "glLinkProgram", (void **)&real_LinkProgram, (void *)w_LinkProgram },
    { "glUseProgram", (void **)&real_UseProgram, (void *)w_UseProgram },
    { "glViewport", (void **)&real_Viewport, (void *)w_Viewport },
};

static void resolve_glsl_helpers(void *(*resolve)(const char *))
{
#define RESOLVE(fname, type) if (!gl_##fname) gl_##fname = (type)resolve("gl" #fname)
    RESOLVE(GetUniformLocation, GLint (*)(GLuint, const GLchar *));
    RESOLVE(GetShaderiv, void (*)(GLuint, GLenum, GLint *));
    RESOLVE(GetShaderInfoLog, void (*)(GLuint, GLsizei, GLsizei *, GLchar *));
    RESOLVE(GetProgramiv, void (*)(GLuint, GLenum, GLint *));
    RESOLVE(GetProgramInfoLog, void (*)(GLuint, GLsizei, GLsizei *, GLchar *));
    RESOLVE(Uniform1f, void (*)(GLint, float));
    RESOLVE(Uniform2f, void (*)(GLint, float, float));
    RESOLVE(Uniform3f, void (*)(GLint, float, float, float));
    RESOLVE(Uniform4f, void (*)(GLint, float, float, float, float));
    RESOLVE(Uniform1i, void (*)(GLint, GLint));
    RESOLVE(Uniform2i, void (*)(GLint, GLint, GLint));
    RESOLVE(Uniform3i, void (*)(GLint, GLint, GLint, GLint));
    RESOLVE(Uniform4i, void (*)(GLint, GLint, GLint, GLint, GLint));
#undef RESOLVE
}

// ---------------------------------------------------------------------------
// Tracing

static GLenum (*gl_GetError)(void);
static void (*gl_GetIntegerv)(GLenum, GLint *);
static void (*gl_TexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*gl_TexStorage3D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
static void (*gl_TexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum,
                                const void *);
static void (*gl_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*gl_TexParameteri)(GLenum, GLenum, GLint);
static void (*gl_GenerateMipmap)(GLenum);

static int gl_budget(_Atomic int *n) { return atomic_fetch_add(n, 1) < 40; }

// Share of non-zero bytes in an RGBA8 upload, sampled; -1 when there is no client data.
static int nonzero_percent(const void *pixels, size_t bytes)
{
    if (!pixels || !bytes)
        return -1;
    const uint8_t *p = pixels;
    size_t seen = 0, nonzero = 0;
    for (size_t i = 0; i < bytes && seen < 65536; i += 61, seen++)
        nonzero += p[i] != 0;
    return (int)(nonzero * 100 / seen);
}

static void gl_report(const char *call)
{
    GLenum e = gl_GetError ? gl_GetError() : 0;
    if (e)
        macho_log("gl", "  %s -> GL error %#x", call, e);
}

static void gl_caps_once(void)
{
    static _Atomic int done;
    if (atomic_exchange(&done, 1) || !gl_GetIntegerv)
        return;
    GLint size = 0, layers = 0, units = 0;
    gl_GetIntegerv(0x0D33, &size);   // GL_MAX_TEXTURE_SIZE
    gl_GetIntegerv(0x88FF, &layers); // GL_MAX_ARRAY_TEXTURE_LAYERS
    gl_GetIntegerv(0x8872, &units);  // GL_MAX_TEXTURE_IMAGE_UNITS
    macho_log("gl", "caps: max texture %d, max array layers %d, texture units %d", size, layers, units);
}

static void t_TexImage3D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLsizei d, GLint border,
                         GLenum format, GLenum type, const void *pixels)
{
    static _Atomic int n;
    gl_caps_once();
    gl_TexImage3D(target, level, internal, w, h, d, border, format, type, pixels);
    if (gl_budget(&n)) {
        macho_log("gl", "glTexImage3D(target %#x, level %d, internal %#x, %dx%dx%d, format %#x, type %#x, pixels %p, nonzero %d%%)",
                  target, level, internal, w, h, d, format, type, pixels,
                  nonzero_percent(pixels, (size_t)w * h * d * 4));
        gl_report("glTexImage3D");
    }
}

static void t_TexStorage3D(GLenum target, GLsizei levels, GLenum internal, GLsizei w, GLsizei h, GLsizei d)
{
    static _Atomic int n;
    gl_caps_once();
    gl_TexStorage3D(target, levels, internal, w, h, d);
    if (gl_budget(&n)) {
        macho_log("gl", "glTexStorage3D(target %#x, levels %d, internal %#x, %dx%dx%d)", target, levels, internal, w,
                  h, d);
        gl_report("glTexStorage3D");
    }
}

static void t_TexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,
                            GLenum format, GLenum type, const void *pixels)
{
    static _Atomic int n;
    gl_TexSubImage3D(target, level, x, y, z, w, h, d, format, type, pixels);
    if (atomic_fetch_add(&n, 1) < 400) {
        macho_log("gl", "glTexSubImage3D(target %#x, level %d, at %d,%d,%d, %dx%dx%d, format %#x, type %#x, pixels %p, nonzero %d%%)",
                  target, level, x, y, z, w, h, d, format, type, pixels,
                  nonzero_percent(pixels, (size_t)w * h * d * 4));
        gl_report("glTexSubImage3D");
    }
}

static void t_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format,
                         GLenum type, const void *pixels)
{
    static _Atomic int n;
    gl_TexImage2D(target, level, internal, w, h, border, format, type, pixels);
    if (gl_budget(&n)) {
        macho_log("gl", "glTexImage2D(target %#x, level %d, internal %#x, %dx%d, format %#x, type %#x, pixels %p)",
                  target, level, internal, w, h, format, type, pixels);
        gl_report("glTexImage2D");
    }
}

static void t_TexParameteri(GLenum target, GLenum pname, GLint param)
{
    static _Atomic int n;
    gl_TexParameteri(target, pname, param);
    if (target == 0x8C1A && gl_budget(&n)) { // GL_TEXTURE_2D_ARRAY only
        macho_log("gl", "glTexParameteri(2D_ARRAY, %#x, %#x)", pname, param);
        gl_report("glTexParameteri");
    }
}

static void t_GenerateMipmap(GLenum target)
{
    static _Atomic int n;
    gl_GenerateMipmap(target);
    if (gl_budget(&n)) {
        macho_log("gl", "glGenerateMipmap(%#x)", target);
        gl_report("glGenerateMipmap");
    }
}

static void (*gl_BindFramebuffer)(GLenum, GLuint);
static void (*gl_FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static void (*gl_FramebufferTexture)(GLenum, GLenum, GLuint, GLint);
static void (*gl_FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static void (*gl_RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
static void (*gl_RenderbufferStorageMultisample)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
static GLenum (*gl_CheckFramebufferStatus)(GLenum);
static void (*gl_DrawElements)(GLenum, GLsizei, GLenum, const void *);
static void (*gl_DrawArrays)(GLenum, GLint, GLsizei);
static void (*gl_DrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei);
static void (*gl_DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
static void (*gl_DrawElementsBaseVertex)(GLenum, GLsizei, GLenum, const void *, GLint);
static void (*gl_DrawElementsInstancedBaseVertex)(GLenum, GLsizei, GLenum, const void *, GLsizei, GLint);

static GLuint t_current_fb;

static void t_BindFramebuffer(GLenum target, GLuint fb)
{
    static _Atomic int n;
    gl_BindFramebuffer(target, fb);
    t_current_fb = fb;
    if (atomic_fetch_add(&n, 1) < 80) {
        macho_log("gl", "glBindFramebuffer(%#x, %u)", target, fb);
        gl_report("glBindFramebuffer");
    }
}

static void t_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint tex, GLint level)
{
    static _Atomic int n;
    gl_FramebufferTexture2D(target, attachment, textarget, tex, level);
    if (gl_budget(&n)) {
        macho_log("gl", "glFramebufferTexture2D(%#x, attachment %#x, %#x, tex %u, level %d) fb %u", target, attachment,
                  textarget, tex, level, t_current_fb);
        gl_report("glFramebufferTexture2D");
    }
}

static void t_FramebufferTexture(GLenum target, GLenum attachment, GLuint tex, GLint level)
{
    static _Atomic int n;
    gl_FramebufferTexture(target, attachment, tex, level);
    if (gl_budget(&n)) {
        macho_log("gl", "glFramebufferTexture(%#x, attachment %#x, tex %u, level %d) fb %u", target, attachment, tex,
                  level, t_current_fb);
        gl_report("glFramebufferTexture");
    }
}

static void t_FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb)
{
    static _Atomic int n;
    gl_FramebufferRenderbuffer(target, attachment, rbtarget, rb);
    if (gl_budget(&n)) {
        macho_log("gl", "glFramebufferRenderbuffer(%#x, attachment %#x, rb %u) fb %u", target, attachment, rb, t_current_fb);
        gl_report("glFramebufferRenderbuffer");
    }
}

static void t_RenderbufferStorage(GLenum target, GLenum internal, GLsizei w, GLsizei h)
{
    static _Atomic int n;
    gl_RenderbufferStorage(target, internal, w, h);
    if (gl_budget(&n)) {
        macho_log("gl", "glRenderbufferStorage(internal %#x, %dx%d)", internal, w, h);
        gl_report("glRenderbufferStorage");
    }
}

static void t_RenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internal, GLsizei w, GLsizei h)
{
    static _Atomic int n;
    gl_RenderbufferStorageMultisample(target, samples, internal, w, h);
    if (gl_budget(&n)) {
        macho_log("gl", "glRenderbufferStorageMultisample(%d samples, internal %#x, %dx%d)", samples, internal, w, h);
        gl_report("glRenderbufferStorageMultisample");
    }
}

static GLenum t_CheckFramebufferStatus(GLenum target)
{
    static _Atomic int n;
    GLenum status = gl_CheckFramebufferStatus(target);
    if (status != 0x8CD5 || gl_budget(&n)) // GL_FRAMEBUFFER_COMPLETE
        macho_log("gl", "glCheckFramebufferStatus(%#x) fb %u -> %#x", target, t_current_fb, status);
    return status;
}

// Draws log when they raise a GL error, and when they target a small offscreen
// viewport (such as the avatar preview).
static void draw_report(const char *call, GLenum mode, GLsizei count)
{
    static _Atomic int n, small;
    GLenum e = gl_GetError ? gl_GetError() : 0;
    if (e && atomic_fetch_add(&n, 1) < 60)
        macho_log("gl", "%s(mode %#x, count %d) fb %u -> GL error %#x", call, mode, count, t_current_fb, e);
    else if (t_current_fb && g_viewport_width && g_viewport_width < 600 && atomic_fetch_add(&small, 1) < 120)
        macho_log("gl", "%s(mode %#x, count %d) fb %u program %u viewport %dx%.0f", call, mode, count, t_current_fb,
                  g_current_program, g_viewport_width, g_viewport_height);
}

static void t_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices)
{
    gl_DrawElements(mode, count, type, indices);
    draw_report("glDrawElements", mode, count);
}

static void t_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
    gl_DrawArrays(mode, first, count);
    draw_report("glDrawArrays", mode, count);
}

static void t_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instances)
{
    gl_DrawElementsInstanced(mode, count, type, indices, instances);
    draw_report("glDrawElementsInstanced", mode, count);
}

static void t_DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances)
{
    gl_DrawArraysInstanced(mode, first, count, instances);
    draw_report("glDrawArraysInstanced", mode, count);
}

static void t_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base)
{
    gl_DrawElementsBaseVertex(mode, count, type, indices, base);
    draw_report("glDrawElementsBaseVertex", mode, count);
}

static void t_DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
                                               GLsizei instances, GLint base)
{
    gl_DrawElementsInstancedBaseVertex(mode, count, type, indices, instances, base);
    draw_report("glDrawElementsInstancedBaseVertex", mode, count);
}

static void *trace_entry(const char *name, void *fn, void *(*resolve)(const char *))
{
    static _Atomic int listed;
    if (atomic_fetch_add(&listed, 1) < 600)
        macho_log("gl", "resolved %s", name);
    if (!gl_GetError)
        gl_GetError = (GLenum (*)(void))resolve("glGetError");
    if (!gl_GetIntegerv)
        gl_GetIntegerv = (void (*)(GLenum, GLint *))resolve("glGetIntegerv");
#define TRACE(fname) if (strcmp(name, "gl" #fname) == 0) { gl_##fname = fn; return (void *)t_##fname; }
    TRACE(TexImage3D)
    TRACE(TexStorage3D)
    TRACE(TexSubImage3D)
    TRACE(TexImage2D)
    TRACE(TexParameteri)
    TRACE(GenerateMipmap)
    TRACE(BindFramebuffer)
    TRACE(FramebufferTexture2D)
    TRACE(FramebufferTexture)
    TRACE(FramebufferRenderbuffer)
    TRACE(RenderbufferStorage)
    TRACE(RenderbufferStorageMultisample)
    TRACE(CheckFramebufferStatus)
    TRACE(DrawElements)
    TRACE(DrawArrays)
    TRACE(DrawElementsInstanced)
    TRACE(DrawArraysInstanced)
    TRACE(DrawElementsBaseVertex)
    TRACE(DrawElementsInstancedBaseVertex)
#undef TRACE
    return fn;
}

/// Returns the entry point the Mach-O caller should use for the GL function `fn`
/// named `name` (without a leading underscore). `resolve` looks up other GL
/// functions for tracing.
void *gl_entry(const char *name, void *fn, void *(*resolve)(const char *))
{
    if (!fn || !name)
        return fn;
    static int trace = -1;
    if (trace < 0)
        trace = getenv("HYPRISM_GL_TRACE") != NULL;
    if (trace)
        fn = trace_entry(name, fn, resolve);
    for (size_t i = 0; i < SHIM_COUNT(k_glsl_entries); i++)
        if (strcmp(k_glsl_entries[i].name, name) == 0) {
            resolve_glsl_helpers(resolve);
            *k_glsl_entries[i].real = fn;
            return k_glsl_entries[i].entry;
        }
    for (size_t i = 0; i < SHIM_COUNT(k_stack_entries); i++)
        if (strcmp(k_stack_entries[i].name, name) == 0) {
            *k_stack_entries[i].real = fn;
            return (void *)k_stack_entries[i].entry;
        }
    return fn;
}
