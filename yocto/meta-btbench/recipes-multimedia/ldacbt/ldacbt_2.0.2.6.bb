SUMMARY = "LDAC Bluetooth audio encoder and adaptive bitrate library"
DESCRIPTION = "AOSP's libldac (the LDAC encoder Sony contributed to Android) and its ABR \
(adaptive bitrate) helper, built as shared libraries with pkg-config files by the ldacBT project."
HOMEPAGE = "https://github.com/EHfive/ldacBT"
SECTION = "libs"

# Opt-in (BTBENCH_EXTRA_CODECS = "1"): there is no LDAC recipe in scarthgap's layers. Encoder only
# (the decoder is not open source), so the board can send LDAC but not receive it. Expect an
# ARM11 to manage the lower bitrates at best.
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

# libldac is a git submodule (gitlab.com/eh5/libldac, a mirror of AOSP's external/libldac).
SRC_URI = "gitsm://github.com/EHfive/ldacBT.git;protocol=https;nobranch=1"
# Tag v2.0.2.6.
SRCREV = "6579bd585a618f2e1612b3c1650d2b7fcfb1d43f"
S = "${WORKDIR}/git"

inherit cmake pkgconfig

# Upstream installs to ${CMAKE_INSTALL_PREFIX}/lib unless told otherwise.
EXTRA_OECMAKE = " \
    -DINSTALL_LIBDIR=${libdir} \
    -DINSTALL_INCLUDEDIR=${includedir} \
"
