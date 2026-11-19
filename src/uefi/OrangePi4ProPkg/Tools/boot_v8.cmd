echo Orange Pi 4 Pro A733 EDK2 v8 EHCI low-DMA schedule fix
echo EHCI low DMA + clean USBCMD + DSB/readback diagnostics
screen_char
setenv stdout serial,vidconsole
setenv stderr serial,vidconsole
echo HDMI inherited from vendor bootloader
ext4load mmc 0:1 0x41000000 /uefi_stage0.uimg
ext4load mmc 0:1 0x43000000 /dtb/allwinner/sun60i-a733-orangepi-4-pro.dtb
echo Starting EDK2 v8 through vendor bootm and BL31...
bootm 0x41000000 - 0x43000000
echo ERROR: EDK2 v8 returned
while true; do sleep 1; done
