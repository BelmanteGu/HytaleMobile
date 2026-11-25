#!/bin/sh
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
#
# Assembles out/bundle, the game half of the single Hyprism APK (Sources/Hyprism.Android picks it up):
#   classes.jar               the game activities and SDL's Java side
#   lib/arm64-v8a/*.so        the Mach-O loader, SDL3, MobileGlues, OpenAL and pwrapply (the game
#                             patcher the launcher runs instead of butler)
#   assets/java               the world server launcher
#   assets/icons/             the on-screen gamepad icons
#   assets/runtime.zip        the world server's Java runtime and native libraries
# Needs build.sh, server/fetch-jdk.sh and server/build-natives.sh to have run, and Go for pwrapply.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/.."
server="$root/server"
"$here/build-apk.sh" >/dev/null
work="$root/build/apk"
out="$root/out/bundle"
rm -rf "$out" && mkdir -p "$out/lib/arm64-v8a" "$out/assets"

(cd "$work/classes" && jar cf "$out/classes.jar" .)
cp "$work"/stage/lib/arm64-v8a/*.so "$out/lib/arm64-v8a/"
(cd "$root/pwrapply" && CGO_ENABLED=0 GOOS=android GOARCH=arm64 go build -trimpath -ldflags "-s -w" \
    -o "$out/lib/arm64-v8a/libpwrapply.so" .)
cp -R "$work/assets/." "$out/assets/"

r="$root/build/runtime-zip"
rm -rf "$r" && mkdir -p "$r/natives" "$r/jar-natives"
cp -a "$server/out/jdk" "$r/jdk"
cp "$server/out/libc++_shared.so" "$r/jdk/lib/"
cp "$server/out/librocksdb.so" "$server/out/libzstd-jni.so" "$r/natives/"
cp "$server/out/libzstd.so" "$server/out/libquiche.so" "$r/jar-natives/"
(cd "$r" && zip -qry -9 "$out/assets/runtime.zip" jdk natives jar-natives)
du -sh "$out"/* "$out/assets/runtime.zip"
