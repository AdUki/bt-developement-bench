# KERNEL=rpi: the Raspberry Pi fork, for comparing against bluetooth-next on the same board.
#
# It gets the bench's fragments like linux-btbench does (btbench.cfg, bt-debug.cfg for the
# Bluetooth logging switched on at runtime, and the board's), so the two kernels differ in their
# source and the board defconfig, not in what the bench switched on. kernel-yocto (which
# linux-raspberrypi uses) applies a .cfg in SRC_URI as a config fragment by itself. The bench's
# fragments live with linux-btbench in the layer's main recipes-kernel/.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../recipes-kernel/linux/files:"
# The board's own fragment (boards/rpi.cfg, BTBENCH_KFRAGMENT_BOARD), after the bench's.
FILESEXTRAPATHS:prepend := "${@os.path.dirname(d.getVar('BTBENCH_KFRAGMENT_BOARD')) + ':' if d.getVar('BTBENCH_KFRAGMENT_BOARD') else ''}"

SRC_URI += "file://btbench.cfg file://bt-debug.cfg \
    ${@'file://' + os.path.basename(d.getVar('BTBENCH_KFRAGMENT_BOARD')) if d.getVar('BTBENCH_KFRAGMENT_BOARD') else ''}"

# The bench's kernels are recognisable by their release: <version>-btbench-<git revision>.
LINUX_VERSION_EXTENSION = "-btbench"
