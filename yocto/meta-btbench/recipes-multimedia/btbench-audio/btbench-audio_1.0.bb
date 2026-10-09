SUMMARY = "Audio modes and the HCI capture ring of the Bluetooth development bench"
DESCRIPTION = "btbench-audio switches the audio stack (pipewire, bluealsa or none) through one \
systemd target per mode; btbench-btsnoop keeps an always-on ring of btmon captures in /data. Also \
loads snd-aloop, the board's sound card, and points root's PipeWire tools at the system-wide daemon."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# The two scripts live in the repo's tools/target/ (deploy-audio-scripts pushes them to a running
# board without a rebuild); the units and config next to this recipe.
FILESEXTRAPATHS:prepend := "${BTBENCH_ROOT}/tools/target:"
SRC_URI = " \
    file://btbench-audio \
    file://btbench-btsnoop \
    file://btbench-audio-pipewire.target \
    file://btbench-audio-bluealsa.target \
    file://btbench-audio-none.target \
    file://btbench-audio-select.service \
    file://btbench-btsnoop.service \
    file://50-btbench-audio-pipewire.conf \
    file://50-btbench-audio-bluealsa.conf \
    file://btbench-pw.sh \
    file://snd-aloop.conf \
"
S = "${WORKDIR}"

inherit allarch systemd

# The mode the board starts with until one is chosen on it (btbench-device.conf).
BTBENCH_AUDIO_MODE ??= "pipewire"

do_install() {
    install -d ${D}${bindir}
    sed -e 's|@DEFAULT_MODE@|${BTBENCH_AUDIO_MODE}|' ${WORKDIR}/btbench-audio > ${D}${bindir}/btbench-audio
    chmod 0755 ${D}${bindir}/btbench-audio
    install -m 0755 ${WORKDIR}/btbench-btsnoop ${D}${bindir}/

    install -d ${D}${systemd_system_unitdir}
    for u in btbench-audio-pipewire.target btbench-audio-bluealsa.target btbench-audio-none.target \
             btbench-audio-select.service btbench-btsnoop.service; do
        install -m 0644 ${WORKDIR}/$u ${D}${systemd_system_unitdir}/
    done

    # Each stack's units follow their mode's target (see the drop-ins). The sockets too: an
    # enabled socket would otherwise start PipeWire again on the first client in another mode.
    for u in pipewire.service pipewire.socket pipewire-manager.socket wireplumber.service; do
        install -d ${D}${systemd_system_unitdir}/$u.d
        install -m 0644 ${WORKDIR}/50-btbench-audio-pipewire.conf ${D}${systemd_system_unitdir}/$u.d/
    done
    install -d ${D}${systemd_system_unitdir}/bluealsa.service.d
    install -m 0644 ${WORKDIR}/50-btbench-audio-bluealsa.conf ${D}${systemd_system_unitdir}/bluealsa.service.d/

    install -d ${D}${sysconfdir}/profile.d
    install -m 0644 ${WORKDIR}/btbench-pw.sh ${D}${sysconfdir}/profile.d/

    install -d ${D}${sysconfdir}/modules-load.d
    install -m 0644 ${WORKDIR}/snd-aloop.conf ${D}${sysconfdir}/modules-load.d/
}

SYSTEMD_SERVICE:${PN} = "btbench-audio-select.service btbench-btsnoop.service"

FILES:${PN} += "${systemd_system_unitdir}"

# btmon (btbench-btsnoop) and bluetoothctl (btbench-audio's disconnect) are in bluez5. The stacks
# themselves come from packagegroup-btbench-audio. snd-aloop is a module only when the kernel
# config makes it one; built in, there is no package and nothing to recommend.
RDEPENDS:${PN} = "bluez5"
RRECOMMENDS:${PN} = "kernel-module-snd-aloop"
