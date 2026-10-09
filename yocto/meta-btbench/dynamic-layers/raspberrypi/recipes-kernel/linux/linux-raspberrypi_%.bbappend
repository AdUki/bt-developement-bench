# KERNEL=rpi: the Raspberry Pi fork, for comparing against bluetooth-next on the same board.
#
# It gets the bench's fragment like linux-btbench does, so the two kernels differ in their source
# and the board defconfig, not in what the bench switched on. kernel-yocto (which
# linux-raspberrypi uses) applies a .cfg in SRC_URI as a config fragment by itself. The fragment
# lives with linux-btbench in the layer's main recipes-kernel/.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../recipes-kernel/linux/files:"

SRC_URI += "file://btbench.cfg"

# The bench's kernels are recognisable by their release: <version>-btbench-<git revision>.
LINUX_VERSION_EXTENSION = "-btbench"
