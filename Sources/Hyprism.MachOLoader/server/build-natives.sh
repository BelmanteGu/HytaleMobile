#!/bin/sh
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds the native libraries the Hytale world server loads, for Android arm64:
#   out/libzstd.so       zstd 1.5.7 (ZstdNative)
#   out/librocksdb.so    RocksDB 11.2 with zstd (RocksDbNative), patches/rocksdb-11.2-hytale.patch
#   out/libquiche.so     quiche 0.30.0 C API (QuicheNative), patches/quiche-0.30.0-hytale.patch
#   out/libzstd-jni.so   zstd-jni 1.5.7-6 (com.github.luben.zstd)
#   out/libc++_shared.so the NDK C++ runtime libquiche.so links against
# Requires git, cmake, ninja, the Android NDK, and Rust with cargo-ndk and the
# aarch64-linux-android target.
set -eu
cd "$(dirname "$0")"
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
[ -d "$sdk" ] || sdk="$HOME/Android/Sdk"
ndk="${ANDROID_NDK_HOME:-$(ls -d "$sdk/ndk/"* | sort -V | tail -1)}"
export ANDROID_NDK_HOME="$ndk"
prebuilt="$(ls -d "$ndk/toolchains/llvm/prebuilt/"* | head -1)"
android="-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=Release -DANDROID_STL=c++_static"
mkdir -p deps build out

[ -d deps/zstd ] || git clone -q --depth 1 --branch v1.5.7 https://github.com/facebook/zstd.git deps/zstd
cmake -S deps/zstd/build/cmake -B build/zstd -G Ninja $android -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF \
    -DZSTD_BUILD_SHARED=ON -DZSTD_BUILD_STATIC=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build build/zstd
"$prebuilt/bin/llvm-strip" --strip-debug -o out/libzstd.so "$(readlink -f build/zstd/lib/libzstd.so)"

if [ ! -d deps/rocksdb ]; then
    git clone -q --depth 1 --branch 11.2.fb https://github.com/facebook/rocksdb.git deps/rocksdb
    git -C deps/rocksdb apply "$PWD/patches/rocksdb-11.2-hytale.patch"
fi
cmake -S deps/rocksdb -B build/rocksdb -G Ninja $android -DROCKSDB_BUILD_SHARED=ON -DWITH_TESTS=OFF -DWITH_TOOLS=OFF \
    -DWITH_CORE_TOOLS=OFF -DWITH_BENCHMARK_TOOLS=OFF -DWITH_TRACE_TOOLS=OFF -DWITH_GFLAGS=OFF -DWITH_ALL_TESTS=OFF \
    -DPORTABLE=1 -DFAIL_ON_WARNINGS=OFF -DWITH_ZSTD=ON -DWITH_SNAPPY=OFF -DWITH_LZ4=OFF -DWITH_ZLIB=OFF -DWITH_BZ2=OFF \
    -DWITH_LIBURING=OFF -DWITH_JEMALLOC=OFF \
    -Dzstd_INCLUDE_DIRS="$PWD/deps/zstd/lib" -Dzstd_LIBRARIES="$PWD/build/zstd/lib/libzstd.a" \
    -DZSTD_INCLUDE_DIRS="$PWD/deps/zstd/lib" -DZSTD_LIBRARIES="$PWD/build/zstd/lib/libzstd.a"
cmake --build build/rocksdb --target rocksdb-shared
"$prebuilt/bin/llvm-strip" --strip-debug -o out/librocksdb.so "$(readlink -f build/rocksdb/librocksdb.so)"

if [ ! -d deps/quiche ]; then
    git clone -q --depth 1 --branch 0.30.0 --recurse-submodules --shallow-submodules https://github.com/cloudflare/quiche.git deps/quiche
    git -C deps/quiche apply "$PWD/patches/quiche-0.30.0-hytale.patch"
fi
(cd deps/quiche/quiche && cargo ndk -t arm64-v8a -P 30 build --release --features ffi,qlog,sfv)
"$prebuilt/bin/llvm-strip" --strip-debug -o out/libquiche.so deps/quiche/target/aarch64-linux-android/release/libquiche.so
cp "$prebuilt/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" out/

[ -d deps/zstd-jni ] || git clone -q --depth 1 --branch v1.5.7-6 https://github.com/luben/zstd-jni.git deps/zstd-jni
n=deps/zstd-jni/src/main/native
"$prebuilt/bin/aarch64-linux-android30-clang" -O2 -fPIC -shared -o out/libzstd-jni.so \
    -DZSTD_LEGACY_SUPPORT=4 -DZSTD_MULTITHREAD=1 -DDYNAMIC_BMI2=0 -DZSTD_DISABLE_ASM \
    -I$n -I$n/common -I$n/legacy $(find $n -name '*.c') -Wl,-z,max-page-size=16384
ls -la out
