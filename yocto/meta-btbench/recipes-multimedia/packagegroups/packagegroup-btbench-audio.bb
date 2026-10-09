SUMMARY = "BlueZ and the audio stacks of the Bluetooth development bench"
DESCRIPTION = "bluetoothd and every BlueZ tool, PipeWire with WirePlumber, BlueALSA, the audio mode \
switch and the btmon capture ring. Which stack runs is chosen on the board (btbench-audio)."

# Not allarch: sbc-examples is renamed to libsbc-examples by debian package naming, and an
# allarch packagegroup cannot follow a per-machine rename (the image then asks for a package that
# does not exist).
PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit packagegroup

RDEPENDS:${PN} = " \
    bluez5 \
    bluez5-btmgmt \
    bluez5-noinst-tools \
    bluez5-testtools \
    bluez5-obex \
    \
    pipewire \
    pipewire-modules-meta \
    pipewire-spa-plugins-meta \
    pipewire-tools \
    pipewire-spa-tools \
    pipewire-alsa \
    wireplumber \
    \
    bluealsa \
    bluealsa-aplay \
    \
    btbench-audio \
    \
    alsa-utils-aplay \
    alsa-utils-amixer \
    alsa-utils-speakertest \
    sbc-examples \
    \
    mpg123 \
    ${@'ffmpeg' if d.getVar('BTBENCH_FFMPEG') == '1' else ''} \
"

# btbenchd's audio streams decode internet radio and UPnP items with these (docs/audio.md,
# "Streams"): mpg123 for MP3, ffmpeg for the rest when BTBENCH_FFMPEG = "1". Defaulted here: an
# untracked btbench-device.conf from before the switch would leave it unset.
BTBENCH_FFMPEG ??= "0"
