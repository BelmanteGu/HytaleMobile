#!/bin/sh
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
#
# Installs the local world server on a connected device that already has the
# game host and the Hytale files (see the Android development guide):
#   - out/jdk and the natives into the host's private directory, the only place
#     Android lets the app execute from;
#   - a HytaleServer.jar patched from <Server/HytaleServer.jar> next to the client;
#   - env.txt and args.txt, which point the client at the bundled Java.
#
#   install.sh <path to the original Server/HytaleServer.jar>
set -eu
cd "$(dirname "$0")"
jar="${1:?usage: $0 <Server/HytaleServer.jar>}"
pkg=org.hyprism.game
home=/data/user/0/$pkg
files=/sdcard/Android/data/$pkg/files

python3 patch-server-jar.py "$jar" build/HytaleServer.jar out/libzstd.so out/libquiche.so
rm -rf build/runtime && mkdir -p build/runtime/natives
cp -a out/jdk build/runtime/jdk
cp out/libc++_shared.so build/runtime/jdk/lib/
cp out/librocksdb.so out/libzstd-jni.so build/runtime/natives/
cp java build/runtime/java
tar -cf build/runtime.tar -C build/runtime jdk natives java

adb push build/runtime.tar /data/local/tmp/hyprism-runtime.tar
adb shell "run-as $pkg sh -c 'cd $home && rm -rf files/jdk files/natives && mkdir -p files tmp && tar -xf /data/local/tmp/hyprism-runtime.tar -C files && chmod 700 files/java'"
adb shell rm /data/local/tmp/hyprism-runtime.tar
adb push build/HytaleServer.jar "$files/Server/HytaleServer.jar"
adb push env.txt args.txt "$files/"
adb shell "chmod a+rw $files/env.txt $files/args.txt"
echo "Local world server installed. Open Worlds in the game and enter a world."
