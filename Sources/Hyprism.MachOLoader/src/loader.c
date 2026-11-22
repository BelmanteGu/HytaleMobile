// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Loads arm64 macOS Mach-O images (the executable and its bundled dylibs) into
// the current Linux/Android process and runs them natively. Imports from Darwin
// system libraries bind to shims, imports between bundled dylibs bind to their
// exports, and unknown functions trap with their name so the shim layer can be
// grown in the order the program needs it.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "macho.h"

#define LOG(...) macho_log("macho", __VA_ARGS__)
#define DIE(...) do { LOG(__VA_ARGS__); _exit(98); } while (0)

#ifdef HYPRISM_MACHO_LIBRARY
#include <android/log.h>
#endif

void macho_log(const char *tag, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef HYPRISM_MACHO_LIBRARY
    __android_log_print(ANDROID_LOG_INFO, "HytaleLoader", "[%s] %s", tag, buf);
#else
    fprintf(stderr, "[%s] %s\n", tag, buf);
#endif
}

#define MAX_IMAGES 64
static struct macho_image *g_images[MAX_IMAGES];
static size_t g_image_count;
static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;

struct macho_image *macho_main_image(void) { return g_images[0]; }

// ---------------------------------------------------------------------------
// Import names and stubs. Indexes are global across images.

static const char **g_import_names;
static size_t g_import_count, g_import_capacity;
static uint64_t *g_trace_targets;

static uint32_t register_import(const char *name)
{
    if (g_import_count == g_import_capacity) {
        g_import_capacity = g_import_capacity ? g_import_capacity * 2 : 1024;
        g_import_names = realloc(g_import_names, g_import_capacity * sizeof *g_import_names);
        g_trace_targets = realloc(g_trace_targets, g_import_capacity * sizeof *g_trace_targets);
    }
    g_import_names[g_import_count] = name;
    g_trace_targets[g_import_count] = 0;
    return (uint32_t)g_import_count++;
}

extern void macho_trap_entry(void);
extern void macho_trace_entry(void);
extern void *macho_tlv_get_addr(struct tlv_descriptor *);

// With MACHO_SOFT_TRAPS=1 a missing import returns 0 instead of exiting, which
// shows how far the program gets without it. Each name is reported once.
uint64_t macho_unresolved_trap(uint64_t index, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    static uint8_t reported[1 << 16];
    const char *name = index < g_import_count ? g_import_names[index] : "?";
    int soft = getenv("MACHO_SOFT_TRAPS") != NULL;
    if (!soft || index >= sizeof reported || !reported[index]) {
        LOG("UNIMPLEMENTED %s(x0=%#llx, x1=%#llx, x2=%#llx, x3=%#llx)%s", name,
            (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3,
            soft ? " -> 0" : "");
        if (index < sizeof reported)
            reported[index] = 1;
    }
    if (!soft)
        _exit(97);
    return 0;
}

void *macho_trace_call(uint64_t index, uint64_t a0, uint64_t a1, uint64_t a2)
{
    LOG("call %s(%#llx, %#llx, %#llx)", g_import_names[index], (unsigned long long)a0,
        (unsigned long long)a1, (unsigned long long)a2);
    return (void *)(uintptr_t)g_trace_targets[index];
}

static void *make_stub(uint32_t index, void (*handler)(void))
{
    static uint32_t *page;
    static size_t used, capacity;
    const size_t stub_words = 8; // movz, movk, ldr, br, nop, nop, .quad handler
    if (!page || used + stub_words > capacity) {
        capacity = 16384 / sizeof(uint32_t);
        page = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
            DIE("cannot allocate trap stubs: %s", strerror(errno));
        used = 0;
    }
    uint32_t *stub = page + used;
    stub[0] = 0xD2800000u | ((index & 0xFFFFu) << 5) | 16;          // movz x16, #lo
    stub[1] = 0xF2A00000u | (((index >> 16) & 0xFFFFu) << 5) | 16;  // movk x16, #hi, lsl #16
    stub[2] = 0x58000071u;                                          // ldr x17, #12 (-> stub[5])
    stub[3] = 0xD61F0220u;                                          // br x17
    stub[4] = 0xD503201Fu;                                          // nop
    stub[5] = 0xD503201Fu;                                          // (literal is at +20)
    // ldr literal at stub[2] reads pc+12 = stub[5]; place the handler there.
    memcpy(&stub[5], &handler, sizeof handler);
    used += stub_words;
    __builtin___clear_cache((char *)stub, (char *)(stub + stub_words));
    return stub;
}

// Data imports must point at memory, not code. These names are known data symbols;
// unknown ones get a zeroed block so reads do not fault while the shim grows.
static int looks_like_data(const char *name)
{
    static const char *const prefixes[] = {
        "_kCF", "_kSec", "_OBJC_CLASS_$_", "_OBJC_METACLASS_$_", "__NSConcrete", "___CFConstantStringClassReference",
        "___stack_chk_guard", "___stderrp", "___stdoutp", "___stdinp", "___gss_", "_mach_task_self_",
        "__DefaultRuneLocale", "__nw_", "_environ", "_vm_page_size", "_NSApp", "_NS", "_kAudio", "_kCG", "_kCT", "__dispatch_main_q",
    };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0)
            return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Load command helpers

#define FOREACH_CMD(header, lc)                                                                  \
    for (struct load_command *lc = (struct load_command *)((header) + 1), *lc##_end =            \
             (struct load_command *)((uint8_t *)((header) + 1) + (header)->sizeofcmds);           \
         lc < lc##_end; lc = (struct load_command *)((uint8_t *)lc + lc->cmdsize))

static int prot_from_vm(int vm)
{
    return ((vm & VM_PROT_READ) ? PROT_READ : 0) | ((vm & VM_PROT_WRITE) ? PROT_WRITE : 0) |
           ((vm & VM_PROT_EXECUTE) ? PROT_EXEC : 0);
}

void *macho_find_section(struct macho_image *image, const char *segname, const char *sectname, size_t *size)
{
    FOREACH_CMD(image->header, lc) {
        if (lc->cmd != LC_SEGMENT_64)
            continue;
        struct segment_command_64 *seg = (struct segment_command_64 *)lc;
        if (strncmp(seg->segname, segname, 16) != 0)
            continue;
        struct section_64 *sect = (struct section_64 *)(seg + 1);
        for (uint32_t i = 0; i < seg->nsects; i++) {
            if (strncmp(sect[i].sectname, sectname, 16) == 0) {
                if (size)
                    *size = sect[i].size;
                return (uint8_t *)(uintptr_t)(sect[i].addr + image->slide);
            }
        }
    }
    return NULL;
}

static struct segment_command_64 *segment_at(struct macho_image *image, uint32_t index)
{
    uint32_t i = 0;
    FOREACH_CMD(image->header, lc)
        if (lc->cmd == LC_SEGMENT_64 && i++ == index)
            return (struct segment_command_64 *)lc;
    return NULL;
}

// Pointer to linkedit data given its file offset (relative to the Mach-O slice).
static uint8_t *linkedit_data(struct macho_image *image, uint32_t fileoff)
{
    FOREACH_CMD(image->header, lc) {
        struct segment_command_64 *seg = (struct segment_command_64 *)lc;
        if (lc->cmd == LC_SEGMENT_64 && strncmp(seg->segname, "__LINKEDIT", 16) == 0)
            return (uint8_t *)(uintptr_t)(seg->vmaddr + image->slide) + (fileoff - seg->fileoff);
    }
    return NULL;
}

static const char *dylib_name(struct macho_image *image, uint32_t ordinal)
{
    uint32_t index = 0;
    FOREACH_CMD(image->header, lc) {
        if (lc->cmd == LC_LOAD_DYLIB || lc->cmd == LC_LOAD_WEAK_DYLIB || lc->cmd == LC_REEXPORT_DYLIB ||
            lc->cmd == LC_LOAD_UPWARD_DYLIB) {
            if (++index == ordinal) {
                struct dylib_command *d = (struct dylib_command *)lc;
                return (const char *)d + d->name_offset;
            }
        }
    }
    return NULL;
}

static uint64_t read_uleb(const uint8_t **p, const uint8_t *end)
{
    uint64_t v = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    return v;
}

static int64_t read_sleb(const uint8_t **p, const uint8_t *end)
{
    int64_t v = 0;
    int shift = 0;
    uint8_t b = 0;
    while (*p < end) {
        b = *(*p)++;
        v |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    if (shift < 64 && (b & 0x40))
        v |= -((int64_t)1 << shift);
    return v;
}

// ---------------------------------------------------------------------------
// Exports trie

static void *trie_lookup(struct macho_image *image, const char *symbol)
{
    if (!image->exports || !image->exports_size)
        return NULL;
    const uint8_t *start = image->exports, *end = start + image->exports_size, *p = start;
    const char *s = symbol;
    for (;;) {
        const uint8_t *node = p;
        uint64_t terminal = read_uleb(&p, end);
        const uint8_t *children = p + terminal;
        if (*s == 0 && terminal) {
            uint64_t flags = read_uleb(&p, end);
            if (flags & 0x08) { // EXPORT_SYMBOL_FLAGS_REEXPORT
                uint64_t ordinal = read_uleb(&p, end);
                const char *alias = (const char *)p;
                const char *lib = dylib_name(image, (uint32_t)ordinal);
                return shim_lookup(*alias ? alias : symbol, lib);
            }
            if (flags & 0x10) { // EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER
                read_uleb(&p, end);
                uint64_t resolver = read_uleb(&p, end);
                return ((void *(*)(void))(void *)(image->base + resolver))();
            }
            uint64_t offset = read_uleb(&p, end);
            if ((flags & 3) == 2) // EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE
                return (void *)(uintptr_t)offset;
            return image->base + offset;
        }
        (void)node;
        p = children;
        if (p >= end)
            return NULL;
        uint8_t count = *p++;
        const uint8_t *next = NULL;
        for (uint8_t i = 0; i < count; i++) {
            const char *edge = (const char *)p;
            size_t len = strlen(edge);
            p += len + 1;
            uint64_t child = read_uleb(&p, end);
            if (!next && strncmp(s, edge, len) == 0) {
                s += len;
                next = start + child;
            }
        }
        if (!next)
            return NULL;
        p = next;
    }
}

void *macho_image_symbol(struct macho_image *image, const char *mangled)
{
    return trie_lookup(image, mangled);
}

// ---------------------------------------------------------------------------
// Image mapping

static int open_slice(const char *path, off_t *slice_offset)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    uint32_t magic;
    *slice_offset = 0;
    if (pread(fd, &magic, 4, 0) != 4) {
        close(fd);
        return -1;
    }
    if (magic == 0xbebafecau) { // FAT_MAGIC, big-endian on disk
        uint32_t n;
        pread(fd, &n, 4, 4);
        n = __builtin_bswap32(n);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t arch[5]; // cputype, cpusubtype, offset, size, align
            pread(fd, arch, sizeof arch, 8 + 20 * i);
            if ((int32_t)__builtin_bswap32(arch[0]) == CPU_TYPE_ARM64) {
                *slice_offset = __builtin_bswap32(arch[2]);
                return fd;
            }
        }
        close(fd);
        errno = ENOEXEC;
        return -1;
    }
    return fd;
}

static struct macho_image *map_image(const char *path)
{
    off_t base_off;
    int fd = open_slice(path, &base_off);
    if (fd < 0) {
        LOG("cannot open %s: %s", path, strerror(errno));
        return NULL;
    }

    struct mach_header_64 header;
    if (pread(fd, &header, sizeof header, base_off) != sizeof header || header.magic != MH_MAGIC_64 ||
        header.cputype != CPU_TYPE_ARM64) {
        LOG("%s is not an arm64 Mach-O", path);
        close(fd);
        return NULL;
    }

    uint8_t *commands = malloc(header.sizeofcmds);
    if (pread(fd, commands, header.sizeofcmds, base_off + (off_t)sizeof header) != (ssize_t)header.sizeofcmds)
        DIE("cannot read load commands of %s", path);

    uint64_t low = UINT64_MAX, high = 0;
    for (uint8_t *p = commands; p < commands + header.sizeofcmds; p += ((struct load_command *)p)->cmdsize) {
        struct segment_command_64 *seg = (struct segment_command_64 *)p;
        if (seg->cmd != LC_SEGMENT_64 || seg->vmsize == 0 || strncmp(seg->segname, "__PAGEZERO", 16) == 0)
            continue;
        if (seg->vmaddr < low)
            low = seg->vmaddr;
        if (seg->vmaddr + seg->vmsize > high)
            high = seg->vmaddr + seg->vmsize;
    }

    // Copy segments into anonymous memory: Android forbids PROT_EXEC mappings of app data files.
    struct macho_image *image = calloc(1, sizeof *image);
    image->size = high - low;
    image->base = mmap(NULL, image->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (image->base == MAP_FAILED)
        DIE("cannot reserve %zu bytes: %s", image->size, strerror(errno));
    image->text_vmaddr = low;
    image->slide = (intptr_t)image->base - (intptr_t)low;
    image->path = strdup(path);

    for (uint8_t *p = commands; p < commands + header.sizeofcmds; p += ((struct load_command *)p)->cmdsize) {
        struct segment_command_64 *seg = (struct segment_command_64 *)p;
        if (seg->cmd != LC_SEGMENT_64 || seg->filesize == 0)
            continue;
        uint8_t *dest = (uint8_t *)(uintptr_t)(seg->vmaddr + image->slide);
        for (uint64_t done = 0; done < seg->filesize;) {
            ssize_t n = pread(fd, dest + done, seg->filesize - done, base_off + (off_t)(seg->fileoff + done));
            if (n <= 0)
                DIE("cannot read segment %.16s of %s: %s", seg->segname, path, strerror(errno));
            done += (uint64_t)n;
        }
    }
    close(fd);
    free(commands);
    image->header = (struct mach_header_64 *)image->base;

    FOREACH_CMD(image->header, lc) {
        if (lc->cmd == LC_DYLD_EXPORTS_TRIE) {
            struct linkedit_data_command *e = (struct linkedit_data_command *)lc;
            image->exports = linkedit_data(image, e->dataoff);
            image->exports_size = e->datasize;
        } else if (lc->cmd == LC_DYLD_INFO_ONLY || lc->cmd == LC_DYLD_INFO) {
            struct dyld_info_command *d = (struct dyld_info_command *)lc;
            image->dyld_info = d;
            if (d->export_size) {
                image->exports = linkedit_data(image, d->export_off);
                image->exports_size = d->export_size;
            }
        } else if (lc->cmd == LC_MAIN) {
            image->entry_offset = ((struct entry_point_command *)lc)->entryoff;
        }
    }
    LOG("mapped %s at %p (%zu KiB)", path, image->base, image->size >> 10);
    return image;
}

static void protect_segments(struct macho_image *image)
{
    FOREACH_CMD(image->header, lc) {
        if (lc->cmd != LC_SEGMENT_64)
            continue;
        struct segment_command_64 *seg = (struct segment_command_64 *)lc;
        if (seg->vmsize == 0 || strncmp(seg->segname, "__PAGEZERO", 16) == 0)
            continue;
        uint8_t *addr = (uint8_t *)(uintptr_t)(seg->vmaddr + image->slide);
        int prot = prot_from_vm(seg->initprot);
        if (strncmp(seg->segname, "__DATA_CONST", 16) == 0)
            prot = PROT_READ | PROT_WRITE; // read-only after fixups on macOS; keep it simple for now
        if (mprotect(addr, seg->vmsize, prot) != 0)
            DIE("mprotect %.16s: %s", seg->segname, strerror(errno));
        if (prot & PROT_EXEC)
            __builtin___clear_cache((char *)addr, (char *)addr + seg->vmsize);
    }
}

// ---------------------------------------------------------------------------
// Symbol resolution

static struct macho_image *load_dependency(struct macho_image *from, const char *install_name);

static int is_system_library(const char *name)
{
    return strncmp(name, "/usr/lib/", 9) == 0 || strncmp(name, "/System/Library/", 16) == 0;
}

static void *resolve_symbol(struct macho_image *image, int64_t ordinal, const char *name, int weak, uint32_t *index_out)
{
    const char *library = ordinal > 0 ? dylib_name(image, (uint32_t)ordinal) : NULL;
    void *target = NULL;
    if (library && !is_system_library(library)) {
        if (!dyld_replacement_symbol(library, name, &target)) {
            struct macho_image *dep = load_dependency(image, library);
            if (dep)
                target = trie_lookup(dep, name);
        }
    } else if (ordinal == 0) {
        target = trie_lookup(image, name);
    } else if (ordinal == -1 && g_image_count) {
        target = trie_lookup(g_images[0], name);
    } else if (ordinal == -2) {
        for (size_t i = 0; !target && i < g_image_count; i++)
            target = trie_lookup(g_images[i], name);
    }
    if (!target)
        target = shim_lookup(name, library);

    uint32_t index = register_import(name);
    if (index_out)
        *index_out = index;
    if (!target && weak)
        return NULL;
    if (!target) {
        if (looks_like_data(name)) {
            target = calloc(1, 4096);
            if (getenv("MACHO_VERBOSE"))
                LOG("data stub %s", name);
        } else {
            target = make_stub(index, macho_trap_entry);
            if (getenv("MACHO_VERBOSE"))
                LOG("unresolved %s", name);
        }
        image->trapped++;
    }
    if (getenv("MACHO_TRACE_ALL") && !looks_like_data(name) && strcmp(name, "__tlv_bootstrap") != 0 &&
        strcmp(name, "___chkstk_darwin") != 0 && strcmp(name, "___error") != 0 &&
        !(target >= (void *)image->base && target < (void *)(image->base + image->size))) {
        g_trace_targets[index] = (uint64_t)(uintptr_t)target;
        target = make_stub(index, macho_trace_entry);
    }
    return target;
}

// ---------------------------------------------------------------------------
// Chained fixups (LC_DYLD_CHAINED_FIXUPS)

static int apply_chained_fixups(struct macho_image *image)
{
    struct linkedit_data_command *fixups = NULL;
    FOREACH_CMD(image->header, lc)
        if (lc->cmd == LC_DYLD_CHAINED_FIXUPS)
            fixups = (struct linkedit_data_command *)lc;
    if (!fixups)
        return 0;

    uint8_t *data = linkedit_data(image, fixups->dataoff);
    struct dyld_chained_fixups_header *h = (struct dyld_chained_fixups_header *)data;
    if (h->imports_format != DYLD_CHAINED_IMPORT)
        DIE("unsupported imports format %u", h->imports_format);

    const uint32_t *imports = (const uint32_t *)(data + h->imports_offset);
    const char *symbols = (const char *)(data + h->symbols_offset);
    uint64_t *targets = calloc(h->imports_count ? h->imports_count : 1, sizeof *targets);
    for (uint32_t i = 0; i < h->imports_count; i++) {
        int32_t lib_ordinal = (int8_t)(imports[i] & 0xFF);
        int weak = (imports[i] >> 8) & 1;
        const char *name = symbols + (imports[i] >> 9);
        targets[i] = (uint64_t)(uintptr_t)resolve_symbol(image, lib_ordinal, name, weak, NULL);
    }

    uint8_t *starts = data + h->starts_offset;
    uint32_t seg_count = *(uint32_t *)starts;
    uint64_t rebases = 0, binds = 0;
    for (uint32_t s = 0; s < seg_count; s++) {
        uint32_t offset = ((uint32_t *)(starts + 4))[s];
        if (!offset)
            continue;
        struct dyld_chained_starts_in_segment *seg = (struct dyld_chained_starts_in_segment *)(starts + offset);
        if (seg->pointer_format != DYLD_CHAINED_PTR_64_OFFSET && seg->pointer_format != DYLD_CHAINED_PTR_64)
            DIE("unsupported chained pointer format %u", seg->pointer_format);
        for (uint32_t page = 0; page < seg->page_count; page++) {
            uint16_t start = seg->page_start[page];
            if (start == DYLD_CHAINED_PTR_START_NONE)
                continue;
            uint8_t *loc = image->base + seg->segment_offset + (uint64_t)page * seg->page_size + start;
            for (;;) {
                uint64_t raw = *(uint64_t *)loc;
                uint64_t next = (raw >> 51) & 0xFFF;
                if (raw >> 63) {
                    uint32_t ordinal = raw & 0xFFFFFF;
                    uint64_t addend = (raw >> 24) & 0xFF;
                    if (ordinal >= h->imports_count)
                        DIE("bind ordinal %u out of range", ordinal);
                    *(uint64_t *)loc = targets[ordinal] ? targets[ordinal] + addend : 0;
                    binds++;
                } else {
                    uint64_t target = raw & 0xFFFFFFFFFull;
                    uint64_t high8 = (raw >> 36) & 0xFF;
                    uint64_t value = seg->pointer_format == DYLD_CHAINED_PTR_64_OFFSET
                        ? (uint64_t)(uintptr_t)image->base + target
                        : target + (uint64_t)image->slide;
                    *(uint64_t *)loc = value | (high8 << 56);
                    rebases++;
                }
                if (!next)
                    break;
                loc += next * 4;
            }
        }
    }
    free(targets);
    LOG("%s: %u imports (%u unresolved), %llu rebases, %llu binds", strrchr(image->path, '/') + 1,
        h->imports_count, image->trapped, (unsigned long long)rebases, (unsigned long long)binds);
    return 1;
}

// ---------------------------------------------------------------------------
// Classic dyld info (LC_DYLD_INFO_ONLY): rebase and bind opcode streams.

static uint8_t *segment_address(struct macho_image *image, uint32_t index, uint64_t offset)
{
    struct segment_command_64 *seg = segment_at(image, index);
    return seg ? (uint8_t *)(uintptr_t)(seg->vmaddr + image->slide) + offset : NULL;
}

static void run_rebase(struct macho_image *image, const uint8_t *p, const uint8_t *end)
{
    uint8_t *addr = NULL;
    while (p < end) {
        uint8_t byte = *p++, op = byte & 0xF0, imm = byte & 0x0F;
        switch (op) {
        case 0x00: return; // DONE
        case 0x10: break;  // SET_TYPE_IMM (pointer)
        case 0x20: addr = segment_address(image, imm, read_uleb(&p, end)); break;
        case 0x30: addr += read_uleb(&p, end); break;
        case 0x40: addr += imm * 8; break;
        case 0x50: for (int i = 0; i < imm; i++, addr += 8) *(uint64_t *)addr += (uint64_t)image->slide; break;
        case 0x60: {
            uint64_t n = read_uleb(&p, end);
            for (uint64_t i = 0; i < n; i++, addr += 8) *(uint64_t *)addr += (uint64_t)image->slide;
            break;
        }
        case 0x70: *(uint64_t *)addr += (uint64_t)image->slide; addr += 8 + read_uleb(&p, end); break;
        case 0x80: {
            uint64_t n = read_uleb(&p, end), skip = read_uleb(&p, end);
            for (uint64_t i = 0; i < n; i++, addr += 8 + skip) *(uint64_t *)addr += (uint64_t)image->slide;
            break;
        }
        default: DIE("unknown rebase opcode %#x in %s", byte, image->path);
        }
    }
}

static void run_bind(struct macho_image *image, const uint8_t *p, const uint8_t *end, int lazy)
{
    uint8_t *addr = NULL;
    int64_t ordinal = 0, addend = 0;
    const char *name = NULL;
    int weak = 0;
    void *cached = NULL;
    while (p < end) {
        uint8_t byte = *p++, op = byte & 0xF0, imm = byte & 0x0F;
        switch (op) {
        case 0x00: if (!lazy) return; break; // DONE separates entries in the lazy stream
        case 0x10: ordinal = imm; break;
        case 0x20: ordinal = (int64_t)read_uleb(&p, end); break;
        case 0x30: ordinal = imm ? (int64_t)(int8_t)(0xF0 | imm) : 0; break;
        case 0x40: name = (const char *)p; weak = imm & 1; p += strlen(name) + 1; cached = NULL; break;
        case 0x50: break; // SET_TYPE_IMM
        case 0x60: addend = read_sleb(&p, end); break;
        case 0x70: addr = segment_address(image, imm, read_uleb(&p, end)); break;
        case 0x80: addr += read_uleb(&p, end); break;
        case 0x90: case 0xA0: case 0xB0: case 0xC0: {
            if (!cached)
                cached = resolve_symbol(image, ordinal, name, weak, NULL);
            uint64_t count = 1, skip = 0;
            if (op == 0xC0) {
                count = read_uleb(&p, end);
                skip = read_uleb(&p, end);
            }
            for (uint64_t i = 0; i < count; i++) {
                *(uint64_t *)addr = cached ? (uint64_t)(uintptr_t)cached + (uint64_t)addend : 0;
                addr += 8 + skip;
            }
            if (op == 0xA0)
                addr += read_uleb(&p, end);
            else if (op == 0xB0)
                addr += imm * 8;
            break;
        }
        default: DIE("unknown bind opcode %#x in %s", byte, image->path);
        }
    }
}

static int apply_dyld_info(struct macho_image *image)
{
    struct dyld_info_command *d = image->dyld_info;
    if (!d)
        return 0;
    if (d->rebase_size)
        run_rebase(image, linkedit_data(image, d->rebase_off), linkedit_data(image, d->rebase_off) + d->rebase_size);
    if (d->bind_size)
        run_bind(image, linkedit_data(image, d->bind_off), linkedit_data(image, d->bind_off) + d->bind_size, 0);
    if (d->lazy_bind_size)
        run_bind(image, linkedit_data(image, d->lazy_bind_off),
                 linkedit_data(image, d->lazy_bind_off) + d->lazy_bind_size, 1);
    LOG("%s: dyld info fixups applied (%u unresolved)", strrchr(image->path, '/') + 1, image->trapped);
    return 1;
}

// ---------------------------------------------------------------------------
// Thread-local variables, one block per image per thread.

struct tlv_image {
    pthread_key_t key;
    uint8_t *template;
    size_t init_size, size;
};

static void tlv_free(void *block) { free(block); }

void *macho_tlv_lookup(struct tlv_descriptor *desc)
{
    struct tlv_image *t = (struct tlv_image *)desc->key;
    uint8_t *block = pthread_getspecific(t->key);
    if (!block) {
        block = calloc(1, t->size ? t->size : 1);
        if (t->init_size)
            memcpy(block, t->template, t->init_size);
        pthread_setspecific(t->key, block);
    }
    return block + desc->offset;
}

static void setup_tlv(struct macho_image *image)
{
    uint8_t *first = NULL, *last_end = NULL, *init_end = NULL;
    struct tlv_descriptor *vars = NULL;
    size_t vars_size = 0;
    FOREACH_CMD(image->header, lc) {
        if (lc->cmd != LC_SEGMENT_64)
            continue;
        struct segment_command_64 *seg = (struct segment_command_64 *)lc;
        struct section_64 *sect = (struct section_64 *)(seg + 1);
        for (uint32_t i = 0; i < seg->nsects; i++) {
            uint32_t type = sect[i].flags & SECTION_TYPE;
            uint8_t *addr = (uint8_t *)(uintptr_t)(sect[i].addr + image->slide);
            if (type == S_THREAD_LOCAL_REGULAR || type == S_THREAD_LOCAL_ZEROFILL) {
                if (!first || addr < first)
                    first = addr;
                if (addr + sect[i].size > last_end)
                    last_end = addr + sect[i].size;
                if (type == S_THREAD_LOCAL_REGULAR && addr + sect[i].size > init_end)
                    init_end = addr + sect[i].size;
            } else if (type == S_THREAD_LOCAL_VARIABLES) {
                vars = (struct tlv_descriptor *)addr;
                vars_size = sect[i].size;
            }
        }
    }
    if (!vars)
        return;
    struct tlv_image *t = calloc(1, sizeof *t);
    pthread_key_create(&t->key, tlv_free);
    t->template = first;
    t->size = first ? (size_t)(last_end - first) : 0;
    t->init_size = init_end ? (size_t)(init_end - first) : 0;
    size_t count = vars_size / sizeof *vars;
    for (size_t i = 0; i < count; i++) {
        vars[i].thunk = macho_tlv_get_addr;
        vars[i].key = (uintptr_t)t;
    }
    LOG("%s: %zu thread-local variables, %zu bytes per thread", strrchr(image->path, '/') + 1, count, t->size);
}

// ---------------------------------------------------------------------------
// Initializers

struct program_vars {
    const void *mh;
    int *NXArgcPtr;
    const char ***NXArgvPtr;
    const char ***environPtr;
    const char **__prognamePtr;
};

static int g_argc;
static const char **g_argv;
static const char **g_envp;
static const char *g_progname;
static const char *g_apple[] = { "executable_path=", NULL };

static void run_initializers(struct macho_image *image)
{
    struct program_vars vars = { image->header, &g_argc, &g_argv, &g_envp, &g_progname };
    typedef void (*initializer)(int, const char **, const char **, const char **, const struct program_vars *);
    FOREACH_CMD(image->header, lc) {
        if (lc->cmd != LC_SEGMENT_64)
            continue;
        struct segment_command_64 *seg = (struct segment_command_64 *)lc;
        struct section_64 *sect = (struct section_64 *)(seg + 1);
        for (uint32_t i = 0; i < seg->nsects; i++) {
            uint32_t type = sect[i].flags & SECTION_TYPE;
            uint8_t *addr = (uint8_t *)(uintptr_t)(sect[i].addr + image->slide);
            if (type == S_INIT_FUNC_OFFSETS) {
                for (size_t k = 0; k < sect[i].size / 4; k++)
                    ((initializer)(void *)(image->base + ((uint32_t *)addr)[k]))(g_argc, g_argv, g_envp, g_apple, &vars);
            } else if (type == S_MOD_INIT_FUNC_POINTERS) {
                for (size_t k = 0; k < sect[i].size / 8; k++)
                    ((initializer)(void *)((uintptr_t *)addr)[k])(g_argc, g_argv, g_envp, g_apple, &vars);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Loading

static struct macho_image *find_loaded(const char *path)
{
    for (size_t i = 0; i < g_image_count; i++)
        if (strcmp(g_images[i]->path, path) == 0)
            return g_images[i];
    return NULL;
}

static struct macho_image *link_image(const char *path, int run_init)
{
    struct macho_image *image = find_loaded(path);
    if (image)
        return image;
    if (g_image_count == MAX_IMAGES)
        return NULL;
    image = map_image(path);
    if (!image)
        return NULL;
    g_images[g_image_count++] = image; // registered before binding so cycles resolve
    if (!apply_chained_fixups(image) && !apply_dyld_info(image))
        LOG("%s has no fixups", path);
    setup_tlv(image);
    protect_segments(image);
    if (run_init)
        run_initializers(image);
    return image;
}

// Resolves @rpath/, @loader_path/ and @executable_path/ against the directory of
// the loading image (bundled dylibs live next to each other).
static struct macho_image *load_dependency(struct macho_image *from, const char *install_name)
{
    const char *base = strrchr(install_name, '/');
    base = base ? base + 1 : install_name;
    char dir[PATH_MAX], path[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", from->path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    snprintf(path, sizeof path, "%s/%s", dir, base);
    if (access(path, R_OK) != 0)
        return NULL;
    return link_image(path, 1);
}

void *macho_dlopen_image(const char *path)
{
    pthread_mutex_lock(&g_load_lock);
    struct macho_image *image = link_image(path, 1);
    pthread_mutex_unlock(&g_load_lock);
    return image;
}

// Describes an address as "image+offset" for crash reports.
void macho_describe_address(uintptr_t addr, char *out, size_t size)
{
    for (size_t i = 0; i < g_image_count; i++) {
        struct macho_image *m = g_images[i];
        if (addr >= (uintptr_t)m->base && addr < (uintptr_t)m->base + m->size) {
            const char *name = strrchr(m->path, '/');
            snprintf(out, size, "%s+%#lx", name ? name + 1 : m->path, (unsigned long)(addr - (uintptr_t)m->base));
            return;
        }
    }
    Dl_info info;
    if (dladdr((void *)addr, &info) && info.dli_fname) {
        const char *name = strrchr(info.dli_fname, '/');
        snprintf(out, size, "%s+%#lx%s%s", name ? name + 1 : info.dli_fname,
                 (unsigned long)(addr - (uintptr_t)info.dli_fbase), info.dli_sname ? " " : "",
                 info.dli_sname ? info.dli_sname : "");
        return;
    }
    snprintf(out, size, "%#lx", (unsigned long)addr);
}

int macho_is_image(const void *handle)
{
    for (size_t i = 0; i < g_image_count; i++)
        if (g_images[i] == handle)
            return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Startup

extern char **environ;
const char *g_executable_path;

int macho_run(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <macho-executable> [args...]\n", argv[0]);
        return 2;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    // Android's public ICU lacks part of the C API that .NET loads from libicucore.
    setenv("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1", 0);
    g_executable_path = realpath(argv[1], NULL);
    if (!g_executable_path)
        DIE("cannot resolve %s", argv[1]);

    g_argc = argc - 1;
    g_argv = (const char **)argv + 1;
    g_envp = (const char **)environ;
    g_progname = strrchr(g_argv[0], '/') ? strrchr(g_argv[0], '/') + 1 : g_argv[0];

    pthread_mutex_lock(&g_load_lock);
    struct macho_image *main_image = link_image(g_executable_path, 0);
    pthread_mutex_unlock(&g_load_lock);
    if (!main_image || !main_image->entry_offset)
        DIE("cannot load %s as an executable", g_executable_path);

    run_initializers(main_image);
    LOG("calling main at +%#llx", (unsigned long long)main_image->entry_offset);
    typedef int (*entry)(int, const char **, const char **, const char **);
    int rc = ((entry)(void *)(main_image->base + main_image->entry_offset))(g_argc, g_argv, g_envp, g_apple);
    LOG("main returned %d", rc);
    return rc;
}

#ifndef HYPRISM_MACHO_LIBRARY
int main(int argc, char **argv) { return macho_run(argc, argv); }
#endif
