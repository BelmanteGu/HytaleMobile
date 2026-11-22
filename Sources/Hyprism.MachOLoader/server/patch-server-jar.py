#!/usr/bin/env python3
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
"""Makes a copy of HytaleServer.jar that loads its native libraries on Android arm64.

ZstdNative and QuicheNative accept Linux only on amd64 and then extract
native/linux-x64/*.so from the jar. This rewrites their "amd64" constant to
"aarch64" and replaces those two entries with Android arm64 builds.
RocksDbNative needs no patch: it honours -Drocksdb.lib.path.
HytaleServer's constructor calls Netty's Quic.ensureAvailability(), whose
native library exists only for glibc; with the QUICHE transport Netty QUIC is
not used, so that call becomes three NOPs.

    patch-server-jar.py <in.jar> <out.jar> <libzstd.so> <libquiche.so>
"""
import struct
import sys
import zipfile

PATCHED_CLASSES = {
    "com/hypixel/hytale/protocol/io/ZstdNative.class",
    "com/hypixel/hytale/lib/quiche/QuicheNative.class",
}


def rewrite_utf8(data: bytes, old: str, new: str) -> bytes:
    """Rewrites CONSTANT_Utf8 entries equal to `old` in a class file."""
    out = bytearray(data[:10])
    count = struct.unpack(">H", data[8:10])[0]
    pos, index, hits = 10, 1, 0
    sizes = {3: 4, 4: 4, 5: 8, 6: 8, 7: 2, 8: 2, 9: 4, 10: 4, 11: 4, 12: 4,
             15: 3, 16: 2, 17: 4, 18: 4, 19: 2, 20: 2}
    while index < count:
        tag = data[pos]
        if tag == 1:
            length = struct.unpack(">H", data[pos + 1:pos + 3])[0]
            text = data[pos + 3:pos + 3 + length]
            if text == old.encode():
                text = new.encode()
                hits += 1
            out += bytes([1]) + struct.pack(">H", len(text)) + text
            pos += 3 + length
        else:
            size = sizes[tag]
            out += data[pos:pos + 1 + size]
            pos += 1 + size
        index += 2 if tag in (5, 6) else 1
    if hits == 0:
        raise SystemExit(f"constant {old!r} not found")
    return bytes(out + data[pos:])


# putstatic #74 (instance); invokestatic #78 (Quic.ensureAvailability); bipush 25
QUIC_CHECK = bytes.fromhex("b3004a" "b8004e" "1019")
QUIC_CHECK_NOP = bytes.fromhex("b3004a" "000000" "1019")
SERVER_CLASS = "com/hypixel/hytale/server/core/HytaleServer.class"


def main() -> None:
    src, dst, zstd, quiche = sys.argv[1:5]
    replacements = {
        "native/linux-x64/libzstd.so": open(zstd, "rb").read(),
        "native/linux-x64/libquiche.so": open(quiche, "rb").read(),
    }
    done = set()
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename in PATCHED_CLASSES:
                data = rewrite_utf8(data, "amd64", "aarch64")
                done.add(item.filename)
            elif item.filename == SERVER_CLASS:
                if data.count(QUIC_CHECK) != 1:
                    raise SystemExit("Quic.ensureAvailability call not found once")
                data = data.replace(QUIC_CHECK, QUIC_CHECK_NOP)
                done.add(item.filename)
            elif item.filename in replacements:
                data = replacements[item.filename]
                done.add(item.filename)
            zout.writestr(item, data)
    missing = (PATCHED_CLASSES | set(replacements) | {SERVER_CLASS}) - done
    if missing:
        raise SystemExit(f"entries not found: {sorted(missing)}")
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
