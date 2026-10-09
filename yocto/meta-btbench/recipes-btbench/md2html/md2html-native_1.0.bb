SUMMARY = "Markdown -> HTML converter (build-time tool for btbenchd's API reference)"
DESCRIPTION = "A pure-stdlib Python 3 script that renders docs/api.md to a static www/api.html. \
The btbenchd build calls it so GET /api serves a pre-rendered page: no runtime renderer, and only \
the HTML ships to the board."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Native-only: it runs on the build host, never on the device. python3-native is the interpreter
# its shebang needs.
inherit native
RDEPENDS:${PN} += "python3-native"

FILESEXTRAPATHS:prepend := "${BTBENCH_ROOT}/tools:"
SRC_URI = "file://md2html"
S = "${WORKDIR}"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/md2html ${D}${bindir}/md2html
}
