# bt-developement-bench — a Bluetooth development bench on a Yocto image
#
#   make              list the targets
#   make configure    hostname, ssh key, Wi-Fi, caches (once)
#   make image        build the image;  make flash DISK=/dev/sdX  to write it
#   make sdk          build + install the SDK the dev targets compile with
#   make src          create the dev trees (src/bluez, src/pipewire, src/wireplumber, src/linux)
#   make bluez && make deploy-bluez      the inner loop (same for pipewire, wireplumber, linux)
#
# Plain poky + bitbake, no kas, no pip: the layers are git clones and the build dir's conf files
# are generated. BOARD picks the hardware; boards/$(BOARD).mk says what that means for the build
# and for the dev targets, yocto/conf/boards/$(BOARD).conf holds its bitbake settings. Each board
# builds in its own dir; downloads and sstate are shared through site.conf.
#
# The targets live in mk/*.mk by area: yocto (image, sdk, flash, configure), dev (src trees,
# component builds, deploys), kernel (build, trial boot, commit), app (btbenchd on this PC).

SHELL := /bin/bash
.DEFAULT_GOAL := help
ROOT := $(CURDIR)

-include bench.conf

BOARD  ?= rpi0w
KERNEL ?= next
TARGET ?= root@10.55.0.1

BOARDS := $(patsubst boards/%.mk,%,$(wildcard boards/*.mk))
ifeq ($(filter $(BOARD),$(BOARDS)),)
  $(error BOARD=$(BOARD) is not one of: $(BOARDS))
endif
ifeq ($(filter $(KERNEL),next rpi),)
  $(error KERNEL=$(KERNEL) is not one of: next rpi)
endif

YOCTO  := yocto
LAYERS := $(YOCTO)/layers
include boards/$(BOARD).mk
BOARD_MK   := boards/$(BOARD).mk
BOARD_CONF := $(YOCTO)/conf/boards/$(BOARD).conf

# Dev trees: worktrees of the upstream checkouts (make src), or whatever *_SRC points at.
BLUEZ_UPSTREAM       ?= $(HOME)/projects/bluez
PIPEWIRE_UPSTREAM    ?= $(HOME)/projects/pipewire
WIREPLUMBER_UPSTREAM ?= $(HOME)/projects/wireplumber
LINUX_UPSTREAM       ?= $(HOME)/projects/linux
BLUEZ_SRC       ?= $(ROOT)/src/bluez
PIPEWIRE_SRC    ?= $(ROOT)/src/pipewire
WIREPLUMBER_SRC ?= $(ROOT)/src/wireplumber
LINUX_SRC       ?= $(ROOT)/src/linux
LINUX_RPI_SRC   ?= $(ROOT)/src/linux-rpi

# Out-of-tree builds and their install trees (DESTDIR), per board.
BUILD_DIR := $(ROOT)/build/$(BOARD)
STAGE     := $(BUILD_DIR)/stage

# The installed SDK the dev targets compile with (make sdk).
SDK_DIR := $(ROOT)/sdk/$(BOARD)
SDK_ENV  = $(firstword $(wildcard $(SDK_DIR)/$(SDK_ENV_GLOB)))

# ssh/rsync to the board. One master connection is shared by every call for two minutes, so a
# deploy of many files pays for one handshake. The board is reflashed often and its host key
# changes with every image, so the key is neither checked nor remembered.
SSH_OPTS := -o ControlMaster=auto -o ControlPath=$(ROOT)/build/.ssh-%C -o ControlPersist=120 \
            -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR
SSH      := ssh $(SSH_OPTS)
RSYNC    := rsync -rlptD --info=stats0 -e "ssh $(SSH_OPTS)"

BOLD := \033[1m
DIM  := \033[2m
OFF  := \033[0m

$(shell mkdir -p $(ROOT)/build)

include mk/yocto.mk
include mk/system.mk
include mk/dev.mk
include mk/kernel.mk
include mk/app.mk

## ─── misc ────────────────────────────────────────────────────────────────────

.PHONY: ssh
ssh: ## Shell on the board (TARGET=root@host)
	@$(SSH) $(TARGET)

.PHONY: help
help:
	@echo -e "$(BOLD)bt-developement-bench$(OFF)  $(DIM)boards: $(BOARDS)  BOARD=$(BOARD) KERNEL=$(KERNEL) TARGET=$(TARGET)$(OFF)"
	@awk 'BEGIN {FS = ":.*## "} \
	     /^## ─/ { gsub(/## /,""); printf "\n\033[2m%s\033[0m\n", $$0; next } \
	     /^[a-zA-Z0-9_%-]+:.*?## / { printf "  \033[1m%-18s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo -e "\n$(DIM)Flags:  BOARD=$(BOARDS)  KERNEL=next|rpi  SRC=local (image from src/)  RESTART=0 (deploy)$(OFF)"
	@echo -e "$(DIM)Vars:   TARGET=root@host  DISK=/dev/...  ARGS=\"...\" (bitbake)  bench.conf for defaults$(OFF)\n"
