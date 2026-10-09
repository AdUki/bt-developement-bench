SUMMARY = "Bench plumbing: /data, persistent journal, USB gadget, networking, Wi-Fi with a setup AP, Bluetooth address"
DESCRIPTION = "Everything that makes a booted image a bench rather than a bare system: the board and \
device settings in /etc/btbench, /data grown to fill the card, the USB gadget link to the PC, \
systemd-networkd and mDNS, Wi-Fi that falls back to its own setup AP, BlueZ's pairings on /data and \
a Bluetooth address of the board's own, a journal that survives reboots, and ssh that answers even \
when the boot went wrong."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# The target scripts live in the repo's tools/target/, where `make deploy-scripts` also takes them
# from; listing them in SRC_URI keeps bitbake tracking their checksums.
FILESEXTRAPATHS:prepend := "${BTBENCH_ROOT}/tools/target:"

SRC_URI = " \
    file://btbench-data \
    file://btbench-bdaddr \
    file://btbench-gadget \
    file://btbench-wifi \
    file://btbench-data.service \
    file://btbench-bdaddr.service \
    file://btbench-gadget.service \
    file://btbench-gadget-reconnect.service \
    file://btbench-wifi.service \
    file://10-usb0.network \
    file://30-wlan0.network \
    file://resolved-btbench.conf \
    file://journald-btbench.conf \
    file://wpa_supplicant-btbench.conf \
    file://dnsmasq-btbench.conf \
    file://wait-online-btbench.conf \
    file://sshd-btbench.conf \
    file://sshd-early.conf \
    file://sshd-at-early.conf \
    file://sshdgenkeys-early.conf \
    file://btbench-tmpfiles.conf \
    file://profile-btbench.sh \
"

S = "${WORKDIR}"

inherit systemd

# /etc/btbench/board.env differs per board (and per KERNEL).
PACKAGE_ARCH = "${MACHINE_ARCH}"

# Defaults for device confs older than the variables they would set; btbench-device.conf.sample
# has the documented ones. The board variables always come from the build's auto.conf.
BTBENCH_HOSTNAME ??= "btbench"
BTBENCH_WIFI_SSID ??= ""
BTBENCH_WIFI_PSK ??= ""
BTBENCH_WIFI_COUNTRY ??= "SK"
BTBENCH_AP_PSK ??= "btbench-setup"
BTBENCH_GADGET ??= "ecm"
BTBENCH_AUDIO_MODE ??= "pipewire"
BTBENCH_HTTP_PORT ??= "80"
BTBENCH_BOARD ??= ""
BTBENCH_KERNEL ??= "next"
BTBENCH_KARCH ??= ""
BTBENCH_KIMAGE ??= ""
BTBENCH_DTBS ??= ""
BTBENCH_TRY_METHOD ??= "none"
BTBENCH_GADGET_UDC ??= ""
BTBENCH_BT_HCI ??= "hci0"

# What the scripts run. bash and jq: btbench-wifi. sfdisk, partx, findmnt, resize2fs: growing
# /data. e2fsck: the fsck pass of /data's fstab entry. coreutils: timeout(1), which busybox is
# built without here. btmgmt is BlueZ's and comes with the audio stack (btbench-bdaddr checks).
RDEPENDS:${PN} = " \
    bash \
    jq \
    coreutils \
    iproute2 \
    iw \
    wpa-supplicant \
    wpa-supplicant-cli \
    wpa-supplicant-passphrase \
    hostapd \
    dnsmasq \
    wireless-regdb-static \
    kmod \
    util-linux-sfdisk \
    util-linux-partx \
    util-linux-findmnt \
    util-linux-flock \
    e2fsprogs-resize2fs \
    e2fsprogs-e2fsck \
"

SYSTEMD_SERVICE:${PN} = " \
    btbench-data.service \
    btbench-bdaddr.service \
    btbench-gadget.service \
    btbench-gadget-reconnect.service \
    btbench-wifi.service \
"
SYSTEMD_AUTO_ENABLE = "enable"

# A value for a shell `KEY=VALUE` line, quoted only when it needs to be.
def btbench_shq(d, var):
    import shlex
    return shlex.quote(d.getVar(var) or '')

do_install() {
    install -d ${D}${bindir}
    for s in btbench-data btbench-bdaddr btbench-gadget btbench-wifi; do
        install -m 0755 ${WORKDIR}/$s ${D}${bindir}/
    done

    install -d ${D}${systemd_system_unitdir}
    for u in btbench-data btbench-bdaddr btbench-gadget btbench-gadget-reconnect btbench-wifi; do
        install -m 0644 ${WORKDIR}/$u.service ${D}${systemd_system_unitdir}/
    done

    # --- settings rendered from the build ---------------------------------------------------
    # The target scripts (this recipe's, the kernel's, the audio stack's) and btbenchd read these
    # instead of knowing about boards and device confs (docs/contracts.md). Values are quoted for
    # the shell when they need it; the heredocs are quoted so the shell does not expand them here.
    install -d ${D}${sysconfdir}/btbench
    cat >${D}${sysconfdir}/btbench/board.env <<'EOF'
# The board this image was built for (boards/<board>.mk). Written by btbench-base; shell syntax.
BOARD=${@btbench_shq(d, 'BTBENCH_BOARD')}
KERNEL=${@btbench_shq(d, 'BTBENCH_KERNEL')}
KARCH=${@btbench_shq(d, 'BTBENCH_KARCH')}
KIMAGE=${@btbench_shq(d, 'BTBENCH_KIMAGE')}
DTBS=${@btbench_shq(d, 'BTBENCH_DTBS')}
TRY_METHOD=${@btbench_shq(d, 'BTBENCH_TRY_METHOD')}
GADGET_UDC=${@btbench_shq(d, 'BTBENCH_GADGET_UDC')}
BT_HCI=${@btbench_shq(d, 'BTBENCH_BT_HCI')}
EOF
    cat >${D}${sysconfdir}/btbench/device.env <<'EOF'
# This bench's settings (btbench-device.conf). Written by btbench-base; shell syntax.
HOSTNAME=${@btbench_shq(d, 'BTBENCH_HOSTNAME')}
AP_PSK=${@btbench_shq(d, 'BTBENCH_AP_PSK')}
WIFI_COUNTRY=${@btbench_shq(d, 'BTBENCH_WIFI_COUNTRY')}
GADGET=${@btbench_shq(d, 'BTBENCH_GADGET')}
AUDIO_MODE=${@btbench_shq(d, 'BTBENCH_AUDIO_MODE')}
HTTP_PORT=${@btbench_shq(d, 'BTBENCH_HTTP_PORT')}
EOF
    chmod 0644 ${D}${sysconfdir}/btbench/board.env ${D}${sysconfdir}/btbench/device.env

    # The first network: btbench-wifi copies it to /data on first use and never reads it again,
    # so networks added and removed on the board are what counts from then on. Root only: it holds
    # the passphrase.
    if [ -n "${BTBENCH_WIFI_SSID}" ]; then
        cat >${D}${sysconfdir}/btbench/wifi-seed.env <<'EOF'
WIFI_SSID=${@btbench_shq(d, 'BTBENCH_WIFI_SSID')}
WIFI_PSK=${@btbench_shq(d, 'BTBENCH_WIFI_PSK')}
EOF
        chmod 0600 ${D}${sysconfdir}/btbench/wifi-seed.env
    fi

    # --- networking -------------------------------------------------------------------------
    # In the vendor directory, next to systemd-conf's 80-wired.network: a file of the same name in
    # /etc/systemd/network replaces one of these on a board.
    install -d ${D}${systemd_unitdir}/network
    install -m 0644 ${WORKDIR}/10-usb0.network ${WORKDIR}/30-wlan0.network ${D}${systemd_unitdir}/network/
    install -d ${D}${sysconfdir}/systemd/resolved.conf.d
    install -m 0644 ${WORKDIR}/resolved-btbench.conf ${D}${sysconfdir}/systemd/resolved.conf.d/10-btbench.conf
    install -d ${D}${systemd_system_unitdir}/systemd-networkd-wait-online.service.d
    sed -e 's|@SYSTEMD_UNITDIR@|${systemd_unitdir}|' ${WORKDIR}/wait-online-btbench.conf \
        >${D}${systemd_system_unitdir}/systemd-networkd-wait-online.service.d/10-btbench.conf
    install -d ${D}${systemd_system_unitdir}/wpa_supplicant@wlan0.service.d
    install -m 0644 ${WORKDIR}/wpa_supplicant-btbench.conf \
        ${D}${systemd_system_unitdir}/wpa_supplicant@wlan0.service.d/10-btbench.conf
    install -d ${D}${systemd_system_unitdir}/dnsmasq.service.d
    install -m 0644 ${WORKDIR}/dnsmasq-btbench.conf ${D}${systemd_system_unitdir}/dnsmasq.service.d/10-btbench.conf

    # --- journal ----------------------------------------------------------------------------
    install -d ${D}${sysconfdir}/systemd/journald.conf.d
    install -m 0644 ${WORKDIR}/journald-btbench.conf ${D}${sysconfdir}/systemd/journald.conf.d/10-btbench.conf

    # --- ssh --------------------------------------------------------------------------------
    install -d ${D}${sysconfdir}/ssh/sshd_config.d
    install -m 0644 ${WORKDIR}/sshd-btbench.conf ${D}${sysconfdir}/ssh/sshd_config.d/10-btbench.conf
    install -d ${D}${systemd_system_unitdir}/sshd.socket.d
    install -m 0644 ${WORKDIR}/sshd-early.conf ${D}${systemd_system_unitdir}/sshd.socket.d/10-early.conf
    install -d ${D}${systemd_system_unitdir}/sshd@.service.d
    install -m 0644 ${WORKDIR}/sshd-at-early.conf ${D}${systemd_system_unitdir}/sshd@.service.d/10-early.conf
    install -d ${D}${systemd_system_unitdir}/sshdgenkeys.service.d
    install -m 0644 ${WORKDIR}/sshdgenkeys-early.conf ${D}${systemd_system_unitdir}/sshdgenkeys.service.d/10-early.conf

    # --- misc -------------------------------------------------------------------------------
    install -d ${D}${nonarch_libdir}/tmpfiles.d
    install -m 0644 ${WORKDIR}/btbench-tmpfiles.conf ${D}${nonarch_libdir}/tmpfiles.d/btbench.conf
    install -d ${D}${sysconfdir}/profile.d
    install -m 0644 ${WORKDIR}/profile-btbench.sh ${D}${sysconfdir}/profile.d/btbench.sh
}

FILES:${PN} += " \
    ${systemd_system_unitdir} \
    ${systemd_unitdir}/network \
    ${nonarch_libdir}/tmpfiles.d \
"

# The settings are baked into /etc/btbench; the package must be rebuilt when they change.
do_install[vardeps] += " \
    BTBENCH_BOARD BTBENCH_KERNEL BTBENCH_KARCH BTBENCH_KIMAGE BTBENCH_DTBS BTBENCH_TRY_METHOD \
    BTBENCH_GADGET_UDC BTBENCH_BT_HCI BTBENCH_HOSTNAME BTBENCH_AP_PSK BTBENCH_WIFI_COUNTRY \
    BTBENCH_GADGET BTBENCH_AUDIO_MODE BTBENCH_HTTP_PORT BTBENCH_WIFI_SSID BTBENCH_WIFI_PSK \
"
