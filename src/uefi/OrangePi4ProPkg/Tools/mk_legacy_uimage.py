#!/usr/bin/env python3
"""Wrap a raw AArch64 EDK2 FD in the legacy U-Boot uImage format.

The Orange Pi 4 Pro vendor U-Boot/bootm path used by the proven Stage0 v13
expects the historical ARM architecture byte (2), not IH_ARCH_ARM64 (22).
This tool intentionally reproduces that working header layout.
"""
from __future__ import annotations

import argparse
import binascii
import os
from pathlib import Path
import struct
import time

MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_ARM = 2
IH_TYPE_KERNEL = 2
IH_COMP_NONE = 0
HEADER = struct.Struct(">7I4B32s")


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def make_header(data: bytes, load: int, entry: int, name: str, timestamp: int) -> bytes:
    encoded_name = name.encode("ascii", "strict")[:31].ljust(32, b"\0")
    data_crc = crc32(data)
    header_zero_crc = HEADER.pack(
        MAGIC, 0, timestamp, len(data), load, entry, data_crc,
        IH_OS_LINUX, IH_ARCH_ARM, IH_TYPE_KERNEL, IH_COMP_NONE, encoded_name,
    )
    header_crc = crc32(header_zero_crc)
    return HEADER.pack(
        MAGIC, header_crc, timestamp, len(data), load, entry, data_crc,
        IH_OS_LINUX, IH_ARCH_ARM, IH_TYPE_KERNEL, IH_COMP_NONE, encoded_name,
    )


def verify(image: bytes) -> None:
    if len(image) < HEADER.size:
        raise ValueError("image is shorter than a uImage header")
    fields = HEADER.unpack(image[:HEADER.size])
    magic, header_crc, _ts, size, _load, _entry, data_crc = fields[:7]
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}")
    zeroed = bytearray(image[:HEADER.size])
    zeroed[4:8] = b"\0\0\0\0"
    if crc32(zeroed) != header_crc:
        raise ValueError("header CRC mismatch")
    payload = image[HEADER.size:HEADER.size + size]
    if len(payload) != size:
        raise ValueError("truncated payload")
    if crc32(payload) != data_crc:
        raise ValueError("payload CRC mismatch")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="raw ORANGEPI4PRO_EFI.fd")
    parser.add_argument("output", type=Path, help="output .uimg")
    parser.add_argument("--load", type=lambda v: int(v, 0), default=0x41000000)
    parser.add_argument("--entry", type=lambda v: int(v, 0), default=0x41000000)
    parser.add_argument("--name", default="Orange Pi 4 Pro EDK2 v4")
    parser.add_argument("--timestamp", type=int, default=None)
    args = parser.parse_args()

    data = args.input.read_bytes()
    timestamp = args.timestamp
    if timestamp is None:
        timestamp = int(os.environ.get("SOURCE_DATE_EPOCH", int(time.time())))
    output = make_header(data, args.load, args.entry, args.name, timestamp) + data
    verify(output)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    print(f"wrote {args.output} ({len(output)} bytes)")
    print(f"load=0x{args.load:08x} entry=0x{args.entry:08x} data_crc=0x{crc32(data):08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
