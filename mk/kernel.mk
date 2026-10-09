# Kernel: dev trees, cross builds with the image's SDK, trial boots on the board. Included by the
# Makefile. The how and why are in docs/kernel.md.
#
#   make src-linux                       src/linux: bluetooth-next, a worktree of LINUX_UPSTREAM
#   make linux                           build it (KERNEL=rpi: src/linux-rpi, make src-linux-rpi)
#   make linux-test                      put it on the board as the test kernel and boot it once
#   make linux-commit / linux-rollback   keep it as the good kernel / go back to the previous one
#
# The kernel is built with the SDK's cross compiler and nothing else from the SDK: the kernel
# brings its own flags and must not see the CFLAGS/LDFLAGS (or the target pkg-config) the SDK's
# environment script exports. Its host programs (kconfig, dtc, sign-file) are built with this
# PC's compiler, bison, flex, openssl and libelf headers.

BLUETOOTH_NEXT_URL := https://git.kernel.org/pub/scm/linux/kernel/git/bluetooth/bluetooth-next.git
LINUX_RPI_URL      := https://github.com/raspberrypi/linux.git

# The tree, build dir and install tree of the selected KERNEL.
LINUX_TREE := $(if $(filter rpi,$(KERNEL)),$(LINUX_RPI_SRC),$(LINUX_SRC))
KOUT       := $(BUILD_DIR)/linux-$(KERNEL)
KSTAGE     := $(STAGE)/linux-$(KERNEL)
# The revision the image's kernel is built from: src/linux starts there (like the other src
# trees, mk/dev.mk), so a first deploy changes nothing but what you then commit on top.
LINUX_PINNED := $(call pinned,LINUX)
# The fragment the image's kernels are configured with, too.
KFRAGMENT  := $(ROOT)/$(YOCTO)/meta-btbench/recipes-kernel/linux/files/btbench.cfg
KDEFCONFIG := $(DEFCONFIG_$(KERNEL))
KDTB       := $(firstword $(DTBS_$(KERNEL)))
KDTS_EXTRA := $(if $(DTS_EXTRA_$(KERNEL)),$(ROOT)/$(DTS_EXTRA_$(KERNEL)))

# The kernel release is <version>$(LINUX_LOCALVERSION)-<git revision> (CONFIG_LOCALVERSION_AUTO
# in the fragment), as for the image's kernel.
LINUX_LOCALVERSION ?= -btbench
JOBS ?= $(shell nproc)

# The kernel's make, in an environment of its own: the cross compiler from the SDK (found through
# its environment script, which is sourced in a subshell only), none of this Makefile's command
# line variables (MAKEFLAGS would hand BOARD=, KERNEL=, ... down to it) and no compiler flags
# from the caller's environment. Defines a shell function kmake; expects set -e.
define kmake_setup
cross=$$(. "$(SDK_ENV)" >/dev/null 2>&1 && command -v "$${TARGET_PREFIX}gcc") || true; \
[ -n "$$cross" ] || { echo "No cross compiler in the SDK ($(SDK_ENV))."; exit 1; }; \
[ -d "$(LINUX_TREE)" ] || { echo -e "No kernel tree at $(LINUX_TREE).\n\n  $(BOLD)make src-linux$(if $(filter rpi,$(KERNEL)),-rpi)$(OFF)\n"; exit 1; }; \
kmake() { env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL -u CFLAGS -u CPPFLAGS -u CXXFLAGS \
	-u LDFLAGS -u KCFLAGS -u KCPPFLAGS -u CC -u LD -u ARCH -u CROSS_COMPILE \
	make -C "$(LINUX_TREE)" O="$(KOUT)" ARCH=$(KARCH) CROSS_COMPILE="$${cross%gcc}" \
	LOCALVERSION=$(LINUX_LOCALVERSION) "$$@"; }
endef

# Fragment values that did not survive olddefconfig (renamed, gone, or a dependency missing):
# merge_config.sh -m does not say. The linux-btbench recipe makes the same check.
define kconfig_check
awk 'FNR == NR { \
	  if (match($$0, /^CONFIG_[A-Za-z0-9_]+=/)) { want[substr($$0, 1, RLENGTH - 1)] = substr($$0, RLENGTH + 1) } \
	  else if (match($$0, /^# CONFIG_[A-Za-z0-9_]+ is not set/)) { want[$$2] = "n" } \
	  next } \
     match($$0, /^CONFIG_[A-Za-z0-9_]+=/) { have[substr($$0, 1, RLENGTH - 1)] = substr($$0, RLENGTH + 1) } \
     END { for (s in want) { h = (s in have) ? have[s] : "n"; \
	   if (h != want[s]) printf "  %s: requested %s, got %s\n", s, want[s], h } }' \
	$(KFRAGMENT) $(KOUT)/.config | sort > $(KOUT)/.btbench-cfg-check; \
if [ -s $(KOUT)/.btbench-cfg-check ]; then \
  echo -e "$(BOLD)btbench.cfg values the kernel did not take:$(OFF)"; cat $(KOUT)/.btbench-cfg-check; fi
endef

# What the .config was generated from; a change regenerates it (make linux-defconfig).
KCONFIG_STAMP = $(KDEFCONFIG) $(shell md5sum < $(KFRAGMENT) | cut -c1-32)

# make clangd (mk/dev.mk): the kernel tree and its build dir.
CLANGD_TREES += $(LINUX_TREE):$(KOUT)

# linux-deploy is deliberately not in DEPLOY_TARGETS (mk/dev.mk's `make deploy`): a kernel goes
# on the board only through a trial boot (make linux-test).

## ─── kernel ──────────────────────────────────────────────────────────────────

.PHONY: src-linux
src-linux: ## src/linux: worktree of LINUX_UPSTREAM at the image's bluetooth-next revision (asks before adding the remote)
	@set -e; \
	if [ -e $(LINUX_SRC) ]; then echo "$(LINUX_SRC) exists."; exit 0; fi; \
	if git -C $(LINUX_UPSTREAM) rev-parse --git-dir >/dev/null 2>&1; then \
	  if ! git -C $(LINUX_UPSTREAM) remote get-url bluetooth-next >/dev/null 2>&1; then \
	    echo -e "$(BOLD)$(LINUX_UPSTREAM)$(OFF) has no bluetooth-next remote. src/linux is a worktree of it on the"; \
	    echo -e "image's bluetooth-next revision, so the remote has to be added to that repository:\n"; \
	    echo -e "  git -C $(LINUX_UPSTREAM) remote add bluetooth-next $(BLUETOOTH_NEXT_URL)"; \
	    echo -e "  git -C $(LINUX_UPSTREAM) fetch bluetooth-next\n"; \
	    ans=""; [ -t 0 ] && read -r -p "Add it and fetch now? [y/N] " ans; \
	    case "$$ans" in \
	      [yY]*) git -C $(LINUX_UPSTREAM) remote add bluetooth-next $(BLUETOOTH_NEXT_URL) ;; \
	      *) echo "Not changed. Add the remote yourself, or set LINUX_UPSTREAM to a tree that has it"; \
	         echo "(or to a path that does not exist, for a fresh clone)."; exit 1 ;; \
	    esac; \
	  fi; \
	  echo "Fetching bluetooth-next into $(LINUX_UPSTREAM)..."; \
	  git -C $(LINUX_UPSTREAM) fetch bluetooth-next; \
	  mkdir -p $(dir $(LINUX_SRC)); \
	  base=bluetooth-next/master; \
	  if [ -n "$(LINUX_PINNED)" ] && git -C $(LINUX_UPSTREAM) cat-file -e "$(LINUX_PINNED)^{commit}" 2>/dev/null; then \
	    base=$(LINUX_PINNED); fi; \
	  if git -C $(LINUX_UPSTREAM) show-ref --verify -q refs/heads/bench; then \
	    echo "Reusing the existing branch bench of $(LINUX_UPSTREAM)."; \
	    git -C $(LINUX_UPSTREAM) worktree add $(LINUX_SRC) bench; \
	  else \
	    git -C $(LINUX_UPSTREAM) worktree add $(LINUX_SRC) -b bench $$base; \
	    git -C $(LINUX_SRC) branch -q --set-upstream-to=bluetooth-next/master; \
	  fi; \
	else \
	  echo "No git tree at $(LINUX_UPSTREAM): cloning bluetooth-next (blobless) into $(LINUX_SRC)."; \
	  git clone --filter=blob:none -b master $(BLUETOOTH_NEXT_URL) $(LINUX_SRC); \
	  git -C $(LINUX_SRC) checkout -q -b bench $(or $(LINUX_PINNED),master); \
	fi
	@$(call src_exclude,$(LINUX_SRC))
	@echo -e "$(BOLD)$(LINUX_SRC)$(OFF) on $$(git -C $(LINUX_SRC) log -1 --format='%h %s')"

# The build's compile_commands.json and .clangd are symlinked into the tree; keep them out of
# git status (info/exclude is shared by all worktrees of a repository).
src_exclude = excl=$$(git -C $(1) rev-parse --path-format=absolute --git-common-dir)/info/exclude; \
	mkdir -p $$(dirname $$excl); \
	for f in compile_commands.json .clangd; do \
	  grep -qxF $$f $$excl 2>/dev/null || echo $$f >> $$excl; done

.PHONY: src-linux-rpi
src-linux-rpi: $(BUILD_CONF) ## src/linux-rpi: raspberrypi/linux at linux-raspberrypi's revision (for KERNEL=rpi)
	@set -e; \
	if [ -e $(LINUX_RPI_SRC) ]; then echo "$(LINUX_RPI_SRC) exists."; exit 0; fi; \
	echo "Reading linux-raspberrypi's revision from bitbake..."; \
	env=$$($(BB) bitbake -e linux-raspberrypi); \
	rev=$$(echo "$$env" | sed -n 's/^SRCREV_machine="\(.*\)"/\1/p'); \
	branch=$$(echo "$$env" | sed -n 's/^LINUX_RPI_BRANCH="\(.*\)"/\1/p'); \
	[ -n "$$rev" ] && [ -n "$$branch" ] || { echo "Could not read SRCREV_machine/LINUX_RPI_BRANCH."; exit 1; }; \
	echo "Cloning $(LINUX_RPI_URL) $$branch (blobless) at $$rev..."; \
	git clone --filter=blob:none --no-checkout -b $$branch $(LINUX_RPI_URL) $(LINUX_RPI_SRC); \
	git -C $(LINUX_RPI_SRC) checkout -q -b bench $$rev
	@$(call src_exclude,$(LINUX_RPI_SRC))
	@echo -e "$(BOLD)$(LINUX_RPI_SRC)$(OFF) on $$(git -C $(LINUX_RPI_SRC) log -1 --format='%h %s')"

.PHONY: linux-defconfig
linux-defconfig: check-sdk ## (Re)generate the kernel .config: board defconfig + btbench.cfg
	@set -e; $(kmake_setup); \
	mkdir -p $(KOUT); \
	if [ -f $(KOUT)/.config ]; then cp $(KOUT)/.config $(KOUT)/.config.btbench-old; \
	  echo "Previous config saved as $(KOUT)/.config.btbench-old"; fi; \
	echo -e "$(BOLD)Config$(OFF) $(KDEFCONFIG) + btbench.cfg $(DIM)→ $(KOUT)/.config$(OFF)"; \
	kmake -s $(KDEFCONFIG); \
	"$(LINUX_TREE)/scripts/kconfig/merge_config.sh" -m -O $(KOUT) $(KOUT)/.config $(KFRAGMENT) >/dev/null; \
	kmake -s olddefconfig; \
	echo "$(KCONFIG_STAMP)" > $(KOUT)/.btbench-config; \
	$(kconfig_check)

.PHONY: linux
linux: check-sdk ## Build kernel, modules, dtb into build/<board>/stage/linux-<kernel> (KERNEL=next|rpi)
	@set -e; \
	if [ ! -f $(KOUT)/.config ] || [ "$$(cat $(KOUT)/.btbench-config 2>/dev/null)" != "$(KCONFIG_STAMP)" ]; then \
	  $(MAKE) --no-print-directory linux-defconfig; fi
	@set -e; $(kmake_setup); \
	echo -e "$(BOLD)Building$(OFF) $(KIMAGE) modules dtbs $(DIM)($(LINUX_TREE) → $(KOUT))$(OFF)"; \
	DTC_FLAGS=-@ kmake -j$(JOBS) $(KIMAGE) modules dtbs; \
	release=$$(kmake -s kernelrelease); \
	rm -rf $(KSTAGE); mkdir -p $(KSTAGE)/boot; \
	kmake -s modules_install INSTALL_MOD_PATH=$(KSTAGE) INSTALL_MOD_STRIP=1; \
	rm -f $(KSTAGE)/lib/modules/$$release/build $(KSTAGE)/lib/modules/$$release/source; \
	cp $(KOUT)/arch/$(KARCH)/boot/$(KIMAGE) $(KSTAGE)/boot/$(KIMAGE); \
	if [ -n "$(KDTS_EXTRA)" ]; then \
	  DTC=$(KOUT)/scripts/dtc/dtc $(ROOT)/tools/dev/mkdtb $(LINUX_TREE) $(KARCH) \
	    $(KDTB:.dtb=.dts) $(KDTS_EXTRA) $(KSTAGE)/boot/board.dtb; \
	else \
	  cp $(KOUT)/arch/$(KARCH)/boot/dts/$(KDTB) $(KSTAGE)/boot/board.dtb; \
	fi; \
	echo "$$release" > $(KSTAGE)/boot/version; \
	kmake -s compile_commands.json >/dev/null 2>&1 \
	  && ln -sfn $(KOUT)/compile_commands.json $(LINUX_TREE)/compile_commands.json \
	  || echo "(compile_commands.json not generated: needs python3)"; \
	echo -e "\n$(BOLD)$$release$(OFF)  $(DIM)$(KSTAGE)$(OFF)"; \
	echo "  boot/$(KIMAGE) $$(du -h $(KSTAGE)/boot/$(KIMAGE) | cut -f1), boot/board.dtb ($(KDTB)$(if $(KDTS_EXTRA), + $(notdir $(KDTS_EXTRA)))), modules $$(du -sh $(KSTAGE)/lib/modules | cut -f1)"; \
	echo -e "  next: $(BOLD)make linux-test$(OFF)  (deploy as the test kernel and boot it once)"

.PHONY: linux-menuconfig
linux-menuconfig: check-sdk ## menuconfig on the kernel's .config (kept until btbench.cfg changes)
	@set -e; [ -f $(KOUT)/.config ] || $(MAKE) --no-print-directory linux-defconfig
	@set -e; $(kmake_setup); kmake menuconfig

.PHONY: linux-savedefconfig
linux-savedefconfig: check-sdk ## Write a minimal defconfig of the current .config (build/<b>/linux-<k>/defconfig)
	@set -e; $(kmake_setup); kmake -s savedefconfig; echo "$(KOUT)/defconfig"

# The test slot on the board takes the image, the dtb and the release; the modules go to
# /lib/modules/<release>, next to the good kernel's. A build with the release of the board's good
# kernel would overwrite the good kernel's modules, and the fallback boot would load them: that
# is refused unless FORCE=1.
.PHONY: linux-deploy
linux-deploy: ## Copy the built kernel to the board's test slot (/boot/bench/test, /lib/modules)
	@set -e; \
	[ -f $(KSTAGE)/boot/version ] || { echo -e "Nothing built for KERNEL=$(KERNEL). Run $(BOLD)make linux$(OFF) first."; exit 1; }; \
	release=$$(cat $(KSTAGE)/boot/version); [ -n "$$release" ]; \
	status=$$($(SSH) $(TARGET) btbench-kernel status) \
	  || { echo "btbench-kernel status failed on $(TARGET)."; exit 1; }; \
	good=$$(echo "$$status" | jq -r '.good.version // empty'); \
	if [ "$$release" = "$$good" ] && [ "$(FORCE)" != 1 ]; then \
	  echo -e "$(BOLD)Refusing:$(OFF) the board's good kernel is $$release as well. Its modules would be"; \
	  echo    "replaced by this build's, and a fallback to the good kernel would load them."; \
	  echo    "Commit your change in $(LINUX_TREE) (the release carries the git revision), build with"; \
	  echo -e "$(BOLD)make linux LINUX_LOCALVERSION=-btbench2$(OFF), or deploy anyway with FORCE=1."; \
	  exit 1; \
	fi; \
	echo -e "$(BOLD)Deploying$(OFF) $$release → $(TARGET):/boot/bench/test"; \
	$(SSH) $(TARGET) "mkdir -p /boot/bench/test && rm -f /boot/bench/test/* && rm -rf /lib/modules/$$release"; \
	$(RSYNC) --no-perms --no-owner --no-group --modify-window=1 \
	  $(KSTAGE)/boot/$(KIMAGE) $(KSTAGE)/boot/board.dtb $(KSTAGE)/boot/version $(TARGET):/boot/bench/test/; \
	$(RSYNC) $(KSTAGE)/lib/modules/$$release $(TARGET):/lib/modules/; \
	$(SSH) $(TARGET) "depmod -a $$release && btbench-kernel prune && sync"; \
	echo -e "Test slot ready. Boot it once with $(BOLD)make linux-test$(OFF) (or btbench-kernel try on the board)."

LINUX_TEST_TIMEOUT ?= 180

.PHONY: linux-test
linux-test: linux-deploy ## Deploy, boot the test kernel once, wait for the board and show what runs
	@set -e; \
	probe="ssh $(SSH_OPTS) -o ControlPath=none -o ConnectTimeout=3 -o BatchMode=yes $(TARGET)"; \
	before=$$($(SSH) $(TARGET) cat /proc/sys/kernel/random/boot_id); \
	echo -e "$(BOLD)Trial boot$(OFF) of the test kernel (once; any failure falls back to the good kernel)"; \
	$(SSH) $(TARGET) btbench-kernel try || true; \
	$(SSH) -O exit $(TARGET) >/dev/null 2>&1 || true; \
	printf "waiting for the board"; t0=$$(date +%s); \
	while :; do \
	  now=$$($$probe cat /proc/sys/kernel/random/boot_id 2>/dev/null || true); \
	  if [ -n "$$now" ] && [ "$$now" != "$$before" ]; then break; fi; \
	  if [ $$(( $$(date +%s) - t0 )) -ge $(LINUX_TEST_TIMEOUT) ]; then \
	    echo -e "\n\n$(BOLD)No answer from $(TARGET) after $(LINUX_TEST_TIMEOUT) s.$(OFF)"; \
	    echo "The test kernel may have hung or lost its network. The trial was a one-shot: the next"; \
	    echo "reset boots the good kernel. A panic reboots by itself (panic=10), a hang is reset by"; \
	    echo "the watchdog within ~16 s of it; otherwise power-cycle the board."; \
	    echo "To watch it: the serial console on the USB gadget (ttyACM0 on this PC, if the test"; \
	    echo "kernel got that far) or the UART on the GPIO header (pins 8/10, 115200 8N1)."; \
	    echo -e "Afterwards: $(BOLD)make linux-status$(OFF), and on the board journalctl -b -1, /sys/fs/pstore."; \
	    exit 1; \
	  fi; \
	  printf "."; sleep 3; \
	done; \
	echo ""; \
	status=$$($(SSH) $(TARGET) btbench-kernel status); \
	slot=$$(echo "$$status" | jq -r .slot); \
	echo -e "running $(BOLD)$$(echo "$$status" | jq -r .running)$(OFF), slot $(BOLD)$$slot$(OFF)"; \
	if [ "$$slot" = test ]; then \
	  echo -e "Keep it: $(BOLD)make linux-commit$(OFF).  Back to the good kernel: reboot the board."; \
	else \
	  echo "The board came back on the good kernel: the test kernel did not make it. On the board:"; \
	  echo "journalctl -b -1 (if it got as far as the journal), ls /sys/fs/pstore"; \
	  exit 1; \
	fi

.PHONY: linux-commit
linux-commit: ## Make the running test kernel the good one (the good one becomes prev)
	@$(SSH) $(TARGET) btbench-kernel commit

.PHONY: linux-rollback
linux-rollback: ## Swap the good and the previous kernel (takes effect at the next boot)
	@$(SSH) $(TARGET) btbench-kernel rollback

.PHONY: linux-status
linux-status: ## Kernel slots on the board: running, good, test, prev
	@$(SSH) $(TARGET) btbench-kernel status | jq .
