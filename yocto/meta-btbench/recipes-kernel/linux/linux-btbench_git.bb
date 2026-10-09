# bluetooth-next: the kernel the bench's image boots by default (KERNEL=next).
#
# The same tree `make linux` builds from (src/linux), so the image's kernel is the one you develop
# against; `make image SRC=local` bakes src/linux in through externalsrc. The revision is pinned in
# yocto/conf/versions.conf: bluetooth-next is rebased onto net-next and is sometimes broken, so it
# is bumped on purpose (`make bump-versions`), never followed with AUTOREV.
#
# Plain `kernel` rather than linux-yocto's kernel-yocto: the configuration is the board's upstream
# defconfig plus one fragment, merged with the kernel's own merge_config.sh, the way `make linux`
# does it. kernel-yocto's kernel-cache/kmeta machinery would only add a second way of doing that.

SUMMARY = "bluetooth-next kernel for the Bluetooth development bench"
SECTION = "kernel"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

inherit kernel

# The fragment lives next to this recipe (the linux-raspberrypi bbappend and `make linux` use the
# same file). mkdtb and the board's dtsi come from the bench checkout, and are listed in SRC_URI
# rather than read in place so that bitbake tracks their checksums: an edit to either rebuilds
# the kernel's device tree.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:${BTBENCH_ROOT}/tools/dev:"
FILESEXTRAPATHS:prepend := "${@os.path.dirname(d.getVar('BTBENCH_DTS_EXTRA')) + ':' if d.getVar('BTBENCH_DTS_EXTRA') else ''}"

BTBENCH_DTS_EXTRA_FILE = "${@os.path.basename(d.getVar('BTBENCH_DTS_EXTRA') or '')}"

# The patches are fixes the bench needs and bluetooth-next does not have yet, each meant for
# upstream; drop one once the pinned revision contains it.
# - dwc2 NAK on OUT endpoints: without it the USB gadget's network dies at nearly every boot. The
#   PC (ModemManager) writes to the ACM port before a getty has opened ttyGS0, and that packet
#   blocks the receive FIFO the NCM endpoint shares with it.
SRC_URI = " \
    git://git.kernel.org/pub/scm/linux/kernel/git/bluetooth/bluetooth-next.git;protocol=https;branch=master \
    file://0001-usb-dwc2-gadget-NAK-OUT-endpoints-until-a-request-is.patch \
    file://btbench.cfg \
    file://mkdtb \
    ${@'file://${BTBENCH_DTS_EXTRA_FILE}' if d.getVar('BTBENCH_DTS_EXTRA') else ''} \
"
SRCREV = "${BTBENCH_LINUX_SRCREV}"
PV = "${BTBENCH_LINUX_PV}"
S = "${WORKDIR}/git"

# Every Raspberry Pi the bench knows. Another SoC family adds its machine override here (and a
# board conf that selects linux-btbench as virtual/kernel).
COMPATIBLE_MACHINE = "^rpi$"

# With SRC=local (externalsrc) ${S} is your src/linux: externalsrc's oe-workdir/oe-logs links are
# not put into it, and nothing below writes there either.
EXTERNALSRC_SYMLINKS = ""

# The board's defconfig from the kernel tree (boards/<b>.mk DEFCONFIG_next, through auto.conf).
KBUILD_DEFCONFIG = "${BTBENCH_KDEFCONFIG}"

# "-btbench" marks the bench's kernels; the fragment's CONFIG_LOCALVERSION_AUTO adds the git
# revision after it (7.3.0-rc2-btbench-00421-g97a128698d9d), so an image kernel and a dev build of
# another revision never share a /lib/modules directory.
KERNEL_LOCALVERSION = "-btbench"

# Symbols in every dtb (-@): overlays applied at runtime (configfs, or U-Boot's fdt apply) can then
# refer to the board's labels, as they can with the downstream kernel's dtbs.
KERNEL_DTC_FLAGS = "-@"

do_configure:prepend() {
	# kernel.bbclass writes .scmversion into ${S} when neither copy exists. With SRC=local ${S}
	# is your src/linux, and the build must not leave files in it. The kernel stopped reading
	# .scmversion in 6.3, so an empty one in ${B} is enough to keep the class out of ${S}.
	touch ${B}/.scmversion

	# The board defconfig, then the bench fragment on top. merge_config.sh -m only merges; the
	# class's olddefconfig (KERNEL_CONFIG_COMMAND) resolves dependencies afterwards. A .config
	# already in ${B} is from a previous run of this task and is replaced, so a changed fragment
	# takes effect.
	rm -f ${B}/.config
	oe_runmake_call -C ${S} O=${B} ${KBUILD_DEFCONFIG}
	${S}/scripts/kconfig/merge_config.sh -m -O ${B} ${B}/.config ${WORKDIR}/btbench.cfg
}

do_configure:append() {
	# merge_config.sh -m does not say when a fragment value does not survive olddefconfig (a
	# symbol renamed or gone, or a dependency missing), and the bench would then quietly lack
	# what it was asked for. Name them in the build log.
	btbench_config_check ${WORKDIR}/btbench.cfg ${B}/.config
}

btbench_config_check() {
	awk '
		FNR == NR {
			if (match($0, /^CONFIG_[A-Za-z0-9_]+=/)) {
				sym = substr($0, 1, RLENGTH - 1); want[sym] = substr($0, RLENGTH + 1)
			} else if (match($0, /^# CONFIG_[A-Za-z0-9_]+ is not set/)) {
				sym = $2; want[sym] = "n"
			}
			next
		}
		match($0, /^CONFIG_[A-Za-z0-9_]+=/) { have[substr($0, 1, RLENGTH - 1)] = substr($0, RLENGTH + 1) }
		END {
			for (sym in want) {
				h = (sym in have) ? have[sym] : "n"
				if (h != want[sym])
					printf "%s: requested %s, got %s\n", sym, want[sym], h
			}
		}' "$1" "$2" | sort | while read -r line; do
		bbwarn "btbench.cfg: $line"
	done
}

do_compile:append() {
	# The board's dtsi (boards/dts/, BTBENCH_DTS_EXTRA) compiled in over the board's dts. The
	# kernel's own dtbs target has just built the plain dtb; it is replaced in ${B}, so
	# do_install, do_deploy and the boot partition all carry the bench's. tools/dev/mkdtb is the
	# same script `make linux` uses, with the dtc this build just made.
	if [ -n "${BTBENCH_DTS_EXTRA}" ]; then
		for dtb in ${BTBENCH_DTBS}; do
			DTC=${B}/scripts/dtc/dtc CPP="${BUILD_CPP}" \
				bash ${WORKDIR}/mkdtb ${S} ${ARCH} ${dtb%.dtb}.dts \
					${WORKDIR}/${BTBENCH_DTS_EXTRA_FILE} \
					${B}/arch/${ARCH}/boot/dts/$dtb
		done
	fi
}

# The kernel generates arch/arm/include/generated/asm/mach-types.h with the path of the script it
# came from in a comment, and the -src (debug source) package picks it up. Nothing on the board
# uses that path.
INSANE_SKIP:${PN}-src += "buildpaths"
