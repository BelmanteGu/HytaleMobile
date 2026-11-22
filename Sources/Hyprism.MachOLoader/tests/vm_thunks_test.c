// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Device test: reproduces the NativeAOT thunk pool sequence (PalAllocateThunksFromTemplate
// and PalMarkThunksAsValidCallTargets) through the Mach VM shims.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "macho.h"

typedef int (*vm_allocate_fn)(unsigned, uint64_t *, uint64_t, int);
typedef int (*vm_remap_fn)(unsigned, uint64_t *, uint64_t, uint64_t, int, unsigned, uint64_t, int, int *, int *, int);
typedef int (*mprotect_fn)(void *, size_t, int);

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else printf("ok: %s\n", #c); } while (0)

static void show_maps(uint64_t a)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && hi > a && lo < a + 0x10000)
            printf("  maps: %s", line);
    }
    fclose(f);
}

int main(void)
{
    vm_allocate_fn vm_allocate = (vm_allocate_fn)shim_lookup("_vm_allocate", NULL);
    vm_remap_fn vm_remap = (vm_remap_fn)shim_lookup("_vm_remap", NULL);
    mprotect_fn mprot = (mprotect_fn)shim_lookup("_mprotect", NULL);

    // Template: 32 KiB of read-execute code starting with "mov w0, #42; ret".
    uint32_t *template = mmap(NULL, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int i = 0; i < 0x8000 / 4; i++)
        template[i] = 0xD503201F; // nop
    template[0] = 0x52800540;     // mov w0, #42
    template[1] = 0xD65F03C0;     // ret
    mprotect(template, 0x8000, PROT_READ | PROT_EXEC);

    uint64_t a = 0;
    CHECK(vm_allocate(0x103, &a, 0x10000, 1) == 0, "vm_allocate");
    CHECK((a & 0x3fff) == 0, "base %#llx not 16 KiB aligned", (unsigned long long)a);

    uint64_t at = a;
    int cur = 0, max = 0;
    CHECK(vm_remap(0x103, &at, 0x8000, 0, 0x4000, 0x103, (uint64_t)(uintptr_t)template, 0, &cur, &max, 0) == 0, "vm_remap");
    CHECK(at == a, "remap moved %#llx -> %#llx", (unsigned long long)a, (unsigned long long)at);
    CHECK(memcmp((void *)(uintptr_t)a, template, 0x8000) == 0, "contents differ");
    CHECK(cur == 5, "cur protection %d", cur);

    // PalMarkThunksAsValidCallTargets with OS_PAGE_SIZE 4096: base + 2 pages, 2 pages, RW.
    CHECK(mprot((void *)(uintptr_t)(a + 0x2000), 0x2000, PROT_READ | PROT_WRITE) == 0, "mprotect data half");

    int (*thunk)(void) = (int (*)(void))(uintptr_t)a;
    CHECK(thunk() == 42, "copied code did not return 42");
    int (*late)(void) = (int (*)(void))(uintptr_t)(a + 0x2000 + 0x1000); // inside the range the runtime touched
    (void)late;
    volatile uint64_t *data = (volatile uint64_t *)(uintptr_t)(a + 0x8000);
    data[0] = 0x1122334455667788ull;
    data[0x7ff] = 7;
    CHECK(data[0] == 0x1122334455667788ull && data[0x7ff] == 7, "data block not writable");
    show_maps(a);
    printf(failures ? "%d FAILURES\n" : "ALL THUNK POOL CHECKS PASSED\n", failures);
    return failures != 0;
}
