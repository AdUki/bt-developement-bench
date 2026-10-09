# The dev loop

Edit BlueZ, PipeWire or WirePlumber on this PC, cross-build them with the image's SDK, push them
to the board, restart, test. A BlueZ change is on the board in well under a minute. Nothing is
built by bitbake in the loop. The image is only rebuilt when you want a new baseline.

```
make sdk                 once per image: toolchain + sysroot of what the board runs
make src                 once: src/bluez, src/pipewire, src/wireplumber, src/linux
make bluez               edit, build ...
make deploy-bluez        ... push, restart bluetoothd, test, repeat
make gdb                 attach to bluetoothd on the board
make clangd              editor support for the cross build
make bump-versions       make the next image carry what you have been deploying
```

The targets live in `mk/dev.mk`. `make` with no target lists them all.

## Dev trees: `make src`

`make src-bluez` (`-pipewire`, `-wireplumber`; `make src` does all of them plus `src-linux`)
makes `src/<comp>` a **git worktree** of your own checkout (`BLUEZ_UPSTREAM`, by default
`~/projects/bluez`). The worktree is on a branch `bench`, which starts at the revision the image
was built from (`BTBENCH_BLUEZ_SRCREV` in `yocto/conf/versions.conf`).

- **Why a worktree.** It shares your checkout's objects, so your branches and remotes are all
  there. It keeps the cross build's files out of your checkout. A tree that is configured
  in-tree (it has `config.status`, as `~/projects/bluez` usually has) breaks the out-of-tree
  build, so `make bluez` refuses one.
- **Why the pinned revision.** The first `make deploy-bluez` then replaces the board's BlueZ
  with the same code built your way, and every difference after that is your change.
  Cherry-pick or rebase onto `bench` to bring your work in.
- **Missing commit.** If your checkout does not have the pinned commit, it is fetched from
  upstream. This fetches objects only: no branch or tag of yours moves.
- **Existing `bench` branch.** It is reused as it is.
- **No checkout at all.** Upstream is cloned into `src/<comp>` instead.
- **Existing `src/<comp>`.** It is used as it is, whatever made it. To use a tree somewhere
  else, set `BLUEZ_SRC=~/projects/bluez-topic` (in `bench.conf` or on the command line).
- **Excludes.** `compile_commands.json` and `.clangd` are added to the checkout's
  `.git/info/exclude`, so they never show up in `git status`.

Only `make src*` touches your checkouts: it adds a worktree, the `bench` branch, the fetched
objects and the two exclude lines.

## Building: `make bluez`, `make pipewire`, `make wireplumber`

All three compile with the **SDK** (`make sdk` installs it into `sdk/<board>`). Its sysroot holds
exactly the libraries the image has. `check-sdk` warns when the image has changed since the
SDK was made.

**Configure options come from the image's recipe.** The first build runs
`tools/dev/bbvars <recipe>`, which reads them with `bitbake -e` (a parse, about a minute the
first time). It caches them in `build/<board>/<comp>/.bbopts`:

| variable | what |
|---|---|
| `BB_CONFARGS` | `EXTRA_OECONF` (autotools) or `EXTRA_OEMESON` (meson). Both already include the PACKAGECONFIG options. |
| `BB_NOINST_TOOLS` | the BlueZ tools that `make install` skips but the image packages: btgatt-client, l2cap-tester, btvirt, ... |
| `BB_PN`, `BB_PV`, `BB_KIND` | for the messages |

- **When the cache is refreshed.** When the recipe, `versions.conf`, `btbench-device.conf` (the
  codec switches) or the build's `local.conf` change. `make bbopts` forces it.
- **What bbvars drops.** Options that name a path inside the bitbake build (`TMPDIR`,
  `WORKDIR`, the recipe sysroot). It warns when it drops one.
- **Which build dir.** It runs in the board's build dir (`yocto/build-<board>`). Elsewhere:
  `tools/dev/bbvars bluez5 --builddir DIR`.

**BlueZ.** A standard autotools build, out of tree:

- **bootstrap.** `./bootstrap` runs in `src/bluez` if there is no `configure` yet.
- **configure.** It runs in `build/<board>/bluez` with the SDK's `CONFIGURE_FLAGS`, plus
  `--prefix=/usr --sysconfdir=/etc --localstatedir=/var --libexecdir=/usr/libexec`, the recipe's
  options and `--enable-debug`. It runs again only when `.bbopts` or `configure` is newer than
  the last run.
- **Compile commands.** `bear` records them while make runs; `--append` keeps them across
  incremental builds. `BEAR=` builds without it.
- **Install.** `make install` goes into `build/<board>/stage/bluez` (emptied first). The
  `BB_NOINST_TOOLS` are copied into its `/usr/bin`.

**PipeWire and WirePlumber.** meson, out of tree, in `build/<board>/<comp>`:

- **setup.** `meson setup --prefix=/usr --libdir=/usr/lib -Dbuildtype=debugoptimized` plus the
  recipe's options (`-Dauto_features=disabled` and the explicit list). It reconfigures when
  `.bbopts` changes. The SDK's `meson` is a wrapper that adds the cross and native files to the
  `setup` command.
- **Compile and install.** `meson compile`, then `meson install --destdir
  build/<board>/stage/<comp>`.

`compile_commands.json` is symlinked into each src tree.

## Deploying: `make deploy-<comp>`, `make deploy`

A deploy rsyncs the component's **whole stage** over the board's `/`. It never pushes single
binaries: a library and the plugins built against it travel together.

- **Nothing is deleted** on the board.
- **Left out:** headers, pkg-config files, `.la`/`.a`, man pages, docs and `/var`. For BlueZ,
  `/etc/bluetooth/*.conf` is left out too: the image ships a tuned `main.conf`.
- **Symlinked directories stay symlinks** (`--keep-dirlinks`). This covers `/lib → usr/lib` and
  state directories that live on `/data`.
- **After the copy:** `systemctl daemon-reload` and `ldconfig`, then the restart:

  | target | restarts |
  |---|---|
  | `deploy-bluez` | `bluetooth` |
  | `deploy-pipewire` | `pipewire` + `wireplumber`, only when `btbench-audio get` says `pipewire` (in the other audio modes they are meant to be off) |
  | `deploy-wireplumber` | `wireplumber`, under the same condition |
  | `deploy-audio-scripts` | pushes `tools/target/btbench-audio` and `btbench-btsnoop` |

  `RESTART=0` skips the restart.
- **`make deploy`** runs every `deploy-*` that has something staged. The kernel and btbenchd
  areas add theirs to `DEPLOY_TARGETS`.
- **The board** is `TARGET` (default `root@10.55.0.1`, the USB link). One ssh master connection
  is shared for two minutes, so a deploy pays for one handshake.

The deployed tree now differs from the image's package. `opkg` does not know, and a reflash
puts the image's version back.

## Debugging: `make gdb PROC=bluetoothd`

1. On the board, `gdbserver --attach` attaches to the running process (`pidof`) on
   `127.0.0.1:GDB_PORT` (2345). An ssh tunnel of its own brings that port to this PC.
2. The SDK's cross gdb connects to it. Its sysroot is `build/<board>/gdb-sysroot`: the SDK's
   target sysroot overlaid with every stage, as links. A library you deployed is therefore read
   from your build, and one you did not is read from the SDK.
3. The binary comes from the same place.

- **Symbols.** Your builds carry `-g`. The image's own libraries have symbols only if the SDK
  was built with debug packages (`SDKIMAGE_FEATURES += "dbg-pkgs"`; that decision is made for
  the image, not here).
- **gdbserver** must be on the board (packagegroup-btbench-tools).
- **Ctrl-C** interrupts the inferior as usual. Quitting gdb detaches, and the process keeps
  running.

## Editor support: `make clangd`

- **compile_commands.json.** Every build symlinks it into its src tree. It holds the SDK's GCC
  command lines.
- **`.clangd`.** `make clangd` writes one into each src tree (`tools/dev/clangd-gen`). It removes
  the GCC-only flags that clang rejects (`-mthumb-interwork`, `-fcanon-prefix-map`,
  `-mtls-dialect=`, `-fzero-call-used-regs=`, the prefix maps) and silences unknown-warning
  noise. The ARM tune flags stay.
- **Query driver.** clangd must also be allowed to ask the cross gcc for its system headers.
  That is a command-line flag of clangd; add the one `make clangd` prints to your editor's
  clangd arguments once:

  ```
  --query-driver=<repo>/sdk/<board>/sysroots/*/usr/bin/*/*-gcc,<repo>/sdk/<board>/sysroots/*/usr/bin/*/*-g++
  ```

The kernel area adds `src/linux` to `CLANGD_TREES`.

## Versions and the ABI policy

- **The image and the SDK** are built from the revisions pinned in `yocto/conf/versions.conf`
  (`BTBENCH_<X>_SRCREV`, `_PV`).
- **The dev trees start there** (see `make src`), so what you deploy is binary-compatible with
  what is already on the board. Your change is the only difference.
- **Bump the image deliberately.** When your trees have moved, `make bump-versions` writes each
  existing tree's HEAD into `versions.conf`. That covers `src/bluez`, `src/pipewire`,
  `src/wireplumber` and `src/linux`.
  - **PV** is the release tag when HEAD is on one (`1.6.9`), otherwise `<last tag>+git`
    (`5.87+git`, `7.3-rc2+git`).
  - **Tags.** Only version-like tags count. A checkout without the release tags needs a
    `git fetch --tags` first.
  - **Unmoved trees.** A tree still at its pinned revision is left as it is.
  - **Next step.** `make image` and `make sdk` then carry the new baseline.
  - **The commit must be fetchable.** The recipes fetch from upstream (`nobranch=1`, so any
    upstream commit works). A commit that only exists in your tree cannot be fetched;
    `bump-versions` warns when HEAD is on no remote branch. Push it, or use `SRC=local`.
- **`make image SRC=local`** builds the image's BlueZ, PipeWire, WirePlumber and kernel directly
  from `src/` (externalsrc), unpinned. Use it to bake your current trees into an image without
  pushing anything.
- **Rebuild the SDK** when you change a component's ABI (a new library version, a changed public
  header) and another component builds against it. Otherwise the other one compiles against the
  SDK's old copy. For BlueZ, PipeWire and WirePlumber this mostly matters between PipeWire and
  WirePlumber. PipeWire's BlueZ plugin and BlueALSA talk to bluetoothd over D-Bus and link
  BlueZ's libbluetooth only for the socket helpers, whose ABI has not moved in years.
- **Mixed revisions.** A deploy on top of an image of different revisions works as long as the
  libraries they share agree. When in doubt, `make bump-versions image` and reflash.
