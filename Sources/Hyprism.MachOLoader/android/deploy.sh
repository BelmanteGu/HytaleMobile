#!/bin/sh
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
#
# Rebuilds the loader and host APK, reinstalls it on a device and starts the client.
#   ANDROID_SERIAL=<serial> android/deploy.sh [seconds-to-wait]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
[ -d "$sdk" ] || sdk="$HOME/Android/Sdk"
adb="$sdk/platform-tools/adb"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
export PATH="$JAVA_HOME/bin:$PATH"
"$here/../build.sh" >/dev/null
"$here/build-apk.sh" >/dev/null
"$adb" shell am force-stop org.hyprism.game
"$adb" install -r "$here/../out/hyprism-game.apk" >/dev/null
"$adb" logcat -c
"$adb" shell am start -n org.hyprism.game/.HytaleActivity >/dev/null
sleep "${1:-30}"
"$adb" shell pidof org.hyprism.game >/dev/null && echo "client running" || echo "client exited"
"$adb" logcat -d -s Hytale:V HytaleLoader:V SDL/ERROR:V DEBUG:V \
    | grep -vE 'dlopen\(|mapped |imports: |fixups: |tlv: |calling main|\[host\] starting' | cut -c32-260 | tail -40
