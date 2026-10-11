# The VIM3L's Wi-Fi and Bluetooth firmware: its AP6398S module is a BCM4359, whose SDIO firmware,
# NVRAM file and Bluetooth patch are not in linux-firmware. LibreELEC collects them, with the
# board-specific names mainline asks for: brcmfmac first tries
# brcm/brcmfmac4359-sdio.khadas,vim3l.{bin,txt} (the dts's first compatible), btbcm
# brcm/BCM4359C0.khadas,vim3l.hcd and then brcm/BCM4359C0.hcd. Only those files: meta-meson's
# linux-firmware bbappend installs the whole collection, and the board conf leaves its packages out.

SUMMARY = "BCM4359 (AP6398S) Wi-Fi and Bluetooth firmware for the Khadas VIM3L"
LICENSE = "CLOSED"

SRC_URI = "git://github.com/LibreELEC/brcmfmac_sdio-firmware.git;protocol=https;branch=master"
SRCREV = "cd0502653db3739843b0a41a560b66dcf245567b"
PV = "0+git${SRCPV}"
S = "${WORKDIR}/git"

inherit allarch

do_compile[noexec] = "1"

do_install() {
	d=${D}${nonarch_base_libdir}/firmware/brcm
	install -d $d
	install -m 0644 ${S}/brcmfmac4359-sdio.khadas,vim3l.bin ${S}/brcmfmac4359-sdio.khadas,vim3l.txt $d/
	install -m 0644 ${S}/BCM4359C0.hcd $d/
}

FILES:${PN} = "${nonarch_base_libdir}/firmware/brcm"
