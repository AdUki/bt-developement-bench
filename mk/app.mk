# btbenchd — the daemon and web console (app/): host build, tests, runs on this PC, the SDK cross
# build, and deploys to the board. Included by the Makefile.

APP       := app
APP_BUILD := $(APP)/build
APP_BIN   := $(APP_BUILD)/btbenchd
APP_XBUILD = $(BUILD_DIR)/btbenchd
PORT      ?= 8080
APP_DATA  ?= /tmp/btbench

# On the board (docs/contracts.md).
BTBENCHD_BIN := /usr/bin/btbenchd
BTBENCHD_WWW := /usr/share/btbenchd/www

# `make deploy` (mk/dev.mk) deploys every component built so far: the daemon once it has been
# cross-built.
DEPLOY_TARGETS += $(if $(wildcard $(BUILD_DIR)/btbenchd/btbenchd),deploy-daemon)

## ─── btbenchd ────────────────────────────────────────────────────────────────

# The libraries are git submodules (header-only, pinned). A clone without --recurse-submodules
# leaves them as empty directories, and the first sign of it would otherwise be a missing header.
.PHONY: check-submodules
check-submodules:
	@if [ ! -e $(APP)/third_party/httplib/httplib.h ]; then \
	  echo -e "$(BOLD)Submodules not checked out.$(OFF) app/third_party/* is empty."; \
	  echo -e "\n  $(BOLD)git submodule update --init$(OFF)\n"; \
	  exit 1; fi

$(APP_BUILD)/CMakeCache.txt:
	@cmake -S $(APP) -B $(APP_BUILD) -DCMAKE_BUILD_TYPE=Release >/dev/null

.PHONY: btbenchd
btbenchd: check-submodules $(APP_BUILD)/CMakeCache.txt ## Build btbenchd for this PC (app/build; compile_commands.json for clangd)
	@cmake --build $(APP_BUILD) -j$$(nproc) -- --no-print-directory
	@ln -sf build/compile_commands.json $(APP)/compile_commands.json

.PHONY: test
test: btbenchd ## Run btbenchd's unit tests (ctest)
	@ctest --test-dir $(APP_BUILD) --output-on-failure

# BT=fake runs it on a private D-Bus bus with tools/fake-bluez: a scripted world of devices, never
# this PC's own Bluetooth. Without BT=fake it runs with --no-bluetooth: the system bus here is the
# desktop's, and the agent would take over its pairings (`make pc` is for a real adapter).
# HCI=replay:FILE feeds the HCI monitor (Monitor page) from a btsnoop capture.
FAKEBT    = $(if $(filter fake,$(BT)),tools/fake-bluez/run )
HCIREPLAY = $(if $(filter replay:%,$(HCI)),--hci-replay $(patsubst replay:%,%,$(HCI)))

.PHONY: run
run: btbenchd ## Run it here on http://localhost:8080 (PORT=; BT=fake: a fake BlueZ; HCI=replay:FILE.btsnoop)
	@mkdir -p $(APP_DATA)/btsnoop
	@echo -e "$(BOLD)http://localhost:$(PORT)$(OFF)  $(DIM)data in $(APP_DATA)$(if $(FAKEBT), · fake BlueZ (tools/fake-bluez/ctl drives it))$(OFF)"
	@$(FAKEBT)$(APP_BIN) --pc --port $(PORT) --www $(APP)/www --data-dir $(APP_DATA) \
	        --capture-dir $(APP_DATA)/btsnoop $(if $(FAKEBT),,--no-bluetooth) $(HCIREPLAY) $(ARGS)

# The daemon against a real adapter of this PC. It becomes BlueZ's default pairing agent while it
# runs, answering only for BT=hciN (the desktop's other adapters' pairings are refused, so use a
# spare dongle). The HCI monitor socket needs CAP_NET_RAW; rather than running it as root, give
# the binary the capability once after each build:
#   sudo setcap cap_net_raw+ep app/build/btbenchd
.PHONY: pc
pc: btbenchd ## Run it against a real adapter of this PC (BT=hciN|address; see the comment for CAP_NET_RAW)
	@if [ -z "$(BT)" ] || [ "$(BT)" = fake ]; then \
	  echo -e "Usage: $(BOLD)make pc BT=hci1$(OFF)   (an adapter that is not the desktop's own)"; \
	  echo "Adapters here:"; ls /sys/class/bluetooth 2>/dev/null | grep -v : | sed 's/^/  /' || true; \
	  echo -e "$(DIM)make run BT=fake runs it without any radio.$(OFF)"; exit 1; fi
	@getcap $(APP_BIN) 2>/dev/null | grep -q cap_net_raw || \
	  echo -e "$(BOLD)Note:$(OFF) no CAP_NET_RAW, so no HCI monitor. $(DIM)sudo setcap cap_net_raw+ep $(APP_BIN)$(OFF)"
	@mkdir -p $(APP_DATA)/btsnoop
	@echo -e "$(BOLD)http://localhost:$(PORT)$(OFF)  $(DIM)adapter $(BT)$(OFF)"
	@$(APP_BIN) --pc --port $(PORT) --www $(APP)/www --data-dir $(APP_DATA) \
	        --capture-dir $(APP_DATA)/btsnoop --bt-adapter $(BT) $(HCIREPLAY) $(ARGS)

# The SDK's CMake toolchain file sets the sysroot and the cross compiler; md2html runs on the host
# from the repo (python3), so the SDK needs nothing for it.
.PHONY: btbenchd-cross
btbenchd-cross: check-submodules check-sdk ## Cross-build btbenchd with the SDK (build/<board>/btbenchd)
	@set -e; . $(SDK_ENV) >/dev/null; \
	cmake -S $(APP) -B $(APP_XBUILD) -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_TOOLCHAIN_FILE=$$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake >/dev/null; \
	cmake --build $(APP_XBUILD) -j$$(nproc) --target btbenchd
	@echo -e "$(BOLD)$(APP_XBUILD)/btbenchd$(OFF)  $$(file -b $(APP_XBUILD)/btbenchd | cut -d, -f1-2)"

.PHONY: deploy-daemon
deploy-daemon: btbenchd-cross ## Copy btbenchd and its www to the board and restart it (TARGET=, RESTART=0)
	@echo -e "$(BOLD)btbenchd → $(TARGET)$(OFF)"
	@$(SSH) $(TARGET) 'systemctl stop btbenchd'
	@$(RSYNC) $(APP_XBUILD)/btbenchd $(TARGET):$(BTBENCHD_BIN)
	@$(RSYNC) --delete $(APP)/www/ $(TARGET):$(BTBENCHD_WWW)/
	@if [ "$(RESTART)" != 0 ]; then $(SSH) $(TARGET) 'systemctl start btbenchd'; \
	 else echo -e "$(DIM)RESTART=0: btbenchd left stopped$(OFF)"; fi

# The daemon reads www from disk per request: no restart, a reload in the browser is enough.
.PHONY: deploy-www
deploy-www: btbenchd ## Copy app/www to the board (no restart; the browser reloads it)
	@echo -e "$(BOLD)app/www → $(TARGET):$(BTBENCHD_WWW)$(OFF)"
	@$(RSYNC) --delete $(APP)/www/ $(TARGET):$(BTBENCHD_WWW)/
	@echo -e "Done. Reload the page."
