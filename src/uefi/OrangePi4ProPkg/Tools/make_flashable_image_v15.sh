#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
if [[ $# -lt 3 || $# -gt 4 ]]; then
  echo "usage: $0 known-good-v13.img ORANGEPI4PRO_EFI.fd output.img [DEBUG|RELEASE]" >&2
  exit 2
fi
BASE=$1
FD=$2
OUT=$3
FLAVOR=${4:-DEBUG}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cc -O2 -Wall -Wextra "$HERE/ext4_replace.c" -o "$TMP/ext4_replace" -lext2fs -lcom_err
cc -O2 -Wall -Wextra "$HERE/ext4_extract.c" -o "$TMP/ext4_extract" -lext2fs -lcom_err
python3 "$HERE/mk_legacy_uimage.py" "$FD" "$TMP/uefi.uimg" --name "Orange Pi 4 Pro EDK2 v15 ${FLAVOR}"
python3 "$HERE/mk_boot_script_uimage.py" "$HERE/boot_v15.cmd" "$TMP/boot.scr" --name "OPi4Pro EDK2 v15 boot"

# Always start from the physically proven v13 image. It already contains the
# exact vendor DTB path used by the successful v13 Windows Setup test.
cp "$BASE" "$OUT"
dd if="$OUT" of="$TMP/root.ext4" bs=512 skip=65536 count=65536 status=none

# Refuse to create an image if the known-good base no longer contains the DTB.
"$TMP/ext4_extract" "$TMP/root.ext4" /dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb "$TMP/board.dtb" >/dev/null
python3 - "$TMP/board.dtb" <<'PY'
import struct,sys
p=sys.argv[1]
b=open(p,'rb').read()
if len(b)<40:
    raise SystemExit('DTB is too small')
magic,total=struct.unpack('>II',b[:8])
if magic != 0xD00DFEED:
    raise SystemExit(f'bad DTB magic 0x{magic:08x}')
if total > len(b) or total < 40:
    raise SystemExit(f'bad DTB totalsize {total} file={len(b)}')
print(f'DTB verified: size={len(b)} totalsize={total}')
PY

"$TMP/ext4_replace" "$TMP/root.ext4" /uefi_stage0.uimg "$TMP/uefi.uimg"
"$TMP/ext4_replace" "$TMP/root.ext4" /boot.scr "$TMP/boot.scr"
dd if="$TMP/root.ext4" of="$OUT" bs=512 seek=65536 conv=notrunc status=none

# Read everything back from the final 64-MiB image, not from temporary inputs.
dd if="$OUT" of="$TMP/check.ext4" bs=512 skip=65536 count=65536 status=none
"$TMP/ext4_extract" "$TMP/check.ext4" /uefi_stage0.uimg "$TMP/readback.uimg" >/dev/null
"$TMP/ext4_extract" "$TMP/check.ext4" /boot.scr "$TMP/readback.scr" >/dev/null
"$TMP/ext4_extract" "$TMP/check.ext4" /dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb "$TMP/readback.dtb" >/dev/null
cmp "$TMP/readback.uimg" "$TMP/uefi.uimg"
cmp "$TMP/readback.scr" "$TMP/boot.scr"
cmp "$TMP/readback.dtb" "$TMP/board.dtb"

# Regression guards for the exact v12 packaging failure.
strings "$TMP/readback.scr" > "$TMP/boot.strings"
grep -Fq '/dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb' "$TMP/boot.strings"
if grep -Fq 'hdmidet' "$TMP/boot.strings"; then
  echo 'ERROR: forbidden unsupported hdmidet command found in boot.scr' >&2
  exit 1
fi
if grep -Fq '/sun60iw2p1-orangepi-4-pro.dtb' "$TMP/boot.strings"; then
  echo 'ERROR: old missing DTB path found in boot.scr' >&2
  exit 1
fi

[[ $(stat -c %s "$OUT") -eq 67108864 ]]
printf 'Flashable v15 image created and verified: %s\n' "$OUT"
