# BOARD=rpi0w — Raspberry Pi Zero W. Included by the Makefile.
#
# BCM2835 (ARMv6, arm1176jzf-s, one core, 512 MB). The BCM43430A1 Wi-Fi/BT combo: Wi-Fi on SDIO
# (brcmfmac), Bluetooth on the PL011 UART (hci_uart over serdev, with RTS/CTS). The data micro-USB
# port is the dwc2 OTG controller: a USB gadget (network + serial console) to the PC.
#
# Everything a target needs to know about the board goes in here, and the bitbake side of it in
# yocto/conf/boards/rpi0w.conf. The image renders these into /etc/btbench/board.env, so the
# target scripts (tools/target/) stay board-independent.

MACHINE    := raspberrypi0-wifi
BSP_LAYERS := $(LAYERS)/meta-raspberrypi

# Kernel build (make linux): ARCH, the make target and the file it leaves in arch/$(KARCH)/boot/.
KARCH  := arm
KIMAGE := zImage

# KERNEL=next: bluetooth-next (mainline). The board dtsi switches the dwc2 port to peripheral and
# is compiled in over the mainline dts by tools/dev/mkdtb (the firmware's overlays are not used).
DEFCONFIG_next := bcm2835_defconfig
DTBS_next      := broadcom/bcm2835-rpi-zero-w.dtb
DTS_EXTRA_next := boards/dts/rpi0w-btbench.dtsi

# KERNEL=rpi: the Raspberry Pi fork (linux-raspberrypi).
DEFCONFIG_rpi  := bcmrpi_defconfig
DTBS_rpi       := broadcom/bcm2708-rpi-zero-w.dtb
DTS_EXTRA_rpi  :=

# Kernel trials (make linux-test): U-Boot boots the test slot once, then the good one again.
# (tryboot needs the downstream kernel to pass the firmware's reboot flag; mainline does not.)
TRY_METHOD  := uboot-oneshot
# Boot partition layout: the RPi firmware loads U-Boot (kernel.img), U-Boot boot.scr the kernel.
BOOT_LAYOUT := rpi-firmware

# The dwc2 controller as a USB device. Empty: no gadget (the OTG port is then a USB host, e.g. for
# a Bluetooth dongle, and the board is only reachable over Wi-Fi).
GADGET_UDC := 20980000.usb

# The onboard controller.
BT_HCI := hci0

# The SDK's environment script (one per tune).
SDK_ENV_GLOB := environment-setup-arm1176jzfshf*
