#!/bin/sh
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
#
# Assembles out/jdk: Termux's OpenJDK 25 build for Android arm64 (Bionic),
# trimmed, with the shared libraries it needs from other Termux packages.
# Termux's cacerts is a link into the Termux prefix, so a JDK 25 on this machine
# provides the trust store: pass its home as the first argument or set JAVA_HOME.
set -eu
cd "$(dirname "$0")"
host_jdk="${1:-${JAVA_HOME:-}}"
[ -f "$host_jdk/lib/security/cacerts" ] || { echo "usage: $0 <JDK home with lib/security/cacerts>" >&2; exit 1; }
repo=https://packages.termux.dev/apt/termux-main
mkdir -p build/termux out
curl -fsS "$repo/dists/stable/main/binary-aarch64/Packages" -o build/termux/Packages
rm -rf build/termux/root && mkdir -p build/termux/root
for p in openjdk-25 libandroid-shmem libandroid-spawn zlib; do
    f=$(awk -v p="$p" '$1=="Package:"{cur=$2} $1=="Filename:" && cur==p{print $2; exit}' build/termux/Packages)
    curl -fsS "$repo/$f" -o "build/termux/$p.deb"
    t=$(mktemp -d)
    (cd "$t" && bsdtar -xf "$OLDPWD/build/termux/$p.deb" 2>/dev/null || (cd "$t" && ar x "$OLDPWD/build/termux/$p.deb"))
    tar -xf "$t"/data.tar.* -C build/termux/root
    rm -rf "$t"
done
prefix=build/termux/root/data/data/com.termux/files/usr
rm -rf out/jdk
cp -a "$prefix/lib/jvm/java-25-openjdk" out/jdk
rm -rf out/jdk/jmods out/jdk/demo out/jdk/man out/jdk/include out/jdk/lib/ct.sym out/jdk/lib/src.zip
cp -L "$prefix/lib/libandroid-spawn.so" "$prefix/lib/libandroid-shmem.so" "$prefix/lib/libz.so.1" out/jdk/lib/
rm -f out/jdk/lib/security/cacerts
cp "$host_jdk/lib/security/cacerts" out/jdk/lib/security/cacerts
du -sh out/jdk
