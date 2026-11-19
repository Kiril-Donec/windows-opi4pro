#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail
if [[ $# -lt 3 || $# -gt 4 ]]; then
  echo "usage: $0 stage0_v13.img ORANGEPI4PRO_EFI.fd output.img [DEBUG|RELEASE]" >&2
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
python3 "$HERE/mk_legacy_uimage.py" "$FD" "$TMP/uefi.uimg" --name "Orange Pi 4 Pro EDK2 v9 ${FLAVOR}"
python3 "$HERE/mk_boot_script_uimage.py" "$HERE/boot_v9.cmd" "$TMP/boot.scr" --name "OPi4Pro EDK2 v9 boot"
cp "$BASE" "$OUT"
# Stage0 v13 MBR partition 1: LBA 65536, 65536 sectors (32 MiB ext4).
dd if="$OUT" of="$TMP/root.ext4" bs=512 skip=65536 count=65536 status=none
"$TMP/ext4_replace" "$TMP/root.ext4" /uefi_stage0.uimg "$TMP/uefi.uimg"
"$TMP/ext4_replace" "$TMP/root.ext4" /boot.scr "$TMP/boot.scr"
dd if="$TMP/root.ext4" of="$OUT" bs=512 seek=65536 conv=notrunc status=none
# Exact readback validation.
dd if="$OUT" of="$TMP/check.ext4" bs=512 skip=65536 count=65536 status=none
"$TMP/ext4_extract" "$TMP/check.ext4" /uefi_stage0.uimg "$TMP/readback.uimg" >/dev/null
"$TMP/ext4_extract" "$TMP/check.ext4" /boot.scr "$TMP/readback.scr" >/dev/null
cmp "$TMP/readback.uimg" "$TMP/uefi.uimg"
cmp "$TMP/readback.scr" "$TMP/boot.scr"
echo "Flashable image created and verified: $OUT"
