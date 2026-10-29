# The bench's HCI controller firmware for a Nordic nRF54L15 DK (docs/system.md "An nRF54L15 DK as
# the controller"): Zephyr's hci_uart sample as it is, from the Zephyr workspace, plus the one
# thing it lacks for a Linux host, a public address (src/addr.c). Built by `make nrf-hci`.
cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(btbench_hci)

target_sources(app PRIVATE ${ZEPHYR_BASE}/samples/bluetooth/hci_uart/src/main.c src/addr.c)
