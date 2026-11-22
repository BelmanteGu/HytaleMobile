#!/bin/sh
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
#
# Fetches and builds the Android arm64 libraries the game host ships:
#   build/sdl/libSDL3.so                  SDL 3.2.26 (same version as the macOS client)
#   build/mobileglues/libmobileglues.so   desktop OpenGL 4.x on OpenGL ES 3.2
#   build/openal/libopenal.so             OpenAL Soft 1.24.3 (OpenSL ES backend)
# Requires git, cmake, ninja and the Android NDK.
set -eu
cd "$(dirname "$0")"
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
[ -d "$sdk" ] || sdk="$HOME/Android/Sdk"
ndk="${ANDROID_NDK_HOME:-$(ls -d "$sdk/ndk/"* | sort -V | tail -1)}"
toolchain="$ndk/build/cmake/android.toolchain.cmake"
android="-DCMAKE_TOOLCHAIN_FILE=$toolchain -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=Release"
mkdir -p deps build

[ -d deps/SDL ] || git clone -q --depth 1 --branch release-3.2.26 https://github.com/libsdl-org/SDL.git deps/SDL
cmake -S deps/SDL -B build/sdl -G Ninja $android -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF
cmake --build build/sdl

mobileglues=97558a69157b033b890958b1e49258c5b51a80c6
if [ ! -d deps/MobileGlues ]; then
    git clone -q https://github.com/MobileGL-Dev/MobileGlues.git deps/MobileGlues
    git -C deps/MobileGlues checkout -q "$mobileglues"
    git -C deps/MobileGlues submodule update --init --depth 1 MobileGlues-cpp/3rdparty/glslang \
        MobileGlues-cpp/3rdparty/SPIRV-Cross MobileGlues-cpp/3rdparty/xxhash MobileGlues-cpp/include/ska
    # glShaderSource passed the original source count after joining the sources.
    git -C deps/MobileGlues apply "$PWD/patches/mobileglues-shader-source-count.patch"
fi
cmake -S deps/MobileGlues/MobileGlues-cpp -B build/mobileglues -G Ninja $android -DANDROID_STL=c++_static
cmake --build build/mobileglues

[ -d deps/openal-soft ] || git clone -q --depth 1 --branch 1.24.3 https://github.com/kcat/openal-soft.git deps/openal-soft
# The bundled fmt relies on <cstdlib> arriving transitively, which newer NDK libc++ no longer does.
cmake -S deps/openal-soft -B build/openal -G Ninja $android -DANDROID_STL=c++_static \
    -DCMAKE_CXX_FLAGS="-include cstdlib" -DALSOFT_UTILS=OFF -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF -DALSOFT_INSTALL=OFF
cmake --build build/openal

echo "dependencies built in build/"
