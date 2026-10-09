SUMMARY = "Open source aptX and aptX HD codec library"
DESCRIPTION = "libfreeaptx, a fork of libopenaptx: encoder and decoder for the aptX and aptX HD \
Bluetooth audio codecs, plus the freeaptxenc and freeaptxdec command line tools."
HOMEPAGE = "https://github.com/regularhunter/libfreeaptx"
SECTION = "libs"

# Opt-in (BTBENCH_EXTRA_CODECS = "1"): scarthgap's layers have no aptX library, and PipeWire and
# BlueALSA only offer aptX when built against this one.
LICENSE = "LGPL-2.1-or-later"
LIC_FILES_CHKSUM = "file://COPYING;md5=4fbd65380cdd255951079008b364516c"

SRC_URI = "git://github.com/regularhunter/libfreeaptx.git;protocol=https;nobranch=1"
# Tag 0.2.2.
SRCREV = "6dee419f934ec781e531f885f7e8e740752e67d1"
S = "${WORKDIR}/git"

# A bare POSIX Makefile. Its own CFLAGS (-O3) and LDFLAGS (-s, which would leave nothing for the
# -dbg package and trip the already-stripped QA check) are replaced by bitbake's.
EXTRA_OEMAKE = " \
    'CC=${CC}' 'CFLAGS=${CFLAGS}' 'LDFLAGS=${LDFLAGS}' \
    PREFIX=${prefix} LIBDIR=${baselib} \
"

# Its install target copies with `cp -a`, which would carry the build user's uid into the
# package; plain copies (links kept as links) are owned by root under pseudo.
do_install() {
    oe_runmake install DESTDIR=${D} 'CP=cp -P'
}

PACKAGES =+ "${PN}-tools"
FILES:${PN}-tools = "${bindir}/freeaptxenc ${bindir}/freeaptxdec"
