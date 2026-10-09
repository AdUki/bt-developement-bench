SUMMARY    = "Session / policy manager implementation for PipeWire"
HOMEPAGE   = "https://gitlab.freedesktop.org/pipewire/wireplumber"
BUGTRACKER = "https://gitlab.freedesktop.org/pipewire/wireplumber/issues"
SECTION    = "multimedia"

# WirePlumber from git at the revision pinned in yocto/conf/versions.conf (the line src/wireplumber
# follows), run as PipeWire's system-wide session manager. Based on meta-oe's wireplumber_0.5.1.bb.

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=17d1fe479cdec331eecbc65d26bc7e77"

DEPENDS = "glib-2.0 glib-2.0-native lua pipewire"

# nobranch: the pin may sit on the 0.5 stable branch or on master.
SRC_URI = " \
    git://gitlab.freedesktop.org/pipewire/wireplumber.git;protocol=https;nobranch=1 \
    file://50-btbench.conf \
    file://10-btbench-state.conf \
"
SRCREV = "${BTBENCH_WIREPLUMBER_SRCREV}"
PV = "${BTBENCH_WIREPLUMBER_PV}"
S = "${WORKDIR}/git"

inherit meson pkgconfig systemd

# system-lua: OE's lua rather than the bundled subproject. No GObject introspection (the bench
# has no GI consumers, and g-ir-scanner under qemu is a slow, fragile step on ARMv6), no docs
# (sphinx), no elogind, no tests.
EXTRA_OEMESON += " \
    -Dintrospection=disabled \
    -Ddoc=disabled \
    -Dsystem-lua=true \
    -Delogind=disabled \
    -Dtests=false \
    -Ddbus-tests=false \
    -Dsystemd-system-unit-dir=${systemd_system_unitdir} \
    -Dsystemd-user-unit-dir=${systemd_user_unitdir} \
"

PACKAGECONFIG ??= "${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'systemd systemd-system-service', '', d)}"
PACKAGECONFIG[systemd] = "-Dsystemd=enabled,-Dsystemd=disabled,systemd"
PACKAGECONFIG[systemd-system-service] = "-Dsystemd-system-service=true -Dsystemd-user-service=false,-Dsystemd-system-service=false,systemd"

WP_MODULE_SUBDIR = "wireplumber-0.5"

do_install:append() {
    # The bench's settings, as a fragment in the admin's directory (upstream's own config stays
    # untouched in ${datadir}).
    install -d ${D}${sysconfdir}/wireplumber/wireplumber.conf.d
    install -m 0644 ${WORKDIR}/50-btbench.conf ${D}${sysconfdir}/wireplumber/wireplumber.conf.d/

    if ${@bb.utils.contains('PACKAGECONFIG', 'systemd-system-service', 'true', 'false', d)}; then
        install -d ${D}${systemd_system_unitdir}/wireplumber.service.d
        install -m 0644 ${WORKDIR}/10-btbench-state.conf ${D}${systemd_system_unitdir}/wireplumber.service.d/
    fi
}

PACKAGESPLITFUNCS:prepend = " split_dynamic_packages "
PACKAGESPLITFUNCS:append = " set_dynamic_metapkg_rdepends "

python split_dynamic_packages () {
    wp_module_libdir = d.expand('${libdir}/${WP_MODULE_SUBDIR}')
    do_split_packages(d, wp_module_libdir, r'^libwireplumber-module-(.*)\.so$', d.expand('${PN}-modules-%s'), 'WirePlumber %s module', extra_depends='', recursive=False)
}

python set_dynamic_metapkg_rdepends () {
    import os
    import oe.utils

    prefix = d.getVar('PN') + '-modules'
    metapkg = prefix + '-meta'
    pkgdest = d.getVar('PKGDEST')

    d.setVar('ALLOW_EMPTY:' + metapkg, "1")
    d.setVar('FILES:' + metapkg, "")

    deps = []
    for pkg in oe.utils.packages_filter_out_system(d):
        if pkg in (prefix, metapkg) or not pkg.startswith(prefix + '-'):
            continue
        pkgdir = os.path.join(pkgdest, pkg)
        if os.path.exists(pkgdir) and os.listdir(pkgdir):
            deps.append(pkg)

    d.setVar('RDEPENDS:' + metapkg, ' '.join(deps))
    d.setVar('DESCRIPTION:' + metapkg, prefix + ' meta package')
}

PACKAGES =+ "\
    libwireplumber \
    ${PN}-scripts \
    ${PN}-modules \
    ${PN}-modules-meta \
"

PACKAGES_DYNAMIC = "^${PN}-modules.*"

# Started by btbench-audio-pipewire.target (btbench-audio recipe), not at boot on its own.
SYSTEMD_SERVICE:${PN} = "${@bb.utils.contains('PACKAGECONFIG', 'systemd-system-service', 'wireplumber.service', '', d)}"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

CONFFILES:${PN} += " \
    ${datadir}/wireplumber/wireplumber.conf \
    ${sysconfdir}/wireplumber/wireplumber.conf.d/50-btbench.conf \
"
# WirePlumber is only useful with a PipeWire daemon to manage, its scripts and its modules.
RDEPENDS:${PN} += "pipewire ${PN}-scripts ${PN}-modules-meta"

FILES:${PN} += " \
    ${datadir}/wireplumber \
    ${sysconfdir}/wireplumber \
    ${systemd_user_unitdir} \
    ${systemd_system_unitdir} \
    ${datadir}/zsh \
    ${datadir}/bash-completion \
"

FILES:libwireplumber = "${libdir}/libwireplumber-*.so.*"

FILES:${PN}-scripts += "${datadir}/wireplumber/scripts/*"

FILES:${PN}-modules = ""
RRECOMMENDS:${PN}-modules += "${PN}-modules-meta"
