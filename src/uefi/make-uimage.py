#!/usr/bin/env python3
import argparse
import struct
import time
import zlib
from pathlib import Path


UIMAGE_MAGIC = 0x27051956
UIMAGE_OS_LINUX = 5
UIMAGE_ARCH_ARM = 2
UIMAGE_TYPE_KERNEL = 2
UIMAGE_TYPE_SCRIPT = 6
UIMAGE_COMPRESSION_NONE = 0


def build_header(data: bytes, name: str, image_type: int, load: int, entry: int) -> bytes:
    encoded_name = name.encode("ascii")
    if len(encoded_name) > 32:
        raise ValueError("uImage name must be at most 32 ASCII bytes")
    padded_name = encoded_name.ljust(32, b"\0")
    data_crc = zlib.crc32(data) & 0xFFFFFFFF
    timestamp = int(time.time())

    values = (
        UIMAGE_MAGIC,
        0,
        timestamp,
        len(data),
        load,
        entry,
        data_crc,
        UIMAGE_OS_LINUX,
        UIMAGE_ARCH_ARM,
        image_type,
        UIMAGE_COMPRESSION_NONE,
        padded_name,
    )
    header = struct.pack(">7I4B32s", *values)
    header_crc = zlib.crc32(header) & 0xFFFFFFFF
    return struct.pack(">I", UIMAGE_MAGIC) + struct.pack(">I", header_crc) + header[8:]


def main() -> None:
    parser = argparse.ArgumentParser(description="Create a legacy U-Boot uImage.")
    parser.add_argument("--type", choices=("kernel", "script"), required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--load", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--entry", type=lambda value: int(value, 0), default=0)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    data = args.input.read_bytes()
    if args.type == "script":
        data = data.replace(b"\r\n", b"\n")
        data = struct.pack(">II", len(data), 0) + data
        image_type = UIMAGE_TYPE_SCRIPT
    else:
        image_type = UIMAGE_TYPE_KERNEL

    header = build_header(data, args.name, image_type, args.load, args.entry)
    args.output.write_bytes(header + data)


if __name__ == "__main__":
    main()
