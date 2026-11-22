<!--
Copyright (C) 2026 Hyprism Launcher
SPDX-License-Identifier: GPL-3.0-only
-->

# Third-party notices

HytaleMobile is licensed under the GNU General Public License v3.0 (see [LICENSE](LICENSE)). It builds on the components below, which keep their own licenses. None of them is vendored in this repository: the build scripts download them from their upstream sources. The licenses of the .NET packages the launcher uses are in [Licenses](Licenses).

| Component | Used for | License | Source |
| --- | --- | --- | --- |
| Hyprism | Launcher and Core, which this project forks | GPL-3.0 | <https://github.com/hyprismteam/Hyprism> |
| SDL 3.2.26 | Window, input, virtual gamepad on the game host | zlib | <https://github.com/libsdl-org/SDL> |
| MobileGlues | OpenGL 4.1 on OpenGL ES 3.2 | LGPL-2.1 | <https://github.com/MobileGL-Dev/MobileGlues> |
| OpenAL Soft 1.24.3 | Audio | LGPL-2.0 or later | <https://github.com/kcat/openal-soft> |
| OpenJDK 25 (Termux package) | Local world server runtime | GPL-2.0 with Classpath Exception | <https://openjdk.org/>, <https://github.com/termux/termux-packages> |
| libandroid-spawn, libandroid-shmem, zlib (Termux packages) | OpenJDK dependencies | BSD-2-Clause, BSD-3-Clause, zlib | <https://github.com/termux/termux-packages> |
| Eclipse Temurin `cacerts` | CA trust store for the server's HTTPS | GPL-2.0 with Classpath Exception (store contents from the Mozilla CA program, MPL-2.0) | <https://adoptium.net/> |
| quiche 0.30.0 | QUIC for the server (patched, see `Sources/Hyprism.MachOLoader/server/patches`) | BSD-2-Clause | <https://github.com/cloudflare/quiche> |
| BoringSSL | TLS inside quiche | ISC / OpenSSL | <https://boringssl.googlesource.com/boringssl> |
| RocksDB 11.2 | World storage for the server (patched) | Apache-2.0 or GPL-2.0 | <https://github.com/facebook/rocksdb> |
| zstd 1.5.7 | Compression | BSD-3-Clause or GPL-2.0 | <https://github.com/facebook/zstd> |
| zstd-jni 1.5.7-6 | Compression from Java | BSD-2-Clause | <https://github.com/luben/zstd-jni> |
| Android NDK `libc++_shared.so` | C++ runtime for quiche | Apache-2.0 with LLVM exception | <https://developer.android.com/ndk> |

Hytale, its client, server and assets are © Hypixel Studios and are not part of this project.
