# BlueZ from bluez.git at the revision pinned in yocto/conf/versions.conf, the same line the dev
# tree in src/bluez follows (make bump-versions moves the pin to its HEAD). poky's bluez5.inc does
# the packaging; this recipe swaps the release tarball for git and builds everything a BlueZ
# developer reaches for on the board: the testers, the emulator, the deprecated hci* tools, the
# BTP client and the python test scripts.
require recipes-connectivity/bluez5/bluez5.inc

# bluez5.inc's SRC_URI also names its init script and ptest runner, which live next to poky's own
# recipe, not this one.
FILESEXTRAPATHS:prepend := "${COREBASE}/meta/recipes-connectivity/bluez5/bluez5:"

# nobranch: the pin is any commit of bluez.git, so `make bump-versions` can take whatever the
# dev tree sits on (master, a tag, a commit picked from a reviewed series) as long as it is
# upstream. poky's 5.72 patches are all upstream by now and are not carried over.
SRC_URI = "git://git.kernel.org/pub/scm/bluetooth/bluez.git;protocol=https;nobranch=1 \
           file://init \
           file://run-ptest \
           file://10-btbench.conf \
           "
SRCREV = "${BTBENCH_BLUEZ_SRCREV}"
PV = "${BTBENCH_BLUEZ_PV}"
S = "${WORKDIR}/git"

# meta-raspberrypi patches tools/hciattach for the BCM43xx UART and makes bluez5 pull in
# pi-bluetooth (btuart, hciuart.service). The bench attaches the radio with the kernel's serdev
# driver (hci_uart bcm) on every kernel it boots, so the UART has no tty for hciattach to open:
# the patches would only be extra fuzz against master, and hciuart.service would sit waiting 90 s
# for dev-serial1.device at every boot.
SRC_URI:remove:rpi = " \
    file://0001-bcm43xx-Add-bcm43xx-3wire-variant.patch \
    file://0002-bcm43xx-The-UART-speed-must-be-reset-after-the-firmw.patch \
    file://0003-Increase-firmware-load-timeout-to-30s.patch \
    file://0004-Move-the-hciattach-firmware-into-lib-firmware.patch \
"
RDEPENDS:${PN}:remove:rpi = "pi-bluetooth"

# bluez5.inc's defaults plus the testers (testing) and the BTP client (btpclient, against poky's
# ell). A configure switch that is not listed here keeps upstream's default, which for the LE Audio
# profiles (bap, bass, vcp, micp, csip, ccp, tmap, gmap, asha) and the new hfp plugin is "on".
PACKAGECONFIG ??= " \
    ${@bb.utils.filter('DISTRO_FEATURES', 'systemd', d)} \
    readline tools deprecated testing btpclient udev \
    a2dp-profiles avrcp-profiles mcp-profiles \
    network-profiles hid-profiles hog-profiles obex-profiles \
"

# SAP and HDP were removed from BlueZ after 5.72; their --disable-* switches would only trip the
# unknown-configure-option QA check.
unset PACKAGECONFIG[sap-profiles]
unset PACKAGECONFIG[health-profiles]

# --enable-experimental gates nothing in the Makefiles at the pinned revision, but it is what
# upstream uses to hide unfinished tools, so they get built when one appears. --enable-debug adds
# -g on top of what bitbake passes (it does nothing else in configure.ac). Neither is the runtime
# switch: that is Experimental in main.conf (below) and -E on the command line.
EXTRA_OECONF += "--enable-experimental --enable-debug"

# Upstream's noinst_PROGRAMS (Makefile.tools) at the pinned revision; bluez5.inc copies each one
# into ${bindir} and packages them as ${PN}-noinst-tools. The list is upstream's, not a selection:
# on a bench the odd one out (btvirt, isotest's testers, the firmware tools) is exactly the one
# that is needed some day. do_install warns when a bump brings a new one that is not listed here.
NOINST_TOOLS_READLINE = " \
    tools/btmgmt \
    tools/obex-client-tool \
    tools/obex-server-tool \
    tools/bluetooth-player \
    tools/obexctl \
"
NOINST_TOOLS_TESTING = " \
    emulator/btvirt \
    emulator/b1ee \
    emulator/hfp \
    peripheral/btsensor \
    tools/3dsp \
    tools/mgmt-tester \
    tools/gap-tester \
    tools/l2cap-tester \
    tools/sco-tester \
    tools/smp-tester \
    tools/hci-tester \
    tools/rfcomm-tester \
    tools/bnep-tester \
    tools/userchan-tester \
    tools/iso-tester \
    tools/mesh-tester \
    tools/ioctl-tester \
    tools/6lowpan-tester \
"
NOINST_TOOLS_BT = " \
    tools/bdaddr \
    tools/avinfo \
    tools/avtest \
    tools/scotest \
    tools/hwdb \
    tools/hcieventmask \
    tools/hcisecfilter \
    tools/btinfo \
    tools/btconfig \
    tools/btsnoop \
    tools/btproxy \
    tools/btiotest \
    tools/bneptest \
    tools/cltest \
    tools/oobtest \
    tools/advtest \
    tools/seq2bseq \
    tools/nokfw \
    tools/rtlfw \
    tools/bcmfw \
    tools/create-image \
    tools/eddystone \
    tools/ibeacon \
    tools/btgatt-client \
    tools/btgatt-server \
    tools/test-runner \
    tools/check-selftest \
    tools/gatt-service \
    profiles/iap/iapd \
    ${@bb.utils.contains('PACKAGECONFIG', 'btpclient', 'client/btpclient/btpclient client/btpclient/btpclientctl', '', d)} \
"

# btmgmt in a package of its own, listed ahead of -noinst-tools so it wins the file: the image's
# board setup (BD address, controller settings) uses it, and it can then be installed without the
# rest of the tools.
PACKAGES =+ "${PN}-btmgmt"
FILES:${PN}-btmgmt = "${bindir}/btmgmt"

do_install:append() {
    # Unlike 5.72, master's make install ships main.conf (conf_DATA). Three of its commented-out
    # defaults are turned on:
    #
    #   Experimental = true      BAP, BASS, VCP, MICP, CSIP, MCP, TMAP, GMAP and ASHA are
    #                            registered as experimental plugins: without this bluetoothd has
    #                            no LE Audio at all.
    #   KernelExperimental = <ISO socket>
    #                            the kernel only creates ISO sockets once this experimental
    #                            feature is set. The BCM43430 has no ISO, but a USB dongle or
    #                            btvirt/vhci does.
    #   JustWorksRepairing = confirm
    #                            a reflash wipes the bonds while the phone keeps its key. "never"
    #                            (the default) refuses the phone until someone finds "forget
    #                            device" on it; "confirm" asks the agent.
    #
    # Each edit is checked, so an upstream rewording cannot silently leave the default in force.
    sed -i -e 's|^#Experimental = false$|Experimental = true|' \
           -e 's|^#KernelExperimental = false$|KernelExperimental = 6fbaf188-05e0-496a-9885-d6ddfdb4e03e|' \
           -e 's|^#JustWorksRepairing = never$|JustWorksRepairing = confirm|' \
           ${D}${sysconfdir}/bluetooth/main.conf
    for l in 'Experimental = true' \
             'KernelExperimental = 6fbaf188-05e0-496a-9885-d6ddfdb4e03e' \
             'JustWorksRepairing = confirm'; do
        grep -qx "$l" ${D}${sysconfdir}/bluetooth/main.conf || \
            bbfatal "main.conf: could not set '$l' - the upstream file has changed"
    done

    # The drop-in that takes bluetoothd's arguments from /data (see the file).
    if ${@bb.utils.contains('PACKAGECONFIG', 'systemd', 'true', 'false', d)}; then
        install -d ${D}${systemd_system_unitdir}/bluetooth.service.d
        sed -e 's|@LIBEXECDIR@|${libexecdir}|g' ${WORKDIR}/10-btbench.conf \
            > ${D}${systemd_system_unitdir}/bluetooth.service.d/10-btbench.conf
        chmod 0644 ${D}${systemd_system_unitdir}/bluetooth.service.d/10-btbench.conf
    fi

    # Warn when the build has a noinst program the lists above do not package (a new tester after
    # a bump, say): it would be built and then silently left out of the image.
    printf 'include Makefile\nbtbench-print-noinst:\n\t@echo $(noinst_PROGRAMS)\n' > ${B}/btbench-noinst.mk
    for t in $(make -s --no-print-directory -C ${B} -f btbench-noinst.mk btbench-print-noinst); do
        case " ${NOINST_TOOLS} " in
        *" $t "*) ;;
        *) bbwarn "$t is built but not packaged: add it to NOINST_TOOLS_* in bluez5_git.bb" ;;
        esac
    done
    rm -f ${B}/btbench-noinst.mk
}

