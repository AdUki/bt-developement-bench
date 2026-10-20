# Firmware for the Intel Bluetooth controllers plugged into a board's USB: the Intel BE200 (Wi-Fi 7
# M.2 card, Bluetooth 5.4 with LE Audio, USB 8087:0036) on an M.2-to-USB adapter. btusb with
# btintel loads intel/ibt-<cnvi>-<cnvr>.sfi and applies the .ddc next to it; for the BE200 (Gale
# Peak 2, hw variant 0x1c) both are ibt-0291-0291. Without them the controller stays in its
# bootloader: an hci with address 00:00:00:00:00:00 that never comes up.
#
# From linux-firmware at a pinned tag rather than poky's linux-firmware: scarthgap's 20240909 carries
# a 2024 build of it, and Intel ships a new one every quarter or so (LE Audio and ISO fixes among
# them). Only these files: linux-firmware-ibt-misc would install all ~85 Intel Bluetooth images.
# To update: pick a newer tag, and refresh the sums and the version below.
#
# 20260916: BT_BlazarU_S_REL120611_24.70.26293.120611 in WHENCE; the controller reports "Firmware
# timestamp 2026.29 buildtype 1 build 120611" when it boots.
#
# Auracast is in the firmware but switched off: the LE features the controller reports come from
# DDC entry 0x0126, and linux-firmware's leaves out Isochronous Broadcaster and Synchronized
# Receiver (bits 30 and 31), so there are no BIG commands either. Intel's Windows driver (24.80,
# the same .sfi byte for byte) reads that entry and sets the two bits when its
# EnableLEAudioBroadcast registry value is on. The .ddc gets one more entry doing the same:
# btintel sends each entry with Intel Write DDC (0xfc8b) right after boot, before the kernel reads
# the features. The entry is the whole 8-byte mask, as read from 24.70 and 24.80 (FF F9 01 3F BE
# 00 00 00) with 0xc0 added; check it against `hcitool cmd 0x3f 0x08c 26 01` on a new release.

SUMMARY = "Intel BE200 Bluetooth firmware (ibt-0291-0291) for a USB-attached Intel controller"
LICENSE = "Firmware-ibt_firmware"
LIC_FILES_CHKSUM = "file://LICENCE.ibt_firmware;md5=fdbee1ddfe0fb7ab0b2fcd6b454a366b"
NO_GENERIC_LICENSE[Firmware-ibt_firmware] = "LICENCE.ibt_firmware"

LINUX_FIRMWARE_TAG = "20260916"
PV = "${LINUX_FIRMWARE_TAG}"

LF_URI = "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain"
SRC_URI = " \
    ${LF_URI}/intel/ibt-0291-0291.sfi?h=${LINUX_FIRMWARE_TAG};downloadfilename=ibt-0291-0291-${LINUX_FIRMWARE_TAG}.sfi;name=sfi \
    ${LF_URI}/intel/ibt-0291-0291.ddc?h=${LINUX_FIRMWARE_TAG};downloadfilename=ibt-0291-0291-${LINUX_FIRMWARE_TAG}.ddc;name=ddc \
    ${LF_URI}/LICENSES/LICENCE.ibt_firmware?h=${LINUX_FIRMWARE_TAG};downloadfilename=LICENCE.ibt_firmware;name=licence \
"
SRC_URI[sfi.sha256sum] = "6ed6d49e87581941a3940fcb28d9507ecef4c6a479c5291dad1ab9b4aa959ec4"
SRC_URI[ddc.sha256sum] = "e0fb10fe29f60607d6507ced3f697492d9611f62c4765feaee59c4bd4101d3e7"
SRC_URI[licence.sha256sum] = "5181b0b51efc79d5acb2c9bb92042878fdbad97a92114d4ab5e32e2b5b52fce4"

S = "${WORKDIR}"

inherit allarch

do_compile[noexec] = "1"

do_install() {
	d=${D}${nonarch_base_libdir}/firmware/intel
	install -d $d
	install -m 0644 ${WORKDIR}/ibt-0291-0291-${LINUX_FIRMWARE_TAG}.sfi $d/ibt-0291-0291.sfi
	install -m 0644 ${WORKDIR}/ibt-0291-0291-${LINUX_FIRMWARE_TAG}.ddc $d/ibt-0291-0291.ddc
	# Entry: length 10, id 0x0126 (LE features), the mask with ISO broadcast on.
	printf '\012\046\001\377\371\001\377\276\000\000\000' >>$d/ibt-0291-0291.ddc
	install -m 0644 ${WORKDIR}/LICENCE.ibt_firmware ${D}${nonarch_base_libdir}/firmware/
}

FILES:${PN} = "${nonarch_base_libdir}/firmware"
