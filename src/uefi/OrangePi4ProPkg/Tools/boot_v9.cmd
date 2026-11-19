echo Orange Pi 4 Pro A733 EDK2 v9 EHCI clock-reset recovery
echo Linux-order clocks + SIDDQ/PHY tune + post-HCReset vendor restore
hdmidet
fatload mmc 0:1 0x41000000 /uefi_stage0.uimg
fatload mmc 0:1 0x43000000 /sun60i-a733-orangepi-4-pro.dtb
echo Starting EDK2 v9 through vendor bootm and BL31...
bootm 0x41000000 - 0x43000000
echo ERROR: EDK2 v9 returned
