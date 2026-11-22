// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Shared object model for the Objective-C runtime and CoreFoundation shims.
// Every object starts with an isa pointer so it can be messaged, and Core
// Foundation objects are the same structures (toll-free bridging).

#pragma once

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

struct objc_class_stub {
    struct objc_class_stub *isa; // metaclass placeholder (points to itself)
    const char *name;
    uint32_t cf_type;            // CFTypeID of instances, 0 when not a CF type
};

enum cf_type {
    CF_TYPE_NONE = 0,
    CF_TYPE_STRING = 7,
    CF_TYPE_DATA = 20,
    CF_TYPE_ARRAY = 19,
    CF_TYPE_DICTIONARY = 18,
    CF_TYPE_NUMBER = 22,
    CF_TYPE_BOOLEAN = 21,
    CF_TYPE_ERROR = 30,
    CF_TYPE_DATE = 42,
    CF_TYPE_OBJECT = 100, // plain Objective-C object (bundle, process info, ...)
    // Security objects (security.c); cf_destroy hands them to security_destroy.
    CF_TYPE_SEC_CERTIFICATE = 300,
    CF_TYPE_SEC_IDENTITY,
    CF_TYPE_SEC_TRUST,
    CF_TYPE_SEC_POLICY,
    CF_TYPE_SEC_KEY,
    CF_TYPE_SSL_CONTEXT,
    CF_TYPE_SEC_KEYCHAIN,
};

struct cf_object {
    struct objc_class_stub *isa;
    _Atomic int32_t refcount; // overlaps the flags word of compile-time constants
    int32_t reserved;
};

// Same layout as the CFString constants ld64 emits in __cfstring:
// {isa = __CFConstantStringClassReference, flags, const char *str, long length}.
struct cf_string {
    struct cf_object base;
    char *utf8;    // NUL-terminated
    size_t length; // bytes
};

_Static_assert(offsetof(struct cf_string, utf8) == 16 && offsetof(struct cf_string, length) == 24,
               "struct cf_string must match __cfstring constants");

/// Class of the CFString constants compiled into the program; never freed.
extern struct objc_class_stub g_constant_string_class;

struct cf_data {
    struct cf_object base;
    size_t length;
    uint8_t *bytes;
    int owns;
};

struct cf_array {
    struct cf_object base;
    size_t count, capacity;
    void **items;
};

struct cf_dictionary {
    struct cf_object base;
    size_t count, capacity;
    void **keys;
    void **values;
};

struct cf_number {
    struct cf_object base;
    int is_float;
    int64_t i;
    double d;
};

struct cf_error {
    struct cf_object base;
    struct cf_string *domain;
    long code;
    struct cf_string *description;
};

struct cf_date {
    struct cf_object base;
    double absolute_time; // seconds since 2001-01-01 UTC
};

struct objc_class_stub *objc_class_named(const char *name);
void *cf_alloc(const char *class_name, size_t size);
void *cf_retain(void *o);
void cf_release(void *o);
uint32_t cf_type_of(const void *o);

struct cf_string *cf_string_from_utf8(const char *s, size_t len);
struct cf_string *cf_string_from_cstr(const char *s);
struct cf_data *cf_data_create(const void *bytes, size_t len);
struct cf_array *cf_array_create(void);
struct cf_dictionary *cf_dictionary_create(void);
void cf_dictionary_set(struct cf_dictionary *d, void *key, void *value);
void *cf_dictionary_get(struct cf_dictionary *d, const void *key);
void security_destroy(struct cf_object *o);
void cf_array_append(struct cf_array *a, void *item);
struct cf_number *cf_number_from_int(int64_t v);
struct cf_error *cf_error_create(const char *domain, long code, const char *description);
int cf_equal(const void *a, const void *b);

extern struct cf_object cf_boolean_true, cf_boolean_false;
