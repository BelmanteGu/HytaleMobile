// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Darwin libSystem shims implemented on top of Linux/Bionic.
// Functions listed in DIRECT share the calling convention and data layout on
// both systems and bind straight to the host libc. Everything else translates.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <grp.h>

#include "macho.h"

extern const char *g_executable_path;
extern void *macho_tlv_get_addr(struct tlv_descriptor *);
extern void macho_chkstk_darwin(void);
extern int __cxa_atexit(void (*)(void *), void *, void *);
extern int __cxa_thread_atexit_impl(void (*)(void *), void *, void *);
extern void *__dso_handle;
extern void *__memcpy_chk(void *, const void *, size_t, size_t);
extern void *__memset_chk(void *, int, size_t, size_t);
extern char *__strcat_chk(char *, const char *, size_t);
extern char *__strncpy_chk(char *, const char *, size_t, size_t);

// ---------------------------------------------------------------------------
// errno

static const struct { int host; int apple; } k_errno_map[] = {
    {EAGAIN, 35}, {EDEADLK, 11}, {EINPROGRESS, 36}, {EALREADY, 37}, {ENOTSOCK, 38}, {EDESTADDRREQ, 39}, {EMSGSIZE, 40},
    {EPROTOTYPE, 41}, {ENOPROTOOPT, 42}, {EPROTONOSUPPORT, 43}, {ESOCKTNOSUPPORT, 44}, {EPFNOSUPPORT, 46},
    {EAFNOSUPPORT, 47}, {EADDRINUSE, 48}, {EADDRNOTAVAIL, 49}, {ENETDOWN, 50}, {ENETUNREACH, 51},
    {ENETRESET, 52}, {ECONNABORTED, 53}, {ECONNRESET, 54}, {ENOBUFS, 55}, {EISCONN, 56}, {ENOTCONN, 57},
    {ESHUTDOWN, 58}, {ETOOMANYREFS, 59}, {ETIMEDOUT, 60}, {ECONNREFUSED, 61}, {ELOOP, 62},
    {ENAMETOOLONG, 63}, {EHOSTDOWN, 64}, {EHOSTUNREACH, 65}, {ENOTEMPTY, 66}, {EUSERS, 68}, {EDQUOT, 69},
    {ESTALE, 70}, {ENOLCK, 77}, {ENOSYS, 78}, {EOVERFLOW, 84}, {ECANCELED, 89}, {EIDRM, 90}, {ENOMSG, 91},
    {EILSEQ, 92}, {EBADMSG, 94}, {EMULTIHOP, 95}, {ENODATA, 96}, {ENOLINK, 97}, {ENOSR, 98}, {ENOSTR, 99},
    {EPROTO, 100}, {ETIME, 101}, {EOPNOTSUPP, 102}, {ENOTRECOVERABLE, 104}, {EOWNERDEAD, 105},
};

// Linux and Darwin share 1 to 34 except 11, which is EAGAIN on Linux and EDEADLK on
// Darwin (EAGAIN is 35 there).
int errno_to_apple(int host)
{
    if (host <= 34 && host != EAGAIN)
        return host;
    for (size_t i = 0; i < sizeof k_errno_map / sizeof k_errno_map[0]; i++)
        if (k_errno_map[i].host == host)
            return k_errno_map[i].apple;
    return host;
}

int errno_from_apple(int apple)
{
    if (apple <= 34 && apple != 11)
        return apple;
    if (apple == 45)
        return EOPNOTSUPP; // ENOTSUP
    for (size_t i = 0; i < sizeof k_errno_map / sizeof k_errno_map[0]; i++)
        if (k_errno_map[i].apple == apple)
            return k_errno_map[i].host;
    return apple;
}

static __thread int t_apple_errno; // the Darwin errno slot the program reads and writes

// Host errno value no call produces, stored after every *__error() access.
#define ERRNO_UNCHANGED (-0x4d41)

// Darwin reads and writes errno through *__error(), which runs before every access.
// The host errno is reset to a sentinel on each access, so any host failure since
// the previous access (even one leaving the same number) replaces the slot, while a
// value the program stored, such as the 0 written before a P/Invoke, is kept.
int apple_setjmp(void *env);
void apple_longjmp(void *env, int value);

static int *apple_error(void)
{
    int host = errno;
    if (host != ERRNO_UNCHANGED)
        t_apple_errno = errno_to_apple(host);
    errno = ERRNO_UNCHANGED;
    return &t_apple_errno;
}

static char *apple_strerror(int e) { return strerror(errno_from_apple(e)); }
static int apple_strerror_r(int e, char *buf, size_t len)
{
    const char *s = strerror(errno_from_apple(e));
    snprintf(buf, len, "%s", s);
    return 0;
}

// ---------------------------------------------------------------------------
// pthread objects with Darwin static initializers

#define APPLE_MUTEX_SIG_INIT 0x32AAABA7L
#define APPLE_RECURSIVE_MUTEX_SIG_INIT 0x32AAABA1L
#define APPLE_ERRORCHECK_MUTEX_SIG_INIT 0x32AAABA2L
#define APPLE_FIRSTFIT_MUTEX_SIG_INIT 0x32AAABA3L
#define APPLE_COND_SIG_INIT 0x3CB0B1BBL
#define APPLE_RWLOCK_SIG_INIT 0x2DA8B3B4L

static pthread_mutex_t *host_mutex(void *m)
{
    _Atomic long *sig = m;
    long value = atomic_load(sig);
    if (value == APPLE_MUTEX_SIG_INIT || value == APPLE_FIRSTFIT_MUTEX_SIG_INIT) {
        atomic_compare_exchange_strong(sig, &value, 0);
    } else if (value == APPLE_RECURSIVE_MUTEX_SIG_INIT || value == APPLE_ERRORCHECK_MUTEX_SIG_INIT) {
        static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&init_lock);
        if (atomic_load(sig) == value) {
            pthread_mutexattr_t attr;
            pthread_mutexattr_init(&attr);
            pthread_mutexattr_settype(&attr, value == APPLE_RECURSIVE_MUTEX_SIG_INIT
                ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK);
            memset(m, 0, sizeof(pthread_mutex_t));
            pthread_mutex_init(m, &attr);
            pthread_mutexattr_destroy(&attr);
        }
        pthread_mutex_unlock(&init_lock);
    }
    return m;
}

static pthread_cond_t *host_cond(void *c)
{
    _Atomic long *sig = c;
    long value = APPLE_COND_SIG_INIT;
    atomic_compare_exchange_strong(sig, &value, 0);
    return c;
}

static pthread_rwlock_t *host_rwlock(void *l)
{
    _Atomic long *sig = l;
    long value = APPLE_RWLOCK_SIG_INIT;
    if (atomic_compare_exchange_strong(sig, &value, 0))
        memset(l, 0, sizeof(pthread_rwlock_t));
    return l;
}

static int apple_mutex_lock(void *m) { return errno_to_apple(pthread_mutex_lock(host_mutex(m))); }
static int apple_mutex_unlock(void *m) { return errno_to_apple(pthread_mutex_unlock(host_mutex(m))); }
static int apple_mutex_destroy(void *m) { return errno_to_apple(pthread_mutex_destroy(host_mutex(m))); }
static int apple_mutex_init(void *m, const pthread_mutexattr_t *a) { return errno_to_apple(pthread_mutex_init(m, a)); }
static int apple_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    // Darwin: NORMAL 0, ERRORCHECK 1, RECURSIVE 2.
    int host = type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
    return errno_to_apple(pthread_mutexattr_settype(a, host));
}
static int apple_cond_wait(void *c, void *m) { return errno_to_apple(pthread_cond_wait(host_cond(c), host_mutex(m))); }
static int apple_cond_timedwait(void *c, void *m, const struct timespec *t)
{
    return errno_to_apple(pthread_cond_timedwait(host_cond(c), host_mutex(m), t));
}
static int apple_cond_timedwait_relative_np(void *c, void *m, const struct timespec *rel)
{
    struct timespec abs;
    clock_gettime(CLOCK_REALTIME, &abs);
    abs.tv_sec += rel->tv_sec;
    abs.tv_nsec += rel->tv_nsec;
    if (abs.tv_nsec >= 1000000000L) {
        abs.tv_sec++;
        abs.tv_nsec -= 1000000000L;
    }
    return errno_to_apple(pthread_cond_timedwait(host_cond(c), host_mutex(m), &abs));
}
static int apple_cond_signal(void *c) { return errno_to_apple(pthread_cond_signal(host_cond(c))); }
static int apple_cond_broadcast(void *c) { return errno_to_apple(pthread_cond_broadcast(host_cond(c))); }
static int apple_cond_destroy(void *c) { return errno_to_apple(pthread_cond_destroy(host_cond(c))); }
static int apple_cond_init(void *c, const pthread_condattr_t *a) { return errno_to_apple(pthread_cond_init(c, a)); }
static int apple_rwlock_rdlock(void *l) { return errno_to_apple(pthread_rwlock_rdlock(host_rwlock(l))); }
static int apple_rwlock_wrlock(void *l) { return errno_to_apple(pthread_rwlock_wrlock(host_rwlock(l))); }
static int apple_rwlock_unlock(void *l) { return errno_to_apple(pthread_rwlock_unlock(host_rwlock(l))); }

static int apple_rwlock_destroy(void *l) { return errno_to_apple(pthread_rwlock_destroy(host_rwlock(l))); }
static int apple_mutex_trylock(void *m) { return errno_to_apple(pthread_mutex_trylock(host_mutex(m))); }

// Darwin pthread_once_t is a signature word followed by 8 opaque bytes, used here as
// the state: 0 = not run, 1 = running, 2 = done. Waiters yield, as in dispatch_once.
static int apple_pthread_once(void *once, void (*init)(void))
{
    _Atomic int *state = (_Atomic int *)((uint8_t *)once + 8);
    if (atomic_load_explicit(state, memory_order_acquire) == 2)
        return 0;
    int expected = 0;
    if (atomic_compare_exchange_strong(state, &expected, 1)) {
        init();
        atomic_store_explicit(state, 2, memory_order_release);
        return 0;
    }
    while (atomic_load_explicit(state, memory_order_acquire) != 2)
        sched_yield();
    return 0;
}

// dispatch_semaphore_t, backed by a POSIX semaphore. dispatch_time_t values other
// than NOW (0) and FOREVER (~0) are absolute mach_absolute_time nanoseconds.
static uint64_t apple_mach_absolute_time(void);

struct apple_semaphore {
    uint32_t magic;
    sem_t sem;
};
#define SEMAPHORE_MAGIC 0x53454d41u

static void *apple_dispatch_semaphore_create(long value)
{
    if (value < 0)
        return NULL;
    struct apple_semaphore *s = calloc(1, sizeof *s);
    s->magic = SEMAPHORE_MAGIC;
    sem_init(&s->sem, 0, (unsigned)value);
    return s;
}

static long apple_dispatch_semaphore_signal(struct apple_semaphore *s)
{
    sem_post(&s->sem);
    return 0;
}

static long apple_dispatch_semaphore_wait(struct apple_semaphore *s, uint64_t when)
{
    int rc;
    if (when == ~0ULL) {
        while ((rc = sem_wait(&s->sem)) < 0 && errno == EINTR)
            ;
        return rc ? -1 : 0;
    }
    if (when == 0)
        return sem_trywait(&s->sem) ? -1 : 0;
    struct timespec now, abs;
    uint64_t now_ns = apple_mach_absolute_time();
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t wait_ns = when > now_ns ? when - now_ns : 0;
    uint64_t abs_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec + wait_ns;
    abs.tv_sec = (time_t)(abs_ns / 1000000000ull);
    abs.tv_nsec = (long)(abs_ns % 1000000000ull);
    while ((rc = sem_timedwait(&s->sem, &abs)) < 0 && errno == EINTR)
        ;
    return rc ? -1 : 0;
}

static void apple_dispatch_release(void *object)
{
    struct apple_semaphore *s = object;
    if (s && s->magic == SEMAPHORE_MAGIC) {
        sem_destroy(&s->sem);
        s->magic = 0;
        free(s);
    }
}

// The loader does not expose its images to dyld queries.
static uint32_t apple_dyld_image_count(void) { return 0; }
static const void *apple_dyld_get_image_header(uint32_t index) { (void)index; return NULL; }
static const char *apple_dyld_get_image_name(uint32_t index) { (void)index; return NULL; }
static intptr_t apple_dyld_get_image_vmaddr_slide(uint32_t index) { (void)index; return 0; }

// CoreGraphics cursor control has no meaning on a touch device; report success.
static int apple_cg_associate_mouse(int connected) { (void)connected; return 0; }
static int apple_cg_set_events_suppression(double seconds) { (void)seconds; return 0; }

static void *apple_get_stackaddr_np(pthread_t t)
{
    pthread_attr_t attr;
    void *low = NULL;
    size_t size = 0;
    if (pthread_getattr_np(t, &attr) == 0) {
        pthread_attr_getstack(&attr, &low, &size);
        pthread_attr_destroy(&attr);
    }
    return (uint8_t *)low + size; // Darwin returns the high end of the stack
}

static size_t apple_get_stacksize_np(pthread_t t)
{
    pthread_attr_t attr;
    void *low = NULL;
    size_t size = 0;
    if (pthread_getattr_np(t, &attr) == 0) {
        pthread_attr_getstack(&attr, &low, &size);
        pthread_attr_destroy(&attr);
    }
    return size;
}

static int apple_threadid_np(pthread_t t, uint64_t *id)
{
    if (!id)
        return 22;
    *id = (uint64_t)(t ? pthread_gettid_np(t) : gettid());
    return 0;
}

static int apple_setname_np(const char *name)
{
    char shortened[16];
    snprintf(shortened, sizeof shortened, "%s", name);
    return errno_to_apple(pthread_setname_np(pthread_self(), shortened));
}

// ---------------------------------------------------------------------------
// dyld, startup and misc

static void apple_dyld_register_add_image(void (*cb)(const void *, intptr_t))
{
    struct macho_image *main = macho_main_image();
    cb(main->header, main->slide);
}

static void apple_dyld_register_remove_image(void (*cb)(const void *, intptr_t)) { (void)cb; }

static uint8_t *apple_getsectiondata(const void *mh, const char *seg, const char *sect, unsigned long *size)
{
    struct macho_image *main = macho_main_image();
    if (mh != main->header)
        return NULL;
    size_t s = 0;
    uint8_t *p = macho_find_section(main, seg, sect, &s);
    if (size)
        *size = s;
    return p;
}

static int apple_NSGetExecutablePath(char *buf, uint32_t *size)
{
    size_t need = strlen(g_executable_path) + 1;
    if (*size < need) {
        *size = (uint32_t)need;
        return -1;
    }
    memcpy(buf, g_executable_path, need);
    return 0;
}

static int apple_availability_version_check(uint32_t count, const void *versions)
{
    (void)count;
    (void)versions;
    return 1;
}

// dispatch_once: 0 = not run, 1 = running, ~0 = done. Waiters yield while
// another thread runs the block, so nested once-blocks cannot deadlock.
static void apple_dispatch_once_f(_Atomic long *predicate, void *context, void (*fn)(void *))
{
    if (atomic_load_explicit(predicate, memory_order_acquire) == ~0L)
        return;
    long expected = 0;
    if (atomic_compare_exchange_strong(predicate, &expected, 1)) {
        fn(context);
        atomic_store_explicit(predicate, ~0L, memory_order_release);
        return;
    }
    while (atomic_load_explicit(predicate, memory_order_acquire) != ~0L)
        sched_yield();
}

// Registers a destructor for a thread-local object, run when the thread exits.
static void apple_tlv_atexit(void (*dtor)(void *), void *object)
{
    __cxa_thread_atexit_impl(dtor, object, &__dso_handle);
}

static uint64_t apple_clock_gettime_nsec_np(int id);

// ---------------------------------------------------------------------------
// Darwin ctype. isalpha() and friends are inline on Darwin: for c < 256 they read
// _DefaultRuneLocale.__runetype[c], otherwise they call __maskrune(). The table must
// use Darwin's layout and _CTYPE_* bits.

#include <ctype.h>
#include <wctype.h>

#define DARWIN_CTYPE_A 0x00000100u
#define DARWIN_CTYPE_C 0x00000200u
#define DARWIN_CTYPE_D 0x00000400u
#define DARWIN_CTYPE_G 0x00000800u
#define DARWIN_CTYPE_L 0x00001000u
#define DARWIN_CTYPE_P 0x00002000u
#define DARWIN_CTYPE_S 0x00004000u
#define DARWIN_CTYPE_U 0x00008000u
#define DARWIN_CTYPE_X 0x00010000u
#define DARWIN_CTYPE_B 0x00020000u
#define DARWIN_CTYPE_R 0x00040000u
#define DARWIN_CTYPE_SW1 0x40000000u

struct darwin_rune_locale {
    char magic[8];
    char encoding[32];
    void *sgetrune;
    void *sputrune;
    int32_t invalid_rune;
    uint32_t runetype[256];
    int32_t maplower[256];
    int32_t mapupper[256];
    uint8_t ranges_and_variables[64];
};

_Static_assert(offsetof(struct darwin_rune_locale, runetype) == 0x3c, "Darwin __runetype offset");

static struct darwin_rune_locale g_rune_locale;

static uint32_t darwin_ctype_flags(int c)
{
    if (c < 0)
        return 0;
    uint32_t f = 0;
    if (c < 128) {
        if (isalpha(c)) f |= DARWIN_CTYPE_A;
        if (iscntrl(c)) f |= DARWIN_CTYPE_C;
        if (isdigit(c)) f |= DARWIN_CTYPE_D;
        if (isgraph(c)) f |= DARWIN_CTYPE_G;
        if (islower(c)) f |= DARWIN_CTYPE_L;
        if (ispunct(c)) f |= DARWIN_CTYPE_P;
        if (isspace(c)) f |= DARWIN_CTYPE_S;
        if (isupper(c)) f |= DARWIN_CTYPE_U;
        if (isblank(c)) f |= DARWIN_CTYPE_B;
        if (isprint(c)) f |= DARWIN_CTYPE_R | DARWIN_CTYPE_SW1;
        if (isxdigit(c)) {
            f |= DARWIN_CTYPE_X;
            f |= (uint32_t)(isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10)); // digittoint() value
        }
        return f;
    }
    if (c < 256)
        return 0; // the "C" locale classifies nothing above ASCII
    if (iswalpha((wint_t)c)) f |= DARWIN_CTYPE_A;
    if (iswcntrl((wint_t)c)) f |= DARWIN_CTYPE_C;
    if (iswdigit((wint_t)c)) f |= DARWIN_CTYPE_D;
    if (iswgraph((wint_t)c)) f |= DARWIN_CTYPE_G;
    if (iswlower((wint_t)c)) f |= DARWIN_CTYPE_L;
    if (iswpunct((wint_t)c)) f |= DARWIN_CTYPE_P;
    if (iswspace((wint_t)c)) f |= DARWIN_CTYPE_S;
    if (iswupper((wint_t)c)) f |= DARWIN_CTYPE_U;
    if (iswblank((wint_t)c)) f |= DARWIN_CTYPE_B;
    if (iswprint((wint_t)c)) f |= DARWIN_CTYPE_R | DARWIN_CTYPE_SW1;
    return f;
}

static void init_rune_locale(void)
{
    memcpy(g_rune_locale.magic, "RuneMagA", 8);
    snprintf(g_rune_locale.encoding, sizeof g_rune_locale.encoding, "NONE");
    g_rune_locale.invalid_rune = 0xFFFD;
    for (int c = 0; c < 256; c++) {
        g_rune_locale.runetype[c] = darwin_ctype_flags(c);
        g_rune_locale.maplower[c] = c < 128 ? tolower(c) : c;
        g_rune_locale.mapupper[c] = c < 128 ? toupper(c) : c;
    }
}

static unsigned long apple_maskrune(int32_t c, unsigned long mask)
{
    return (c >= 0 && c < 256 ? g_rune_locale.runetype[c] : darwin_ctype_flags(c)) & mask;
}
static int32_t apple_tolower(int32_t c) { return c >= 0 && c < 256 ? g_rune_locale.maplower[c] : (int32_t)towlower((wint_t)c); }
static int32_t apple_toupper(int32_t c) { return c >= 0 && c < 256 ? g_rune_locale.mapupper[c] : (int32_t)towupper((wint_t)c); }

struct sincos_double { double sin, cos; };
static struct sincos_double apple_sincos_stret(double x) { return (struct sincos_double){ sin(x), cos(x) }; }

// Mach absolute time is reported in nanoseconds (timebase 1/1).
struct mach_timebase { uint32_t numer, denom; };
static int apple_mach_timebase_info(struct mach_timebase *t)
{
    t->numer = 1;
    t->denom = 1;
    return 0;
}
static uint64_t apple_mach_absolute_time(void) { return apple_clock_gettime_nsec_np(8); }

struct sincos_result { float sin, cos; };
static struct sincos_result apple_sincosf_stret(float x) { return (struct sincos_result){ sinf(x), cosf(x) }; }
static float apple_exp10f(float x) { return powf(10.0f, x); }

static void apple_memset_pattern16(void *b, const void *pattern, size_t len)
{
    uint8_t *p = b;
    for (size_t i = 0; i < len; i++)
        p[i] = ((const uint8_t *)pattern)[i % 16];
}

static void apple_stack_chk_fail(void)
{
    fprintf(stderr, "[macho] stack smashing detected\n");
    abort();
}

static void apple_bzero(void *p, size_t n) { memset(p, 0, n); }

// C11 memset_s: never optimized away; returns 0 or EINVAL.
static int apple_memset_s(void *dest, size_t destsz, int c, size_t n)
{
    if (!dest)
        return 22;
    volatile uint8_t *p = dest;
    for (size_t i = 0; i < (n < destsz ? n : destsz); i++)
        p[i] = (uint8_t)c;
    return n > destsz ? 22 : 0;
}

// Bionic has no thread cancellation, so the cancel state is always "disabled".
// pthread_key_t is unsigned long on Darwin and int on Bionic; results are error numbers.
static int apple_pthread_key_create(unsigned long *key, void (*destructor)(void *))
{
    pthread_key_t k;
    int rc = pthread_key_create(&k, destructor);
    if (rc == 0)
        *key = (unsigned long)k;
    return errno_to_apple(rc);
}
static int apple_pthread_key_delete(unsigned long key) { return errno_to_apple(pthread_key_delete((pthread_key_t)key)); }
static void *apple_pthread_getspecific(unsigned long key) { return pthread_getspecific((pthread_key_t)key); }
static int apple_pthread_setspecific(unsigned long key, const void *value)
{
    return errno_to_apple(pthread_setspecific((pthread_key_t)key, value));
}

static int apple_setcancelstate(int state, int *old)
{
    if (old)
        *old = 1;
    return 0;
}

static int apple_getpagesize(void) { return (int)sysconf(_SC_PAGESIZE); }

static void apple_arc4random_buf(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r > 0) {
            p += r;
            n -= (size_t)r;
        }
    }
}

// Darwin clock ids: REALTIME 0, MONOTONIC_RAW 4, MONOTONIC_RAW_APPROX 5, MONOTONIC 6,
// UPTIME_RAW 8, UPTIME_RAW_APPROX 9, PROCESS_CPUTIME 12, THREAD_CPUTIME 16.
static clockid_t clock_from_apple(int id)
{
    switch (id) {
    case 0: return CLOCK_REALTIME;
    case 4: case 5: return CLOCK_MONOTONIC_RAW;
    case 6: return CLOCK_BOOTTIME;
    case 8: case 9: return CLOCK_MONOTONIC;
    case 12: return CLOCK_PROCESS_CPUTIME_ID;
    case 16: return CLOCK_THREAD_CPUTIME_ID;
    default: return CLOCK_MONOTONIC;
    }
}

static int apple_clock_gettime(int id, struct timespec *ts)
{
    int rc = clock_gettime(clock_from_apple(id), ts);
    return rc;
}

static uint64_t apple_clock_gettime_nsec_np(int id)
{
    struct timespec ts;
    if (clock_gettime(clock_from_apple(id), &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Data symbols

static uintptr_t g_stack_chk_guard;
static FILE *g_stderrp;
static FILE *g_stdoutp;
static unsigned int g_mach_task_self = 0x103; // an arbitrary non-zero port name

// The client's abort() follows its fatal error message on stderr, which the
// Android host forwards to logcat from another thread. Give that thread time
// to forward the message before the process dies.
static void apple_abort(void)
{
    fprintf(stderr, "[macho] abort() called by the program\n");
    usleep(500 * 1000);
    abort();
}

// Bionic has no backtrace(); report no frames so callers fall back to their own output.
static int apple_backtrace(void **frames, int size) { (void)frames; (void)size; return 0; }

static void apple_exit_now(int status)
{
    fprintf(stderr, "[macho] _exit(%d) called by the program\n", status);
    usleep(500 * 1000);
    _exit(status);
}

// ---------------------------------------------------------------------------
// Symbol table

#define DIRECT(fn) { "_" #fn, (void *)fn }
#define NOCANCEL(fn) { "_" #fn "$NOCANCEL", (void *)fn }
#define SHIM(name, fn) { name, (void *)fn }

static const struct shim k_shims[] = {
    // memory and strings
    DIRECT(memcpy), DIRECT(memmove), DIRECT(memset), DIRECT(memcmp), DIRECT(memchr), SHIM("_bzero", apple_bzero), SHIM("_memset_s", apple_memset_s),
    DIRECT(strlen), DIRECT(strcmp), DIRECT(strncmp), DIRECT(strchr), DIRECT(strrchr), DIRECT(strstr),
    DIRECT(strcpy), DIRECT(strncpy), DIRECT(strcat), DIRECT(strncat), DIRECT(strdup), DIRECT(strcasecmp),
    DIRECT(strtol), DIRECT(strtoll), DIRECT(strtoull), DIRECT(strtok_r), DIRECT(qsort), DIRECT(bsearch),
    DIRECT(malloc), DIRECT(calloc), DIRECT(realloc), DIRECT(free), DIRECT(aligned_alloc),
    SHIM("___memcpy_chk", __memcpy_chk), SHIM("___memset_chk", __memset_chk),
    SHIM("___strcat_chk", __strcat_chk), SHIM("___strncpy_chk", __strncpy_chk),
    SHIM("_strerror", apple_strerror), SHIM("_strerror_r", apple_strerror_r),
    // math
    DIRECT(cos), DIRECT(cosf), DIRECT(sin), DIRECT(sinf), DIRECT(tan), DIRECT(tanf), DIRECT(tanh),
    DIRECT(asin), DIRECT(asinf), DIRECT(acosf), DIRECT(atan2), DIRECT(atan2f), DIRECT(cbrt), DIRECT(cbrtf),
    DIRECT(pow), DIRECT(powf), DIRECT(expf), DIRECT(log), DIRECT(logf), DIRECT(log2), DIRECT(log2f),
    DIRECT(log10f), DIRECT(fmod), DIRECT(fmodf),
    // process and files with matching ABI
    DIRECT(getenv), DIRECT(getpid), DIRECT(geteuid), DIRECT(getegid), DIRECT(sched_yield), DIRECT(exit),
    // child processes (the local world server): same ABI on both systems
    DIRECT(fork), DIRECT(execve), DIRECT(execv), DIRECT(setgroups),
    SHIM("_abort", apple_abort), DIRECT(atexit), DIRECT(close), NOCANCEL(close), DIRECT(read), NOCANCEL(read),
    DIRECT(write), NOCANCEL(write), DIRECT(lseek), DIRECT(pipe), DIRECT(dup2), DIRECT(unlink), DIRECT(rmdir),
    DIRECT(rename), DIRECT(chdir), DIRECT(getcwd), DIRECT(readlink), DIRECT(access), DIRECT(mkdir),
    DIRECT(isatty), DIRECT(sync), DIRECT(link), DIRECT(time), DIRECT(gettimeofday), DIRECT(gmtime_r),
    DIRECT(timegm), SHIM("_nanosleep$NOCANCEL", nanosleep), SHIM("__exit", apple_exit_now), SHIM("_backtrace", apple_backtrace),
    SHIM("___cxa_atexit", __cxa_atexit),
    // errno
    SHIM("___error", apple_error),
    // non-local jumps (trampolines.S)
    SHIM("_setjmp", apple_setjmp), SHIM("__setjmp", apple_setjmp), SHIM("_longjmp", apple_longjmp),
    SHIM("__longjmp", apple_longjmp),
    // pthread
    DIRECT(pthread_self), DIRECT(pthread_equal), DIRECT(pthread_join), DIRECT(pthread_create), DIRECT(pthread_attr_init),
    DIRECT(pthread_attr_destroy), DIRECT(pthread_attr_setstacksize), DIRECT(pthread_attr_setdetachstate),
    DIRECT(pthread_mutexattr_init), DIRECT(pthread_mutexattr_destroy), DIRECT(pthread_condattr_init),
    DIRECT(pthread_condattr_destroy), SHIM("_pthread_setcancelstate", apple_setcancelstate),
    SHIM("_pthread_key_create", apple_pthread_key_create), SHIM("_pthread_key_delete", apple_pthread_key_delete),
    SHIM("_pthread_getspecific", apple_pthread_getspecific), SHIM("_pthread_setspecific", apple_pthread_setspecific),
    SHIM("_pthread_mutex_init", apple_mutex_init), SHIM("_pthread_mutex_lock", apple_mutex_lock),
    SHIM("_pthread_mutex_unlock", apple_mutex_unlock), SHIM("_pthread_mutex_destroy", apple_mutex_destroy),
    SHIM("_pthread_mutexattr_settype", apple_mutexattr_settype),
    SHIM("_pthread_cond_init", apple_cond_init), SHIM("_pthread_cond_destroy", apple_cond_destroy),
    SHIM("_pthread_cond_signal", apple_cond_signal), SHIM("_pthread_cond_broadcast", apple_cond_broadcast),
    SHIM("_pthread_cond_wait$NOCANCEL", apple_cond_wait), SHIM("_pthread_cond_timedwait", apple_cond_timedwait),
    SHIM("_pthread_cond_timedwait_relative_np", apple_cond_timedwait_relative_np),
    SHIM("_pthread_rwlock_rdlock", apple_rwlock_rdlock), SHIM("_pthread_rwlock_wrlock", apple_rwlock_wrlock),
    SHIM("_pthread_rwlock_unlock", apple_rwlock_unlock), SHIM("_pthread_rwlock_destroy", apple_rwlock_destroy),
    SHIM("_pthread_mutex_trylock", apple_mutex_trylock), SHIM("_pthread_once", apple_pthread_once),
    SHIM("_CGAssociateMouseAndMouseCursorPosition", apple_cg_associate_mouse),
    SHIM("_CGSetLocalEventsSuppressionInterval", apple_cg_set_events_suppression),
    DIRECT(pthread_atfork), DIRECT(posix_memalign), DIRECT(getentropy), DIRECT(writev),
    SHIM("_dispatch_semaphore_create", apple_dispatch_semaphore_create),
    SHIM("_dispatch_semaphore_signal", apple_dispatch_semaphore_signal),
    SHIM("_dispatch_semaphore_wait", apple_dispatch_semaphore_wait), SHIM("_dispatch_release", apple_dispatch_release),
    SHIM("__dyld_image_count", apple_dyld_image_count), SHIM("__dyld_get_image_header", apple_dyld_get_image_header),
    SHIM("__dyld_get_image_name", apple_dyld_get_image_name),
    SHIM("__dyld_get_image_vmaddr_slide", apple_dyld_get_image_vmaddr_slide),
    SHIM("_pthread_get_stackaddr_np", apple_get_stackaddr_np),
    SHIM("_pthread_get_stacksize_np", apple_get_stacksize_np),
    SHIM("_pthread_threadid_np", apple_threadid_np), SHIM("_pthread_setname_np", apple_setname_np),
    // dyld and startup
    SHIM("__tlv_bootstrap", macho_tlv_get_addr), SHIM("__tlv_atexit", apple_tlv_atexit),
    SHIM("__dyld_register_func_for_add_image", apple_dyld_register_add_image),
    SHIM("__dyld_register_func_for_remove_image", apple_dyld_register_remove_image),
    SHIM("_getsectiondata", apple_getsectiondata), SHIM("__NSGetExecutablePath", apple_NSGetExecutablePath),
    SHIM("__availability_version_check", apple_availability_version_check),
    SHIM("___chkstk_darwin", macho_chkstk_darwin), SHIM("___stack_chk_fail", apple_stack_chk_fail),
    SHIM("_dispatch_once_f", apple_dispatch_once_f), SHIM("_getpagesize", apple_getpagesize),
    SHIM("_arc4random_buf", apple_arc4random_buf), SHIM("_clock_gettime", apple_clock_gettime),
    SHIM("_clock_gettime_nsec_np", apple_clock_gettime_nsec_np),
    SHIM("_mach_timebase_info", apple_mach_timebase_info), SHIM("_mach_absolute_time", apple_mach_absolute_time),
    SHIM("___sincosf_stret", apple_sincosf_stret), SHIM("___exp10f", apple_exp10f),
    SHIM("___sincos_stret", apple_sincos_stret), DIRECT(ldexp), DIRECT(ldexpf), DIRECT(frexp),
    SHIM("__DefaultRuneLocale", &g_rune_locale), SHIM("___maskrune", apple_maskrune),
    SHIM("___tolower", apple_tolower), SHIM("___toupper", apple_toupper),
    SHIM("_memset_pattern16", apple_memset_pattern16), DIRECT(rand), DIRECT(srand), DIRECT(puts),
    DIRECT(exp2f), DIRECT(atanf), DIRECT(sqrtf), DIRECT(floorf), DIRECT(ceilf), DIRECT(roundf),
    // data
    SHIM("___stack_chk_guard", &g_stack_chk_guard), SHIM("___stderrp", &g_stderrp),
    SHIM("___stdoutp", &g_stdoutp), SHIM("_mach_task_self_", &g_mach_task_self),
};

void *gl_entry(const char *name, void *fn, void *(*resolve)(const char *));
static void *opengl_lookup(const char *name);

static void *opengl_resolve(const char *name)
{
    char mangled[128];
    snprintf(mangled, sizeof mangled, "_%s", name);
    return opengl_lookup(mangled);
}

// OpenGL.framework imports (Noesis links them directly) resolve to the desktop
// GL implementation the client uses on Android, through gl_entry (see gl.c).
static void *opengl_lookup(const char *name)
{
    static void *gl;
    if (!gl)
        gl = dlopen("libmobileglues.so", RTLD_NOW | RTLD_GLOBAL);
    if (!gl)
        return NULL;
    void *fn = dlsym(gl, name + 1);
    if (!fn) {
        static void *(*get_proc)(const char *);
        if (!get_proc)
            get_proc = (void *(*)(const char *))dlsym(gl, "eglGetProcAddress");
        fn = get_proc ? get_proc(name + 1) : NULL;
    }
    return gl_entry(name + 1, fn, opengl_resolve);
}

void *shim_lookup(const char *name, const char *library)
{
    if (library && strstr(library, "OpenGL.framework")) {
        void *fn = opengl_lookup(name);
        if (fn)
            return fn;
    }
    static int initialized;
    if (!initialized) {
        initialized = 1;
        apple_arc4random_buf(&g_stack_chk_guard, sizeof g_stack_chk_guard);
        init_rune_locale();
        g_stderrp = stderr;
        g_stdoutp = stdout;
    }
    static const struct { const struct shim *table; const size_t *count; } tables[] = {
        { signal_shims, &signal_shims_count },
        { posix_shims, &posix_shims_count },
        { dyld_shims, &dyld_shims_count },
        { crypto_shims, &crypto_shims_count },
        { net_shims, &net_shims_count },
        { objc_shims, &objc_shims_count },
        { cf_shims, &cf_shims_count },
        { cxx_shims, &cxx_shims_count },
        { security_shims, &security_shims_count },
    };
    for (size_t i = 0; i < SHIM_COUNT(k_shims); i++)
        if (strcmp(k_shims[i].name, name) == 0)
            return k_shims[i].address;
    for (size_t t = 0; t < SHIM_COUNT(tables); t++)
        for (size_t i = 0; i < *tables[t].count; i++)
            if (strcmp(tables[t].table[i].name, name) == 0)
                return tables[t].table[i].address;
    return objc_class_import(name);
}
