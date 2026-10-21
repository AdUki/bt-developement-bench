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

The four components the bench develops are in the repository, as submodules in `src/`:

| tree | the bench's fork | its upstream |
|---|---|---|
| `src/bluez` | `github.com/AdUki/bluez` | bluez.git |
| `src/pipewire` | `github.com/AdUki/pipewire` | gitlab.freedesktop.org pipewire |
| `src/wireplumber` | `github.com/AdUki/wireplumber` | gitlab.freedesktop.org wireplumber |
| `src/linux` | `github.com/AdUki/linux` (a GitHub fork of torvalds/linux) | bluetooth-next |

Each is on its fork's branch `bench`, at the commit the image is built from: the code you edit,
build (`make bluez`, `make pipewire`, `make wireplumber`, `make linux`) and deploy is the code
the image has, and a change is a commit, not a patch in the Yocto layer.

`make src` (or `make src-bluez`, `-pipewire`, `-wireplumber`, `-linux`) checks out the ones a
clone does not have yet, at the recorded commit, on a branch `bench` that pushes to the fork's
(`git submodule update --init`, with `--depth 1` for the kernel, does the same, but leaves
them on a detached HEAD; without `--depth 1`, git fetches the kernel's whole history). A tree that is
already there is left alone, whatever it holds. `src/linux` is a shallow submodule: the clone
gets the one commit it needs, not the kernel's history (`git -C src/linux fetch --unshallow` when
you want it). To build a tree somewhere else, set `BLUEZ_SRC=~/projects/bluez-topic` (in
`bench.conf` or on the command line). `compile_commands.json` and `.clangd` go to the tree's
`info/exclude`, so they never show up in `git status`.

A tree configured in-tree (it has a `config.status`) breaks the out-of-tree build, so `make bluez`
refuses one.

### The forks

Every fork has upstream's `master` (bluetooth-next's for the kernel, under the upstream remote)
and a branch `bench`: the image's pin and the bench's commits on top (BlueZ: AVRCP browsing in the
target role and its fixes; the kernel: the dwc2 gadget fix and Bluetooth fixes on their way
upstream, see [upstream.md](upstream.md)). The recipes fetch from the forks
(`BTBENCH_<COMP>_GIT` in `yocto/conf/versions.conf`) at `BTBENCH_<COMP>_SRCREV`, which is the
submodule's commit: the same commit recorded twice, by git for the tree and by bitbake for the
image.

In a tree, `origin` is the fork; add the upstream for rebasing, e.g.
`git -C src/bluez remote add upstream https://git.kernel.org/pub/scm/bluetooth/bluez.git` (or
`https://git.kernel.org/pub/scm/linux/kernel/git/bluetooth/bluetooth-next.git` for the kernel,
`https://gitlab.freedesktop.org/pipewire/{pipewire,wireplumber}.git`).

The kernel's own changes are kept per SoC: a branch `soc/<soc>` (`soc/bcm2835`: the PL011 fixes
the Zero W's Bluetooth UART needs) holds one SoC's commits on the pin, and `bench` is the pin with
each of them merged in (`git merge --no-ff soc/<soc>`). A board's work that is not ready to share
stays on a local `soc/` branch that is neither merged nor pushed (`soc/imx8mm`, the s1955).

A change goes in as a commit on `bench` (the kernel: on its `soc/` branch, merged into `bench`),
pushed to the fork, then pinned:

```bash
git -C src/bluez push origin bench
make bump-versions                     # BTBENCH_*_SRCREV = each tree's HEAD
git add src/bluez yocto/conf/versions.conf && git commit -s -m "BlueZ: ..."
```

`make bump-versions` pins only a commit some remote has (it leaves out commits not pushed yet,
and says so), since bitbake fetches from the fork. To follow upstream, rebase `bench` onto the
upstream's new revision and push it (`--force-with-lease`). The kernel's `bench` sits on
bluetooth-next, which the fork does not track by itself: push the base along with it (it is in
`bench`'s history, so pushing `bench` does).

A fix that should go upstream leaves `bench` the other way: a branch cut from upstream, with
the commits picked over, checked against the project's rules and sent from there.
`make upstream-start` and the other `upstream-*` targets do that, see [upstream.md](upstream.md).

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

The board's root is read-only (docs/system.md "The SD card"). Every deploy target, `make pkg`
and `make linux-deploy` first open it with `btbench-rw` on the board, and close it when the
recipe ends, failed or not. A deploy that replaces a file a running process still uses (one that
is not restarted, such as `btbench-wifi` after `make deploy-scripts`) leaves the root writable and
says which unit to restart; then `btbench-rw reset`, or reboot.

- **Nothing is deleted** on the board.
- **Left out:** headers, pkg-config files, `.la`/`.a`, man pages, docs and `/var`. For BlueZ,
  `/etc/bluetooth/*.conf` is left out too: the image ships a tuned `main.conf`.
- **Symlinked directories stay symlinks** (`--keep-dirlinks`). This covers `/lib → usr/lib` and
  state directories that live on `/data`.
- **After the copy:** `systemctl daemon-reload` and `ldconfig`, then the restart:

  | target | restarts |
  |---|---|
  | `deploy-bluez` | `bluetooth` |
  | `deploy-pipewire` | `pipewire` + `wireplumber`, restarting them |
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

## Logs: `make logs`, clog

btbench-logd (btbench-logs.service) mirrors the journal into `/tmp/logs` on the board, one file per
program in the format the program prints itself, so clog (`app/clog`, on the board and built for
the PC in `app/build/clog/clog`) colours each with its group:

```bash
make logs                      # clog -t /tmp/logs/bluez.log on the board (LOG=pipewire|wireplumber|
                               #   btbenchd|kernel|system|wpasupplicant)
make logs-pull                 # the files to build/logs/<time>/ (build/logs/latest)
app/build/clog/clog build/logs/latest/bluez.log
ssh root@btbench.local clog /tmp/logs/pipewire.log
```

`/tmp/logs/btsnoop` links to the capture ring. Debug output: `bluetoothd -d` (System → bluetoothd
arguments, `bench bt-args "-d"`), `PIPEWIRE_DEBUG`, and the kernel's own Bluetooth messages
(`make bt-debug AREAS=l2cap`, docs/kernel.md) all land in these files.

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
  WirePlumber. PipeWire's BlueZ plugin talks to bluetoothd over D-Bus and links
  BlueZ's libbluetooth only for the socket helpers, whose ABI has not moved in years.
- **Mixed revisions.** A deploy on top of an image of different revisions works as long as the
  libraries they share agree. When in doubt, `make bump-versions image` and reflash.
