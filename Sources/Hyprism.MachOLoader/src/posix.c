// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Darwin libSystem calls whose constants or structures differ from Linux:
// memory, files, directories, stdio formatting, system information and the
// small Mach surface used by the .NET runtime.

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/membarrier.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <unistd.h>

#include "macho.h"

#define TRACE(...) do { if (getenv("MACHO_TRACE")) { fprintf(stderr, "[shim] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// Logs lookups of files that do not exist (first 200), to find wrong resource paths.
static void note_missing(const char *what, const char *path)
{
    static _Atomic int count;
    if (errno == ENOENT && path && atomic_fetch_add(&count, 1) < 200)
        macho_log("fs", "%s missing: %s", what, path);
}

// Logs successful lookups of interface images (first 40), to see which variants load.
static void note_found(const char *what, const char *path)
{
    static _Atomic int count;
    if (path && strstr(path, "/Interface/") && strstr(path, ".png") && atomic_fetch_add(&count, 1) < 40)
        macho_log("fs", "%s found: %s", what, strstr(path, "/Interface/"));
}

// Newer clients (build 39) look for their data next to the executable, Contents/MacOS/Data, which
// on macOS resolves through the bundle; the files are in Contents/Resources/Data.
static const char *bundle_path(const char *path, char *buf, size_t size)
{
    static const char macos_data[] = "/Contents/MacOS/Data";
    const char *at = path ? strstr(path, macos_data) : NULL;
    if (!at)
        return path;
    const char *rest = at + sizeof macos_data - 1;
    if (*rest && *rest != '/')
        return path;
    snprintf(buf, size, "%.*s/Contents/Resources/Data%s", (int)(at - path), path, rest);
    return buf;
}

static int fail(int host_errno)
{
    errno = host_errno;
    return -1;
}

// ---------------------------------------------------------------------------
// Variadic helpers: build an AAPCS64 va_list that reads every argument from
// the Darwin stack slots.

struct aapcs_va {
    void *stack;
    void *gr_top;
    void *vr_top;
    int gr_offs;
    int vr_offs;
};

#define VA_FROM_STACK(ap, slots)                                       \
    va_list ap;                                                        \
    do {                                                               \
        struct aapcs_va v_ = { (slots), NULL, NULL, 0, 0 };            \
        memcpy(&ap, &v_, sizeof v_);                                   \
    } while (0)

int shim_printf_stack(const char *fmt, void *slots) { VA_FROM_STACK(ap, slots); return vprintf(fmt, ap); }
int shim_fprintf_stack(FILE *f, const char *fmt, void *slots) { VA_FROM_STACK(ap, slots); return vfprintf(f, fmt, ap); }
int shim_snprintf_stack(char *buf, size_t n, const char *fmt, void *slots)
{
    VA_FROM_STACK(ap, slots);
    return vsnprintf(buf, n, fmt, ap);
}
int shim_asprintf_stack(char **out, const char *fmt, void *slots) { VA_FROM_STACK(ap, slots); return vasprintf(out, fmt, ap); }
int shim_sscanf_stack(const char *s, const char *fmt, void *slots) { VA_FROM_STACK(ap, slots); return vsscanf(s, fmt, ap); }
void shim_syslog_stack(int prio, const char *fmt, void *slots)
{
    VA_FROM_STACK(ap, slots);
    vsyslog(prio, fmt, ap);
}

// Darwin's va_list is a plain pointer to the stack slots.
static int apple_vsnprintf(char *buf, size_t n, const char *fmt, void *apple_ap)
{
    VA_FROM_STACK(ap, apple_ap);
    return vsnprintf(buf, n, fmt, ap);
}
static int apple_vsnprintf_l(char *buf, size_t n, void *locale, const char *fmt, void *apple_ap)
{
    (void)locale;
    return apple_vsnprintf(buf, n, fmt, apple_ap);
}
// The *_l shims ignore the locale and use the C locale, which is what callers ask
// for (.NET passes LC_ALL_MASK and "C"), so a locale handle only has to be non-null.
static void *apple_newlocale(int mask, const char *name, void *base)
{
    (void)mask; (void)name;
    static char c_locale;
    return base ? base : &c_locale;
}
static double apple_strtod_l(const char *s, char **end, void *locale) { (void)locale; return strtod(s, end); }
static int apple_vfprintf(FILE *f, const char *fmt, void *apple_ap)
{
    VA_FROM_STACK(ap, apple_ap);
    return vfprintf(f, fmt, ap);
}

extern int apple_printf(const char *, ...);
extern int apple_fprintf(FILE *, const char *, ...);
extern int apple_snprintf(char *, size_t, const char *, ...);
extern int apple_asprintf(char **, const char *, ...);
extern int apple_sscanf(const char *, const char *, ...);
extern void apple_syslog(int, const char *, ...);
extern int apple_open(const char *, int, ...);
extern int apple_fcntl(int, int, ...);
extern int apple_ioctl(int, unsigned long, ...);

// ---------------------------------------------------------------------------
// open and fcntl

static const struct { int apple; int host; } k_open_flags[] = {
    {0x0004, O_NONBLOCK}, {0x0008, O_APPEND}, {0x0080, O_SYNC}, {0x0100, O_NOFOLLOW}, {0x0200, O_CREAT},
    {0x0400, O_TRUNC}, {0x0800, O_EXCL}, {0x8000, O_NOCTTY}, {0x00100000, O_DIRECTORY},
    {0x01000000, O_CLOEXEC}, {0x00400000, O_DSYNC},
};

static int oflags_from_apple(int f)
{
    int host = f & 3;
    for (size_t i = 0; i < SHIM_COUNT(k_open_flags); i++)
        if (f & k_open_flags[i].apple)
            host |= k_open_flags[i].host;
    return host;
}

static int oflags_to_apple(int f)
{
    int apple = f & 3;
    for (size_t i = 0; i < SHIM_COUNT(k_open_flags); i++)
        if (f & k_open_flags[i].host)
            apple |= k_open_flags[i].apple;
    return apple;
}

int shim_open_stack(const char *path, int flags, void *slots)
{
    mode_t mode = (mode_t)*(uint64_t *)slots;
    char fixed[PATH_MAX];
    path = bundle_path(path, fixed, sizeof fixed);
    int fd = open(path, oflags_from_apple(flags), mode);
    if (fd < 0)
        note_missing("open", path);
    else
        note_found("open", path);
    TRACE("open(%s, %#x) = %d", path, flags, fd);
    return fd;
}

struct apple_flock {
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int16_t l_type;   // Darwin: F_RDLCK 1, F_UNLCK 2, F_WRLCK 3
    int16_t l_whence;
};

static int lock_type_from_apple(int t) { return t == 1 ? F_RDLCK : t == 3 ? F_WRLCK : F_UNLCK; }
static int lock_type_to_apple(int t) { return t == F_RDLCK ? 1 : t == F_WRLCK ? 3 : 2; }

int shim_fcntl_stack(int fd, int cmd, void *slots)
{
    uint64_t arg = *(uint64_t *)slots;
    switch (cmd) {
    case 0: return fcntl(fd, F_DUPFD, (int)arg);
    case 1: return fcntl(fd, F_GETFD);
    case 2: return fcntl(fd, F_SETFD, (int)arg);
    case 3: {
        int f = fcntl(fd, F_GETFL);
        return f < 0 ? f : oflags_to_apple(f);
    }
    case 4: return fcntl(fd, F_SETFL, oflags_from_apple((int)arg));
    case 7: case 8: case 9: {
        struct apple_flock *a = (struct apple_flock *)(uintptr_t)arg;
        struct flock h = { .l_type = (short)lock_type_from_apple(a->l_type), .l_whence = a->l_whence,
                           .l_start = a->l_start, .l_len = a->l_len, .l_pid = a->l_pid };
        int host_cmd = cmd == 7 ? F_GETLK : cmd == 8 ? F_SETLK : F_SETLKW;
        int rc = fcntl(fd, host_cmd, &h);
        if (rc == 0 && cmd == 7) {
            a->l_type = (int16_t)lock_type_to_apple(h.l_type);
            a->l_start = h.l_start;
            a->l_len = h.l_len;
            a->l_pid = h.l_pid;
        }
        return rc;
    }
    case 48: return 0;                     // F_NOCACHE
    case 51: return fsync(fd);             // F_FULLFSYNC
    case 50: {                             // F_GETPATH
        char link[64];
        snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, (char *)(uintptr_t)arg, 1023);
        if (n < 0)
            return -1;
        ((char *)(uintptr_t)arg)[n] = 0;
        return 0;
    }
    case 67: return fcntl(fd, F_DUPFD_CLOEXEC, (int)arg);
    default:
        TRACE("fcntl unsupported cmd %d", cmd);
        return fail(EINVAL);
    }
}

int shim_ioctl_stack(int fd, unsigned long request, void *slots)
{
    void *arg = (void *)(uintptr_t)*(uint64_t *)slots;
    switch ((uint32_t)request) {
    case 0x4004667fu: return ioctl(fd, FIONREAD, arg);
    case 0x8004667eu: return ioctl(fd, FIONBIO, arg);
    case 0x40087468u: return ioctl(fd, TIOCGWINSZ, arg);
    case 0x20006601u: return fcntl(fd, F_SETFD, FD_CLOEXEC);  // FIOCLEX
    default:
        TRACE("ioctl unsupported request %#lx", request);
        return fail(ENOTTY);
    }
}

// ---------------------------------------------------------------------------
// stat family

struct apple_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct apple_stat {
    int32_t st_dev;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint64_t st_ino;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t st_rdev;
    struct apple_timespec st_atim, st_mtim, st_ctim, st_birthtim;
    int64_t st_size;
    int64_t st_blocks;
    int32_t st_blksize;
    uint32_t st_flags;
    uint32_t st_gen;
    int32_t st_lspare;
    int64_t st_qspare[2];
};

_Static_assert(sizeof(struct apple_stat) == 144, "Darwin struct stat is 144 bytes");

static void stat_to_apple(const struct stat *h, struct apple_stat *a)
{
    memset(a, 0, sizeof *a);
    a->st_dev = (int32_t)h->st_dev;
    a->st_mode = (uint16_t)h->st_mode;
    a->st_nlink = (uint16_t)h->st_nlink;
    a->st_ino = h->st_ino;
    a->st_uid = h->st_uid;
    a->st_gid = h->st_gid;
    a->st_rdev = (int32_t)h->st_rdev;
    a->st_atim = (struct apple_timespec){ h->st_atim.tv_sec, h->st_atim.tv_nsec };
    a->st_mtim = (struct apple_timespec){ h->st_mtim.tv_sec, h->st_mtim.tv_nsec };
    a->st_ctim = (struct apple_timespec){ h->st_ctim.tv_sec, h->st_ctim.tv_nsec };
    a->st_birthtim = a->st_ctim;
    a->st_size = h->st_size;
    a->st_blocks = h->st_blocks;
    a->st_blksize = (int32_t)h->st_blksize;
}

static int apple_stat(const char *path, struct apple_stat *out)
{
    struct stat h;
    char fixed[PATH_MAX];
    path = bundle_path(path, fixed, sizeof fixed);
    int rc = stat(path, &h);
    if (rc == 0) {
        stat_to_apple(&h, out);
        note_found("stat", path);
    } else
        note_missing("stat", path);
    return rc;
}

static int apple_lstat(const char *path, struct apple_stat *out)
{
    struct stat h;
    char fixed[PATH_MAX];
    path = bundle_path(path, fixed, sizeof fixed);
    int rc = lstat(path, &h);
    if (rc == 0) {
        stat_to_apple(&h, out);
        note_found("lstat", path);
    } else
        note_missing("lstat", path);
    return rc;
}

static int apple_fstat(int fd, struct apple_stat *out)
{
    struct stat h;
    int rc = fstat(fd, &h);
    if (rc == 0)
        stat_to_apple(&h, out);
    return rc;
}

struct apple_statfs {
    uint32_t f_bsize;
    int32_t f_iosize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t f_fsid[2];
    uint32_t f_owner;
    uint32_t f_type;
    uint32_t f_flags;
    uint32_t f_fssubtype;
    char f_fstypename[16];
    char f_mntonname[1024];
    char f_mntfromname[1024];
    uint32_t f_flags_ext;
    uint32_t f_reserved[7];
};

static void statfs_to_apple(const struct statfs *h, struct apple_statfs *a, const char *path)
{
    memset(a, 0, sizeof *a);
    a->f_bsize = (uint32_t)h->f_bsize;
    a->f_iosize = (int32_t)h->f_bsize;
    a->f_blocks = h->f_blocks;
    a->f_bfree = h->f_bfree;
    a->f_bavail = h->f_bavail;
    a->f_files = h->f_files;
    a->f_ffree = h->f_ffree;
    a->f_type = (uint32_t)h->f_type;
    snprintf(a->f_fstypename, sizeof a->f_fstypename, "apfs");
    snprintf(a->f_mntonname, sizeof a->f_mntonname, "%s", path ? path : "/");
}

static int apple_statfs(const char *path, struct apple_statfs *out)
{
    struct statfs h;
    int rc = statfs(path, &h);
    if (rc == 0)
        statfs_to_apple(&h, out, path);
    return rc;
}

static int apple_fstatfs(int fd, struct apple_statfs *out)
{
    struct statfs h;
    int rc = fstatfs(fd, &h);
    if (rc == 0)
        statfs_to_apple(&h, out, NULL);
    return rc;
}

static int apple_getfsstat(struct apple_statfs *buf, int size, int mode)
{
    (void)mode;
    if (!buf || size < (int)sizeof *buf)
        return 1;
    return apple_statfs("/", buf) == 0 ? 1 : -1;
}

// ---------------------------------------------------------------------------
// Directories

struct apple_dirent {
    uint64_t d_ino;
    uint64_t d_seekoff;
    uint16_t d_reclen;
    uint16_t d_namlen;
    uint8_t d_type;
    char d_name[1024];
};

struct apple_dir {
    DIR *host;
    struct apple_dirent entry;
};

static char *apple_realpath(const char *path, char *resolved)
{
    char fixed[PATH_MAX];
    return realpath(bundle_path(path, fixed, sizeof fixed), resolved);
}

static FILE *apple_fopen(const char *path, const char *mode)
{
    char fixed[PATH_MAX];
    return fopen(bundle_path(path, fixed, sizeof fixed), mode);
}

static struct apple_dir *apple_opendir(const char *path)
{
    char fixed[PATH_MAX];
    path = bundle_path(path, fixed, sizeof fixed);
    DIR *d = opendir(path);
    if (!d) {
        note_missing("opendir", path);
        return NULL;
    }
    struct apple_dir *a = calloc(1, sizeof *a);
    a->host = d;
    return a;
}

static int apple_dirfd(struct apple_dir *a) { return dirfd(a->host); }

static struct apple_dirent *apple_readdir(struct apple_dir *a)
{
    struct dirent *e = readdir(a->host);
    if (!e)
        return NULL;
    a->entry.d_ino = e->d_ino;
    a->entry.d_seekoff = (uint64_t)e->d_off;
    a->entry.d_type = e->d_type; // DT_* values match
    a->entry.d_namlen = (uint16_t)strlen(e->d_name);
    memcpy(a->entry.d_name, e->d_name, a->entry.d_namlen + 1);
    a->entry.d_reclen = (uint16_t)sizeof a->entry;
    return &a->entry;
}

static int apple_closedir(struct apple_dir *a)
{
    int rc = closedir(a->host);
    free(a);
    return rc;
}

// ---------------------------------------------------------------------------
// Memory

#define APPLE_MAP_FIXED 0x0010
#define APPLE_MAP_NORESERVE 0x0040
#define APPLE_MAP_JIT 0x0800
#define APPLE_MAP_ANON 0x1000

static void *apple_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    int host = flags & 3;
    if (flags & APPLE_MAP_FIXED)
        host |= MAP_FIXED;
    if (flags & APPLE_MAP_NORESERVE)
        host |= MAP_NORESERVE;
    if (flags & APPLE_MAP_ANON)
        host |= MAP_ANONYMOUS;
    // Darwin passes VM tags through fd for anonymous memory (VM_MAKE_TAG).
    if (flags & APPLE_MAP_ANON)
        fd = -1;
    void *p = mmap(addr, len, prot, host, fd, off);
    if (p == MAP_FAILED)
        macho_log("mem", "mmap(%p, %#zx, prot %d, flags %#x) failed: %s", addr, len, prot, flags, strerror(errno));
    return p;
}

// Areas created by apple_vm_remap (the NativeAOT thunk pool), see below.
static struct { uintptr_t base; size_t size; } g_remapped[32];
static int g_remapped_count;

static int apple_mprotect(void *addr, size_t len, int prot)
{
    uintptr_t a = (uintptr_t)addr;
    for (int i = 0; i < g_remapped_count; i++) {
        uintptr_t b = g_remapped[i].base;
        size_t s = g_remapped[i].size;
        if (a >= b && a + len <= b + s && !(prot & PROT_EXEC)) {
            // PalMarkThunksAsValidCallTargets computes the data half with a 4 KiB page size in
            // a pool laid out for 16 KiB pages; the intended target is [base + size, base + 2*size).
            return mprotect((void *)(b + s), s, prot);
        }
    }
    int rc = mprotect(addr, len, prot);
    if (rc != 0)
        macho_log("mem", "mprotect(%p, %#zx, %d) failed: %s", addr, len, prot, strerror(errno));
    return rc;
}

static int apple_madvise(void *addr, size_t len, int advice)
{
    // Darwin: NORMAL 0, RANDOM 1, SEQUENTIAL 2, WILLNEED 3, DONTNEED 4, FREE 5,
    // ZERO_WIRED_PAGES 6, FREE_REUSABLE 7, FREE_REUSE 8.
    switch (advice) {
    case 0: case 1: case 2: case 3: case 4: return madvise(addr, len, advice);
    case 5: case 7: return madvise(addr, len, MADV_DONTNEED);
    default: return 0;
    }
}

static int apple_msync(void *addr, size_t len, int flags)
{
    int host = ((flags & 1) ? MS_ASYNC : 0) | ((flags & 2) ? MS_INVALIDATE : 0) | ((flags & 0x10) ? MS_SYNC : 0);
    return msync(addr, len, host);
}

// Mach virtual memory. Darwin arm64 pages are 16 KiB and the runtime relies on that
// alignment (the thunk pool finds its data block with "& ~0x3fff").

#define APPLE_VM_FLAGS_ANYWHERE 0x0001
#define APPLE_VM_FLAGS_OVERWRITE 0x4000
#define APPLE_VM_PAGE 0x4000ull
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static uint64_t vm_round(uint64_t n) { return (n + APPLE_VM_PAGE - 1) & ~(APPLE_VM_PAGE - 1); }

static void *mmap_aligned(size_t size, size_t align, int prot)
{
    size_t span = size + align;
    uint8_t *raw = mmap(NULL, span, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED)
        return MAP_FAILED;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + align - 1) & ~(uintptr_t)(align - 1));
    if (p != raw)
        munmap(raw, (size_t)(p - raw));
    size_t tail = (size_t)((raw + span) - (p + size));
    if (tail)
        munmap(p + size, tail);
    return p;
}

static void *vm_map_at(uint64_t *addr, uint64_t size, int flags, size_t align)
{
    if (flags & APPLE_VM_FLAGS_ANYWHERE)
        return mmap_aligned(size, align, PROT_READ | PROT_WRITE);
    int fixed = (flags & APPLE_VM_FLAGS_OVERWRITE) ? MAP_FIXED : MAP_FIXED_NOREPLACE;
    void *p = mmap((void *)(uintptr_t)*addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | fixed, -1, 0);
    if (p != MAP_FAILED && (uintptr_t)p != *addr) {
        munmap(p, size);
        errno = EEXIST;
        return MAP_FAILED;
    }
    return p;
}

static int apple_vm_allocate(unsigned int task, uint64_t *addr, uint64_t size, int flags)
{
    (void)task;
    size = vm_round(size);
    void *p = vm_map_at(addr, size, flags, APPLE_VM_PAGE);
    if (p == MAP_FAILED) {
        macho_log("mem", "vm_allocate(%#llx, flags %#x) failed: %s", (unsigned long long)size, flags, strerror(errno));
        return 3; // KERN_NO_SPACE
    }
    *addr = (uint64_t)(uintptr_t)p;
    return 0;
}

static int apple_vm_deallocate(unsigned int task, uint64_t addr, uint64_t size)
{
    (void)task;
    if (size)
        munmap((void *)(uintptr_t)addr, vm_round(size));
    return 0;
}

// The only caller is the NativeAOT thunk pool (PalAllocateThunksFromTemplate), which maps
// its immutable code template from __TEXT right before a writable data block. The image is
// private anonymous memory, so the pages cannot be shared; a read-execute copy is equivalent.
static int apple_vm_remap(unsigned int target, uint64_t *addr, uint64_t size, uint64_t mask, int flags,
                          unsigned int src_task, uint64_t src, int copy, int *cur, int *max, int inherit)
{
    (void)target; (void)src_task; (void)copy; (void)inherit;
    size = vm_round(size);
    size_t align = mask + 1 > APPLE_VM_PAGE ? (size_t)(mask + 1) : APPLE_VM_PAGE;
    void *dst = vm_map_at(addr, size, flags, align);
    if (dst == MAP_FAILED) {
        macho_log("mem", "vm_remap(%#llx <- %#llx, %#llx, flags %#x) failed: %s", (unsigned long long)*addr,
                  (unsigned long long)src, (unsigned long long)size, flags, strerror(errno));
        return 3; // KERN_NO_SPACE
    }
    memcpy(dst, (const void *)(uintptr_t)src, size);
    if (mprotect(dst, size, PROT_READ | PROT_EXEC) != 0) {
        macho_log("mem", "vm_remap: mprotect R-X failed: %s", strerror(errno));
        return 2; // KERN_PROTECTION_FAILURE
    }
    __builtin___clear_cache((char *)dst, (char *)dst + size);
    if (g_remapped_count < 32) {
        g_remapped[g_remapped_count].base = (uintptr_t)dst;
        g_remapped[g_remapped_count].size = size;
        g_remapped_count++;
    }
    macho_log("mem", "vm_remap %#llx bytes %#llx -> %p", (unsigned long long)size, (unsigned long long)src, dst);
    *addr = (uint64_t)(uintptr_t)dst;
    if (cur)
        *cur = 5; // VM_PROT_READ | VM_PROT_EXECUTE
    if (max)
        *max = 7;
    return 0;
}

static int apple_mach_port_deallocate(unsigned int task, unsigned int name) { (void)task; (void)name; return 0; }
static int apple_task_set_exception_ports(unsigned int task, unsigned int mask, unsigned int port, int behavior, int flavor)
{
    (void)task; (void)mask; (void)port; (void)behavior; (void)flavor;
    return 0;
}
static const char *apple_mach_error_string(int e) { (void)e; return "mach error"; }
static void apple_mach_error(const char *s, int e) { fprintf(stderr, "%s mach error %#x\n", s, e); }

// FlushProcessWriteBuffers enumerates threads and reads their registers to force
// a memory barrier on each core. membarrier gives the same guarantee on Linux.
static int g_membarrier_ready;

static int apple_task_threads(unsigned int task, unsigned int **threads, unsigned int *count)
{
    (void)task;
    if (!g_membarrier_ready) {
        g_membarrier_ready = syscall(__NR_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0) == 0 ? 1 : -1;
    }
    if (g_membarrier_ready > 0)
        syscall(__NR_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);
    else
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *threads = NULL;
    *count = 0;
    return 0;
}

static int apple_thread_get_register_pointer_values(unsigned int thread, uint64_t *sp, size_t *length, uint64_t *values)
{
    (void)thread; (void)values;
    if (sp)
        *sp = 0;
    if (length)
        *length = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// System information. The program believes it runs on macOS, so report Darwin.

static int apple_sysconf_name(int name)
{
    switch (name) {
    case 1: return _SC_ARG_MAX;
    case 2: return _SC_CHILD_MAX;
    case 3: return _SC_CLK_TCK;
    case 4: return _SC_NGROUPS_MAX;
    case 5: return _SC_OPEN_MAX;
    case 29: return _SC_PAGESIZE;
    case 57: return _SC_NPROCESSORS_CONF;
    case 58: return _SC_NPROCESSORS_ONLN;
    case 200: return _SC_PHYS_PAGES;
    case 71: return _SC_GETPW_R_SIZE_MAX;
    case 70: return _SC_GETGR_R_SIZE_MAX;
    default: return -1;
    }
}

static long apple_sysconf(int name)
{
    int host = apple_sysconf_name(name);
    if (host < 0) {
        TRACE("sysconf unsupported %d", name);
        errno = EINVAL;
        return -1;
    }
    return sysconf(host);
}

struct apple_utsname {
    char sysname[256];
    char nodename[256];
    char release[256];
    char version[256];
    char machine[256];
};

static int apple_uname(struct apple_utsname *a)
{
    struct utsname h;
    if (uname(&h) != 0)
        return -1;
    memset(a, 0, sizeof *a);
    snprintf(a->sysname, sizeof a->sysname, "Darwin");
    snprintf(a->nodename, sizeof a->nodename, "%s", h.nodename);
    snprintf(a->release, sizeof a->release, "24.0.0");
    snprintf(a->version, sizeof a->version, "Darwin Kernel Version 24.0.0 (Hyprism on %s %s)", h.sysname, h.release);
    snprintf(a->machine, sizeof a->machine, "arm64");
    return 0;
}

static int rlimit_from_apple(int r)
{
    switch (r) {
    case 0: return RLIMIT_CPU;
    case 1: return RLIMIT_FSIZE;
    case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE;
    case 5: return RLIMIT_AS;
    case 6: return RLIMIT_MEMLOCK;
    case 7: return RLIMIT_NPROC;
    case 8: return RLIMIT_NOFILE;
    default: return -1;
    }
}

static int apple_getrlimit(int r, struct rlimit *out)
{
    int host = rlimit_from_apple(r);
    return host < 0 ? fail(EINVAL) : getrlimit(host, out);
}

static int apple_setrlimit(int r, const struct rlimit *in)
{
    int host = rlimit_from_apple(r);
    return host < 0 ? fail(EINVAL) : setrlimit(host, in);
}

static int write_sysctl_value(void *old, size_t *oldlen, const void *value, size_t size)
{
    if (old) {
        if (!oldlen || *oldlen < size)
            return fail(ENOMEM);
        memcpy(old, value, size);
    }
    if (oldlen)
        *oldlen = size;
    return 0;
}

static int sysctl_int(void *old, size_t *oldlen, int64_t v)
{
    if (oldlen && *oldlen == 8)
        return write_sysctl_value(old, oldlen, &v, 8);
    int32_t i = (int32_t)v;
    return write_sysctl_value(old, oldlen, &i, 4);
}

static int sysctl_string(void *old, size_t *oldlen, const char *s)
{
    return write_sysctl_value(old, oldlen, s, strlen(s) + 1);
}

static int apple_sysctlbyname(const char *name, void *old, size_t *oldlen, const void *new_value, size_t newlen)
{
    (void)new_value; (void)newlen;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    struct sysinfo si;
    sysinfo(&si);
    TRACE("sysctlbyname(%s)", name);
    if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.activecpu") || !strcmp(name, "hw.logicalcpu") ||
        !strcmp(name, "hw.physicalcpu") || !strcmp(name, "hw.logicalcpu_max") || !strcmp(name, "hw.physicalcpu_max"))
        return sysctl_int(old, oldlen, ncpu);
    if (!strcmp(name, "hw.memsize") || !strcmp(name, "hw.memsize_usable")) {
        int64_t mem = (int64_t)si.totalram * si.mem_unit;
        return write_sysctl_value(old, oldlen, &mem, 8);
    }
    if (!strcmp(name, "hw.pagesize") || !strcmp(name, "vm.pagesize"))
        return sysctl_int(old, oldlen, sysconf(_SC_PAGESIZE));
    if (!strcmp(name, "hw.cachelinesize"))
        return sysctl_int(old, oldlen, 64);
    if (!strcmp(name, "hw.l1dcachesize"))
        return sysctl_int(old, oldlen, 64 * 1024);
    if (!strcmp(name, "hw.l2cachesize"))
        return sysctl_int(old, oldlen, 4 * 1024 * 1024);
    if (!strcmp(name, "hw.l3cachesize"))
        return fail(ENOENT);
    if (!strcmp(name, "hw.perflevel0.l2cachesize") || !strcmp(name, "hw.perflevel1.l2cachesize"))
        return sysctl_int(old, oldlen, 4 * 1024 * 1024);
    if (!strcmp(name, "hw.nperflevels"))
        return sysctl_int(old, oldlen, 1);
    if (!strcmp(name, "hw.perflevel0.logicalcpu") || !strcmp(name, "hw.perflevel0.physicalcpu"))
        return sysctl_int(old, oldlen, ncpu);
    if (!strcmp(name, "kern.osrelease"))
        return sysctl_string(old, oldlen, "24.0.0");
    if (!strcmp(name, "kern.osproductversion"))
        return sysctl_string(old, oldlen, "15.0");
    if (!strcmp(name, "kern.ostype"))
        return sysctl_string(old, oldlen, "Darwin");
    if (!strcmp(name, "hw.machine"))
        return sysctl_string(old, oldlen, "arm64");
    if (!strcmp(name, "hw.model"))
        return sysctl_string(old, oldlen, "Android");
    if (!strcmp(name, "machdep.cpu.brand_string"))
        return sysctl_string(old, oldlen, "Android arm64");
    if (!strcmp(name, "kern.hostname")) {
        char host[256];
        gethostname(host, sizeof host);
        return sysctl_string(old, oldlen, host);
    }
    if (!strcmp(name, "sysctl.proc_translated"))
        return sysctl_int(old, oldlen, 0);
    if (!strncmp(name, "hw.optional.", 12))
        return sysctl_int(old, oldlen, 1);
    if (!strcmp(name, "kern.boottime")) {
        struct { int64_t sec; int32_t usec; int32_t pad; } tv = { time(NULL) - si.uptime, 0, 0 };
        return write_sysctl_value(old, oldlen, &tv, sizeof tv);
    }
    if (getenv("MACHO_VERBOSE"))
        fprintf(stderr, "[shim] sysctlbyname unknown %s\n", name);
    return fail(ENOENT);
}

static int apple_sysctl(int *mib, unsigned int count, void *old, size_t *oldlen, void *new_value, size_t newlen)
{
    // CTL_KERN 1, CTL_HW 6
    if (count >= 2 && mib[0] == 6) {
        switch (mib[1]) {
        case 3: return apple_sysctlbyname("hw.ncpu", old, oldlen, new_value, newlen);
        case 7: return apple_sysctlbyname("hw.pagesize", old, oldlen, new_value, newlen);
        case 24: return apple_sysctlbyname("hw.memsize", old, oldlen, new_value, newlen);
        case 25: return apple_sysctlbyname("hw.activecpu", old, oldlen, new_value, newlen);
        }
    }
    if (count >= 2 && mib[0] == 1) {
        switch (mib[1]) {
        case 1: return apple_sysctlbyname("kern.ostype", old, oldlen, new_value, newlen);
        case 2: return apple_sysctlbyname("kern.osrelease", old, oldlen, new_value, newlen);
        case 21: return apple_sysctlbyname("kern.boottime", old, oldlen, new_value, newlen);
        case 10: return apple_sysctlbyname("kern.hostname", old, oldlen, new_value, newlen);
        case 1000: {
            // Percentage of memory available, as Darwin's memorystatus reports it. Android keeps
            // most RAM as reclaimable cache, so use MemAvailable rather than free pages.
            int32_t level = 50;
            FILE *mi = fopen("/proc/meminfo", "r");
            if (mi) {
                char line[128];
                long long total = 0, avail = 0;
                while (fgets(line, sizeof line, mi)) {
                    sscanf(line, "MemTotal: %lld", &total);
                    sscanf(line, "MemAvailable: %lld", &avail);
                }
                fclose(mi);
                if (total > 0)
                    level = (int32_t)(avail * 100 / total);
            }
            return write_sysctl_value(old, oldlen, &level, sizeof level);
        }
        }
    }
    TRACE("sysctl unknown mib %d.%d (count %u)", count > 0 ? mib[0] : -1, count > 1 ? mib[1] : -1, count);
    return fail(ENOENT);
}

static int apple_sysctlnametomib(const char *name, int *mib, size_t *count)
{
    TRACE("sysctlnametomib(%s)", name);
    static const struct { const char *name; int mib[3]; size_t count; } known[] = {
        { "kern.memorystatus_level", { 1, 1000 }, 2 }, // private id answered by apple_sysctl
        { "kern.proc.pid", { 1, 14, 1 }, 3 },
        { "hw.ncpu", { 6, 3 }, 2 },
        { "hw.memsize", { 6, 24 }, 2 },
    };
    for (size_t i = 0; i < SHIM_COUNT(known); i++) {
        if (strcmp(name, known[i].name) != 0)
            continue;
        if (mib) {
            if (*count < known[i].count)
                return fail(ENOMEM);
            memcpy(mib, known[i].mib, known[i].count * sizeof(int));
        }
        *count = known[i].count; // a NULL mib asks only for the length
        return 0;
    }
    return fail(ENOENT);
}

// ---------------------------------------------------------------------------
// Users

struct apple_passwd {
    char *pw_name;
    char *pw_passwd;
    uint32_t pw_uid;
    uint32_t pw_gid;
    int64_t pw_change;
    char *pw_class;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
    int64_t pw_expire;
};

static int passwd_to_apple(struct passwd *h, struct apple_passwd *a, char *buf, size_t len, struct apple_passwd **result)
{
    if (!h) {
        *result = NULL;
        return 0;
    }
    const char *home = getenv("HOME") ? getenv("HOME") : h->pw_dir;
    size_t need = strlen(h->pw_name) + strlen(home) + 16;
    if (len < need)
        return 34; // ERANGE
    char *p = buf;
    a->pw_name = strcpy(p, h->pw_name); p += strlen(p) + 1;
    a->pw_dir = strcpy(p, home); p += strlen(p) + 1;
    a->pw_shell = strcpy(p, "/bin/sh");
    a->pw_passwd = a->pw_class = a->pw_gecos = (char *)"";
    a->pw_uid = h->pw_uid;
    a->pw_gid = h->pw_gid;
    a->pw_change = a->pw_expire = 0;
    *result = a;
    return 0;
}

static int apple_getpwuid_r(uid_t uid, struct apple_passwd *a, char *buf, size_t len, struct apple_passwd **result)
{
    return passwd_to_apple(getpwuid(uid), a, buf, len, result);
}

static int apple_getpwnam_r(const char *name, struct apple_passwd *a, char *buf, size_t len, struct apple_passwd **result)
{
    return passwd_to_apple(getpwnam(name), a, buf, len, result);
}

// ---------------------------------------------------------------------------
// Misc

// APFS clones do not exist here; EXDEV makes callers (.NET File.Copy) fall back to a copy.
static int apple_clonefile(const char *src, const char *dst, uint32_t flags)
{
    (void)src; (void)dst; (void)flags;
    return fail(EXDEV);
}

// fcopyfile(from, to, state, flags): COPYFILE_STAT 0x2, COPYFILE_DATA 0x8.
static int apple_fcopyfile(int from, int to, void *state, uint32_t flags)
{
    (void)state;
    if (flags & 0x8) {
        char buf[1 << 16];
        off_t off = 0;
        for (;;) {
            ssize_t n = pread(from, buf, sizeof buf, off);
            if (n < 0)
                return -1;
            if (n == 0)
                break;
            for (ssize_t done = 0; done < n;) {
                ssize_t w = pwrite(to, buf + done, (size_t)(n - done), off + done);
                if (w < 0)
                    return -1;
                done += w;
            }
            off += n;
        }
        if (ftruncate(to, off) != 0)
            return -1;
    }
    if (flags & 0x2) {
        struct stat st;
        if (fstat(from, &st) == 0) {
            fchmod(to, st.st_mode & 07777);
            struct timespec times[2] = { st.st_atim, st.st_mtim };
            futimens(to, times);
        }
    }
    return 0;
}

// Darwin struct termios differs; report that no terminal is attached.
static int apple_not_a_tty(int fd, ...)
{
    (void)fd;
    return fail(ENOTTY);
}

extern const char *g_executable_path;

static int apple_dladdr(const void *addr, Dl_info *info)
{
    struct macho_image *main = macho_main_image();
    if ((const uint8_t *)addr >= main->base && (const uint8_t *)addr < main->base + main->size) {
        info->dli_fname = g_executable_path;
        info->dli_fbase = main->base;
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
        return 1;
    }
    return dladdr(addr, info);
}

// ---------------------------------------------------------------------------

const struct shim posix_shims[] = {
    { "_printf", (void *)apple_printf },
    { "_fprintf", (void *)apple_fprintf },
    { "_snprintf", (void *)apple_snprintf },
    { "_asprintf", (void *)apple_asprintf },
    { "_sscanf", (void *)apple_sscanf },
    { "_syslog$DARWIN_EXTSN", (void *)apple_syslog },
    { "_vsnprintf", (void *)apple_vsnprintf },
    { "_vsnprintf_l", (void *)apple_vsnprintf_l },
    { "_newlocale", (void *)apple_newlocale },
    { "_strtod_l", (void *)apple_strtod_l },
    { "_vfprintf", (void *)apple_vfprintf },
    { "_open", (void *)apple_open },
    { "_open$NOCANCEL", (void *)apple_open },
    { "_fcntl$NOCANCEL", (void *)apple_fcntl },
    { "_fcntl", (void *)apple_fcntl },
    { "_ioctl", (void *)apple_ioctl },
    { "_stat", (void *)apple_stat },
    { "_lstat", (void *)apple_lstat },
    { "_fstat", (void *)apple_fstat },
    { "_statfs", (void *)apple_statfs },
    { "_fstatfs", (void *)apple_fstatfs },
    { "_getfsstat", (void *)apple_getfsstat },
    { "_opendir", (void *)apple_opendir },
    { "_dirfd", (void *)apple_dirfd },
    { "_readdir", (void *)apple_readdir },
    { "_closedir", (void *)apple_closedir },
    { "_mmap", (void *)apple_mmap },
    { "_munmap", (void *)munmap },
    { "_mprotect", (void *)apple_mprotect },
    { "_madvise", (void *)apple_madvise },
    { "_msync$NOCANCEL", (void *)apple_msync },
    { "_vm_allocate", (void *)apple_vm_allocate },
    { "_vm_deallocate", (void *)apple_vm_deallocate },
    { "_vm_remap", (void *)apple_vm_remap },
    { "_mach_port_deallocate", (void *)apple_mach_port_deallocate },
    { "_task_set_exception_ports", (void *)apple_task_set_exception_ports },
    { "_task_threads", (void *)apple_task_threads },
    { "_thread_get_register_pointer_values", (void *)apple_thread_get_register_pointer_values },
    { "_mach_error", (void *)apple_mach_error },
    { "_mach_error_string", (void *)apple_mach_error_string },
    { "_sysconf", (void *)apple_sysconf },
    { "_uname", (void *)apple_uname },
    { "_getrlimit", (void *)apple_getrlimit },
    { "_setrlimit", (void *)apple_setrlimit },
    { "_getrusage", (void *)getrusage },
    { "_sysctl", (void *)apple_sysctl },
    { "_sysctlbyname", (void *)apple_sysctlbyname },
    { "_sysctlnametomib", (void *)apple_sysctlnametomib },
    { "_getpwuid_r", (void *)apple_getpwuid_r },
    { "_getpwnam_r", (void *)apple_getpwnam_r },
    { "_fopen", (void *)apple_fopen }, { "_fileno", (void *)fileno },
    { "_fopen$DARWIN_EXTSN", (void *)fopen },
    { "_fclose", (void *)fclose },
    { "_fread", (void *)fread },
    { "_fwrite", (void *)fwrite }, { "_feof", (void *)feof }, { "_ferror", (void *)ferror },
    { "_fgets", (void *)fgets }, { "_perror", (void *)perror },
    { "_fflush", (void *)fflush },
    { "_fseek", (void *)fseek },
    { "_ftell", (void *)ftell },
    { "_rewind", (void *)rewind },
    { "_fputs", (void *)fputs },
    { "_getline", (void *)getline },
    { "_ftruncate", (void *)ftruncate },
    { "_futimens", (void *)futimens },
    { "_utimensat", (void *)utimensat },
    { "_flock", (void *)flock },
    { "_mkstemps", (void *)mkstemps },
    { "_realpath$DARWIN_EXTSN", (void *)apple_realpath },
    { "_gethostname", (void *)gethostname },
    { "_getsid", (void *)getsid },
    { "_setsid", (void *)setsid },
    { "_getpriority", (void *)getpriority },
    { "_setpriority", (void *)setpriority },
    { "_waitpid", (void *)waitpid },
    { "_waitpid$NOCANCEL", (void *)waitpid },
    { "_pread$NOCANCEL", (void *)pread },
    { "_pwrite$NOCANCEL", (void *)pwrite },
    { "_preadv$NOCANCEL", (void *)preadv },
    { "_pwritev$NOCANCEL", (void *)pwritev },
    { "_clonefile", (void *)apple_clonefile },
    { "_fcopyfile", (void *)apple_fcopyfile },
    { "_tcgetattr", (void *)apple_not_a_tty },
    { "_tcsetattr", (void *)apple_not_a_tty },
    { "_setuid", (void *)setuid },
    { "_setgid", (void *)setgid },
    { "_strftime", (void *)strftime },
    { "_dladdr", (void *)apple_dladdr },
};
const size_t posix_shims_count = SHIM_COUNT(posix_shims);
