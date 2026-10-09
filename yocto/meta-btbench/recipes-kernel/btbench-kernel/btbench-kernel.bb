# Kernel trials on the board: `btbench-kernel` (try, commit, rollback; tools/target/) and the
# watchdog that turns a hung trial kernel into a reset (docs/kernel.md).
#
# What the trial needs from the boot loader (fw_setenv, /etc/fw_env.config) depends on the
# board's TRY_METHOD and comes with packagegroup-btbench-kernel.

SUMMARY = "Kernel trial boots for the Bluetooth development bench"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

FILESEXTRAPATHS:prepend := "${BTBENCH_ROOT}/tools/target:"

SRC_URI = " \
    file://btbench-kernel \
    file://10-btbench-watchdog.conf \
"

S = "${WORKDIR}"

inherit allarch

do_install() {
	install -D -m 0755 ${WORKDIR}/btbench-kernel ${D}${bindir}/btbench-kernel
	install -D -m 0644 ${WORKDIR}/10-btbench-watchdog.conf \
		${D}${sysconfdir}/systemd/system.conf.d/10-btbench-watchdog.conf
}

FILES:${PN} = " \
    ${bindir}/btbench-kernel \
    ${sysconfdir}/systemd/system.conf.d/10-btbench-watchdog.conf \
"
