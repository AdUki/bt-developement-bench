# The SoC watchdog in U-Boot, for kernel trials: the bench's boot.scr arms it right before it
# boots a test kernel, so a kernel that hangs before systemd takes the watchdog over is reset
# too, and U-Boot then boots the good one (docs/kernel.md).
#
# U-Boot's environment needs nothing from the bench: rpi_*_defconfig keep it in uboot.env on
# the boot partition (ENV_IS_IN_FAT, mmc 0:1, ENV_SIZE 0x4000, no redundant copy), and
# meta-raspberrypi's u-boot bbappend already installs the matching /etc/fw_env.config
# ("/boot/uboot.env 0x0000 0x4000", package u-boot-env) for fw_setenv/fw_printenv.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append:rpi = " file://btbench-watchdog.cfg"
