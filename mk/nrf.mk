# nRF HCI controller: a Nordic nRF54L15 DK running Zephyr's hci_uart sample with Zephyr's own
# link layer, plugged into a board's USB as a second adapter (docs/system.md "An nRF54L15 DK as
# the controller"). Included by the Makefile.
#
#   make nrf-setup     once: west in a venv, the Zephyr workspace, the Zephyr SDK's arm toolchain
#   make nrf-hci       build the firmware (tools/nrf-hci: the hci_uart sample + a public address)
#   make nrf-flash     flash it over the DK's J-Link (the DK on this PC's USB)
#   make nrf-check     talk HCI to the DK on this PC's USB: reset, version, LE features
#
# The Zephyr tree is not a submodule: it is a west workspace outside the repo (ZEPHYR_WORKSPACE),
# pinned to ZEPHYR_VERSION, with only the modules this build needs. SEGGER's J-Link software is
# the one thing nrf-setup cannot fetch: its download takes a licence click, so it is unpacked by
# hand (dpkg-deb -x JLink_Linux_x86_64.deb, no root needed) and found through JLINK_DIR.

ZEPHYR_WORKSPACE ?= $(HOME)/projects/zephyrproject
ZEPHYR_VERSION   ?= v4.5.0
ZEPHYR_VENV      ?= $(HOME)/opt/zephyr-venv
ZEPHYR_SDK       ?= $(HOME)/opt/zephyr-sdk
JLINK_DIR        ?= $(HOME)/opt/jlink
NRF_BOARD        ?= nrf54l15dk/nrf54l15/cpuapp
# The DK's second CDC ACM port on this PC (interface 02 of the J-Link: the HCI UART).
NRF_TTY          ?= $(firstword $(wildcard /dev/serial/by-id/usb-SEGGER_J-Link_*-if02))

NRF_APP   := $(ROOT)/tools/nrf-hci
NRF_B     := $(ROOT)/build/nrf-hci
# The west workspace's modules the hci_uart build pulls in; `west update` of all of them is GBs.
NRF_MODULES := hal_nordic cmsis cmsis_6 mbedtls tf-psa-crypto picolibc

# west and the build run with the venv first in PATH and the SDK and J-Link where Zephyr's
# cmake and its jlink runner look for them.
NRF_ENV := PATH=$(ZEPHYR_VENV)/bin:$(JLINK_DIR):$$PATH ZEPHYR_SDK_INSTALL_DIR=$(ZEPHYR_SDK)
WEST    := $(NRF_ENV) west

## ─── nrf ─────────────────────────────────────────────────────────────────────

.PHONY: check-nrf
check-nrf:
	@if [ ! -x $(ZEPHYR_VENV)/bin/west ] || [ ! -d $(NRF_APP) ] || [ ! -f $(ZEPHYR_SDK)/sdk_version ]; then \
	  echo -e "$(BOLD)No Zephyr workspace.$(OFF) Run $(BOLD)make nrf-setup$(OFF) first (ZEPHYR_WORKSPACE=$(ZEPHYR_WORKSPACE))."; exit 1; fi

.PHONY: nrf-setup
nrf-setup: ## Once: west venv, the Zephyr workspace with its modules, the SDK's arm toolchain
	@set -e; \
	[ -x $(ZEPHYR_VENV)/bin/west ] || { python3 -m venv $(ZEPHYR_VENV); $(ZEPHYR_VENV)/bin/pip -q install west; }; \
	if [ ! -d $(ZEPHYR_WORKSPACE)/zephyr ]; then \
	  mkdir -p $(ZEPHYR_WORKSPACE); \
	  git clone --branch $(ZEPHYR_VERSION) https://github.com/zephyrproject-rtos/zephyr.git $(ZEPHYR_WORKSPACE)/zephyr; \
	  cd $(ZEPHYR_WORKSPACE) && $(WEST) init -l zephyr; \
	fi; \
	cd $(ZEPHYR_WORKSPACE) && $(WEST) update --narrow -o=--depth=1 $(NRF_MODULES); \
	$(ZEPHYR_VENV)/bin/pip -q install -r $(ZEPHYR_WORKSPACE)/zephyr/scripts/requirements-base.txt; \
	if [ ! -f $(ZEPHYR_SDK)/sdk_version ]; then \
	  v=$$(cat $(ZEPHYR_WORKSPACE)/zephyr/SDK_VERSION); dir=$$(dirname $(ZEPHYR_SDK)); mkdir -p $$dir; \
	  curl -sSL https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v$$v/zephyr-sdk-$${v}_linux-x86_64_minimal.tar.xz | tar -xJ -C $$dir; \
	  $$dir/zephyr-sdk-$$v/setup.sh -t arm-zephyr-eabi -h -c; \
	  ln -sfn $$dir/zephyr-sdk-$$v $(ZEPHYR_SDK); \
	fi; \
	[ -x $(JLINK_DIR)/JLinkExe ] || echo -e "$(BOLD)No J-Link at $(JLINK_DIR).$(OFF) Unpack SEGGER's JLink_Linux_x86_64.deb there (dpkg-deb -x) for make nrf-flash."

.PHONY: nrf-hci
nrf-hci: check-nrf ## Build the nRF54L15 DK's HCI controller firmware (build/nrf-hci)
	cd $(ZEPHYR_WORKSPACE) && $(WEST) build -b $(NRF_BOARD) -d $(NRF_B) $(NRF_APP)

.PHONY: nrf-flash
nrf-flash: check-nrf ## Flash build/nrf-hci onto the DK over its J-Link (the DK on this PC's USB)
	@[ -x $(JLINK_DIR)/JLinkExe ] || { echo -e "$(BOLD)No J-Link at $(JLINK_DIR)$(OFF) (JLINK_DIR=)."; exit 1; }
	cd $(ZEPHYR_WORKSPACE) && $(WEST) flash -d $(NRF_B) --runner jlink

.PHONY: nrf-check
nrf-check: ## HCI reset, version and LE features from the DK on this PC's USB (NRF_TTY=/dev/ttyACMn)
	@[ -n "$(NRF_TTY)" ] || { echo -e "$(BOLD)No DK on this PC's USB$(OFF) (no SEGGER J-Link CDC port)."; exit 1; }
	python3 -I tools/nrf-hci/hci-check $(NRF_TTY)
