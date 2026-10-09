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

BLUEZ_URL       := https://git.kernel.org/pub/scm/bluetooth/bluez.git
PIPEWIRE_URL    := https://gitlab.freedesktop.org/pipewire/pipewire.git
WIREPLUMBER_URL := https://gitlab.freedesktop.org/pipewire/wireplumber.git

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

# $(call src_tree,<name>,<upstream checkout>,<src dir>,<url>,<pinned sha>)
#
# A git worktree of your own checkout, on a branch "bench" that starts at the revision the image
# was built from (versions.conf): the first deploy then replaces the board's build with the same
# code, and everything after that is your change. Worktrees share the checkout's objects and
# keep the cross build's artefacts out of it (a checkout configured in-tree breaks the
# out-of-tree build anyway). The pinned commit is fetched into the checkout if it lacks it
# (objects only: no branch or tag of yours moves; a server that refuses fetching a commit by
# its id is asked for all its branches and tags into a scratch namespace, removed right after). An existing "bench" branch is reused as it is.
# No checkout at all: a fresh clone. A src dir that already exists is used as it is, whatever
# made it. The generated compile_commands.json and .clangd are added to the checkout's
# info/exclude (shared by all its worktrees), so they never show up in git status.
# mk/kernel.mk can use this for src-linux as well.
define src_tree
set -e; name="$(1)"; up="$(2)"; src="$(3)"; url="$(4)"; sha="$(5)"; \
[ -n "$$sha" ] || { echo "No pinned revision for $$name in $(VERSIONS_CONF)."; exit 1; }; \
if [ -e "$$src" ]; then \
  echo -e "$(BOLD)$$src$(OFF) exists, used as it is $(DIM)($$(git -C "$$src" log -1 --format='%h %s' 2>/dev/null || echo 'not a git tree'))$(OFF)"; \
elif git -C "$$up" rev-parse --git-dir >/dev/null 2>&1; then \
  if ! git -C "$$up" cat-file -e "$$sha^{commit}" 2>/dev/null; then \
    echo "Fetching $$sha from $$url into $$up (objects only, no ref moves)"; \
    git -C "$$up" fetch -q --no-tags "$$url" "$$sha" 2>/dev/null || \
      { git -C "$$up" fetch -q --no-tags "$$url" '+refs/heads/*:refs/bench-fetch/h/*' '+refs/tags/*:refs/bench-fetch/t/*'; \
        git -C "$$up" for-each-ref --format='delete %(refname)' refs/bench-fetch | git -C "$$up" update-ref --stdin; }; \
    git -C "$$up" cat-file -e "$$sha^{commit}" 2>/dev/null || { echo "$$sha is not in $$url."; exit 1; }; \
  fi; \
  mkdir -p "$$(dirname "$$src")"; \
  if git -C "$$up" show-ref -q --verify refs/heads/bench; then \
    echo -e "$(BOLD)$$src$(OFF): worktree of $$up on its existing branch bench"; \
    git -C "$$up" worktree add -q "$$src" bench; \
  else \
    echo -e "$(BOLD)$$src$(OFF): worktree of $$up, new branch bench at $$(git -C "$$up" log -1 --format='%h %s' $$sha)"; \
    git -C "$$up" worktree add -q -b bench "$$src" "$$sha"; \
  fi; \
else \
  echo -e "No checkout at $$up: cloning $$url into $(BOLD)$$src$(OFF)"; \
  git clone -q "$$url" "$$src"; \
  git -C "$$src" cat-file -e "$$sha^{commit}" 2>/dev/null || git -C "$$src" fetch -q --no-tags origin "$$sha"; \
  git -C "$$src" checkout -q -b bench "$$sha"; \
fi; \
if gd=$$(git -C "$$src" rev-parse --path-format=absolute --git-common-dir 2>/dev/null); then \
  mkdir -p "$$gd/info"; \
  for p in compile_commands.json .clangd; do \
    grep -qxF "$$p" "$$gd/info/exclude" 2>/dev/null || echo "$$p" >> "$$gd/info/exclude"; \
  done; \
fi
endef

.PHONY: src src-bluez src-pipewire src-wireplumber
src: src-bluez src-pipewire src-wireplumber src-linux ## Create the dev trees in src/ (worktrees at the image's revisions)

src-bluez: ## src/bluez: worktree of BLUEZ_UPSTREAM, branch bench
	@$(call src_tree,bluez,$(BLUEZ_UPSTREAM),$(BLUEZ_SRC),$(BLUEZ_URL),$(call pinned,BLUEZ))

src-pipewire: ## src/pipewire: worktree of PIPEWIRE_UPSTREAM, branch bench
	@$(call src_tree,pipewire,$(PIPEWIRE_UPSTREAM),$(PIPEWIRE_SRC),$(PIPEWIRE_URL),$(call pinned,PIPEWIRE))

src-wireplumber: ## src/wireplumber: worktree of WIREPLUMBER_UPSTREAM, branch bench
	@$(call src_tree,wireplumber,$(WIREPLUMBER_UPSTREAM),$(WIREPLUMBER_SRC),$(WIREPLUMBER_URL),$(call pinned,WIREPLUMBER))

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
deploy_stage = set -e; \
	[ -d $(STAGE)/$(1) ] || { echo -e "Nothing staged for $(1).  $(BOLD)make $(1)$(OFF) first."; exit 1; }; \
	echo -e "$(BOLD)deploy $(1)$(OFF) $(DIM)$(STAGE:$(ROOT)/%=%)/$(1) → $(TARGET):/$(OFF)"; \
	$(DEPLOY_RSYNC) $(2) $(STAGE)/$(1)/ $(TARGET):/

# Run on the board after a deploy: unit files may have changed, libraries may have new names.
remote_reload = systemctl daemon-reload; command -v ldconfig >/dev/null && ldconfig; true
# PipeWire and WirePlumber only run in the pipewire audio mode; restarting them in another one
# would start them next to BlueALSA.
remote_if_pipewire = if [ "$$(btbench-audio get 2>/dev/null)" = pipewire ]; then $(1); \
	else echo "audio mode is not pipewire: $(2) not restarted"; fi

.PHONY: deploy-bluez deploy-pipewire deploy-wireplumber deploy-audio-scripts deploy
deploy-bluez: ## Push the BlueZ stage to the board, restart bluetooth (RESTART=0: don't)
	@$(call deploy_stage,bluez,'--exclude=/etc/bluetooth/*.conf')
	@$(SSH) $(TARGET) '$(remote_reload); \
	  $(if $(filter 0,$(RESTART)),true,systemctl restart bluetooth && echo "bluetooth restarted")'

deploy-pipewire: ## Push the PipeWire stage, restart pipewire + wireplumber if the audio mode is pipewire
	@$(call deploy_stage,pipewire)
	@$(SSH) $(TARGET) '$(remote_reload); \
	  $(if $(filter 0,$(RESTART)),true,$(call remote_if_pipewire,systemctl restart pipewire.service wireplumber.service && echo "pipewire + wireplumber restarted",pipewire))'

deploy-wireplumber: ## Push the WirePlumber stage, restart wireplumber if the audio mode is pipewire
	@$(call deploy_stage,wireplumber)
	@$(SSH) $(TARGET) '$(remote_reload); \
	  $(if $(filter 0,$(RESTART)),true,$(call remote_if_pipewire,systemctl restart wireplumber.service && echo "wireplumber restarted",wireplumber))'

deploy-audio-scripts: ## Push tools/target/btbench-audio and btbench-btsnoop to /usr/bin on the board
	@$(RSYNC) --chmod=F755 $(ROOT)/tools/target/btbench-audio $(ROOT)/tools/target/btbench-btsnoop $(TARGET):/usr/bin/
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
	  if tag=$$(git -C "$$d" describe --tags --exact-match --match '[0-9]*' --match 'v[0-9]*' 2>/dev/null); then \
	    pv=$${tag#v}; \
	  elif tag=$$(git -C "$$d" describe --tags --abbrev=0 --match '[0-9]*' --match 'v[0-9]*' 2>/dev/null); then \
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
