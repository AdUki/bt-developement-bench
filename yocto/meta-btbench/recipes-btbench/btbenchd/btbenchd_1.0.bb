SUMMARY = "btbenchd: the Bluetooth bench's daemon and web console"
DESCRIPTION = "BlueZ over sd-bus (adapters, discovery, pairing agent, GATT client and server, LE \
advertising, media endpoints/transports), the HCI monitor (throughput, latency, AVDTP), the \
btsnoop capture ring, and the board's audio mode, Wi-Fi and kernel trials through the target \
scripts, behind a REST/WebSocket API and a vanilla-JS console."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# systemd for sd-bus and sd-journal (libsystemd is on the image anyway: systemd is the init).
# md2html-native renders docs/api.md to www/api.html at build time (GET /api); only the HTML ships.
DEPENDS = "systemd md2html-native"

# The daemon is built from app/ of this repository (BTBENCH_ROOT, written into auto.conf by the
# Makefile). Its parts are listed one by one rather than as file://app so that a host build left in
# app/build (a CMake cache full of host paths, and every object file) is neither checksummed on
# each parse nor copied into the build. app/third_party/* are git submodules: an uninitialised
# clone fails in do_configure, where CMake says which command to run. docs/ is staged for the
# API page (app/CMakeLists.txt reads ../docs/api.md).
FILESEXTRAPATHS:prepend := "${THISDIR}/files:${BTBENCH_ROOT}:"
SRC_URI = " \
    file://app/CMakeLists.txt \
    file://app/src \
    file://app/www \
    file://app/tests \
    file://app/third_party \
    file://docs/api.md \
    file://tools/bench \
    file://btbenchd.service \
"
S = "${WORKDIR}/app"

inherit cmake systemd pkgconfig

EXTRA_OECMAKE = "-DCMAKE_BUILD_TYPE=Release -DBTB_TESTS=OFF"

SYSTEMD_SERVICE:${PN} = "btbenchd.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Defaulted here as well as in the .sample: btbench-device.conf is untracked, and a copy made
# before the setting existed would otherwise give the unit a bare "--port".
BTBENCH_HTTP_PORT ??= "80"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    sed -e 's|@PORT@|${BTBENCH_HTTP_PORT}|' ${WORKDIR}/btbenchd.service \
        > ${D}${systemd_system_unitdir}/btbenchd.service
    chmod 0644 ${D}${systemd_system_unitdir}/btbenchd.service

    # The CLI, so the same commands work in a shell on the board (it defaults to localhost there).
    install -D -m 0755 ${WORKDIR}/tools/bench ${D}${bindir}/bench
}

FILES:${PN} += "${datadir}/btbenchd ${systemd_system_unitdir}"

# What the API runs: systemctl/journal are systemd's; btmon (capture analyze) and the jobs
# runner's tools come with the BlueZ and audio packagegroups.
RDEPENDS:${PN} = "libsystemd bash curl jq"

do_install[vardeps] += "BTBENCH_HTTP_PORT"
