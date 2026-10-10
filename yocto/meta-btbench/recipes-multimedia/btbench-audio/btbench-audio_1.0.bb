SUMMARY = "The audio stack and the HCI capture ring of the Bluetooth development bench"
DESCRIPTION = "btbench-audio starts and restarts the audio stack (PipeWire and WirePlumber, one \
systemd target); btbench-btsnoop keeps an always-on ring of btmon captures. Also loads snd-aloop, \
the board's sound card, and points root's PipeWire tools at the system-wide daemon."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# The two scripts live in the repo's tools/target/ (deploy-audio-scripts pushes them to a running
# board without a rebuild); the units and config next to this recipe.
FILESEXTRAPATHS:prepend := "${BTBENCH_ROOT}/tools/target:"
SRC_URI = " \
    file://btbench-audio \
    file://btbench-btsnoop \
    file://btbench-audio-pipewire.target \
    file://btbench-audio-select.service \
    file://btbench-btsnoop.service \
    file://50-btbench-audio-pipewire.conf \
    file://btbench-pw.sh \
    file://snd-aloop.conf \
"
S = "${WORKDIR}"

inherit allarch systemd

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/btbench-audio ${D}${bindir}/
    install -m 0755 ${WORKDIR}/btbench-btsnoop ${D}${bindir}/

    install -d ${D}${systemd_system_unitdir}
    for u in btbench-audio-pipewire.target btbench-audio-select.service btbench-btsnoop.service; do
        install -m 0644 ${WORKDIR}/$u ${D}${systemd_system_unitdir}/
    done

    # PipeWire's units follow the stack's target (see the drop-in), the sockets too, so that a
    # restart of the target restarts all of them.
    for u in pipewire.service pipewire.socket pipewire-manager.socket wireplumber.service; do
        install -d ${D}${systemd_system_unitdir}/$u.d
        install -m 0644 ${WORKDIR}/50-btbench-audio-pipewire.conf ${D}${systemd_system_unitdir}/$u.d/
    done

    install -d ${D}${sysconfdir}/profile.d
    install -m 0644 ${WORKDIR}/btbench-pw.sh ${D}${sysconfdir}/profile.d/

    install -d ${D}${sysconfdir}/modules-load.d
    install -m 0644 ${WORKDIR}/snd-aloop.conf ${D}${sysconfdir}/modules-load.d/
}

SYSTEMD_SERVICE:${PN} = "btbench-audio-select.service btbench-btsnoop.service"

FILES:${PN} += "${systemd_system_unitdir}"

# btmon (btbench-btsnoop) and bluetoothctl (btbench-audio's disconnect) are in bluez5. PipeWire
# itself comes from packagegroup-btbench-audio. snd-aloop is a module only when the kernel
# config makes it one; built in, there is no package and nothing to recommend.
RDEPENDS:${PN} = "bluez5"
RRECOMMENDS:${PN} = "kernel-module-snd-aloop"
