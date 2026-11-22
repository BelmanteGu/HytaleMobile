// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// A small Objective-C runtime. Classes are named stubs, selectors are interned
// strings and objc_msgSend dispatches on (class, selector) to C functions.
// Unknown messages log once and behave like messages to nil (return 0).

#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include "macho.h"
#include "objc.h"

typedef void *id;
typedef const char *SEL;

extern const char *g_executable_path;

// ---------------------------------------------------------------------------
// Classes

#define MAX_CLASSES 256
static struct objc_class_stub g_classes[MAX_CLASSES];
static size_t g_class_count;
static pthread_mutex_t g_class_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t cf_type_for_class(const char *name)
{
    static const struct { const char *name; uint32_t type; } k_security[] = {
        { "SecCertificate", CF_TYPE_SEC_CERTIFICATE }, { "SecIdentity", CF_TYPE_SEC_IDENTITY },
        { "SecTrust", CF_TYPE_SEC_TRUST }, { "SecPolicy", CF_TYPE_SEC_POLICY }, { "SecKey", CF_TYPE_SEC_KEY },
        { "SSLContext", CF_TYPE_SSL_CONTEXT }, { "SecKeychain", CF_TYPE_SEC_KEYCHAIN },
    };
    for (size_t i = 0; i < sizeof k_security / sizeof k_security[0]; i++)
        if (strcmp(name, k_security[i].name) == 0)
            return k_security[i].type;
    if (strstr(name, "Date"))
        return CF_TYPE_DATE;
    if (strstr(name, "String"))
        return CF_TYPE_STRING;
    if (strstr(name, "Data"))
        return CF_TYPE_DATA;
    if (strstr(name, "Array"))
        return CF_TYPE_ARRAY;
    if (strstr(name, "Dictionary"))
        return CF_TYPE_DICTIONARY;
    if (strstr(name, "Number"))
        return CF_TYPE_NUMBER;
    if (strstr(name, "Error"))
        return CF_TYPE_ERROR;
    return CF_TYPE_OBJECT;
}

struct objc_class_stub *objc_class_named(const char *name)
{
    pthread_mutex_lock(&g_class_lock);
    for (size_t i = 0; i < g_class_count; i++) {
        if (strcmp(g_classes[i].name, name) == 0) {
            pthread_mutex_unlock(&g_class_lock);
            return &g_classes[i];
        }
    }
    struct objc_class_stub *c = NULL;
    if (g_class_count < MAX_CLASSES) {
        c = &g_classes[g_class_count++];
        c->isa = c;
        c->name = strdup(name);
        c->cf_type = cf_type_for_class(name);
    }
    pthread_mutex_unlock(&g_class_lock);
    return c;
}

static int is_class(const void *p)
{
    return (p >= (const void *)g_classes && p < (const void *)(g_classes + MAX_CLASSES)) ||
           p == &g_constant_string_class;
}

static const char *class_name_of(id self)
{
    if (!self)
        return "nil";
    struct objc_class_stub *c = is_class(self) ? self : ((struct cf_object *)self)->isa;
    return c && c->name ? c->name : "?";
}

// Collapses concrete CF class names onto the Foundation class used for lookups.
static const char *family(const char *name)
{
    if (strstr(name, "String"))
        return "NSString";
    if (strstr(name, "Array"))
        return "NSArray";
    if (strstr(name, "Dictionary"))
        return "NSDictionary";
    if (strstr(name, "Number") || strstr(name, "Boolean"))
        return "NSNumber";
    if (strstr(name, "Data"))
        return "NSData";
    if (strstr(name, "Error"))
        return "NSError";
    return name;
}

// ---------------------------------------------------------------------------
// Selectors

static const char *g_selectors[4096];
static size_t g_selector_count;
static pthread_mutex_t g_selector_lock = PTHREAD_MUTEX_INITIALIZER;

static SEL sel_registerName(const char *name)
{
    pthread_mutex_lock(&g_selector_lock);
    for (size_t i = 0; i < g_selector_count; i++) {
        if (strcmp(g_selectors[i], name) == 0) {
            pthread_mutex_unlock(&g_selector_lock);
            return g_selectors[i];
        }
    }
    const char *s = strdup(name);
    if (g_selector_count < SHIM_COUNT(g_selectors))
        g_selectors[g_selector_count++] = s;
    pthread_mutex_unlock(&g_selector_lock);
    return s;
}

static const char *sel_getName(SEL s) { return s; }

// ---------------------------------------------------------------------------
// Foundation objects

static struct cf_string *nsstring(const char *s) { return cf_string_from_cstr(s); }

static char g_bundle_path[PATH_MAX], g_resource_path[PATH_MAX];

static void init_paths(void)
{
    if (g_bundle_path[0])
        return;
    // .../Hytale.app/Contents/MacOS/HytaleClient
    snprintf(g_bundle_path, sizeof g_bundle_path, "%s", g_executable_path);
    for (int i = 0; i < 3; i++) {
        char *slash = strrchr(g_bundle_path, '/');
        if (slash)
            *slash = 0;
    }
    snprintf(g_resource_path, sizeof g_resource_path, "%s/Contents/Resources", g_bundle_path);
}

static id g_main_bundle, g_process_info, g_file_manager, g_current_locale;

static id singleton(id *slot, const char *class_name)
{
    if (!*slot)
        *slot = cf_alloc(class_name, sizeof(struct cf_object));
    return *slot;
}

// NSObject
static id m_self(id self, SEL sel) { (void)sel; return self; }
static id m_retain(id self, SEL sel) { (void)sel; return cf_retain(self); }
static void m_release(id self, SEL sel) { (void)sel; cf_release(self); }
static id m_class(id self, SEL sel) { (void)sel; return is_class(self) ? self : ((struct cf_object *)self)->isa; }
static bool m_yes(id self, SEL sel) { (void)self; (void)sel; return true; }
static bool m_no(id self, SEL sel) { (void)self; (void)sel; return false; }
static unsigned long m_hash(id self, SEL sel) { (void)sel; return (unsigned long)(uintptr_t)self; }
static bool m_is_equal(id self, SEL sel, id other) { (void)sel; return cf_equal(self, other); }
static id m_alloc(id cls, SEL sel) { (void)sel; return cf_alloc(((struct objc_class_stub *)cls)->name, sizeof(struct cf_string)); }
static id m_description(id self, SEL sel) { (void)sel; return nsstring(class_name_of(self)); }

// NSString
static id s_with_utf8(id cls, SEL sel, const char *s) { (void)cls; (void)sel; return s ? nsstring(s) : NULL; }
static id s_with_cstring_encoding(id cls, SEL sel, const char *s, unsigned long enc) { (void)enc; return s_with_utf8(cls, sel, s); }
static id s_init_utf8(id self, SEL sel, const char *s)
{
    (void)sel;
    cf_release(self);
    return s ? nsstring(s) : NULL;
}
static id s_init_bytes(id self, SEL sel, const char *bytes, unsigned long len, unsigned long enc)
{
    (void)sel; (void)enc;
    cf_release(self);
    return cf_string_from_utf8(bytes, len);
}
static const char *s_utf8(id self, SEL sel) { (void)sel; return ((struct cf_string *)self)->utf8; }
static const char *s_cstring(id self, SEL sel, unsigned long enc) { (void)enc; return s_utf8(self, sel); }
static unsigned long s_length(id self, SEL sel)
{
    (void)sel;
    struct cf_string *s = self;
    unsigned long n = 0;
    for (size_t i = 0; i < s->length; i++) {
        uint8_t c = (uint8_t)s->utf8[i];
        if ((c & 0xC0) != 0x80)
            n += c >= 0xF0 ? 2 : 1;
    }
    return n;
}
static bool s_is_equal_string(id self, SEL sel, id other) { (void)sel; return cf_equal(self, other); }
static id s_append_path(id self, SEL sel, id component)
{
    (void)sel;
    char buf[PATH_MAX * 2];
    snprintf(buf, sizeof buf, "%s/%s", ((struct cf_string *)self)->utf8, ((struct cf_string *)component)->utf8);
    return nsstring(buf);
}
static id s_copy(id self, SEL sel) { (void)sel; return cf_retain(self); }

// NSBundle
static id b_main(id cls, SEL sel) { (void)cls; (void)sel; init_paths(); return singleton(&g_main_bundle, "NSBundle"); }
static id b_bundle_path(id self, SEL sel) { (void)self; (void)sel; init_paths(); return nsstring(g_bundle_path); }
static id b_resource_path(id self, SEL sel) { (void)self; (void)sel; init_paths(); return nsstring(g_resource_path); }
static id b_executable_path(id self, SEL sel) { (void)self; (void)sel; return nsstring(g_executable_path); }
static id b_identifier(id self, SEL sel) { (void)self; (void)sel; return nsstring("com.hypixel.hytale"); }
static id b_info_dictionary(id self, SEL sel) { (void)self; (void)sel; return cf_alloc("__NSCFDictionary", sizeof(struct cf_dictionary)); }

// NSProcessInfo
struct os_version { long major, minor, patch; };
static id p_process_info(id cls, SEL sel) { (void)cls; (void)sel; return singleton(&g_process_info, "NSProcessInfo"); }
static unsigned long p_processor_count(id self, SEL sel) { (void)self; (void)sel; return (unsigned long)sysconf(_SC_NPROCESSORS_ONLN); }
static unsigned long long p_physical_memory(id self, SEL sel)
{
    (void)self; (void)sel;
    struct sysinfo si;
    sysinfo(&si);
    return (unsigned long long)si.totalram * si.mem_unit;
}
static struct os_version p_os_version(id self, SEL sel) { (void)self; (void)sel; return (struct os_version){ 15, 0, 0 }; }
static bool p_at_least(id self, SEL sel, struct os_version v) { (void)self; (void)sel; return v.major < 15 || (v.major == 15 && v.minor == 0); }
static id p_os_version_string(id self, SEL sel) { (void)self; (void)sel; return nsstring("Version 15.0 (Build 24A335)"); }
extern char **environ;
// The client builds child process environments (the local world server) from this.
static id p_environment(id self, SEL sel)
{
    (void)self; (void)sel;
    struct cf_dictionary *d = cf_dictionary_create();
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (eq)
            cf_dictionary_set(d, cf_string_from_utf8(*e, (size_t)(eq - *e)), cf_string_from_cstr(eq + 1));
    }
    return d;
}
static id p_process_name(id self, SEL sel) { (void)self; (void)sel; return nsstring("HytaleClient"); }
static id p_host_name(id self, SEL sel)
{
    (void)self; (void)sel;
    char h[256] = "android";
    gethostname(h, sizeof h);
    return nsstring(h);
}
static long p_thermal_state(id self, SEL sel) { (void)self; (void)sel; return 0; }
static double p_uptime(id self, SEL sel)
{
    (void)self; (void)sel;
    struct sysinfo si;
    sysinfo(&si);
    return (double)si.uptime;
}
static int p_pid(id self, SEL sel) { (void)self; (void)sel; return getpid(); }

// NSFileManager
static id f_default(id cls, SEL sel) { (void)cls; (void)sel; return singleton(&g_file_manager, "NSFileManager"); }
static bool f_file_exists(id self, SEL sel, id path)
{
    (void)self; (void)sel;
    return path && access(((struct cf_string *)path)->utf8, F_OK) == 0;
}

// Darwin search path directories: Library 5, Documents 9, Caches 13, ApplicationSupport 14.
static id f_urls_for_directory(id self, SEL sel, unsigned long directory, unsigned long domains)
{
    (void)self; (void)sel; (void)domains;
    const char *home = getenv("HOME") ? getenv("HOME") : "/data/local/tmp";
    const char *sub = directory == 9 ? "Documents" : directory == 13 ? "Library/Caches"
                    : directory == 14 ? "Library/Application Support" : "Library";
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", home, sub);
    struct cf_array *a = cf_array_create();
    struct cf_string *url = nsstring(path);
    url->base.isa = objc_class_named("NSURL");
    url->base.isa->cf_type = CF_TYPE_STRING; // NSURL objects here are file paths
    cf_array_append(a, url);
    cf_release(url);
    return a;
}

static id u_path(id self, SEL sel) { (void)sel; return nsstring(((struct cf_string *)self)->utf8); }

// NSLocale
static id l_current(id cls, SEL sel) { (void)cls; (void)sel; return singleton(&g_current_locale, "NSLocale"); }
static id l_identifier(id self, SEL sel) { (void)self; (void)sel; return nsstring("en_US"); }
static id l_language_code(id self, SEL sel) { (void)self; (void)sel; return nsstring("en"); }
static id l_country_code(id self, SEL sel) { (void)self; (void)sel; return nsstring("US"); }
static id l_preferred_languages(id cls, SEL sel)
{
    (void)cls; (void)sel;
    struct cf_array *a = cf_array_create();
    struct cf_string *en = nsstring("en-US");
    cf_array_append(a, en);
    cf_release(en);
    return a;
}

// NSNumber
static id n_with_int(id cls, SEL sel, int v) { (void)cls; (void)sel; return cf_number_from_int(v); }
static id n_with_long(id cls, SEL sel, long long v) { (void)cls; (void)sel; return cf_number_from_int(v); }
static id n_with_bool(id cls, SEL sel, bool v) { (void)cls; (void)sel; return v ? &cf_boolean_true : &cf_boolean_false; }
static long long n_long(id self, SEL sel)
{
    (void)sel;
    if (self == &cf_boolean_true) return 1;
    if (self == &cf_boolean_false) return 0;
    return ((struct cf_number *)self)->i;
}
static int n_int(id self, SEL sel) { return (int)n_long(self, sel); }
static bool n_bool(id self, SEL sel) { return n_long(self, sel) != 0; }
static double n_double(id self, SEL sel)
{
    (void)sel;
    if (cf_type_of(self) != CF_TYPE_NUMBER)
        return (double)n_long(self, sel);
    return ((struct cf_number *)self)->d;
}

// NSArray / NSDictionary
static unsigned long a_count(id self, SEL sel) { (void)sel; return ((struct cf_array *)self)->count; }
static id a_object_at(id self, SEL sel, unsigned long i)
{
    (void)sel;
    struct cf_array *a = self;
    return i < a->count ? a->items[i] : NULL;
}
static id a_first(id self, SEL sel) { return a_object_at(self, sel, 0); }
static id a_last(id self, SEL sel) { return a_object_at(self, sel, ((struct cf_array *)self)->count - 1); }
static unsigned long d_count(id self, SEL sel) { (void)sel; return ((struct cf_dictionary *)self)->count; }
static id d_object_for_key(id self, SEL sel, id key)
{
    (void)sel;
    struct cf_dictionary *d = self;
    for (size_t i = 0; i < d->count; i++)
        if (cf_equal(d->keys[i], key))
            return d->values[i];
    return NULL;
}

// ---------------------------------------------------------------------------
// Method table. '+' entries match messages to the class object.

struct method {
    char kind; // '+' class method, '-' instance method
    const char *cls;
    const char *sel;
    void *imp;
};

static const struct method k_methods[] = {
    { '+', "*", "alloc", m_alloc }, { '+', "*", "allocWithZone:", m_alloc }, { '+', "*", "class", m_self },
    { '-', "*", "init", m_self }, { '-', "*", "self", m_self }, { '-', "*", "retain", m_retain },
    { '-', "*", "release", m_release }, { '-', "*", "autorelease", m_self }, { '-', "*", "class", m_class },
    { '-', "*", "hash", m_hash }, { '-', "*", "isEqual:", m_is_equal }, { '-', "*", "description", m_description },
    { '-', "*", "respondsToSelector:", m_no }, { '-', "*", "isKindOfClass:", m_yes }, { '-', "*", "copy", s_copy },
    { '+', "NSString", "stringWithUTF8String:", s_with_utf8 },
    { '+', "NSString", "stringWithCString:encoding:", s_with_cstring_encoding },
    { '-', "NSString", "initWithUTF8String:", s_init_utf8 },
    { '-', "NSString", "initWithBytes:length:encoding:", s_init_bytes },
    { '-', "NSString", "UTF8String", s_utf8 }, { '-', "NSString", "fileSystemRepresentation", s_utf8 },
    { '-', "NSString", "cStringUsingEncoding:", s_cstring }, { '-', "NSString", "length", s_length },
    { '-', "NSString", "isEqualToString:", s_is_equal_string },
    { '-', "NSString", "stringByAppendingPathComponent:", s_append_path },
    { '+', "NSBundle", "mainBundle", b_main }, { '-', "NSBundle", "bundlePath", b_bundle_path },
    { '-', "NSBundle", "resourcePath", b_resource_path }, { '-', "NSBundle", "executablePath", b_executable_path },
    { '-', "NSBundle", "bundleIdentifier", b_identifier }, { '-', "NSBundle", "infoDictionary", b_info_dictionary },
    { '+', "NSProcessInfo", "processInfo", p_process_info },
    { '-', "NSProcessInfo", "processorCount", p_processor_count },
    { '-', "NSProcessInfo", "activeProcessorCount", p_processor_count },
    { '-', "NSProcessInfo", "physicalMemory", p_physical_memory },
    { '-', "NSProcessInfo", "operatingSystemVersion", p_os_version },
    { '-', "NSProcessInfo", "isOperatingSystemAtLeastVersion:", p_at_least },
    { '-', "NSProcessInfo", "operatingSystemVersionString", p_os_version_string },
    { '-', "NSProcessInfo", "environment", p_environment },
    { '-', "NSProcessInfo", "processName", p_process_name }, { '-', "NSProcessInfo", "hostName", p_host_name },
    { '-', "NSProcessInfo", "thermalState", p_thermal_state }, { '-', "NSProcessInfo", "systemUptime", p_uptime },
    { '-', "NSProcessInfo", "isLowPowerModeEnabled", m_no }, { '-', "NSProcessInfo", "processIdentifier", p_pid },
    { '-', "NSProcessInfo", "isMacCatalystApp", m_no }, { '-', "NSProcessInfo", "isiOSAppOnMac", m_no },
    { '+', "NSFileManager", "defaultManager", f_default },
    { '-', "NSFileManager", "fileExistsAtPath:", f_file_exists },
    { '-', "NSFileManager", "URLsForDirectory:inDomains:", f_urls_for_directory },
    { '-', "NSURL", "path", u_path }, { '-', "NSURL", "fileSystemRepresentation", s_utf8 },
    { '+', "NSLocale", "currentLocale", l_current }, { '+', "NSLocale", "autoupdatingCurrentLocale", l_current },
    { '+', "NSLocale", "systemLocale", l_current }, { '+', "NSLocale", "preferredLanguages", l_preferred_languages },
    { '-', "NSLocale", "localeIdentifier", l_identifier }, { '-', "NSLocale", "languageCode", l_language_code },
    { '-', "NSLocale", "countryCode", l_country_code },
    { '+', "NSNumber", "numberWithInt:", n_with_int }, { '+', "NSNumber", "numberWithLongLong:", n_with_long },
    { '+', "NSNumber", "numberWithLong:", n_with_long }, { '+', "NSNumber", "numberWithBool:", n_with_bool },
    { '-', "NSNumber", "intValue", n_int }, { '-', "NSNumber", "longValue", n_long },
    { '-', "NSNumber", "longLongValue", n_long }, { '-', "NSNumber", "integerValue", n_long },
    { '-', "NSNumber", "boolValue", n_bool }, { '-', "NSNumber", "doubleValue", n_double },
    { '-', "NSArray", "count", a_count }, { '-', "NSArray", "objectAtIndex:", a_object_at },
    { '-', "NSArray", "firstObject", a_first }, { '-', "NSArray", "lastObject", a_last },
    { '-', "NSDictionary", "count", d_count },
    { '-', "NSDictionary", "objectForKey:", d_object_for_key },
};

extern void macho_objc_nil(void);
#define nil_imp macho_objc_nil

static int seen(const char *cls, SEL sel, char kind)
{
    static struct { const char *cls; SEL sel; char kind; } reported[512];
    static size_t count;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < count; i++)
        if (reported[i].cls == cls && reported[i].sel == sel && reported[i].kind == kind) {
            pthread_mutex_unlock(&lock);
            return 1;
        }
    if (count < SHIM_COUNT(reported))
        reported[count++] = (typeof(reported[0])){ cls, sel, kind };
    pthread_mutex_unlock(&lock);
    return 0;
}

// Called by macho_objc_msgSend with the receiver and selector; returns the IMP.
void *macho_objc_lookup(id self, SEL sel)
{
    if (!self || !sel)
        return (void *)nil_imp;
    char kind = is_class(self) ? '+' : '-';
    const char *name = family(class_name_of(self));
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < SHIM_COUNT(k_methods); i++) {
            const struct method *m = &k_methods[i];
            if (m->kind == kind && strcmp(m->sel, sel) == 0 && strcmp(m->cls, pass ? "*" : name) == 0)
                return m->imp;
        }
    }
    if (!seen(name, sel, kind))
        macho_log("objc", "unhandled %c[%s %s] -> nil", kind, class_name_of(self), sel);
    return (void *)nil_imp;
}

// ---------------------------------------------------------------------------
// Runtime entry points

extern void macho_objc_msgSend(void);

static id objc_getClass(const char *name) { return name ? objc_class_named(name) : NULL; }
static id objc_lookUpClass(const char *name) { return objc_getClass(name); }
static id object_getClass(id o) { return o ? (is_class(o) ? o : ((struct cf_object *)o)->isa) : NULL; }
static const char *class_getName(id cls) { return cls ? ((struct objc_class_stub *)cls)->name : "nil"; }
static const char *object_getClassName(id o) { return class_name_of(o); }
static bool class_respondsToSelector(id cls, SEL sel) { (void)cls; (void)sel; return false; }
static id objc_alloc(id cls) { return cls ? m_alloc(cls, "alloc") : NULL; }
static id objc_alloc_init(id cls) { return objc_alloc(cls); }
static id NSClassFromString(id name) { return name ? objc_class_named(((struct cf_string *)name)->utf8) : NULL; }
static id objc_retain(id o) { return cf_retain(o); }
static void objc_release(id o) { cf_release(o); }
static id objc_autorelease(id o) { return o; }
static id objc_retainAutorelease(id o) { return o; }
static id objc_retainAutoreleasedReturnValue(id o) { return o; }
static id objc_autoreleaseReturnValue(id o) { return o; }
static void *objc_autoreleasePoolPush(void) { return (void *)1; }
static void objc_autoreleasePoolPop(void *p) { (void)p; }

static struct objc_class_stub *class_ref(const char *name) { return objc_class_named(name); }

const struct shim objc_shims[] = {
    { "_objc_msgSend", (void *)macho_objc_msgSend },
    { "_objc_msgSendSuper", (void *)nil_imp },
    { "_objc_msgSendSuper2", (void *)nil_imp },
    { "_objc_getClass", (void *)objc_getClass },
    { "_objc_lookUpClass", (void *)objc_lookUpClass },
    { "_objc_getMetaClass", (void *)objc_getClass },
    { "_object_getClass", (void *)object_getClass },
    { "_class_getName", (void *)class_getName },
    { "_object_getClassName", (void *)object_getClassName },
    { "_class_respondsToSelector", (void *)class_respondsToSelector },
    { "_sel_registerName", (void *)sel_registerName },
    { "_sel_getUid", (void *)sel_registerName },
    { "_sel_getName", (void *)sel_getName },
    { "_objc_retain", (void *)objc_retain },
    { "_objc_alloc", (void *)objc_alloc },
    { "_objc_alloc_init", (void *)objc_alloc_init },
    { "_objc_opt_new", (void *)objc_alloc_init },
    { "_NSClassFromString", (void *)NSClassFromString },
    { "_objc_release", (void *)objc_release },
    { "_objc_autorelease", (void *)objc_autorelease },
    { "_objc_retainAutorelease", (void *)objc_retainAutorelease },
    { "_objc_retainAutoreleasedReturnValue", (void *)objc_retainAutoreleasedReturnValue },
    { "_objc_autoreleaseReturnValue", (void *)objc_autoreleaseReturnValue },
    { "_objc_autoreleasePoolPush", (void *)objc_autoreleasePoolPush },
    { "_objc_autoreleasePoolPop", (void *)objc_autoreleasePoolPop },
};
const size_t objc_shims_count = SHIM_COUNT(objc_shims);

/// Resolves _OBJC_CLASS_$_Name data imports to class stubs.
void *objc_class_import(const char *symbol)
{
    static const char prefix[] = "_OBJC_CLASS_$_";
    if (strncmp(symbol, prefix, sizeof prefix - 1) == 0)
        return class_ref(symbol + sizeof prefix - 1);
    return NULL;
}
