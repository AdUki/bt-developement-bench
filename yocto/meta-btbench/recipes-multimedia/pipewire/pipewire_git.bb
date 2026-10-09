SUMMARY     = "Multimedia processing server for Linux"
DESCRIPTION = "Linux server for handling and routing audio and video streams between applications and multimedia I/O devices"
HOMEPAGE    = "https://pipewire.org/"
BUGTRACKER  = "https://gitlab.freedesktop.org/pipewire/pipewire/issues"
SECTION     = "multimedia"

# PipeWire from git at the revision pinned in yocto/conf/versions.conf, the line src/pipewire
# follows. The body is meta-oe's pipewire_1.0.9.bb, made self-contained and cut down to what a
# headless Bluetooth audio bench runs: the ALSA and BlueZ SPA plugins, the system-wide service and
# the command line tools. No video, JACK, PulseAudio server, GStreamer, network audio or desktop
# integration — each of those is RAM and build time on a 512 MB ARMv6 board for nothing.

LICENSE = "MIT & LGPL-2.1-or-later & GPL-2.0-only"
LIC_FILES_CHKSUM = " \
    file://LICENSE;md5=2158739e172e58dc9ab1bdd2d6ec9c72 \
    file://COPYING;md5=97be96ca4fab23e9657ffa590b931c1a \
"

DEPENDS = "dbus ncurses"

# nobranch: the pin may be a release tag (on a stable branch) or any master commit, whichever
# `make bump-versions` found in src/pipewire.
SRC_URI = " \
    git://gitlab.freedesktop.org/pipewire/pipewire.git;protocol=https;nobranch=1 \
    file://50-btbench.conf \
"
SRCREV = "${BTBENCH_PIPEWIRE_SRCREV}"
PV = "${BTBENCH_PIPEWIRE_PV}"
S = "${WORKDIR}/git"

inherit meson pkgconfig systemd gettext useradd

# The system-wide daemon runs as this user (upstream's system units say User=pipewire). audio for
# the ALSA devices; video is upstream's habit and harmless.
USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "--system pipewire"
USERADD_PARAM:${PN} = "--system --home / --no-create-home \
                       --comment 'PipeWire multimedia daemon' \
                       --gid pipewire --groups audio,video \
                       pipewire"

SYSTEMD_PACKAGES = "${PN}"

# Every "auto" feature is off unless a PACKAGECONFIG below turns it on: what ends up in the build
# then depends on this recipe, not on whatever else happens to be in the sysroot. The options
# below are the ones upstream defaults to "enabled" that the bench does not want, plus the
# directories and the session-manager subproject (an empty list: WirePlumber is its own recipe,
# meson must not try to clone it).
EXTRA_OEMESON += " \
    -Dauto_features=disabled \
    -Dsession-managers= \
    -Dtests=disabled \
    -Dexamples=disabled \
    -Dman=disabled \
    -Ddocs=disabled \
    -Dflatpak=disabled \
    -Dpipewire-jack=disabled \
    -Djack-devel=false \
    -Dpipewire-v4l2=disabled \
    -Dvideoconvert=disabled \
    -Dvideotestsrc=disabled \
    -Dbluez5-backend-ofono=disabled \
    -Dbluez5-backend-hsphfpd=disabled \
    -Dlegacy-rtkit=false \
    -Dudevrulesdir=${nonarch_base_libdir}/udev/rules.d/ \
    -Dsystemd-system-unit-dir=${systemd_system_unitdir} \
    -Dsystemd-user-unit-dir=${systemd_user_unitdir} \
"

# SBC (and SBC-XQ), mSBC, CVSD, Opus, LC3 (BAP and HFP LC3-SWB) and G.722 (ASHA) by default.
# aptX and LDAC need libfreeaptx and ldacbt (BTBENCH_EXTRA_CODECS = "1"), AAC needs fdk-aac, whose
# license is flagged commercial (BTBENCH_AAC = "1"; the distro then accepts the flag).
PACKAGECONFIG ??= " \
    alsa bluez bluez-opus bluez-lc3 bluez-g722 udev readline sndfile pw-cat \
    ${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'systemd systemd-system-service', '', d)} \
    ${@'bluez-aptx bluez-ldac' if d.getVar('BTBENCH_EXTRA_CODECS') == '1' else ''} \
    ${@'bluez-aac' if d.getVar('BTBENCH_AAC') == '1' else ''} \
"

PACKAGECONFIG[alsa] = "-Dalsa=enabled -Dpipewire-alsa=enabled,-Dalsa=disabled -Dpipewire-alsa=disabled,alsa-lib udev,,pipewire-alsa-card-profile"
# The native backend is the HFP/HSP implementation inside PipeWire itself (no oFono/hsphfpd).
PACKAGECONFIG[bluez] = "-Dbluez5=enabled -Dbluez5-backend-hfp-native=enabled -Dbluez5-backend-hsp-native=enabled,-Dbluez5=disabled,bluez5 sbc glib-2.0 glib-2.0-native"
PACKAGECONFIG[bluez-opus] = "-Dopus=enabled -Dbluez5-codec-opus=enabled,-Dopus=disabled -Dbluez5-codec-opus=disabled,libopus"
PACKAGECONFIG[bluez-lc3] = "-Dbluez5-codec-lc3=enabled,-Dbluez5-codec-lc3=disabled,liblc3"
PACKAGECONFIG[bluez-g722] = "-Dbluez5-codec-g722=enabled,-Dbluez5-codec-g722=disabled"
PACKAGECONFIG[bluez-aptx] = "-Dbluez5-codec-aptx=enabled,-Dbluez5-codec-aptx=disabled,libfreeaptx"
# The ldacbt recipe has the encoder and ABR only; there is no free LDAC decoder.
PACKAGECONFIG[bluez-ldac] = "-Dbluez5-codec-ldac=enabled -Dbluez5-codec-ldac-dec=disabled,-Dbluez5-codec-ldac=disabled,ldacbt"
PACKAGECONFIG[bluez-aac] = "-Dbluez5-codec-aac=enabled,-Dbluez5-codec-aac=disabled,fdk-aac"
PACKAGECONFIG[readline] = "-Dreadline=enabled,-Dreadline=disabled,readline"
PACKAGECONFIG[sndfile] = "-Dsndfile=enabled,-Dsndfile=disabled,libsndfile1"
# pw-cat/pw-play/pw-record; needs sndfile.
PACKAGECONFIG[pw-cat] = "-Dpw-cat=enabled,-Dpw-cat=disabled"
PACKAGECONFIG[udev] = "-Dudev=enabled,-Dudev=disabled,udev"
# libsystemd for sd_notify and journal logging; logind is for desktop seats and stays off.
PACKAGECONFIG[systemd] = "-Dlibsystemd=enabled -Dlogind=disabled,-Dlibsystemd=disabled,systemd"
PACKAGECONFIG[systemd-system-service] = "-Dsystemd-system-service=enabled -Dsystemd-user-service=disabled,-Dsystemd-system-service=disabled,systemd"

# poky's time64.inc builds PipeWire with a 32-bit time_t, because pipewire-v4l2 plays tricks with
# _FILE_OFFSET_BITS. This build has no pipewire-v4l2, and everything else on the image (WirePlumber
# included, which passes struct timespec into libpipewire) and the SDK the dev loop compiles
# src/pipewire with use 64-bit time, so PipeWire does as well.
GLIBC_64BIT_TIME_FLAGS:pn-pipewire = " -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64"

SPA_SUBDIR = "spa-0.2"
PW_MODULE_SUBDIR = "pipewire-0.3"

do_install:append() {
    # The pipewire-alsa plugin's PCM definition, where alsa-lib looks for add-on config. Only
    # that one: 99-pipewire-default.conf would make PipeWire the default ALSA device, and in the
    # bluealsa and none audio modes there is no PipeWire to answer. Use `aplay -D pipewire`.
    if ${@bb.utils.contains('PACKAGECONFIG', 'alsa', 'true', 'false', d)}; then
        install -d ${D}${sysconfdir}/alsa/conf.d
        ln -sf ${datadir}/alsa/alsa.conf.d/50-pipewire.conf ${D}${sysconfdir}/alsa/conf.d/50-pipewire.conf
    fi

    # minimal.conf is an example of how to configure the daemon by hand, not one to run.
    rm -f ${D}${datadir}/pipewire/minimal.conf
    # The daemon's name for the AVB module, which this build does not have.
    rm -f ${D}${bindir}/pipewire-avb
    # libspa.so is SPA's inline helpers compiled into a library for bindings in other languages
    # (Rust); nothing on the board links it, and an unversioned .so would need its own package.
    rm -f ${D}${libdir}/${SPA_SUBDIR}/libspa.so

    install -d ${D}${sysconfdir}/pipewire/pipewire.conf.d
    install -m 0644 ${WORKDIR}/50-btbench.conf ${D}${sysconfdir}/pipewire/pipewire.conf.d/
}

# One package per SPA plugin and per module, with a -meta package for each kind that depends on
# all of them (meta-oe's scheme, unchanged).
PACKAGESPLITFUNCS:prepend = " split_dynamic_packages "
PACKAGESPLITFUNCS:append = " set_dynamic_metapkg_rdepends "

python split_dynamic_packages () {
    spa_libdir = d.expand('${libdir}/${SPA_SUBDIR}')
    do_split_packages(d, spa_libdir, r'^libspa-(.*)\.so$', d.expand('${PN}-spa-plugins-%s'), 'PipeWire SPA plugin for %s', extra_depends='', recursive=True)

    pw_module_libdir = d.expand('${libdir}/${PW_MODULE_SUBDIR}')
    do_split_packages(d, pw_module_libdir, r'^libpipewire-module-(.*)\.so$', d.expand('${PN}-modules-%s'), 'PipeWire %s module', extra_depends='', recursive=False)
}

python set_dynamic_metapkg_rdepends () {
    import os
    import oe.utils

    base_pn = d.getVar('PN')
    pkgdest = d.getVar('PKGDEST')

    for kind in ('spa-plugins', 'modules'):
        prefix = base_pn + '-' + kind
        metapkg = prefix + '-meta'
        d.setVar('ALLOW_EMPTY:' + metapkg, "1")
        d.setVar('FILES:' + metapkg, "")

        deps = []
        for pkg in oe.utils.packages_filter_out_system(d):
            if pkg in (prefix, metapkg) or not pkg.startswith(prefix + '-'):
                continue
            # Empty packages (nothing matched their FILES) are not created, so leave them out.
            pkgdir = os.path.join(pkgdest, pkg)
            if os.path.exists(pkgdir) and os.listdir(pkgdir):
                deps.append(pkg)

        d.setVar('RDEPENDS:' + metapkg, ' '.join(deps))
        d.setVar('DESCRIPTION:' + metapkg, prefix + ' meta package')
}

PACKAGES =+ "\
    libpipewire \
    ${PN}-tools \
    ${PN}-pulse \
    ${PN}-alsa \
    ${PN}-spa-plugins \
    ${PN}-spa-plugins-meta \
    ${PN}-spa-tools \
    ${PN}-modules \
    ${PN}-modules-meta \
    ${PN}-alsa-card-profile \
    ${PN}-aes67 \
"

PACKAGES_DYNAMIC = "^${PN}-spa-plugins.* ^${PN}-modules.*"

# The system units are installed but not enabled: btbench-audio-pipewire.target starts them when
# the audio mode is pipewire (see the btbench-audio recipe), so they stay off in the other modes.
SYSTEMD_SERVICE:${PN} = "${@bb.utils.contains('PACKAGECONFIG', 'systemd-system-service', 'pipewire.service pipewire.socket pipewire-manager.socket', '', d)}"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

CONFFILES:${PN} += "${datadir}/pipewire/pipewire.conf ${sysconfdir}/pipewire/pipewire.conf.d/50-btbench.conf"
FILES:${PN} = " \
    ${datadir}/pipewire \
    ${sysconfdir}/pipewire \
    ${systemd_system_unitdir}/pipewire.* \
    ${systemd_system_unitdir}/pipewire-manager.* \
    ${bindir}/pipewire \
"

RRECOMMENDS:${PN}:class-target += " \
    ${PN}-modules-meta \
    ${PN}-spa-plugins-meta \
"

CONFFILES:libpipewire += "${datadir}/pipewire/client.conf"
FILES:libpipewire = " \
    ${datadir}/pipewire/client.conf \
    ${libdir}/libpipewire-*.so.* \
"
# The bare minimum of modules and plugins without which libpipewire cannot connect to anything.
RDEPENDS:libpipewire += " \
    ${PN}-modules-client-node \
    ${PN}-modules-protocol-native \
    ${PN}-spa-plugins-support \
"

FILES:${PN}-tools = "${bindir}/pw-*"

# pipewire-pulse (the PulseAudio protocol server) is built anyway — it is a module plus a name for
# the daemon — and packaged apart, so the image need not carry it.
CONFFILES:${PN}-pulse += "${datadir}/pipewire/pipewire-pulse.conf"
FILES:${PN}-pulse = " \
    ${datadir}/pipewire/pipewire-pulse.conf \
    ${systemd_system_unitdir}/pipewire-pulse.* \
    ${systemd_user_unitdir}/pipewire-pulse.* \
    ${bindir}/pipewire-pulse \
"
RDEPENDS:${PN}-pulse += "${PN}-modules-protocol-pulse"

# The ALSA plugin that lets plain ALSA programs play into PipeWire (aplay -D pipewire).
FILES:${PN}-alsa = "\
    ${libdir}/alsa-lib/* \
    ${datadir}/alsa/alsa.conf.d/* \
    ${sysconfdir}/alsa/conf.d/50-pipewire.conf \
"

FILES:${PN}-spa-plugins = ""
RRECOMMENDS:${PN}-spa-plugins += "${PN}-spa-plugins-meta"

FILES:${PN}-spa-plugins-bluez5 += "${datadir}/${SPA_SUBDIR}/bluez5/*"

FILES:${PN}-spa-tools = "${bindir}/spa-*"

FILES:${PN}-modules = ""
RRECOMMENDS:${PN}-modules += "${PN}-modules-meta"

CONFFILES:${PN}-modules-rt = "${datadir}/pipewire/client-rt.conf"
FILES:${PN}-modules-rt += " \
    ${datadir}/pipewire/client-rt.conf \
    ${sysconfdir}/security/limits.d/* \
"

CONFFILES:${PN}-modules-filter-chain = "${datadir}/pipewire/filter-chain/*"
FILES:${PN}-modules-filter-chain += "${datadir}/pipewire/filter-chain/*"

FILES:${PN}-alsa-card-profile = " \
    ${datadir}/alsa-card-profile/* \
    ${nonarch_base_libdir}/udev/rules.d/90-pipewire-alsa.rules \
"

FILES:${PN}-aes67 += "${bindir}/pipewire-aes67"
