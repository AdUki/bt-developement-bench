# BOARD=vim3l — Khadas VIM3L. Included by the Makefile.
#
# Amlogic S905D3 (SM1: four Cortex-A55, arm64), 2 GB. The AP6398S module is a BCM4359: Wi-Fi on
# SDIO (brcmfmac), Bluetooth on UART_A with RTS/CTS (hci_uart over serdev, brcm,bcm43438-bt in the
# mainline dts). The USB-C port is the dwc2 controller, which the mainline dts already makes a
# peripheral: the gadget link to the PC, and the board's power, over one cable. Gigabit Ethernet
# as well (DHCP through systemd-conf's 80-wired.network).
#
# Boot: the SoC's ROM loads Amlogic's signed BL2/BL3x and mainline U-Boot from the start of the
# SD card (meta-meson's amlogic-boot-fip, written by its image class); U-Boot's distro boot finds
# the bench's boot.scr on the FAT partition. A board whose eMMC or SPI flash holds a boot loader
# boots that first: see docs/kernel.md "Khadas VIM3L".

MACHINE    := khadas-vim3l
BSP_LAYERS := $(LAYERS)/meta-meson

# Only bluetooth-next: KERNEL=rpi is the Raspberry Pi fork.
KERNELS := next

KARCH  := arm64
KIMAGE := Image

# arm64's defconfig, cut down to Amlogic by the board fragment (other SoCs' drivers off).
DEFCONFIG_next := defconfig
DTBS_next      := amlogic/meson-sm1-khadas-vim3l.dtb
DTS_EXTRA_next :=
KFRAGMENT_BOARD := boards/vim3l.cfg

# U-Boot keeps its environment in uboot.env on the boot partition (the bench's U-Boot config), as
# on the Raspberry Pis, and boot.scr runs the trial.
TRY_METHOD  := uboot-oneshot
BOOT_LAYOUT := amlogic-fip

GADGET_UDC := ff400000.usb
BT_HCI     := hci0

SDK_ENV_GLOB := environment-setup-armv8*
