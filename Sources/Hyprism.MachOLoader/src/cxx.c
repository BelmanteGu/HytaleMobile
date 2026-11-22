// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// The libc++ (std::__1) functions bundled Mach-O dylibs import from Apple's
// libc++.1.dylib. Apple arm64 uses the alternate std::string layout, which the
// Android libc++ does not, so the string members are implemented here on that
// layout:
//   short: char data[23]; uint8_t size;          (bit 7 of byte 23 clear)
//   long:  char *data; size_t size; size_t cap;  (cap = allocation | 1 << 63)

#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "macho.h"

struct apple_string {
    union {
        struct {
            char *data;
            size_t size;
            size_t cap; // allocation size | long flag
        } l;
        struct {
            char data[23];
            uint8_t size;
        } s;
    };
};

_Static_assert(sizeof(struct apple_string) == 24, "Apple std::string is 24 bytes");

#define LONG_FLAG (1ull << 63)
#define SHORT_CAPACITY 22

static int is_long(const struct apple_string *s) { return (s->s.size & 0x80) != 0; }
static size_t str_size(const struct apple_string *s) { return is_long(s) ? s->l.size : s->s.size; }
static char *str_data(struct apple_string *s) { return is_long(s) ? s->l.data : s->s.data; }
static const char *str_cdata(const struct apple_string *s) { return is_long(s) ? s->l.data : s->s.data; }
static size_t str_capacity(const struct apple_string *s)
{
    return is_long(s) ? (size_t)(s->l.cap & ~LONG_FLAG) - 1 : SHORT_CAPACITY;
}

static void set_size(struct apple_string *s, size_t n)
{
    if (is_long(s))
        s->l.size = n;
    else
        s->s.size = (uint8_t)n;
    str_data(s)[n] = 0;
}

// Grows the buffer so it holds at least n characters, keeping the contents.
static void str_reserve(struct apple_string *s, size_t n)
{
    if (n <= str_capacity(s))
        return;
    size_t size = str_size(s);
    size_t alloc = ((n + 1) + 15) & ~(size_t)15;
    char *buf = (char *)malloc(alloc);
    memcpy(buf, str_cdata(s), size + 1);
    if (is_long(s))
        free(s->l.data);
    s->l.data = buf;
    s->l.size = size;
    s->l.cap = alloc | LONG_FLAG;
}

static void str_replace(struct apple_string *s, size_t pos, size_t remove, const char *insert, size_t insert_len)
{
    size_t size = str_size(s);
    if (pos > size)
        pos = size;
    if (remove > size - pos)
        remove = size - pos;
    size_t new_size = size - remove + insert_len;
    char *tmp = NULL;
    if (insert >= str_cdata(s) && insert < str_cdata(s) + size + 1) {
        tmp = (char *)malloc(insert_len ? insert_len : 1);
        memcpy(tmp, insert, insert_len);
        insert = tmp;
    }
    str_reserve(s, new_size);
    char *d = str_data(s);
    memmove(d + pos + insert_len, d + pos + remove, size - pos - remove);
    if (insert_len)
        memcpy(d + pos, insert, insert_len);
    set_size(s, new_size);
    free(tmp);
}

#define NPOS ((size_t)-1)

// std::string member functions (the mangled names are bound in the table below)
static size_t s_find_char(const struct apple_string *s, char c, size_t pos)
{
    size_t size = str_size(s);
    const char *d = str_cdata(s);
    for (size_t i = pos; i < size; i++)
        if (d[i] == c)
            return i;
    return NPOS;
}

static size_t s_rfind_char(const struct apple_string *s, char c, size_t pos)
{
    size_t size = str_size(s);
    if (!size)
        return NPOS;
    const char *d = str_cdata(s);
    for (size_t i = pos < size ? pos : size - 1;; i--) {
        if (d[i] == c)
            return i;
        if (i == 0)
            return NPOS;
    }
}

static struct apple_string *s_erase(struct apple_string *s, size_t pos, size_t n) { str_replace(s, pos, n, "", 0); return s; }
static struct apple_string *s_append_cstr(struct apple_string *s, const char *p) { str_replace(s, str_size(s), 0, p, strlen(p)); return s; }
static struct apple_string *s_append_n(struct apple_string *s, const char *p, size_t n) { str_replace(s, str_size(s), 0, p, n); return s; }
static struct apple_string *s_append_sub(struct apple_string *s, const struct apple_string *o, size_t pos, size_t n)
{
    size_t osize = str_size(o);
    if (pos > osize)
        pos = osize;
    if (n > osize - pos)
        n = osize - pos;
    str_replace(s, str_size(s), 0, str_cdata(o) + pos, n);
    return s;
}
static struct apple_string *s_insert_sub(struct apple_string *s, size_t at, const struct apple_string *o, size_t pos, size_t n)
{
    size_t osize = str_size(o);
    if (pos > osize)
        pos = osize;
    if (n > osize - pos)
        n = osize - pos;
    str_replace(s, at, 0, str_cdata(o) + pos, n);
    return s;
}
static struct apple_string *s_insert_chars(struct apple_string *s, size_t at, size_t n, char c)
{
    char *tmp = (char *)malloc(n ? n : 1);
    memset(tmp, c, n);
    str_replace(s, at, 0, tmp, n);
    free(tmp);
    return s;
}
static void s_resize(struct apple_string *s, size_t n, char c)
{
    size_t size = str_size(s);
    if (n <= size) {
        set_size(s, n);
        return;
    }
    s_insert_chars(s, size, n - size, c);
}
static void s_reserve(struct apple_string *s, size_t n) { str_reserve(s, n); }
static void s_push_back(struct apple_string *s, char c) { str_replace(s, str_size(s), 0, &c, 1); }
static void s_destroy(struct apple_string *s)
{
    if (is_long(s))
        free(s->l.data);
}
static struct apple_string *s_assign(struct apple_string *s, const struct apple_string *o)
{
    if (s != o)
        str_replace(s, 0, str_size(s), str_cdata(o), str_size(o));
    return s;
}

static struct apple_string string_from(const char *p, size_t n)
{
    struct apple_string s;
    memset(&s, 0, sizeof s);
    str_replace(&s, 0, 0, p, n);
    return s;
}

static struct apple_string to_string_float(float v)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%f", v);
    return string_from(buf, (size_t)n);
}
static struct apple_string to_string_int(int v)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d", v);
    return string_from(buf, (size_t)n);
}
static struct apple_string to_string_uint(unsigned v)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%u", v);
    return string_from(buf, (size_t)n);
}

// ---------------------------------------------------------------------------
// Runtime support

static void *op_new(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        macho_log("cxx", "operator new(%zu) failed", n);
        abort();
    }
    return p;
}

static void op_delete(void *p) { free(p); }

static void pure_virtual(void)
{
    macho_log("cxx", "pure virtual function called");
    abort();
}

// std::mutex wraps a Darwin pthread_mutex_t; the pthread shims understand it.
extern void *shim_lookup(const char *name, const char *library);
static int (*g_mutex_lock)(void *);
static int (*g_mutex_unlock)(void *);
static void mutex_lock(void *m)
{
    if (!g_mutex_lock)
        g_mutex_lock = (int (*)(void *))shim_lookup("_pthread_mutex_lock", NULL);
    g_mutex_lock(m);
}
static void mutex_unlock(void *m)
{
    if (!g_mutex_unlock)
        g_mutex_unlock = (int (*)(void *))shim_lookup("_pthread_mutex_unlock", NULL);
    g_mutex_unlock(m);
}
static void mutex_destroy(void *m) { (void)m; }

// std::chrono::steady_clock::now() returns nanoseconds as a 64-bit duration.
static int64_t steady_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static size_t next_prime(size_t n)
{
    if (n <= 2)
        return 2;
    for (size_t c = n | 1;; c += 2) {
        int prime = 1;
        for (size_t d = 3; d * d <= c; d += 2)
            if (c % d == 0) {
                prime = 0;
                break;
            }
        if (prime)
            return c;
    }
}

static int float_less(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void sort_floats(float *first, float *last, void *less)
{
    (void)less;
    qsort(first, (size_t)(last - first), sizeof *first, float_less);
}

#define BS "NSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE"

const struct shim cxx_shims[] = {
    { "__ZNK" BS "4findEcm", (void *)s_find_char },
    { "__ZNK" BS "5rfindEcm", (void *)s_rfind_char },
    { "__Z" BS "5eraseEmm", (void *)s_erase },
    { "__Z" BS "6appendEPKc", (void *)s_append_cstr },
    { "__Z" BS "6appendEPKcm", (void *)s_append_n },
    { "__Z" BS "6appendERKS5_mm", (void *)s_append_sub },
    { "__Z" BS "6insertEmRKS5_mm", (void *)s_insert_sub },
    { "__Z" BS "6insertEmmc", (void *)s_insert_chars },
    { "__Z" BS "6resizeEmc", (void *)s_resize },
    { "__Z" BS "7reserveEm", (void *)s_reserve },
    { "__Z" BS "9push_backEc", (void *)s_push_back },
    { "__Z" BS "D1Ev", (void *)s_destroy },
    { "__Z" BS "aSERKS5_", (void *)s_assign },
    { "__ZNSt3__19to_stringEf", (void *)to_string_float },
    { "__ZNSt3__19to_stringEi", (void *)to_string_int },
    { "__ZNSt3__19to_stringEj", (void *)to_string_uint },
    { "__Znwm", (void *)op_new },
    { "__Znam", (void *)op_new },
    { "__ZdlPv", (void *)op_delete },
    { "__ZdaPv", (void *)op_delete },
    { "__ZdlPvm", (void *)op_delete },
    { "___cxa_pure_virtual", (void *)pure_virtual },
    { "__ZNSt3__15mutex4lockEv", (void *)mutex_lock },
    { "__ZNSt3__15mutex6unlockEv", (void *)mutex_unlock },
    { "__ZNSt3__15mutexD1Ev", (void *)mutex_destroy },
    { "__ZNSt3__16chrono12steady_clock3nowEv", (void *)steady_now },
    { "__ZNSt3__112__next_primeEm", (void *)next_prime },
    { "__ZNSt3__16__sortIRNS_6__lessIffEEPfEEvT0_S5_T_", (void *)sort_floats },
};
const size_t cxx_shims_count = SHIM_COUNT(cxx_shims);
