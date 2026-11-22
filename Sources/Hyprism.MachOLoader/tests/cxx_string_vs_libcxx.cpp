// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Host test (macOS arm64 only): applies random operations to std::string through
// Apple's libc++ and through the shim implementation on the same memory layout.
//   clang++ -std=c++17 -Isrc tests/cxx_string_vs_libcxx.cpp -o /tmp/cxxtest && /tmp/cxxtest

#include <cstdio>
#include <cstdlib>
#include <string>
extern "C" {
void macho_log(const char *, const char *, ...) {}
void *shim_lookup(const char *, const char *) { return nullptr; }
#define static
#include "../src/cxx.c"
#undef static
}

int main()
{
    srand(42);
    int failures = 0;
    for (int round = 0; round < 2000; round++) {
        std::string ref;
        struct apple_string mine;
        memset(&mine, 0, sizeof mine);
        for (int op = 0; op < 40; op++) {
            size_t size = ref.size(), pos = size ? rand() % (size + 1) : 0, n = rand() % 50;
            char c = 'a' + rand() % 26;
            std::string chunk(n, c);
            switch (rand() % 8) {
            case 0: ref.append(chunk.c_str()); s_append_cstr(&mine, chunk.c_str()); break;
            case 1: ref.append(chunk.data(), n / 2); s_append_n(&mine, chunk.data(), n / 2); break;
            case 2: ref.insert(pos, n % 7, c); s_insert_chars(&mine, pos, n % 7, c); break;
            case 3: ref.erase(pos, n); s_erase(&mine, pos, n); break;
            case 4: ref.resize(n * 2, c); s_resize(&mine, n * 2, c); break;
            case 5: ref.push_back(c); s_push_back(&mine, c); break;
            case 6: ref.reserve(n * 3); s_reserve(&mine, n * 3); break;
            case 7: if (ref.find(c) != s_find_char(&mine, c, 0) || ref.rfind(c) != s_rfind_char(&mine, c, NPOS)) failures++; break;
            }
            // The real libc++ reads our object in place: same layout, same contents.
            std::string *view = reinterpret_cast<std::string *>(&mine);
            if (*view != ref || view->size() != ref.size() || strlen(view->c_str()) != ref.size()) {
                failures++;
                printf("mismatch round %d op %d: '%s' vs '%s'\n", round, op, view->c_str(), ref.c_str());
                break;
            }
        }
        s_destroy(&mine);
    }
    struct apple_string f = to_string_float(1.5f);
    std::string *fv = reinterpret_cast<std::string *>(&f);
    if (*fv != std::to_string(1.5f)) failures++;
    printf(failures ? "%d failures\n" : "all std::string checks passed against Apple libc++\n", failures);
    return failures != 0;
}
