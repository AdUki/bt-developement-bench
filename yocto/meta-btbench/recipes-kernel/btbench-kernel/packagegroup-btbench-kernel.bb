# What the image needs for its kernel and for kernel trials (docs/contracts.md: Image
# composition): the modules of whichever kernel the build selected (linux-btbench, or
# linux-raspberrypi with KERNEL=rpi), btbench-kernel, and for a board that tries kernels through
# U-Boot (TRY_METHOD=uboot-oneshot) fw_setenv/fw_printenv with the board's /etc/fw_env.config.
#
# The Wi-Fi and Bluetooth firmware are not here: the machine config recommends them
# (linux-firmware-rpidistro-bcm43430, bluez-firmware-rpidistro-bcm43430a1-hcd), and they already
# carry the board-specific names mainline asks for (docs/kernel.md).

SUMMARY = "Kernel modules and kernel trial tools for the Bluetooth development bench"

# Depends on the board's TRY_METHOD, so not the allarch a packagegroup defaults to.
PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit packagegroup

RDEPENDS:${PN} = " \
    kernel-modules \
    btbench-kernel \
    ${@'libubootenv-bin u-boot-default-env' if d.getVar('BTBENCH_TRY_METHOD') == 'uboot-oneshot' else ''} \
"
