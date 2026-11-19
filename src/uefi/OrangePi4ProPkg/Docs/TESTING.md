# Hardware test procedure

## Safety

Use a spare microSD card. The image overwrites the beginning of the selected
drive. Keep the official Orange Pi card untouched as a recovery card.

## Flash

On Linux:

```bash
sudo dd if=orangepi4pro_edk2_v2_dxe_fix_debug_flashable.img of=/dev/sdX \
  bs=4M conv=fsync status=progress
```

On Windows, use Rufus or balenaEtcher in raw-image mode. The image is only
64 MiB; writing it to a larger card is expected.

## Connections

- UART: 115200, 8 data bits, no parity, 1 stop bit.
- Connect HDMI before power-on.
- Start with no USB storage; add a simple keyboard after the EDK2 console
  appears. Add a FAT32 USB stick only after keyboard testing.

## Expected UART checkpoints

1. Vendor boot0 / BL31 / U-Boot output.
2. Boot script prints `Orange Pi 4 Pro A733 EDK2 v2 DXE fix`.
3. Earliest EDK2 marker:

   ```text
   [A733 EDK2 v2] SEC entered at 0x41000000
   ```

4. DEBUG messages from PrePi and DXE.
5. `A733 EHCI1 CAPBASE=...` if USB initialization reaches the controller.
6. BDS/UEFI Shell startup.

## Interpreting a stop

- No vendor text: image/early boot issue; restore the known-good Stage0 v13.
- Vendor text but no EDK2 marker: legacy uImage handoff/load address problem.
- Marker only: failure in VFP, stack, MMU or HOB construction.
- DXE logs but no HDMI: inherited framebuffer/GOP mismatch; UART remains usable.
- Shell but no keyboard: USB clock/PHY/EHCI or interrupt/coherency issue.
- Keyboard works but USB storage does not: mass-storage transport or DMA issue.

## Manual U-Boot test

The same payload can be tested without replacing `boot.scr`:

```text
screen_char
setenv stdout serial,vidconsole
setenv stderr serial,vidconsole
ext4load mmc 0:1 0x41000000 /uefi_stage0.uimg
ext4load mmc 0:1 0x43000000 /dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb
bootm 0x41000000 - 0x43000000
```

Use DEBUG first. RELEASE keeps the early UART marker but removes most EDK2
DEBUG output, so it is less useful during first board bring-up.
