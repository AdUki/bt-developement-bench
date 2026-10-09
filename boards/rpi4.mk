# BOARD=rpi4 — Raspberry Pi 4 Model B (BCM2711, Cortex-A72, arm64). Included by the Makefile.
#
# A second board, there to keep the bench honest about what is board-specific. It parses and its
# kernel builds; it has not been booted. BCM4345C0 on the PL011 (hci_uart serdev), USB-C is the
# dwc2 OTG port.

MACHINE    := raspberrypi4-64
BSP_LAYERS := $(LAYERS)/meta-raspberrypi

KARCH  := arm64
KIMAGE := Image

DEFCONFIG_next := defconfig
DTBS_next      := broadcom/bcm2711-rpi-4-b.dtb
DTS_EXTRA_next :=

DEFCONFIG_rpi  := bcm2711_defconfig
DTBS_rpi       := broadcom/bcm2711-rpi-4-b.dtb
DTS_EXTRA_rpi  :=

# The Pi 4 firmware documents tryboot; it needs the downstream kernel to pass the reboot flag,
# so a mainline kernel on this board would use uboot-oneshot like the Zero W.
TRY_METHOD  := $(if $(filter rpi,$(KERNEL)),tryboot,uboot-oneshot)
BOOT_LAYOUT := rpi-firmware

GADGET_UDC := fe980000.usb
BT_HCI     := hci0

SDK_ENV_GLOB := environment-setup-cortexa72*
