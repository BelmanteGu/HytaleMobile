// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Minimal CoreFoundation: strings, data, arrays, dictionaries, numbers,
// booleans and errors, sharing the object model in objc.h.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macho.h"
#include "objc.h"

struct objc_class_stub g_constant_string_class = { &g_constant_string_class, "__NSCFConstantString", CF_TYPE_STRING };

static struct objc_class_stub g_boolean_class = { &g_boolean_class, "__NSCFBoolean", CF_TYPE_BOOLEAN };
struct cf_object cf_boolean_true = { &g_boolean_class, 1, 0 };
struct cf_object cf_boolean_false = { &g_boolean_class, 1, 0 };

// ---------------------------------------------------------------------------
// Lifetime

void *cf_alloc(const char *class_name, size_t size)
{
    struct cf_object *o = calloc(1, size);
    o->isa = objc_class_named(class_name);
    o->refcount = 1;
    return o;
}

static int immortal(const struct cf_object *o)
{
    return o->isa == &g_constant_string_class || o->isa == &g_boolean_class;
}

void *cf_retain(void *p)
{
    struct cf_object *o = p;
    if (o && !immortal(o))
        atomic_fetch_add(&o->refcount, 1);
    return o;
}

static void cf_destroy(struct cf_object *o)
{
    switch (cf_type_of(o)) {
    case CF_TYPE_STRING: free(((struct cf_string *)o)->utf8); break;
    case CF_TYPE_DATA:
        if (((struct cf_data *)o)->owns)
            free(((struct cf_data *)o)->bytes);
        break;
    case CF_TYPE_ARRAY: {
        struct cf_array *a = (struct cf_array *)o;
        for (size_t i = 0; i < a->count; i++)
            cf_release(a->items[i]);
        free(a->items);
        break;
    }
    case CF_TYPE_DICTIONARY: {
        struct cf_dictionary *d = (struct cf_dictionary *)o;
        for (size_t i = 0; i < d->count; i++) {
            cf_release(d->keys[i]);
            cf_release(d->values[i]);
        }
        free(d->keys);
        free(d->values);
        break;
    }
    case CF_TYPE_ERROR: {
        struct cf_error *e = (struct cf_error *)o;
        cf_release(e->domain);
        cf_release(e->description);
        break;
    }
    default:
        if (cf_type_of(o) >= CF_TYPE_SEC_CERTIFICATE)
            security_destroy(o);
        break;
    }
    free(o);
}

void cf_release(void *p)
{
    struct cf_object *o = p;
    if (!o || immortal(o))
        return;
    if (atomic_fetch_sub(&o->refcount, 1) == 1)
        cf_destroy(o);
}

uint32_t cf_type_of(const void *p)
{
    const struct cf_object *o = p;
    return o && o->isa ? o->isa->cf_type : CF_TYPE_NONE;
}

// ---------------------------------------------------------------------------
// Strings

#define kCFStringEncodingMacRoman 0
#define kCFStringEncodingASCII 0x0600
#define kCFStringEncodingUTF8 0x08000100
#define kCFStringEncodingUnicode 0x0100
#define kCFStringEncodingUTF16BE 0x10000100
#define kCFStringEncodingUTF16LE 0x14000100

struct cf_string *cf_string_from_utf8(const char *s, size_t len)
{
    struct cf_string *str = cf_alloc("__NSCFString", sizeof *str);
    str->utf8 = malloc(len + 1);
    memcpy(str->utf8, s, len);
    str->utf8[len] = 0;
    str->length = len;
    return str;
}

struct cf_string *cf_string_from_cstr(const char *s) { return cf_string_from_utf8(s ? s : "", s ? strlen(s) : 0); }

static size_t utf16_to_utf8(const uint16_t *in, size_t n, int big_endian, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = big_endian ? (uint32_t)(in[i] >> 8 | (in[i] & 0xff) << 8) : in[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t lo = big_endian ? (uint32_t)(in[i + 1] >> 8 | (in[i + 1] & 0xff) << 8) : in[i + 1];
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (c < 0x80) {
            if (out) out[o] = (char)c;
            o += 1;
        } else if (c < 0x800) {
            if (out) { out[o] = (char)(0xC0 | c >> 6); out[o + 1] = (char)(0x80 | (c & 0x3F)); }
            o += 2;
        } else if (c < 0x10000) {
            if (out) { out[o] = (char)(0xE0 | c >> 12); out[o + 1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o + 2] = (char)(0x80 | (c & 0x3F)); }
            o += 3;
        } else {
            if (out) {
                out[o] = (char)(0xF0 | c >> 18); out[o + 1] = (char)(0x80 | ((c >> 12) & 0x3F));
                out[o + 2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o + 3] = (char)(0x80 | (c & 0x3F));
            }
            o += 4;
        }
    }
    return o;
}

// Decodes UTF-8 into UTF-16 units; returns the unit count. out may be NULL.
static size_t utf8_to_utf16(const char *s, size_t len, uint16_t *out)
{
    const uint8_t *p = (const uint8_t *)s;
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        uint32_t c = p[i];
        size_t extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        if (extra) {
            c &= 0x3F >> extra;
            for (size_t k = 1; k <= extra && i + k < len; k++)
                c = c << 6 | (p[i + k] & 0x3F);
        }
        i += extra + 1;
        if (c >= 0x10000) {
            if (out) {
                out[o] = (uint16_t)(0xD800 + ((c - 0x10000) >> 10));
                out[o + 1] = (uint16_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
            }
            o += 2;
        } else {
            if (out)
                out[o] = (uint16_t)c;
            o += 1;
        }
    }
    return o;
}

static struct cf_string *string_from_encoding(const uint8_t *bytes, size_t n, uint32_t encoding)
{
    if (encoding == kCFStringEncodingUnicode || encoding == kCFStringEncodingUTF16LE ||
        encoding == kCFStringEncodingUTF16BE) {
        int be = encoding == kCFStringEncodingUTF16BE;
        const uint16_t *u = (const uint16_t *)bytes;
        size_t units = n / 2;
        if (encoding == kCFStringEncodingUnicode && units && (u[0] == 0xFEFF || u[0] == 0xFFFE)) {
            be = u[0] == 0xFFFE;
            u++;
            units--;
        }
        size_t len = utf16_to_utf8(u, units, be, NULL);
        char *buf = malloc(len + 1);
        utf16_to_utf8(u, units, be, buf);
        struct cf_string *s = cf_string_from_utf8(buf, len);
        free(buf);
        return s;
    }
    return cf_string_from_utf8((const char *)bytes, n); // UTF-8, ASCII and MacRoman ASCII subset
}

static struct cf_string *CFStringCreateWithBytes(void *alloc, const uint8_t *bytes, long n, uint32_t encoding, bool ext)
{
    (void)alloc; (void)ext;
    return string_from_encoding(bytes, (size_t)n, encoding);
}

static struct cf_string *CFStringCreateWithCString(void *alloc, const char *s, uint32_t encoding)
{
    (void)alloc; (void)encoding;
    return cf_string_from_cstr(s);
}

static struct cf_string *CFStringCreateWithCharacters(void *alloc, const uint16_t *chars, long n)
{
    (void)alloc;
    return string_from_encoding((const uint8_t *)chars, (size_t)n * 2, kCFStringEncodingUTF16LE);
}

static const char *CFStringGetCStringPtr(struct cf_string *s, uint32_t encoding)
{
    if (!s || (encoding != kCFStringEncodingUTF8 && encoding != kCFStringEncodingASCII &&
               encoding != kCFStringEncodingMacRoman))
        return NULL;
    return s->utf8;
}

static bool CFStringGetCString(struct cf_string *s, char *buf, long size, uint32_t encoding)
{
    (void)encoding;
    if (!s || size <= 0 || (long)s->length >= size)
        return false;
    memcpy(buf, s->utf8, s->length + 1);
    return true;
}

static long CFStringGetLength(struct cf_string *s) { return s ? (long)utf8_to_utf16(s->utf8, s->length, NULL) : 0; }

static void CFStringGetCharacters(struct cf_string *s, long location, long length, uint16_t *buffer)
{
    size_t total = utf8_to_utf16(s->utf8, s->length, NULL);
    uint16_t *all = malloc((total + 1) * sizeof *all);
    utf8_to_utf16(s->utf8, s->length, all);
    if (location + length <= (long)total)
        memcpy(buffer, all + location, (size_t)length * sizeof *all);
    free(all);
}

static long CFStringGetMaximumSizeForEncoding(long length, uint32_t encoding)
{
    (void)encoding;
    return length * 4;
}

static struct cf_data *CFStringCreateExternalRepresentation(void *alloc, struct cf_string *s, uint32_t encoding, uint8_t loss)
{
    (void)alloc; (void)loss;
    if (encoding == kCFStringEncodingUnicode || encoding == kCFStringEncodingUTF16LE) {
        size_t units = utf8_to_utf16(s->utf8, s->length, NULL);
        uint16_t *u = malloc(units * 2 + 2);
        utf8_to_utf16(s->utf8, s->length, u);
        struct cf_data *d = cf_data_create(u, units * 2);
        free(u);
        return d;
    }
    return cf_data_create(s->utf8, s->length);
}

static uint32_t CFStringGetTypeID(void) { return CF_TYPE_STRING; }

// ---------------------------------------------------------------------------
// Data

struct cf_data *cf_data_create(const void *bytes, size_t len)
{
    struct cf_data *d = cf_alloc("__NSCFData", sizeof *d);
    d->bytes = malloc(len ? len : 1);
    if (len)
        memcpy(d->bytes, bytes, len);
    d->length = len;
    d->owns = 1;
    return d;
}

static struct cf_data *CFDataCreate(void *alloc, const uint8_t *bytes, long len) { (void)alloc; return cf_data_create(bytes, (size_t)len); }

static struct cf_data *CFDataCreateWithBytesNoCopy(void *alloc, const uint8_t *bytes, long len, void *deallocator)
{
    (void)alloc; (void)deallocator;
    struct cf_data *d = cf_alloc("__NSCFData", sizeof *d);
    d->bytes = (uint8_t *)bytes;
    d->length = (size_t)len;
    d->owns = 0;
    return d;
}

static long CFDataGetLength(struct cf_data *d) { return d ? (long)d->length : 0; }
static const uint8_t *CFDataGetBytePtr(struct cf_data *d) { return d ? d->bytes : NULL; }
static void CFDataGetBytes(struct cf_data *d, long location, long length, uint8_t *buffer)
{
    memcpy(buffer, d->bytes + location, (size_t)length);
}
static uint32_t CFDataGetTypeID(void) { return CF_TYPE_DATA; }

// ---------------------------------------------------------------------------
// Arrays

struct cf_array *cf_array_create(void) { return cf_alloc("__NSCFArray", sizeof(struct cf_array)); }

void cf_array_append(struct cf_array *a, void *item)
{
    if (a->count == a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 8;
        a->items = realloc(a->items, a->capacity * sizeof *a->items);
    }
    a->items[a->count++] = cf_retain(item);
}

static struct cf_array *CFArrayCreate(void *alloc, void **values, long n, const void *callbacks)
{
    (void)alloc; (void)callbacks;
    struct cf_array *a = cf_array_create();
    for (long i = 0; i < n; i++)
        cf_array_append(a, values[i]);
    return a;
}

static struct cf_array *CFArrayCreateMutable(void *alloc, long capacity, const void *callbacks)
{
    (void)alloc; (void)capacity; (void)callbacks;
    return cf_array_create();
}

static void CFArrayAppendValue(struct cf_array *a, void *v) { cf_array_append(a, v); }
static long CFArrayGetCount(struct cf_array *a) { return a ? (long)a->count : 0; }
static void *CFArrayGetValueAtIndex(struct cf_array *a, long i) { return a && i >= 0 && (size_t)i < a->count ? a->items[i] : NULL; }
static uint32_t CFArrayGetTypeID(void) { return CF_TYPE_ARRAY; }

// ---------------------------------------------------------------------------
// Dictionaries

static long dict_find(struct cf_dictionary *d, const void *key)
{
    for (size_t i = 0; i < d->count; i++)
        if (cf_equal(d->keys[i], key))
            return (long)i;
    return -1;
}

static struct cf_dictionary *CFDictionaryCreateMutable(void *alloc, long capacity, const void *kcb, const void *vcb)
{
    (void)alloc; (void)capacity; (void)kcb; (void)vcb;
    return cf_alloc("__NSCFDictionary", sizeof(struct cf_dictionary));
}

static void CFDictionarySetValue(struct cf_dictionary *d, void *key, void *value)
{
    long i = dict_find(d, key);
    if (i >= 0) {
        void *old = d->values[i];
        d->values[i] = cf_retain(value);
        cf_release(old);
        return;
    }
    if (d->count == d->capacity) {
        d->capacity = d->capacity ? d->capacity * 2 : 8;
        d->keys = realloc(d->keys, d->capacity * sizeof *d->keys);
        d->values = realloc(d->values, d->capacity * sizeof *d->values);
    }
    d->keys[d->count] = cf_retain(key);
    d->values[d->count] = cf_retain(value);
    d->count++;
}

struct cf_dictionary *cf_dictionary_create(void) { return CFDictionaryCreateMutable(NULL, 0, NULL, NULL); }
void cf_dictionary_set(struct cf_dictionary *d, void *key, void *value) { CFDictionarySetValue(d, key, value); }

static void CFDictionaryAddValue(struct cf_dictionary *d, void *key, void *value)
{
    if (dict_find(d, key) < 0)
        CFDictionarySetValue(d, key, value);
}

static void *CFDictionaryGetValue(struct cf_dictionary *d, const void *key)
{
    if (!d)
        return NULL;
    long i = dict_find(d, key);
    return i >= 0 ? d->values[i] : NULL;
}

void *cf_dictionary_get(struct cf_dictionary *d, const void *key) { return CFDictionaryGetValue(d, key); }

static long CFDictionaryGetCount(struct cf_dictionary *d) { return d ? (long)d->count : 0; }

static void CFDictionaryApplyFunction(struct cf_dictionary *d, void (*fn)(const void *, const void *, void *), void *ctx)
{
    for (size_t i = 0; d && i < d->count; i++)
        fn(d->keys[i], d->values[i], ctx);
}

static uint32_t CFDictionaryGetTypeID(void) { return CF_TYPE_DICTIONARY; }

// ---------------------------------------------------------------------------
// Dates

static struct cf_date *CFDateCreate(void *alloc, double at)
{
    (void)alloc;
    struct cf_date *d = cf_alloc("__NSCFDate", sizeof *d);
    d->absolute_time = at;
    return d;
}

static double CFDateGetAbsoluteTime(struct cf_date *d) { return d ? d->absolute_time : 0; }
static uint32_t CFDateGetTypeID(void) { return CF_TYPE_DATE; }

// ---------------------------------------------------------------------------
// Numbers and booleans

struct cf_number *cf_number_from_int(int64_t v)
{
    struct cf_number *n = cf_alloc("__NSCFNumber", sizeof *n);
    n->i = v;
    n->d = (double)v;
    return n;
}

static struct cf_number *CFNumberCreate(void *alloc, long type, const void *value)
{
    (void)alloc;
    struct cf_number *n = cf_alloc("__NSCFNumber", sizeof *n);
    switch (type) {
    case 1: case 7: n->i = *(const int8_t *)value; break;
    case 2: case 8: n->i = *(const int16_t *)value; break;
    case 3: case 9: n->i = *(const int32_t *)value; break;
    case 5: case 12: n->is_float = 1; n->d = *(const float *)value; break;
    case 6: case 13: case 16: n->is_float = 1; n->d = *(const double *)value; break;
    default: n->i = *(const int64_t *)value; break;
    }
    if (n->is_float)
        n->i = (int64_t)n->d;
    else
        n->d = (double)n->i;
    return n;
}

static bool CFNumberGetValue(struct cf_number *n, long type, void *out)
{
    if (!n)
        return false;
    switch (type) {
    case 1: case 7: *(int8_t *)out = (int8_t)n->i; break;
    case 2: case 8: *(int16_t *)out = (int16_t)n->i; break;
    case 3: case 9: *(int32_t *)out = (int32_t)n->i; break;
    case 5: case 12: *(float *)out = (float)n->d; break;
    case 6: case 13: case 16: *(double *)out = n->d; break;
    default: *(int64_t *)out = n->i; break;
    }
    return true;
}

static uint32_t CFNumberGetTypeID(void) { return CF_TYPE_NUMBER; }
static uint32_t CFBooleanGetTypeID(void) { return CF_TYPE_BOOLEAN; }
static bool CFBooleanGetValue(const struct cf_object *b) { return b == &cf_boolean_true; }

// ---------------------------------------------------------------------------
// Errors

struct cf_error *cf_error_create(const char *domain, long code, const char *description)
{
    struct cf_error *e = cf_alloc("__NSCFError", sizeof *e);
    e->domain = cf_string_from_cstr(domain);
    e->code = code;
    e->description = cf_string_from_cstr(description);
    return e;
}

static struct cf_string *CFErrorCopyDescription(struct cf_error *e) { return e ? cf_retain(e->description) : NULL; }
static long CFErrorGetCode(struct cf_error *e) { return e ? e->code : 0; }
static struct cf_string *CFErrorGetDomain(struct cf_error *e) { return e ? e->domain : NULL; }
static uint32_t CFErrorGetTypeID(void) { return CF_TYPE_ERROR; }

// ---------------------------------------------------------------------------
// Generic

int cf_equal(const void *a, const void *b)
{
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    uint32_t ta = cf_type_of(a), tb = cf_type_of(b);
    if (ta != tb)
        return 0;
    if (ta == CF_TYPE_STRING) {
        const struct cf_string *x = a, *y = b;
        return x->length == y->length && memcmp(x->utf8, y->utf8, x->length) == 0;
    }
    if (ta == CF_TYPE_NUMBER)
        return ((const struct cf_number *)a)->d == ((const struct cf_number *)b)->d;
    if (ta == CF_TYPE_DATA) {
        const struct cf_data *x = a, *y = b;
        return x->length == y->length && memcmp(x->bytes, y->bytes, x->length) == 0;
    }
    return 0;
}

static bool CFEqual(const void *a, const void *b) { return cf_equal(a, b); }
static uint32_t CFGetTypeID(const void *o) { return cf_type_of(o); }
static void *CFRetain(void *o) { return cf_retain(o); }
static void CFRelease(void *o) { cf_release(o); }
static long CFGetRetainCount(const struct cf_object *o) { return o ? o->refcount : 0; }

static unsigned long CFHash(const void *o)
{
    if (cf_type_of(o) == CF_TYPE_STRING) {
        const struct cf_string *s = o;
        unsigned long h = 5381;
        for (size_t i = 0; i < s->length; i++)
            h = h * 33 + (uint8_t)s->utf8[i];
        return h;
    }
    return (unsigned long)(uintptr_t)o;
}

// Allocators and callback tables are only identified by address.
static uint8_t kCFAllocatorDefault_[16], kCFAllocatorNull_[16];
static uint8_t kCFTypeArrayCallBacks_[48], kCFTypeDictionaryKeyCallBacks_[56], kCFTypeDictionaryValueCallBacks_[48];
static void *g_alloc_default = kCFAllocatorDefault_, *g_alloc_null = kCFAllocatorNull_;
static void *g_true = &cf_boolean_true, *g_false = &cf_boolean_false;

#define F(name) { "_" #name, (void *)name }
const struct shim cf_shims[] = {
    F(CFStringCreateWithBytes), F(CFStringCreateWithCString), F(CFStringCreateWithCharacters),
    F(CFStringGetCStringPtr), F(CFStringGetCString), F(CFStringGetLength), F(CFStringGetCharacters),
    F(CFStringGetMaximumSizeForEncoding), F(CFStringCreateExternalRepresentation), F(CFStringGetTypeID),
    F(CFDataCreate), F(CFDataCreateWithBytesNoCopy), F(CFDataGetLength), F(CFDataGetBytePtr), F(CFDataGetBytes),
    F(CFDataGetTypeID), F(CFArrayCreate), F(CFArrayCreateMutable), F(CFArrayAppendValue), F(CFArrayGetCount),
    F(CFArrayGetValueAtIndex), F(CFArrayGetTypeID), F(CFDictionaryCreateMutable), F(CFDictionarySetValue),
    F(CFDictionaryAddValue), F(CFDictionaryGetValue), F(CFDictionaryGetCount), F(CFDictionaryApplyFunction),
    F(CFDictionaryGetTypeID), F(CFNumberCreate), F(CFNumberGetValue), F(CFNumberGetTypeID), F(CFBooleanGetTypeID),
    F(CFBooleanGetValue), F(CFErrorCopyDescription), F(CFErrorGetCode), F(CFErrorGetDomain), F(CFErrorGetTypeID),
    F(CFDateCreate), F(CFDateGetAbsoluteTime), F(CFDateGetTypeID),
    F(CFEqual), F(CFGetTypeID), F(CFRetain), F(CFRelease), F(CFGetRetainCount), F(CFHash),
    { "___CFConstantStringClassReference", &g_constant_string_class },
    { "_kCFAllocatorDefault", &g_alloc_default }, { "_kCFAllocatorNull", &g_alloc_null },
    { "_kCFBooleanTrue", &g_true }, { "_kCFBooleanFalse", &g_false },
    { "_kCFTypeArrayCallBacks", kCFTypeArrayCallBacks_ },
    { "_kCFTypeDictionaryKeyCallBacks", kCFTypeDictionaryKeyCallBacks_ },
    { "_kCFTypeDictionaryValueCallBacks", kCFTypeDictionaryValueCallBacks_ },
};
const size_t cf_shims_count = SHIM_COUNT(cf_shims);
