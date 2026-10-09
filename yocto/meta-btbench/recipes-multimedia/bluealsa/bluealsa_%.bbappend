# BlueALSA, the bench's second audio stack (audio mode "bluealsa"). It registers its own A2DP
# endpoints and HFP profiles with bluetoothd, so it must never run next to PipeWire:
# btbench-audio-bluealsa.target starts it, and the targets of the other modes conflict with that
# one (btbench-audio recipe). Hence no auto-enable here.
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

# meta-oe pins a 2022 snapshot of 4.0.0. 4.3.1 is the last release that keeps the names this
# recipe packages (5.0 renames the daemon to bluealsad), and it brings what a codec bench wants on
# top: mSBC and LC3-SWB for HFP, libfreeaptx for aptX (the 2022 code only knows the abandoned
# libopenaptx), Opus and a long list of fixes. It only talks to bluetoothd over D-Bus and takes
# BlueZ's headers for the socket API, both stable, so BlueZ master does not upset it.
SRCREV = "f11569451b98a765adf9abc081632bb013f89f57"
PV = "4.3.1"
LIC_FILES_CHKSUM = "file://LICENSE;md5=143bc4e73f39cc5e89d6e096ac0315ba"
# 4.3.1 generates its D-Bus glue with gdbus-codegen.
DEPENDS += "glib-2.0-native"

# meta-oe's defaults (aplay, cli, hcitop, systemd) plus debug logging (bluealsa -B/--loglevel only
# reaches debug messages that are compiled in) and the codecs. aptX (and HD) and LDAC follow
# BTBENCH_EXTRA_CODECS like PipeWire's, AAC follows BTBENCH_AAC.
PACKAGECONFIG ??= " \
    aplay cli hcitop debug faststream msbc lc3-swb opus \
    ${@bb.utils.filter('DISTRO_FEATURES', 'systemd', d)} \
    ${@'aptx ldac' if d.getVar('BTBENCH_EXTRA_CODECS') == '1' else ''} \
    ${@'aac' if d.getVar('BTBENCH_AAC') == '1' else ''} \
"
PACKAGECONFIG[aptx] = "--enable-aptx --enable-aptx-hd --with-libfreeaptx,--disable-aptx --disable-aptx-hd,libfreeaptx"
PACKAGECONFIG[ldac] = "--enable-ldac,--disable-ldac,ldacbt"
PACKAGECONFIG[msbc] = "--enable-msbc,--disable-msbc,spandsp"
PACKAGECONFIG[lc3-swb] = "--enable-lc3-swb,--disable-lc3-swb,liblc3"
PACKAGECONFIG[opus] = "--enable-opus,--disable-opus,libopus"

# meta-oe hard-codes --disable-aptx and --disable-ldac; the PACKAGECONFIGs above decide instead.
EXTRA_OECONF:remove = "--disable-aptx --disable-ldac"

# All four profiles of both stacks' common ground: A2DP both ways and both ends of HFP (HSP is
# left out as in PipeWire's roles). -S logs to syslog, i.e. the journal. SBC, CVSD, mSBC and
# LC3-SWB are on by default; every other codec has to be enabled by name, and only the ones built
# in can be (an unknown name stops the daemon).
BLUEALSA_CODECS = " \
    ${@'-c FastStream' if bb.utils.contains('PACKAGECONFIG', 'faststream', True, False, d) else ''} \
    ${@'-c Opus' if bb.utils.contains('PACKAGECONFIG', 'opus', True, False, d) else ''} \
    ${@'-c aptX -c aptX-HD' if bb.utils.contains('PACKAGECONFIG', 'aptx', True, False, d) else ''} \
    ${@'-c LDAC' if bb.utils.contains('PACKAGECONFIG', 'ldac', True, False, d) else ''} \
"
SYSTEMD_BLUEALSA_ARGS = "-S -p a2dp-source -p a2dp-sink -p hfp-ag -p hfp-hf ${@' '.join(d.getVar('BLUEALSA_CODECS').split())}"

# 4.3.1 puts its D-Bus policy (who may own org.bluealsa) where dbus reads the distribution's
# defaults, not in /etc as the 2022 code did.
FILES:${PN} += "${datadir}/dbus-1/system.d"
