#!/usr/bin/env bash
# Build the Orange Pi 4 Pro (A733) EDK2 UEFI and pack it into a 64 MiB
# flashable image (same layout as firmware/opi4pro-a733-uefi-v60.img.gz).
#
#   EDK2=/path/to/edk2 ./build-v60.sh
#
# Needs: an EDK2 tree (with BaseTools built), clang/lld (LLVM 19 was used),
# python3, e2fsprogs (debugfs, e2fsck). Linux or WSL.
#
# The first 32 MiB of the image (MBR, boot0, U-Boot/BL31 from Orange Pi) and
# the device tree in partition 1 come from BASE_IMAGE; only /uefi_stage0.uimg
# and /boot.scr in the ext4 partition 1 are replaced.
#
# Options (environment):
#   A733_CPUFREQ_LEVEL  1..4, default 2 (A76 1896 MHz). 3 = 1992 MHz, 4 = overclock (needs cooling)
#   A733_SPCR_UART      7 (default: Windows EMS/SAC console on UART7 header pins 8/10) or 0 (UART0)
#   A733_UART_BAUD      115200
#   BASE_IMAGE          default: ../../firmware/opi4pro-a733-uefi-v60.img.gz (unpacked)
#   OUT                 default: ./out
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
EDK2=${EDK2:?set EDK2 to your edk2 directory}
LEVEL=${A733_CPUFREQ_LEVEL:-2}
SPCRUART=${A733_SPCR_UART:-7}
UARTBAUD=${A733_UART_BAUD:-115200}
OUT=${OUT:-$HERE/out}
WORK=$OUT/work
mkdir -p "$OUT" "$WORK"

if [ -z "${BASE_IMAGE:-}" ]; then
  BASE_IMAGE=$WORK/base.img
  gzip -dc "$HERE/../../firmware/opi4pro-a733-uefi-v60.img.gz" > "$BASE_IMAGE"
fi

PKG=$EDK2/Platform/OrangePi/OrangePi4ProPkg
rm -rf "$PKG"
mkdir -p "$(dirname "$PKG")"
cp -r "$HERE/OrangePi4ProPkg" "$PKG"

cd "$EDK2"
export WORKSPACE="$EDK2" PACKAGES_PATH="$EDK2" PYTHON_COMMAND=python3
export EDK_TOOLS_PATH="$EDK2/BaseTools" CONF_PATH="$EDK2/Conf"
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"
if [ -d /usr/lib/llvm-19/bin ]; then export PATH="/usr/lib/llvm-19/bin:$PATH"; fi   # the toolchain this was tested with
set +u
source edksetup.sh --reconfig
set -u
rm -rf "$EDK2/Build/OrangePi4Pro-AARCH64"
build -a AARCH64 -t CLANGDWARF -b DEBUG \
  -p Platform/OrangePi/OrangePi4ProPkg/OrangePi4ProUsbOem.dsc \
  --pcd gOrangePi4ProTokenSpaceGuid.PcdA733CpuFreqLevel="$LEVEL" \
  --pcd gOrangePi4ProTokenSpaceGuid.PcdA733UsbBridgeClockMode=0 \
  --pcd gEfiMdeModulePkgTokenSpaceGuid.PcdSerialBaudRate="$UARTBAUD" \
  --pcd gOrangePi4ProTokenSpaceGuid.PcdA733SpcrUart="$SPCRUART" \
  --pcd gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate="$UARTBAUD" \
  -n "$(nproc)" 2>&1 | tee "$OUT/build.log"
test "${PIPESTATUS[0]}" -eq 0
FD=$EDK2/Build/OrangePi4Pro-AARCH64/DEBUG_CLANGDWARF/FV/ORANGEPI4PRO_EFI.fd

python3 "$HERE/make-uimage.py" --type kernel --name "OPi4Pro EDK2 v60 L$LEVEL" \
  --load 0x41000000 --entry 0x41000000 "$FD" "$WORK/uefi_stage0.uimg"
python3 "$HERE/make-uimage.py" --type script --name "OrangePi4Pro EDK2 v60 boot" \
  "$HERE/OrangePi4ProPkg/Tools/boot_v60.cmd" "$WORK/boot.scr"

IMG=$OUT/opi4pro-a733-uefi-v60-l$LEVEL.img
cp "$BASE_IMAGE" "$IMG"
dd if="$IMG" of="$WORK/root.ext4" bs=512 skip=65536 count=65536 status=none
debugfs -w -R "rm /uefi_stage0.uimg" "$WORK/root.ext4"
debugfs -w -R "write $WORK/uefi_stage0.uimg /uefi_stage0.uimg" "$WORK/root.ext4"
debugfs -w -R "rm /boot.scr" "$WORK/root.ext4"
debugfs -w -R "write $WORK/boot.scr /boot.scr" "$WORK/root.ext4"
e2fsck -fn "$WORK/root.ext4"
dd if="$WORK/root.ext4" of="$IMG" bs=512 seek=65536 conv=notrunc status=none
cmp -n $((65536 * 512)) "$BASE_IMAGE" "$IMG"   # boot area (MBR, boot0, U-Boot) unchanged
gzip -9kf "$IMG"
sha256sum "$IMG"
echo "built: $IMG(.gz) - copy the .gz over firmware/opi4pro-a733-uefi-v60.img.gz to use it with the installer"
