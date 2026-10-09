# base-files owns /etc/hostname (it writes it from its own `hostname` variable), so the hostname is
# set here rather than shipped as a second copy of the file from btbench-base: opkg refuses to
# build a rootfs where two packages claim one path.
BTBENCH_HOSTNAME ??= "btbench"
hostname = "${BTBENCH_HOSTNAME}"

# The two partitions next to the root filesystem (wic/btbench-rpi.wks), by label, so the entries
# hold whatever the SD card's device is called under a given kernel:
#   /boot  the firmware's FAT partition, read-write: the kernel trial flow writes test and previous
#          kernels under /boot/bench and U-Boot's environment (/boot/uboot.env) there.
#   /data  the bench's state: settings, Wi-Fi networks, pairings, captures. Grown to fill the card
#          at first boot (btbench-data).
#
# nofail on both: a partition that is missing or damaged must not drop the board into
# emergency.target, which on a headless board looks exactly like a hang. The device timeout is NOT
# short: on the Zero W, udevd only gets going ~20 s into the boot, after the by-label links are
# already being waited for, and a 10 s timeout gave up on two healthy partitions on the first
# board that booted. Without
# /boot the trial flow cannot work, and without /data the bench keeps its state on the root
# filesystem, but the board stays reachable to fix either. /data gets an fsck pass (a power cut
# mid-capture is the normal way a bench is switched off); a FAT check needs dosfstools, which the
# image does not carry.
#
# These must be the only /boot and /data entries in the image: wic's own fstab update is turned
# off in btbench-image.bb, since it would append a second entry per partition with plain
# "defaults", and systemd's fstab-generator lets the last entry for a mountpoint win.
do_install:append() {
    cat >>${D}${sysconfdir}/fstab <<'EOF2'
LABEL=boot  /boot  vfat  defaults,noatime,nofail,x-systemd.device-timeout=90s  0  0
LABEL=data  /data  ext4  defaults,noatime,nofail,x-systemd.device-timeout=90s  0  2
EOF2
    install -d ${D}/data
}

do_install[vardeps] += "BTBENCH_HOSTNAME"

# Our motd instead of poky's "reference distribution" warning. files/btbench/ wins over
# meta-poky's files/poky/: both are distro overrides, and btbench comes later in DISTROOVERRIDES.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
