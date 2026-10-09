SUMMARY = "Bench tools: debugging, tracing, networking and Wi-Fi, and a usable command line"
DESCRIPTION = "What you reach for on the board itself. BlueZ's own tools and test scripts come with \
packagegroup-btbench-audio, the kernel's with packagegroup-btbench-kernel."
LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    ${BTBENCH_TOOLS_DEBUG} \
    ${BTBENCH_TOOLS_NET} \
    ${BTBENCH_TOOLS_SHELL} \
    ${BTBENCH_TOOLS_PYTHON} \
"

# gdbserver for `make gdb` (the SDK's gdb attaches to it); gdb for a quick look on the board.
# perf is built from the image kernel's own source tree, so it always matches the running kernel
# of the image (a trial kernel's perf events may differ).
BTBENCH_TOOLS_DEBUG = " \
    gdb \
    gdbserver \
    strace \
    ltrace \
    perf \
    trace-cmd \
"

# Throughput over the gadget link and Wi-Fi (iperf3), packet captures, and the Wi-Fi stack
# btbench-wifi drives — also there by hand, for debugging the radio.
BTBENCH_TOOLS_NET = " \
    iperf3 \
    tcpdump \
    ethtool \
    iproute2 \
    iw \
    wpa-supplicant \
    wpa-supplicant-cli \
    wpa-supplicant-passphrase \
    hostapd \
    dnsmasq \
    wireless-regdb-static \
    curl \
    rsync \
"

BTBENCH_TOOLS_SHELL = " \
    bash \
    coreutils \
    less \
    htop \
    jq \
    file \
    nano \
    procps \
    util-linux \
    usbutils \
    parted \
    e2fsprogs-resize2fs \
"

# For BlueZ's test/*.py scripts (simple-agent, example-gatt-server, example-advertisement,
# example-endpoint, ...), which talk to bluetoothd over D-Bus and run a GLib main loop.
BTBENCH_TOOLS_PYTHON = " \
    python3-core \
    python3-dbus \
    python3-pygobject \
"
