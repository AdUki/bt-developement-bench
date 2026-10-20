# BOARD=rpi5 — Raspberry Pi 5 with an Intel BE200 on an M.2 HAT. Included by the Makefile.
#
# BCM2712 (four Cortex-A76, arm64), 2-16 GB. Onboard, a CYW43455 (Wi-Fi on SDIO, brcmfmac;
# Bluetooth on the BCM2712's UART with RTS/CTS, hci_uart over serdev), as on a Pi 4. On the PCIe
# FFC connector, an M.2 E-key HAT with the Intel BE200 (Wi-Fi 7, Bluetooth 5.4 with LE Audio):
# Wi-Fi on PCIe (iwlwifi), Bluetooth on the slot's USB pins (btusb + btintel). USB-C is the power
# input and the BCM2712's dwc2 controller: the gadget link to the PC. Gigabit Ethernet on the RP1.
#
# Not booted yet: the image builds. docs/kernel.md "Raspberry Pi 5" has what to check first.

MACHINE := raspberrypi5
# meta-lts-mixins' scarthgap/u-boot: U-Boot 2025.04. Scarthgap's own 2024.01 predates the Pi 5;
# meta-raspberrypi recommends this layer for it (LAYERRECOMMENDS). Only this board takes it.
BSP_LAYERS := $(LAYERS)/meta-raspberrypi $(LAYERS)/meta-lts-mixins-u-boot

KARCH  := arm64
KIMAGE := Image

# The SoC stepping, which mainline gives a device tree each: the D0 moved the pin controllers and
# the debug UART's interrupt, and a C1 dtb on a D0 board (or the other way round) does not boot.
# The firmware cannot be asked to choose (config.txt has no filter for the stepping), so it is
# set here. d0: every 2 GB and 16 GB board, and later 4 GB and 8 GB ones; c1: the first 4 GB and
# 8 GB boards. Board revision 1.0 (`Revision : c04170`/`d04170` in /proc/cpuinfo under Raspberry
# Pi OS) has been a C1, 1.1 (...4171) a D0. A wrong one stops after the firmware: no kernel
# output on the debug UART.
RPI5_SOC ?= d0
ifeq ($(filter c1 d0,$(RPI5_SOC)),)
  $(error RPI5_SOC=$(RPI5_SOC) is not one of: c1 d0)
endif

# KERNEL=next: bluetooth-next (mainline). arm64's defconfig cut down to Broadcom by the board
# fragment. The board dtsi adds the USB-C port's dwc2 (absent from mainline) as a peripheral and
# the ramoops region.
DEFCONFIG_next := defconfig
DTBS_next      := broadcom/$(if $(filter d0,$(RPI5_SOC)),bcm2712-d-rpi-5-b,bcm2712-rpi-5-b).dtb
DTS_EXTRA_next := boards/dts/rpi5-btbench.dtsi

# KERNEL=rpi: the Raspberry Pi fork. Its firmware picks the stepping's device tree by itself.
DEFCONFIG_rpi  := bcm2712_defconfig
DTBS_rpi       := broadcom/bcm2712-rpi-5-b.dtb
DTS_EXTRA_rpi  :=

KFRAGMENT_BOARD := boards/rpi5.cfg

# As on the Pi 4: tryboot needs the downstream kernel to pass the firmware's reboot flag.
TRY_METHOD  := $(if $(filter rpi,$(KERNEL)),tryboot,uboot-oneshot)
BOOT_LAYOUT := rpi-firmware

# The BCM2712's dwc2 (usb@1000480000 in the board dtsi; the downstream kernel's dwc2 overlay
# names it the same).
GADGET_UDC := 1000480000.usb

# The onboard CYW43455. The BE200 on USB comes up as another hciN (btbench-bdaddr tells them apart
# by their bus, docs/system.md "A second adapter").
BT_HCI := hci0

SDK_ENV_GLOB := environment-setup-cortexa76*
