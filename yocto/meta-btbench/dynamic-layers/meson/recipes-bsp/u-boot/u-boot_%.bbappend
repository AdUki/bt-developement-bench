# U-Boot for kernel trials on the VIM3L (docs/kernel.md): the bench's boot.scr keeps bench_try
# in U-Boot's environment, and btbench-kernel sets it from Linux with fw_setenv.
#
# khadas-vim3l_defconfig keeps no environment (ENV_IS_NOWHERE): saveenv has nowhere to go and
# fw_setenv nothing to write. btbench-env.cfg puts it in uboot.env on the SD card's boot
# partition, as rpi_*_defconfig do, and fw_env.config (package u-boot-env) points libubootenv at
# the same file.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append:khadas-vim3l = " file://btbench-env.cfg file://fw_env.config"
