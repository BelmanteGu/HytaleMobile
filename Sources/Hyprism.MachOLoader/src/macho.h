// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Minimal Mach-O definitions for loading arm64 macOS executables on Linux and Android.

#pragma once

#include <stddef.h>
#include <stdint.h>

#define MH_MAGIC_64 0xfeedfacfu
#define CPU_TYPE_ARM64 0x0100000c
#define MH_EXECUTE 0x2
#define MH_DYLIB 0x6

#define LC_SEGMENT_64 0x19
#define LC_LOAD_DYLIB 0xc
#define LC_LOAD_WEAK_DYLIB 0x80000018
#define LC_REEXPORT_DYLIB 0x8000001f
#define LC_MAIN 0x80000028
#define LC_DYLD_CHAINED_FIXUPS 0x80000034
#define LC_DYLD_EXPORTS_TRIE 0x80000033
#define LC_SYMTAB 0x2
#define LC_DYLD_INFO 0x22
#define LC_DYLD_INFO_ONLY 0x80000022
#define LC_LOAD_UPWARD_DYLIB 0x80000023

#define SECTION_TYPE 0x000000ff
#define S_ZEROFILL 0x1
#define S_THREAD_LOCAL_REGULAR 0x11
#define S_THREAD_LOCAL_ZEROFILL 0x12
#define S_THREAD_LOCAL_VARIABLES 0x13
#define S_MOD_INIT_FUNC_POINTERS 0x9
#define S_INIT_FUNC_OFFSETS 0x16

#define VM_PROT_READ 0x1
#define VM_PROT_WRITE 0x2
#define VM_PROT_EXECUTE 0x4

#define DYLD_CHAINED_PTR_64 2
#define DYLD_CHAINED_PTR_64_OFFSET 6
#define DYLD_CHAINED_IMPORT 1
#define DYLD_CHAINED_PTR_START_NONE 0xFFFF

struct mach_header_64 {
    uint32_t magic;
    int32_t cputype;
    int32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};

struct load_command {
    uint32_t cmd;
    uint32_t cmdsize;
};

struct segment_command_64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    int32_t maxprot;
    int32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};

struct section_64 {
    char sectname[16];
    char segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
};

struct dylib_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t name_offset;
    uint32_t timestamp;
    uint32_t current_version;
    uint32_t compatibility_version;
};

struct entry_point_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
};

struct linkedit_data_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dataoff;
    uint32_t datasize;
};

struct dyld_info_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t rebase_off, rebase_size;
    uint32_t bind_off, bind_size;
    uint32_t weak_bind_off, weak_bind_size;
    uint32_t lazy_bind_off, lazy_bind_size;
    uint32_t export_off, export_size;
};

struct dyld_chained_fixups_header {
    uint32_t fixups_version;
    uint32_t starts_offset;
    uint32_t imports_offset;
    uint32_t symbols_offset;
    uint32_t imports_count;
    uint32_t imports_format;
    uint32_t symbols_format;
};

struct dyld_chained_starts_in_segment {
    uint32_t size;
    uint16_t page_size;
    uint16_t pointer_format;
    uint64_t segment_offset;
    uint32_t max_valid_pointer;
    uint16_t page_count;
    uint16_t page_start[];
} __attribute__((packed));

/// Thread-local variable descriptor emitted by ld64 in __thread_vars.
struct tlv_descriptor {
    void *(*thunk)(struct tlv_descriptor *);
    uintptr_t key;
    uintptr_t offset;
};

/// One loaded Mach-O image.
struct macho_image {
    const char *path;
    uint8_t *base;              // address of the mach header in memory
    uint64_t text_vmaddr;       // link-time address of __TEXT
    intptr_t slide;             // base - text_vmaddr
    size_t size;
    struct mach_header_64 *header;
    uint64_t entry_offset;
    const uint8_t *exports;     // exports trie
    size_t exports_size;
    struct dyld_info_command *dyld_info;
    uint32_t trapped;           // imports without an implementation
};

struct shim {
    const char *name;
    void *address;
};

#define SHIM_COUNT(table) (sizeof(table) / sizeof((table)[0]))

/// Shim tables provided by each module.
extern const struct shim signal_shims[];
extern const size_t signal_shims_count;
extern const struct shim posix_shims[];
extern const size_t posix_shims_count;
extern const struct shim dyld_shims[];
extern const size_t dyld_shims_count;
extern const struct shim crypto_shims[];
extern const size_t crypto_shims_count;
extern const struct shim net_shims[];
extern const size_t net_shims_count;
extern const struct shim objc_shims[];
extern const size_t objc_shims_count;
extern const struct shim cf_shims[];
extern const size_t cf_shims_count;
extern const struct shim cxx_shims[];
extern const size_t cxx_shims_count;
extern const struct shim security_shims[];
extern const size_t security_shims_count;
void *objc_class_import(const char *symbol);

int errno_to_apple(int host);
int errno_from_apple(int apple);

/// Writes a loader diagnostic to stderr and, on Android, straight to logcat so
/// messages survive an immediate _exit.
int dyld_replacement_symbol(const char *install_name, const char *name, void **out);
void macho_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/// Resolves a symbol to a shim implementation, or returns NULL.
void *shim_lookup(const char *name, const char *library);

/// Loads a bundled Mach-O dylib (and its dependencies) and returns its handle.
void *macho_dlopen_image(const char *path);
int macho_is_image(const void *handle);
void macho_describe_address(uintptr_t addr, char *out, size_t size);
void *macho_image_symbol(struct macho_image *image, const char *mangled);

/// Loader services used by the shims.
struct macho_image *macho_main_image(void);
void *macho_find_section(struct macho_image *image, const char *segname, const char *sectname, size_t *size);
