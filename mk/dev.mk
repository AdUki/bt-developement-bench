# Dev loop: the src/ trees, component builds with the SDK, deploys to the board, version bumps,
# remote gdb, clangd. Included by the Makefile after mk/yocto.mk (it uses $(BB), $(BUILD_CONF),
# check-sdk and check-devconf from there). docs/dev.md walks through it.
#
# The idea: BlueZ, PipeWire and WirePlumber are built outside bitbake, in build/<board>/<comp>,
# with the toolchain and sysroot of the image's SDK and with the configure options the image's
# recipe uses (tools/dev/bbvars reads them from bitbake once and caches them). The result is
# installed into build/<board>/stage/<comp> and rsynced over the board's root, so a deploy
# replaces exactly what the image's package installed, built the same way, from your tree.

VERSIONS_CONF ?= $(ROOT)/$(YOCTO)/conf/versions.conf


BLUEZ_B       := $(BUILD_DIR)/bluez
PIPEWIRE_B    := $(BUILD_DIR)/pipewire
WIREPLUMBER_B := $(BUILD_DIR)/wireplumber

# The revision the image is built from: $(call pinned,BLUEZ) → BTBENCH_BLUEZ_SRCREV.
pinned = $(shell sed -n 's/^BTBENCH_$(1)_SRCREV *?*= *"\(.*\)"/\1/p' $(VERSIONS_CONF))

# make bluez prefers bear (compile_commands.json for clangd); BEAR= builds without it.
BEAR ?= bear

RESTART ?= 1

## ─── dev ─────────────────────────────────────────────────────────────────────

# ─── src trees ───────────────────────────────────────────────────────────────

# $(call src_submodule,<name>,<src dir>)
#
# The dev trees are submodules of this repository (.gitmodules): the bench's forks on GitHub,
# whose branch bench is what the image is built from (docs/dev.md "The forks"). A tree not checked
# out yet is (at the commit the repository records, on a branch bench that pushes to the fork's);
# one that is is left alone, whatever it holds, so `make src` never moves your work. A submodule
# marked shallow in .gitmodules (src/linux) comes as one commit: --depth 1, which unlike git's own
# --recommend-shallow also holds for fetching the recorded commit when it is not a branch's tip. A src dir
# other than src/<name> (BLUEZ_SRC=~/projects/bluez-topic, say) is used as it is. The generated
# compile_commands.json and .clangd go to the tree's info/exclude, so they never show up in git
# status.
define src_submodule
set -e; name="$(1)"; src="$(2)"; \
if [ "$$src" != "$(ROOT)/src/$$name" ]; then \
  [ -d "$$src" ] || { echo "No tree at $$src ($(shell echo $(1) | tr a-z A-Z)_SRC)."; exit 1; }; \
  echo -e "$(BOLD)$$src$(OFF) used as it is"; \
elif [ -e "$$src/.git" ]; then \
  echo -e "$(BOLD)src/$$name$(OFF) is there, left as it is $(DIM)($$(git -C "$$src" log -1 --format='%h %s'))$(OFF)"; \
else \
  echo -e "$(BOLD)src/$$name$(OFF): checking out the submodule"; \
  depth=; [ "$$(git -C $(ROOT) config -f .gitmodules --bool "submodule.src/$$name.shallow" 2>/dev/null)" = true ] && depth="--depth 1"; \
  git -C $(ROOT) submodule update --init $$depth "src/$$name"; \
  git -C "$$src" checkout -q -B bench; \
  git -C "$$src" config branch.bench.remote origin; \
  git -C "$$src" config branch.bench.merge refs/heads/bench; \
  echo -e "$(BOLD)src/$$name$(OFF): $$(git -C "$$src" log -1 --format='%h %s')"; \
fi; \
if gd=$$(git -C "$$src" rev-parse --path-format=absolute --git-common-dir 2>/dev/null); then \
  mkdir -p "$$gd/info"; \
  for p in compile_commands.json .clangd; do \
    grep -qxF "$$p" "$$gd/info/exclude" 2>/dev/null || echo "$$p" >> "$$gd/info/exclude"; \
  done; \
fi
endef

.PHONY: src src-bluez src-pipewire src-wireplumber
src: src-bluez src-pipewire src-wireplumber src-linux ## Check out the dev trees in src/ (submodules: the bench's forks, branch bench)

src-bluez: ## src/bluez: the BlueZ submodule (the bench's fork, branch bench)
	@$(call src_submodule,bluez,$(BLUEZ_SRC))

src-pipewire: ## src/pipewire: the PipeWire submodule (the bench's fork, branch bench)
	@$(call src_submodule,pipewire,$(PIPEWIRE_SRC))

src-wireplumber: ## src/wireplumber: the WirePlumber submodule (the bench's fork, branch bench)
	@$(call src_submodule,wireplumber,$(WIREPLUMBER_SRC))

# ─── configure options from the image's recipes ──────────────────────────────

# build/<board>/<comp>/.bbopts: what bitbake passes to configure / meson setup for the image's
# package (see tools/dev/bbvars). Read again when the recipe, versions.conf, the device conf
# (codec switches) or local.conf change; `make bbopts` forces it. bitbake -e only parses, but the
# first parse of a build dir takes a minute.
BBOPTS_DEPS = $(VERSIONS_CONF) $(ROOT)/tools/dev/bbvars $(wildcard $(DEVCONF) $(YB)/conf/local.conf)
bbvars = @mkdir -p $(@D) && \
	echo -e "$(BOLD)bitbake -e $(1)$(OFF) $(DIM)— caching the image's configure options in $(@:$(ROOT)/%=%)$(OFF)" && \
	$(BB) $(ROOT)/tools/dev/bbvars $(1) --out $@

$(BLUEZ_B)/.bbopts: $(BBOPTS_DEPS) $(wildcard $(YOCTO)/meta-btbench/recipes-*/bluez5/*) | check-devconf $(BUILD_CONF)
	$(call bbvars,bluez5)

$(PIPEWIRE_B)/.bbopts: $(BBOPTS_DEPS) $(wildcard $(YOCTO)/meta-btbench/recipes-*/pipewire/*) | check-devconf $(BUILD_CONF)
	$(call bbvars,pipewire)

$(WIREPLUMBER_B)/.bbopts: $(BBOPTS_DEPS) $(wildcard $(YOCTO)/meta-btbench/recipes-*/wireplumber/*) | check-devconf $(BUILD_CONF)
	$(call bbvars,wireplumber)

.PHONY: bbopts
bbopts: ## Re-read the configure options of bluez5, pipewire, wireplumber from bitbake
	@rm -f $(BLUEZ_B)/.bbopts $(PIPEWIRE_B)/.bbopts $(WIREPLUMBER_B)/.bbopts
	@$(MAKE) --no-print-directory $(BLUEZ_B)/.bbopts $(PIPEWIRE_B)/.bbopts $(WIREPLUMBER_B)/.bbopts

# ─── component builds ────────────────────────────────────────────────────────

# BlueZ: autotools, out of tree. The SDK's environment script sets CC, CFLAGS, the sysroot and
# CONFIGURE_FLAGS (--host, --with-libtool-sysroot); the recipe's options come from .bbopts;
# --enable-debug on top. configure runs again when .bbopts or configure is newer than the last
# run. The uninstalled tools the image packages (btgatt-client, the testers, ...) are copied
# into the stage's /usr/bin, as the recipe does. bear records the compile commands; --append
# keeps the files an incremental build did not touch. BlueZ's own make must not inherit this
# make's MAKEFLAGS: they carry the command line's variables (TARGET=..., BOARD=...), which would
# override BlueZ's Makefile variables of the same name.
.PHONY: bluez
bluez: check-sdk $(BLUEZ_B)/.bbopts ## Build src/bluez with the SDK and the image's options, install into the stage
	@unset MAKEFLAGS MFLAGS MAKELEVEL; . $(SDK_ENV) >/dev/null; set -e; src="$(BLUEZ_SRC)"; b="$(BLUEZ_B)"; stage="$(STAGE)/bluez"; \
	[ -d "$$src" ] || { echo -e "No BlueZ tree at $$src.  $(BOLD)make src-bluez$(OFF)"; exit 1; }; \
	if [ -f "$$src/config.status" ]; then \
	  echo -e "$(BOLD)$$src is configured in-tree$(OFF) (it has a config.status), which breaks an"; \
	  echo    "out-of-tree build of it. Run 'make distclean' there, or point BLUEZ_SRC at a worktree."; \
	  exit 1; fi; \
	. "$$b/.bbopts"; \
	if [ ! -x "$$src/configure" ]; then echo "bootstrap"; (cd "$$src" && ./bootstrap); fi; \
	cd "$$b"; \
	if [ ! -f config.status ] || [ .bbopts -nt config.status ] || [ "$$src/configure" -nt config.status ]; then \
	  echo -e "$(BOLD)configure$(OFF) $(DIM)$$BB_PN $$BB_PV options + --enable-debug$(OFF)"; \
	  "$$src/configure" -q $$CONFIGURE_FLAGS --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
	    --libexecdir=/usr/libexec "$${BB_CONFARGS[@]}" --enable-debug; \
	fi; \
	if [ -n "$(BEAR)" ] && command -v $(BEAR) >/dev/null; then \
	  app=""; [ -f compile_commands.json ] && app=--append; \
	  $(BEAR) $$app --output compile_commands.json -- make -j$$(nproc); \
	else \
	  [ -z "$(BEAR)" ] || echo "bear not found (make host-deps): no compile_commands.json"; \
	  make -j$$(nproc); \
	fi; \
	rm -rf "$$stage"; \
	make -s install DESTDIR="$$stage" >/dev/null; \
	install -d "$$stage/usr/bin"; \
	for t in "$${BB_NOINST_TOOLS[@]}"; do \
	  if [ -f "$$t" ]; then install -m 0755 "$$t" "$$stage/usr/bin/"; \
	  else echo "warning: $$t (packaged by the recipe) was not built"; fi; \
	done; \
	[ ! -f compile_commands.json ] || ln -sfn "$$b/compile_commands.json" "$$src/compile_commands.json"; \
	stray=$$(find "$$stage" -path '*sysroots*' -print -quit); \
	[ -z "$$stray" ] || echo -e "$(BOLD)warning:$(OFF) an SDK path leaked into the install: $$stray"; \
	echo -e "$(BOLD)bluez$(OFF) staged in $$stage  $(DIM)→ make deploy-bluez$(OFF)"

# $(call meson_build,<comp>,<src dir>,<build dir>)
#
# PipeWire and WirePlumber: meson, out of tree. `meson setup` must be spelled out: the SDK's
# meson is a wrapper that adds the SDK's cross and native files only for the setup command
# (and unsets CC/CXX, which the cross file replaces). buildtype debugoptimized instead of the
# recipe's plain: -O2 -g, asserts on. meson writes compile_commands.json itself.
define meson_build
. $(SDK_ENV) >/dev/null; set -e; src="$(2)"; b="$(3)"; stage="$(STAGE)/$(1)"; \
[ -d "$$src" ] || { echo -e "No $(1) tree at $$src.  $(BOLD)make src-$(1)$(OFF)"; exit 1; }; \
. "$$b/.bbopts"; \
opts=(--prefix=/usr --libdir=/usr/lib -Dbuildtype=debugoptimized "$${BB_CONFARGS[@]}"); \
if [ ! -f "$$b/build.ninja" ]; then \
  echo -e "$(BOLD)meson setup$(OFF) $(DIM)$$BB_PN $$BB_PV options, debugoptimized$(OFF)"; \
  meson setup "$$b" "$$src" "$${opts[@]}"; \
elif [ "$$b/.bbopts" -nt "$$b/build.ninja" ]; then \
  echo -e "$(BOLD)meson setup --reconfigure$(OFF) $(DIM)the recipe's options changed$(OFF)"; \
  meson setup --reconfigure "$$b" "$$src" "$${opts[@]}"; \
fi; \
meson compile -C "$$b"; \
rm -rf "$$stage"; \
meson install -C "$$b" --no-rebuild --destdir "$$stage" >/dev/null; \
ln -sfn "$$b/compile_commands.json" "$$src/compile_commands.json"; \
echo -e "$(BOLD)$(1)$(OFF) staged in $$stage  $(DIM)→ make deploy-$(1)$(OFF)"
endef

.PHONY: pipewire wireplumber
pipewire: check-sdk $(PIPEWIRE_B)/.bbopts ## Build src/pipewire with the SDK and the image's options, install into the stage
	@$(call meson_build,pipewire,$(PIPEWIRE_SRC),$(PIPEWIRE_B))

wireplumber: check-sdk $(WIREPLUMBER_B)/.bbopts ## Build src/wireplumber the same way
	@$(call meson_build,wireplumber,$(WIREPLUMBER_SRC),$(WIREPLUMBER_B))

# ─── deploy ──────────────────────────────────────────────────────────────────

# A deploy rsyncs a component's whole stage over the board's / (not single binaries: a library
# and the plugins built against it go together), then restarts what runs it (RESTART=0: no
# restart). Nothing on the board is deleted. Left out:
#   - what only a build needs: headers, pkg-config files, libtool archives, static libs, docs;
#   - /var: the stage has no state worth pushing, and the board's may be a link into /data;
#   - per component, the config the image tunes (BlueZ: /etc/bluetooth/*.conf).
# --keep-dirlinks: a directory that is a symlink on the board (/lib → usr/lib, a state dir moved
# to /data) stays a symlink; plain rsync would replace it with a real directory.
DEPLOY_RSYNC = $(RSYNC) --keep-dirlinks --exclude=/usr/include/ --exclude=pkgconfig/ \
               --exclude='*.la' --exclude='*.a' --exclude=/usr/share/man/ \
               --exclude=/usr/share/doc/ --exclude=/var/

# $(call deploy_stage,<comp>,<extra rsync options>)
# The root stays open (board_rw) until the shell this starts ends: what follows it on the same
# recipe line (ldconfig writes /etc/ld.so.cache) is inside.
deploy_stage = set -e; \
	[ -d $(STAGE)/$(1) ] || { echo -e "Nothing staged for $(1).  $(BOLD)make $(1)$(OFF) first."; exit 1; }; \
	echo -e "$(BOLD)deploy $(1)$(OFF) $(DIM)$(STAGE:$(ROOT)/%=%)/$(1) → $(TARGET):/$(OFF)"; \
	$(call board_rw,/) \
	$(DEPLOY_RSYNC) $(2) $(STAGE)/$(1)/ $(TARGET):/

# Run on the board after a deploy: unit files may have changed, libraries may have new names.
remote_reload = systemctl daemon-reload; command -v ldconfig >/dev/null && ldconfig; true
# PipeWire is the audio stack, always: a deploy restarts what it replaced.
remote_if_pipewire = $(1)

.PHONY: deploy-bluez deploy-pipewire deploy-wireplumber deploy-audio-scripts deploy
deploy-bluez: ## Push the BlueZ stage to the board, restart bluetooth (RESTART=0: don't)
	@$(call deploy_stage,bluez,'--exclude=/etc/bluetooth/*.conf'); \
	$(SSH) $(TARGET) '$(remote_reload); \
	  $(if $(filter 0,$(RESTART)),true,systemctl restart bluetooth && echo "bluetooth restarted")'

deploy-pipewire: ## Push the PipeWire stage, restart pipewire + wireplumber
	@$(call deploy_stage,pipewire); \
	$(SSH) $(TARGET) '$(remote_reload); \
	  $(if $(filter 0,$(RESTART)),true,$(call remote_if_pipewire,systemctl restart pipewire.service wireplumber.service && echo "pipewire + wireplumber restarted",pipewire))'

deploy-wireplumber: ## Push the WirePlumber stage, restart wireplumber
	@$(call deploy_stage,wireplumber); \
	$(SSH) $(TARGET) '$(remote_reload); \
	  $(if $(filter 0,$(RESTART)),true,$(call remote_if_pipewire,systemctl restart wireplumber.service && echo "wireplumber restarted",wireplumber))'

deploy-audio-scripts: ## Push tools/target/btbench-audio and btbench-btsnoop to /usr/bin on the board
	@set -e; $(call board_rw,/) \
	$(RSYNC) --chmod=F755 $(ROOT)/tools/target/btbench-audio $(ROOT)/tools/target/btbench-btsnoop $(TARGET):/usr/bin/
	@$(SSH) $(TARGET) '$(if $(filter 0,$(RESTART)),true,systemctl try-restart btbench-btsnoop.service; true)'
	@echo -e "$(BOLD)btbench-audio, btbench-btsnoop$(OFF) deployed"

# Every component with a stage. The other areas add theirs in their own mk file:
#   DEPLOY_TARGETS += $(if $(wildcard <their stage>),deploy-<them>)
# Those files are included after this one, so `deploy` must not name DEPLOY_TARGETS in its
# prerequisites (expanded the moment this line is read, before they appended); the recipe is
# expanded only when it runs, after every mk file was read, and runs them in a sub-make.
DEPLOY_TARGETS += $(if $(wildcard $(STAGE)/bluez),deploy-bluez)
DEPLOY_TARGETS += $(if $(wildcard $(STAGE)/pipewire),deploy-pipewire)
DEPLOY_TARGETS += $(if $(wildcard $(STAGE)/wireplumber),deploy-wireplumber)

deploy: ## Deploy every component built so far (DEPLOY_TARGETS)
	@[ -n "$(strip $(DEPLOY_TARGETS))" ] || { echo "Nothing built yet: make bluez (pipewire, wireplumber, ...) first."; exit 1; }
	@$(MAKE) --no-print-directory $(DEPLOY_TARGETS)

# ─── versions ────────────────────────────────────────────────────────────────

# Pin the image to what the dev trees have now: each existing src tree's HEAD becomes its
# BTBENCH_<X>_SRCREV, and PV is taken from the nearest release tag (exactly on a tag: that
# version; past it: <tag>+git, which bitbake turns into <tag>+git<n>+<sha> for the package).
# Only version-like tags count (5.87, v7.3-rc2, 1.6.9), not a local tag of yours. The recipes
# fetch with nobranch=1, so any commit of the upstream repository works, but a commit that only
# exists here cannot be fetched by bitbake: push it first, or build with SRC=local. A tree whose
# HEAD is still the pinned revision is left alone (its PV may be better than the local tags say).
# PV needs the release tags: in a checkout that lacks them, `git fetch --tags <upstream>` first.
.PHONY: bump-versions
bump-versions: ## Pin versions.conf to the HEADs of src/{bluez,pipewire,wireplumber,linux}
	@set -e; f="$(VERSIONS_CONF)"; changed=0; \
	for c in BLUEZ:$(BLUEZ_SRC) PIPEWIRE:$(PIPEWIRE_SRC) WIREPLUMBER:$(WIREPLUMBER_SRC) LINUX:$(LINUX_SRC); do \
	  n=$${c%%:*}; d=$${c#*:}; \
	  if ! git -C "$$d" rev-parse --git-dir >/dev/null 2>&1; then echo -e "$(DIM)$$n: no tree at $$d, left as is$(OFF)"; continue; fi; \
	  rev=$$(git -C "$$d" rev-parse HEAD); \
	  local_n=$$(git -C "$$d" rev-list --count HEAD --not --remotes 2>/dev/null || echo 0); \
	  if [ "$$local_n" -gt 0 ] && [ "$$local_n" -lt "$$(git -C "$$d" rev-list --count HEAD)" ]; then \
	    rev=$$(git -C "$$d" rev-parse "$$(git -C "$$d" rev-list --reverse HEAD --not --remotes | head -n 1)^"); \
	    echo -e "$(DIM)$$n: $$local_n commit(s) not pushed anywhere, pinning the one below them: bitbake can"; \
	    echo -e "  fetch only what a remote has (push bench to the fork, docs/dev.md \"The forks\")$(OFF)"; \
	  fi; \
	  if tag=$$(git -C "$$d" describe --tags --exact-match --match '[0-9]*' --match 'v[0-9]*' "$$rev" 2>/dev/null); then \
	    pv=$${tag#v}; \
	  elif tag=$$(git -C "$$d" describe --tags --abbrev=0 --match '[0-9]*' --match 'v[0-9]*' "$$rev" 2>/dev/null); then \
	    pv="$${tag#v}+git"; \
	  else \
	    echo "$$n: no release tag below HEAD in $$d (shallow clone?); fetch the tags first."; exit 1; \
	  fi; \
	  old_rev=$$(sed -n "s/^BTBENCH_$${n}_SRCREV *?*= *\"\(.*\)\"/\1/p" "$$f"); \
	  old_pv=$$(sed -n "s/^BTBENCH_$${n}_PV *?*= *\"\(.*\)\"/\1/p" "$$f"); \
	  [ -n "$$old_rev" ] || { echo "$$n: no BTBENCH_$${n}_SRCREV line in $$f"; exit 1; }; \
	  if [ "$$old_rev" = "$$rev" ]; then echo -e "$(DIM)$$n: unchanged ($$old_pv $${rev:0:12})$(OFF)"; continue; fi; \
	  sed -i -e "s|^\(BTBENCH_$${n}_SRCREV *?*= *\)\".*\"|\1\"$$rev\"|" \
	         -e "s|^\(BTBENCH_$${n}_PV *?*= *\)\".*\"|\1\"$$pv\"|" "$$f"; \
	  echo -e "$(BOLD)$$n$(OFF): $$old_pv $${old_rev:0:12} → $(BOLD)$$pv $${rev:0:12}$(OFF)"; changed=1; \
	  if [ -z "$$(git -C "$$d" branch -r --contains "$$rev" 2>/dev/null)" ]; then \
	    echo -e "  $(BOLD)WARNING:$(OFF) $${rev:0:12} is on no remote-tracking branch of $$d. bitbake fetches"; \
	    echo    "  from upstream and cannot find a commit that exists only here: push it, or use SRC=local."; \
	  fi; \
	done; \
	[ $$changed = 0 ] || echo -e "\n$(DIM)$(VERSIONS_CONF:$(ROOT)/%=%) updated. Next: make image (and make sdk).$(OFF)"

# ─── debugging ───────────────────────────────────────────────────────────────

PROC     ?= bluetoothd
GDB_PORT ?= 2345
# The tunnel gets its own connection: a forward made through the shared master connection
# would outlive the session and hold the port for the next one.
GDB_SSH  := ssh -o ControlMaster=no -o ControlPath=none -o StrictHostKeyChecking=no \
            -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ExitOnForwardFailure=yes

# gdbserver attaches to the running process on the board; the SDK's cross gdb connects through
# an ssh tunnel. gdb reads the target's libraries locally, from a sysroot made of the SDK's
# (what the image installed) overlaid with the stages (what you deployed since), as links, so a
# deployed libpipewire or SPA plugin is matched with its own symbols and not the image's. (A
# link such as /lib → usr/lib is copied as a link, so it leads into the overlay too.)
# Symbols for the image's own libraries need an SDK with debug info (dbg-pkgs).
.PHONY: gdb
gdb: check-sdk ## Debug a process on the board with the SDK's gdb (PROC=bluetoothd, GDB_PORT=2345)
	@. $(SDK_ENV) >/dev/null; set -e; \
	pid=$$($(SSH) $(TARGET) pidof -s $(PROC)) || { echo "$(PROC) is not running on $(TARGET)."; exit 1; }; \
	exe=$$($(SSH) $(TARGET) readlink /proc/$$pid/exe); \
	root=$(BUILD_DIR)/gdb-sysroot; rm -rf "$$root"; mkdir -p "$$root/usr"; \
	for d in lib usr/lib usr/libexec usr/bin usr/sbin; do \
	  if [ -L "$$SDKTARGETSYSROOT/$$d" ]; then cp -P "$$SDKTARGETSYSROOT/$$d" "$$root/$$d"; \
	  elif [ -d "$$SDKTARGETSYSROOT/$$d" ]; then cp -rs "$$SDKTARGETSYSROOT/$$d" "$$root/$$d"; fi; \
	done; \
	for s in $(wildcard $(STAGE)/*); do cp -rsf "$$s/." "$$root/"; done; \
	bin="$$root$$exe"; [ -e "$$bin" ] || { echo "warning: $$exe is in neither the SDK nor a stage"; bin=""; }; \
	echo -e "$(BOLD)gdbserver$(OFF) on $(TARGET): $(PROC) pid $$pid $(DIM)($$exe), tunnel :$(GDB_PORT)$(OFF)"; \
	$(GDB_SSH) -L $(GDB_PORT):127.0.0.1:$(GDB_PORT) $(TARGET) \
	  "gdbserver --attach 127.0.0.1:$(GDB_PORT) $$pid" & tunnel=$$!; \
	trap 'kill $$tunnel 2>/dev/null' EXIT; sleep 2; \
	$$GDB -q -ex "set sysroot $$root" -ex "set debug-file-directory $$root/usr/lib/debug" \
	  -ex "target remote 127.0.0.1:$(GDB_PORT)" $$bin

# ─── clangd ──────────────────────────────────────────────────────────────────

# <src dir>:<build dir> pairs that get a .clangd. mk/kernel.mk adds the kernel tree.
CLANGD_TREES += $(BLUEZ_SRC):$(BLUEZ_B) $(PIPEWIRE_SRC):$(PIPEWIRE_B) $(WIREPLUMBER_SRC):$(WIREPLUMBER_B)

.PHONY: clangd
clangd: ## Write .clangd into each src tree (strips GCC-only flags) and print clangd's --query-driver
	@for t in $(CLANGD_TREES); do \
	  s=$${t%%:*}; b=$${t#*:}; [ -d "$$s" ] || continue; \
	  $(ROOT)/tools/dev/clangd-gen "$$s" $$([ -d "$$b" ] && echo "$$b") --sdk $(SDK_DIR) | grep -v '^clangd needs'; \
	done
	@echo -e "Start clangd with:  $(BOLD)--query-driver=$(SDK_DIR)/sysroots/*/usr/bin/*/*-gcc,$(SDK_DIR)/sysroots/*/usr/bin/*/*-g++$(OFF)"

# ---- Logs (btbench-logd: /tmp/logs on the board, one native-format file per program) -------------
# The files are named for clog's groups (app/clog/clog.conf): bluez, pipewire, wireplumber,
# wpasupplicant; btbenchd, kernel and system are in the nsdk format of its default group.
LOG ?= bluez

.PHONY: logs logs-pull
logs: ## Follow a board log in clog (LOG=bluez|pipewire|wireplumber|btbenchd|kernel|system|wpasupplicant)
	@$(SSH) -t $(TARGET) clog -t /tmp/logs/$(LOG).log

logs-pull: ## Copy the board's /tmp/logs to build/logs/<time>/ (and build/logs/latest) for clog on the PC
	@set -e; dest=$(ROOT)/build/logs/$$(date +%Y%m%d-%H%M%S); mkdir -p $$dest; \
	$(SSH) $(TARGET) 'cd /tmp/logs && tar cf - $$(ls *.log *.log.1 2>/dev/null)' | tar xf - -C $$dest; \
	ln -sfn $$dest $(ROOT)/build/logs/latest; \
	echo -e "$(BOLD)Logs$(OFF) in $$dest $(DIM)(build/logs/latest; read them with app/build/clog/clog build/logs/latest/bluez.log)$(OFF)"
