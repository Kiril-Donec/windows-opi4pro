#!/usr/bin/env python3
"""Build a legacy U-Boot script image without requiring mkimage."""
from __future__ import annotations
import argparse, binascii, os, struct, time
from pathlib import Path
MAGIC=0x27051956; HEADER=struct.Struct('>7I4B32s')
def crc32(b: bytes)->int: return binascii.crc32(b)&0xffffffff
p=argparse.ArgumentParser(); p.add_argument('input',type=Path); p.add_argument('output',type=Path); p.add_argument('--name',default='OPi4Pro EDK2 v4 boot'); a=p.parse_args()
script=a.input.read_bytes()
# Legacy script payload starts with total length and zero, matching the vendor image.
payload=struct.pack('>II',len(script),0)+script
ts=int(os.environ.get('SOURCE_DATE_EPOCH',int(time.time())))
name=a.name.encode('ascii')[:31].ljust(32,b'\0')
h0=HEADER.pack(MAGIC,0,ts,len(payload),0,0,crc32(payload),5,2,6,0,name)
h=HEADER.pack(MAGIC,crc32(h0),ts,len(payload),0,0,crc32(payload),5,2,6,0,name)
a.output.parent.mkdir(parents=True,exist_ok=True); a.output.write_bytes(h+payload)
print(f'wrote {a.output} ({len(h)+len(payload)} bytes)')
