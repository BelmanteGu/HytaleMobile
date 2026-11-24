#!/bin/sh
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
#
# Packages the game host APK without Gradle: javac, d8, aapt2, zipalign, apksigner.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/.."
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
[ -d "$sdk" ] || sdk="$HOME/Android/Sdk"
tools="$(ls -d "$sdk/build-tools/"* | sort -V | tail -1)"
jar="$sdk/platforms/android-35/android.jar"
work="$root/build/apk"
rm -rf "$work" && mkdir -p "$work/classes" "$work/dex" "$work/stage/lib/arm64-v8a"

# The client runs on SDLThread (SDL3 requires video calls on its main thread),
# so give that thread a stack sized for the .NET runtime.
mkdir -p "$work/src"
cp "$root"/deps/SDL/android-project/app/src/main/java/org/libsdl/app/*.java "$work/src/"
sed -i.orig 's|new Thread(new SDLMain(), "SDLThread")|new Thread(null, new SDLMain(), "SDLThread", 64L * 1024 * 1024)|' "$work/src/SDLActivity.java"
grep -q '64L \* 1024 \* 1024' "$work/src/SDLActivity.java"
javac --release 11 -nowarn -classpath "$jar" -d "$work/classes" \
    "$work"/src/*.java "$here"/java/org/hyprism/game/*.java 2>&1 | grep -v '^Note:' || true
"$tools/d8" --release --lib "$jar" --min-api 30 --output "$work/dex" $(find "$work/classes" -name '*.class')
# Target SDK 28: newer targets forbid executing files from the app's data
# directory, and the client starts the local world server as a Java process.
# assets/java is the world server launcher; HytaleActivity copies it to files/java.
mkdir -p "$work/assets"
cp "$root/server/java" "$work/assets/java"
# assets/icons: the on-screen gamepad icons (Lucide, ISC).
cp -R "$here/assets/icons" "$work/assets/icons"
rm -f "$work/assets/icons/README.md"
"$tools/aapt2" link -o "$work/base.apk" -I "$jar" --manifest "$here/AndroidManifest.xml" -A "$work/assets" \
    --min-sdk-version 28 --target-sdk-version 28

cp "$work/dex/classes.dex" "$work/stage/"
cp "$root/build/sdl/libSDL3.so" "$root/out/libmain.so" "$work/stage/lib/arm64-v8a/"
strip="$(ls -d "$(ls -d "$sdk/ndk/"* | sort -V | tail -1)/toolchains/llvm/prebuilt/"*/bin | head -1)/llvm-strip"
"$strip" --strip-debug -o "$work/stage/lib/arm64-v8a/libmobileglues.so" "$root/build/mobileglues/libmobileglues.so"
"$strip" --strip-debug -o "$work/stage/lib/arm64-v8a/libopenal.so" "$root/build/openal/libopenal.so"
(cd "$work/stage" && zip -qr "$work/base.apk" classes.dex lib)
"$tools/zipalign" -f -p 4 "$work/base.apk" "$work/aligned.apk"
"$tools/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android \
    --out "$root/out/hyprism-game.apk" "$work/aligned.apk"
echo "built $root/out/hyprism-game.apk"
