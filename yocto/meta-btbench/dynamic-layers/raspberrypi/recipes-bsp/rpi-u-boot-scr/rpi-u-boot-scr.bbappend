# The bench's boot.scr: meta-raspberrypi's normal boot, plus the one-shot trial boot of
# /boot/bench/test/ that `btbench-kernel try` arms (docs/kernel.md).
#
# Our boot.cmd.in takes the place of meta-raspberrypi's (FILESEXTRAPATHS is searched first). The
# recipe's own do_compile fills in the kernel image type and boot command; the trial path adds
# the board's kernel file name for the test slot, which is the same file name.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
