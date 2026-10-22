# Firmware for the Intel BE200's Wi-Fi (PCIe 8086:272b, Gale Peak 2 with the Fm radio): iwlwifi
# with the MLD op mode asks for iwlwifi-gl-c0-fm-c0-c<N>.ucode, N from the kernel's
# IWL_BZ_UCODE_CORE_MAX down to its _MIN (107 to 102 in bluetooth-next 7.3), then loads the
# .pnvm (the radio's calibration and regulatory tables) next to it. The Bluetooth half of the
# card is btbench-firmware-ibt.
#
# From linux-firmware at a pinned tag, as btbench-firmware-ibt: scarthgap's linux-firmware has
# nothing a 7.x kernel accepts for this card, and linux-firmware-iwlwifi-misc would install every
# Intel Wi-Fi image there is. Only the newest core the kernel knows: a kernel bump that raises
# IWL_BZ_UCODE_CORE_MAX still loads this one. To update: pick a newer tag and core, and refresh
# the sums.
#
# KERNEL=rpi's 6.12 asks for the older numbering, iwlwifi-gl-c0-fm-c0-<N>.ucode with N from 93
# down to 90, and accepts none of the c1xx cores: the image carries the newest of those too (92;
# there is no 93 at this tag), so the card works under either kernel, with the same .pnvm.

SUMMARY = "Intel BE200 Wi-Fi firmware (iwlwifi-gl-c0-fm-c0) for a PCIe-attached Intel card"
LICENSE = "Firmware-iwlwifi_firmware"
LIC_FILES_CHKSUM = "file://LICENCE.iwlwifi_firmware;md5=2ce6786e0fc11ac6e36b54bb9b799f1b"
NO_GENERIC_LICENSE[Firmware-iwlwifi_firmware] = "LICENCE.iwlwifi_firmware"

LINUX_FIRMWARE_TAG = "20260916"
PV = "${LINUX_FIRMWARE_TAG}"

# 107.1b444747.0 in WHENCE.
IWL_UCODE = "iwlwifi-gl-c0-fm-c0-c107.ucode"
IWL_UCODE_612 = "iwlwifi-gl-c0-fm-c0-92.ucode"
IWL_PNVM = "iwlwifi-gl-c0-fm-c0.pnvm"

LF_URI = "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain"
SRC_URI = " \
    ${LF_URI}/intel/iwlwifi/${IWL_UCODE}?h=${LINUX_FIRMWARE_TAG};downloadfilename=${IWL_UCODE}-${LINUX_FIRMWARE_TAG};name=ucode \
    ${LF_URI}/intel/iwlwifi/${IWL_UCODE_612}?h=${LINUX_FIRMWARE_TAG};downloadfilename=${IWL_UCODE_612}-${LINUX_FIRMWARE_TAG};name=ucode612 \
    ${LF_URI}/intel/iwlwifi/${IWL_PNVM}?h=${LINUX_FIRMWARE_TAG};downloadfilename=${IWL_PNVM}-${LINUX_FIRMWARE_TAG};name=pnvm \
    ${LF_URI}/LICENSES/LICENCE.iwlwifi_firmware?h=${LINUX_FIRMWARE_TAG};downloadfilename=LICENCE.iwlwifi_firmware;name=licence \
"
SRC_URI[ucode.sha256sum] = "cb33a383c65f3bc3bb19db319f50eedcae07c2ea2e53bfb4c88173d0b51144ed"
SRC_URI[ucode612.sha256sum] = "c7092f1f78676f9e9c2c4d12716401db2116b009c5d7c464e8387c5b1c645d6f"
SRC_URI[pnvm.sha256sum] = "f481e3b0403ca43a0a444b01c04fa4d1582868afcb026b1e697905f0039ccfa3"
SRC_URI[licence.sha256sum] = "16d5040c7cf851fc693b7542e20870935b99802533ea1bfd231c377a2305e5c5"

S = "${WORKDIR}"

inherit allarch

do_compile[noexec] = "1"

# iwlwifi requests the files by their bare names, from the top of /lib/firmware (linux-firmware
# keeps them in intel/iwlwifi/ and links them there).
do_install() {
	d=${D}${nonarch_base_libdir}/firmware
	install -d $d
	install -m 0644 ${WORKDIR}/${IWL_UCODE}-${LINUX_FIRMWARE_TAG} $d/${IWL_UCODE}
	install -m 0644 ${WORKDIR}/${IWL_UCODE_612}-${LINUX_FIRMWARE_TAG} $d/${IWL_UCODE_612}
	install -m 0644 ${WORKDIR}/${IWL_PNVM}-${LINUX_FIRMWARE_TAG} $d/${IWL_PNVM}
	install -m 0644 ${WORKDIR}/LICENCE.iwlwifi_firmware $d/
}

FILES:${PN} = "${nonarch_base_libdir}/firmware"
