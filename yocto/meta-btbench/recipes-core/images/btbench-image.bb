SUMMARY = "Bluetooth development bench — the one image"
DESCRIPTION = "A writable development image: BlueZ, PipeWire/WirePlumber or BlueALSA, the bench \
daemon and its web console, debugging and tracing tools, ssh and opkg. Deploys rsync over it and \
kernel trials boot next to it, so nothing about it is read-only."
LICENSE = "MIT"

inherit core-image

# Writable on purpose: `make deploy-*` rsyncs stage trees into /usr, `make pkg` installs packages
# with opkg, and `journalctl -b -1` after a failed kernel trial needs the journal on disk.
IMAGE_FEATURES += "ssh-server-openssh package-management"

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    packagegroup-btbench-tools \
    packagegroup-btbench-audio \
    packagegroup-btbench-kernel \
    btbench-base \
    btbenchd \
    ${BTBENCH_MACHINE_FIRMWARE} \
    ${CORE_IMAGE_EXTRA_INSTALL} \
"

# The machine configs list the right Wi-Fi and Bluetooth firmware for their board in
# MACHINE_EXTRA_RRECOMMENDS (bcm43430 and its BCM43430A1.hcd on a Zero W, 43455 on a Pi 4), but
# only packagegroup-base honours that variable, and this image does not install it: the firmware
# would go missing with no error at build time, and Wi-Fi and Bluetooth fail at runtime. Take the
# machine's own list and install it outright.
BTBENCH_MACHINE_FIRMWARE = "${@' '.join(p for p in (d.getVar('MACHINE_EXTRA_RRECOMMENDS') or '').split() if p.startswith(('linux-firmware', 'bluez-firmware')))}"

IMAGE_LINGUAS = ""

# --- SDK (make sdk) ---------------------------------------------------------------------------

# Debug symbols of everything in the image, so `make gdb` resolves the libraries' frames too.
SDKIMAGE_FEATURES += "dbg-pkgs"
# Host tools the PipeWire and WirePlumber builds run: gdbus-codegen (-codegen) and glib-mkenums
# (-utils).
TOOLCHAIN_HOST_TASK:append = " nativesdk-glib-2.0-codegen nativesdk-glib-2.0-utils"
# Libraries the dev trees build against that nothing in the image needs headers for, so the image's
# own -dev packages would not bring them (BlueZ: ell, libical, readline, sbc, lc3; lua for
# WirePlumber).
TOOLCHAIN_TARGET_TASK:append = " ell-dev libical-dev readline-dev sbc-dev liblc3-dev lua-dev"

# --- disk image -------------------------------------------------------------------------------

# Per board (BTBENCH_WKS, from yocto/conf/boards/<board>.conf), set here at recipe scope on purpose:
# a BSP's machine conf is parsed after every conf file of ours and may hard-set WKS_FILE to its
# own layout, which has no data partition.
WKS_FILE = "${BTBENCH_WKS}"
BTBENCH_WKS ??= "btbench-rpi.wks"

# Only what `make flash` writes: the compressed card image and its block map (bmaptool skips the
# empty blocks, which is most of a fresh card). meta-raspberrypi's default also makes a tarball
# and an ext3 image nobody uses.
IMAGE_FSTYPES = "wic.bz2 wic.bmap"

# Free space in the root filesystem beyond what the packages take (KiB): room for deployed stage
# trees, kernels' modules next to the image's, and packages installed with `make pkg`.
IMAGE_ROOTFS_EXTRA_SPACE = "524288"

# Do NOT let wic rewrite /etc/fstab. It would append a line per partition with a mountpoint in the
# .wks, with plain "defaults" and the device named after --ondisk verbatim, after base-files'
# entries; systemd's fstab-generator lets the last entry for a mountpoint win, so the nofail of
# base-files' /boot and /data lines would be silently lost. The per-partition --no-fstab-update
# flag in the .wks does not prevent it in scarthgap; only this imager-level switch does.
WIC_CREATE_EXTRA_ARGS += "--no-fstab-update"

# The disk identifier of the msdos partition table, and with it the PARTUUIDs (<id>-01, -02, -03)
# the kernel command line names its root by (CMDLINE_ROOT_PARTITION = "PARTUUID=b7be4c4e-02" in
# the board conf). Scarthgap's wic picks a random one and has no option to choose it, so it is
# written into the MBR (offset 440, little-endian) once wic has made the image and before it is
# compressed and mapped.
BTBENCH_DISKID = "0xb7be4c4e"

def btbench_diskid_bytes(d):
    v = int(d.getVar('BTBENCH_DISKID'), 16)
    return ''.join('\\%03o' % b for b in v.to_bytes(4, 'little'))

IMAGE_CMD:wic:append () {
	printf '${@btbench_diskid_bytes(d)}' | dd of="$out.wic" bs=1 seek=440 count=4 conv=notrunc status=none
}
IMAGE_CMD:wic[vardeps] += "BTBENCH_DISKID"

# --- login ------------------------------------------------------------------------------------

# The root password from the device conf. Not extrausers/"usermod -P": in current shadow, -P
# means --prefix, so OE's old plaintext spelling now fails with "usermod: prefix must be an
# absolute path". chpasswd takes the plaintext and hashes it itself, which also keeps us out of the
# business of hashing on the host (Python's crypt module is gone in 3.13).
BTBENCH_ROOT_PASSWORD ??= "btbench"
btbench_root_password() {
    echo "root:${BTBENCH_ROOT_PASSWORD}" | chpasswd -R ${IMAGE_ROOTFS}
}
ROOTFS_POSTPROCESS_COMMAND += "btbench_root_password;"
PACKAGE_INSTALL:append = " base-passwd shadow"
do_rootfs[depends] += "shadow-native:do_populate_sysroot"

# The public key from the device conf (a path on this build host) as root's only authorized key:
# the deploy targets log in with it. Read when the image is built, and tracked, so a new key makes
# a new image. Without one the image still builds, and every deploy asks for the password.
BTBENCH_SSH_PUBKEY ??= ""
python btbench_authorized_keys() {
    path = d.getVar('BTBENCH_SSH_PUBKEY')
    if not path:
        bb.warn("BTBENCH_SSH_PUBKEY is not set: root gets no authorized ssh key, deploys will ask for the password")
        return
    if not os.path.isfile(path):
        bb.warn("BTBENCH_SSH_PUBKEY=%s does not exist: root gets no authorized ssh key" % path)
        return
    with open(path) as f:
        keys = f.read().strip()
    if not keys:
        bb.warn("BTBENCH_SSH_PUBKEY=%s is empty: root gets no authorized ssh key" % path)
        return
    home = oe.path.join(d.getVar('IMAGE_ROOTFS'), d.getVar('ROOT_HOME'))
    sshdir = os.path.join(home, '.ssh')
    bb.utils.mkdirhier(sshdir)
    os.chmod(sshdir, 0o700)
    keyfile = os.path.join(sshdir, 'authorized_keys')
    with open(keyfile, 'w') as f:
        f.write(keys + '\n')
    os.chmod(keyfile, 0o600)
}
ROOTFS_POSTPROCESS_COMMAND += "btbench_authorized_keys;"
do_rootfs[vardeps] += "BTBENCH_ROOT_PASSWORD BTBENCH_SSH_PUBKEY"
do_rootfs[file-checksums] += "${@'%s:%s' % (d.getVar('BTBENCH_SSH_PUBKEY'), os.path.exists(d.getVar('BTBENCH_SSH_PUBKEY'))) if d.getVar('BTBENCH_SSH_PUBKEY') else ''}"

export IMAGE_BASENAME = "btbench-image"
