#!/bin/sh
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds the Mach-O loader for Android arm64 with the Android NDK:
#   out/hyprism-macho   command-line loader for adb shell tests
#   out/libmain.so      SDLActivity entry point used by the host APK
set -eu
cd "$(dirname "$0")"
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
[ -d "$sdk" ] || sdk="$HOME/Android/Sdk"
ndk="${ANDROID_NDK_HOME:-$(ls -d "$sdk/ndk/"* | sort -V | tail -1)}"
cc="$(ls -d "$ndk/toolchains/llvm/prebuilt/"*/bin | head -1)/aarch64-linux-android30-clang"
sources="src/loader.c src/shims.c src/signals.c src/posix.c src/dyld.c src/crypto.c src/net.c src/cf.c src/objc.c src/cxx.c src/sdl_scale.c src/gl.c src/security.c src/trampolines.S"
flags="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Ideps/SDL/include"
mkdir -p out
"$cc" $flags -fPIE -pie $sources -o out/hyprism-macho -ldl -lm
if [ -f build/sdl/libSDL3.so ]; then
    "$cc" $flags -fPIC -shared -DHYPRISM_MACHO_LIBRARY $sources src/android_host.c src/gamepad.c \
        -o out/libmain.so -Lbuild/sdl -lSDL3 -ldl -lm -llog
fi
echo "built $(ls out)"
