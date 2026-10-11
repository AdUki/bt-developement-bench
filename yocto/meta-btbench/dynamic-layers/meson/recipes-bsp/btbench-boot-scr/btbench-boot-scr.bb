# The bench's boot.scr for the Amlogic boards (BOARD=vim3l): U-Boot's distro boot runs it from the
# boot partition. It boots the good kernel, or once the test slot of a kernel trial (docs/kernel.md);
# the Raspberry Pis have theirs from meta-raspberrypi's rpi-u-boot-scr, with the trial added.

SUMMARY = "U-Boot boot script with the bench's kernel trial, for the Amlogic boards"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

COMPATIBLE_MACHINE = "^meson64all$"

DEPENDS = "u-boot-mkimage-native"

SRC_URI = "file://boot.cmd.in"
S = "${WORKDIR}"

inherit deploy nopackages

# The kernel command line, from the board conf. The good dtb is the first of the board's
# (boards/<board>.mk DTBS_next), under its basename on the boot partition (IMAGE_BOOT_FILES).
BTBENCH_CMDLINE ??= ""
BTBENCH_BOOT_DTB = "${@os.path.basename((d.getVar('BTBENCH_DTBS') or '').split()[0]) if d.getVar('BTBENCH_DTBS') else ''}"

do_compile() {
	[ -n "${BTBENCH_BOOT_DTB}" ] || bbfatal "BTBENCH_DTBS is empty: no device tree to boot"
	sed -e 's|@@KERNEL_IMAGETYPE@@|${KERNEL_IMAGETYPE}|g' \
	    -e 's|@@DTB@@|${BTBENCH_BOOT_DTB}|g' \
	    -e 's|@@CMDLINE@@|${BTBENCH_CMDLINE}|g' \
	    ${WORKDIR}/boot.cmd.in > ${B}/boot.cmd
	mkimage -A arm64 -O linux -T script -C none -n "btbench boot" -d ${B}/boot.cmd ${B}/boot.scr
}
do_compile[vardeps] += "BTBENCH_CMDLINE BTBENCH_BOOT_DTB KERNEL_IMAGETYPE"

do_deploy() {
	install -m 0644 ${B}/boot.scr ${DEPLOYDIR}/boot.scr
}
addtask deploy after do_compile before do_build
