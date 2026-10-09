# Kernel

The bench boots **bluetooth-next** by default, both in the image and as the dev tree you build
with `make linux`. The Raspberry Pi fork (`linux-raspberrypi`) is there as `KERNEL=rpi`, for
comparing the two on the same board.

A kernel you build is never written over the one that boots. It goes into a *test slot*, boots
once, and then you keep it (`commit`) or reboot back to the known-good kernel. If the test kernel
panics, hangs or loses its network, the next reset brings the good kernel back on its own.

## Which kernel

| `KERNEL=` | source | image recipe | dev tree | defconfig (rpi0w) | device tree (rpi0w) |
|---|---|---|---|---|---|
| `next` (default) | bluetooth-next `master` | `linux-btbench` | `src/linux` | `bcm2835_defconfig` | `broadcom/bcm2835-rpi-zero-w.dtb` + `boards/dts/rpi0w-btbench.dtsi` |
| `rpi` | raspberrypi/linux `rpi-6.12.y` | `linux-raspberrypi` (meta-raspberrypi) | `src/linux-rpi` | `bcmrpi_defconfig` | `broadcom/bcm2708-rpi-zero-w.dtb` |

**Why bluetooth-next.** This is where the Bluetooth stack you develop against lives. The image's
kernel and your dev tree come from the same tree, so the board runs what you build and its
modules match it.

**The pinned revision.** The image's revision is pinned in `yocto/conf/versions.conf`
(`BTBENCH_LINUX_SRCREV`). bluetooth-next is rebased onto net-next and is sometimes broken, so the
pin only moves when you move it, with `make bump-versions` (it takes `src/linux`'s HEAD). To bake
whatever `src/linux` holds into the image without changing the pin, use
`make image SRC=local` (externalsrc). The recipe never writes into that tree.

**Configuration.** The board's upstream defconfig, plus one fragment:
`yocto/meta-btbench/recipes-kernel/linux/files/btbench.cfg`. The same fragment is used by
`linux-btbench`, the `linux-raspberrypi` bbappend and `make linux`. It switches on:

- **Bluetooth.** Built as modules, so a rebuilt `bluetooth.ko` can be reloaded without a reboot.
  It covers hci_uart over serdev with the Broadcom protocol (the onboard BCM43430A1), btusb for
  dongles, vhci for btvirt and the BlueZ testers, `BT_DEBUGFS`, and the MSFT and AOSP extensions.
- **Tracing.** Dynamic debug, ftrace (function and graph tracers), kprobe and uprobe events.
- **Recovery.** `IKCONFIG_PROC`, `PANIC_TIMEOUT=10`, `BCM2835_WDT` built in, pstore/ramoops, and
  the raspberrypi-hwmon undervoltage alarm.
- **USB gadget.** dwc2 in dual-role mode with configfs NCM, ECM and ACM, all built in.
- **Wi-Fi.** brcmfmac and cfg80211, both as modules.
- **Audio.** snd-aloop and snd-usb-audio.

`BT_FEATURE_DEBUG` is not set, because it excludes `DYNAMIC_DEBUG`. Use dynamic debug instead:
`echo 'module bluetooth +p' > /sys/kernel/debug/dynamic_debug/control`.

Sometimes a fragment value does not survive `olddefconfig` (the symbol was renamed, or a
dependency is missing). Both the recipe and `make linux` print a line for each such value.

**Kernel release.** The release is `<version>-btbench-<git revision>`, for example
`7.3.0-rc2-btbench-00421-g97a128698d9d`. `CONFIG_LOCALVERSION_AUTO` adds the revision, and a
`-dirty` suffix when the tree has uncommitted changes. Each kernel's modules go to
`/lib/modules/<release>`, side by side with the others.

**Device tree (next).** The mainline `bcm2835-rpi-zero-w.dts` already has what the bench needs:

- the Bluetooth node on uart0 (`brcm,bcm43438-bt`, with `shutdown-gpios` on GPIO45);
- the Wi-Fi node on sdhci (`brcm,bcm4329-fmac` with its power sequence);
- the dwc2 FIFO sizes for device mode.

`boards/dts/rpi0w-btbench.dtsi` adds two things:

- `&usb { dr_mode = "peripheral"; }`, so the dev link always comes up;
- a 1 MiB ramoops region at `0x0b000000`, marked `no-map`.

`tools/dev/mkdtb` compiles it in after the board's dts. It uses the kernel's include paths, the
kernel's own dtc and `-@`. The recipe and `make linux` both use this one script. The firmware loads the result as
`device_tree=bcm2835-rpi-zero-w.dtb` (`yocto/conf/boards/rpi0w.conf`).

**Firmware names.** Mainline looks for board-specific firmware names first. The rpidistro
packages the machine config already pulls in cover them all, so the bench adds no symlinks:

- **Wi-Fi.** brcmfmac asks for `brcm/brcmfmac43430-sdio.raspberrypi,model-zero-w.{bin,txt,clm_blob}`
  (from the root `compatible`). `linux-firmware-rpidistro-bcm43430` ships exactly these, as links
  to the Cypress files.
- **Bluetooth.** btbcm asks for `brcm/BCM43430A1.raspberrypi,model-zero-w.hcd`, then
  `brcm/BCM43430A1.hcd`. `bluez-firmware-rpidistro-bcm43430a1-hcd` ships the second, and the
  first missing is not an error (`firmware_request_nowarn`).

## What mainline does not have (compared with the downstream kernel)

- **No firmware overlays.** `dtoverlay=` lines in config.txt mean nothing to a mainline dtb.
  Whatever the bench needs from them goes into the board's dtsi. An I2S DAC would be a
  `simple-audio-card` node there, or use `KERNEL=rpi`.
- **No `vcgencmd` or vcio.** Undervoltage and throttling show up in
  `/sys/class/hwmon/hwmon*/in0_lcrit_alarm` (raspberrypi-hwmon). Temperature is in the thermal
  zone.
- **No tryboot reboot flag.** Only the downstream kernel passes `reboot "0 tryboot"` to the
  firmware. Kernel trials therefore go through U-Boot, which works for either kernel.
- **Different console name.** The mini-UART on the GPIO header is `ttyS1` (alias `serial1`), not
  `serial0`. The PL011 (`ttyAMA0`) is the Bluetooth radio's.
- **No VideoCore audio, HDMI audio or camera stack.** None of these matter for Bluetooth audio
  work, where snd-aloop is the PCM.

## The dev loop

```bash
make src-linux            # once: src/linux, a worktree of ~/projects/linux on bluetooth-next
make sdk                  # once per image: the cross compiler comes from the image's SDK
make linux                # build: build/rpi0w/linux-next, installed into build/rpi0w/stage/linux-next
make linux-test           # copy it to the test slot, boot it once, report what came up
make linux-commit         # keep it
```

**`make src-linux`.** This adds a worktree of `LINUX_UPSTREAM` (default `~/projects/linux`) at
`src/linux`, on a branch `bench` that starts at the image's pinned revision (like the other src
trees, so the first deploy changes nothing but what you commit on top) and tracks
`bluetooth-next/master`. An existing `bench` branch is reused as it is. If that repository has no
`bluetooth-next` remote, `src-linux` shows the two git commands and asks before running them.
If there is no repository at `LINUX_UPSTREAM`, it makes a blobless clone of bluetooth-next
instead. `compile_commands.json` and `.clangd` are added to the repository's `info/exclude`.

**`make src-linux-rpi`.** A blobless clone of raspberrypi/linux at the revision the image's
`linux-raspberrypi` recipe uses (read with `bitbake -e`).

**`make linux`.**

- Runs `make -C src/linux O=build/<board>/linux-<kernel> ARCH=<KARCH>` with the SDK's cross
  compiler and nothing else from the SDK. The host's gcc, bison, flex and libssl/libelf headers
  build the kernel's host tools.
- Generates the config once (`make linux-defconfig`: defconfig, `merge_config.sh -m btbench.cfg`,
  `olddefconfig`). It keeps that config until the fragment or the defconfig changes, so
  `make linux-menuconfig` changes survive. `linux-defconfig` saves the previous config as
  `.config.btbench-old`.
- Builds `<KIMAGE> modules dtbs` with `DTC_FLAGS=-@`.
- Installs into `build/<board>/stage/linux-<kernel>/`: `boot/{zImage,board.dtb,version}` and
  `lib/modules/<release>` (stripped).
- Links `compile_commands.json` into the tree.

Variables you can set:

- `JOBS=`
- `LINUX_LOCALVERSION=` (default `-btbench`)

**Reloading modules without a reboot.** This works when the running kernel has the same release
as your build and only module code changed. Copy the `.ko` files from
`build/<board>/stage/linux-<kernel>/lib/modules/<release>/` to the board, then
`modprobe -r btbcm hci_uart bluetooth` (stop bluetoothd first) and `modprobe hci_uart`.

## Trial, commit, rollback

The kernel files live on the FAT boot partition, mounted at `/boot`:

| slot | kernel | device tree | written by |
|---|---|---|---|
| good | `/boot/zImage` | `/boot/bcm2835-rpi-zero-w.dtb` (the one the firmware loads) | the image, `commit`, `rollback` |
| test | `/boot/bench/test/zImage` | `/boot/bench/test/board.dtb` | `make linux-deploy` |
| prev | `/boot/bench/prev/zImage` | `/boot/bench/prev/board.dtb` | `commit` (the good one before it) |

Each slot directory also holds `version`, the kernel release. The good kernel's release is in
`/boot/bench/good.version` once it has been recorded (`try` and `commit` record it), and is
`uname -r` while it runs.

**The boot chain (rpi0w).** The firmware reads config.txt and loads the good slot's device tree,
then starts U-Boot (`kernel.img`). U-Boot runs `boot.scr`, which is meta-raspberrypi's script
plus the trial path in `dynamic-layers/raspberrypi/recipes-bsp/rpi-u-boot-scr/files/boot.cmd.in`.

**`btbench-kernel try`** (what `make linux-test` runs) sets `bench_try=1` in U-Boot's
environment (`fw_setenv`, `/boot/uboot.env`) and reboots. `boot.scr` then:

1. Sets `bench_try=0` and saves the environment before anything else. If the save fails, it
   does not boot the test kernel.
2. Takes the firmware's command line and adds `panic=10 btbench.slot=test` and anything you put
   in the env variable `bench_bootargs` (`fw_setenv bench_bootargs "dyndbg=..."`).
3. Loads `bench/test/zImage`. It loads `bench/test/board.dtb` if there is one; otherwise it uses
   the firmware's device tree. It copies the board serial (`serial-number`, which the bench's
   Bluetooth address is derived from) into the test device tree.
4. Arms the SoC watchdog (`wdt start 15000`) and boots.

**Every way out leads to the good kernel.**

- **Panic.** The kernel reboots after 10 s.
- **Hang before userspace.** U-Boot's watchdog was armed, so a kernel that never reaches the
  watchdog driver resets the board within about 16 s. The driver feeds the watchdog from its
  probe until systemd takes it over.
- **Hang later.** systemd feeds the watchdog (`RuntimeWatchdogSec=15`, from
  `/etc/systemd/system.conf.d/10-btbench-watchdog.conf`). A frozen kernel or a dead PID 1 resets
  the board.
- **Up, but useless** (no network, for example). Power-cycle the board, or `reboot` from the
  serial console.

Because `bench_try` is already 0 by then, the next boot runs the good kernel.

**After a failed trial,** on the board:

- `journalctl -b -1` (the journal is persistent, if the test kernel got that far);
- `ls /sys/fs/pstore` (dmesg of the crashed or hung boot, `console-ramoops-0`).

**The commands:**

| on the PC | on the board | does |
|---|---|---|
| `make linux-deploy` | — | copies stage → `/boot/bench/test/`, modules → `/lib/modules/<release>`, runs `depmod`, prunes unused `/lib/modules/*btbench*` |
| `make linux-test` | `btbench-kernel try` | deploys, boots the test slot once, waits up to 180 s (`LINUX_TEST_TIMEOUT`) for the board, then prints `uname -r` and the slot |
| `make linux-commit` | `btbench-kernel commit` | good → prev, test → good. Only while the test kernel runs (`--force` overrides) |
| `make linux-rollback` | `btbench-kernel rollback` | swaps good and prev (running it twice undoes it); takes effect at the next boot |
| `make linux-status` | `btbench-kernel status` | JSON: running release, slot, method, pending trial, the three slots' versions |

**Deploy refusal.** `make linux-deploy` refuses a build whose release equals the good kernel's.
Deploying it would overwrite the good kernel's modules, and a fallback boot would then load the
test build's modules. This happens with an unchanged checkout, or with uncommitted changes on the
same commit (both are `-dirty`). Commit the change, or build with
`make linux LINUX_LOCALVERSION=-btbench2`, or pass `FORCE=1` if the modules are known to be
compatible.

**`TRY_METHOD=tryboot`.** This is for boards that run the downstream kernel without U-Boot. It
writes `/boot/tryboot.txt` (config.txt plus `os_prefix=bench/test/`) and the test slot's
`cmdline.txt`, then runs `systemctl reboot "0 tryboot"`. It is not used by the rpi0w and is
untested.

## Serial console

There are two ways in when the network is gone:

- **USB gadget ACM** (`ttyGS0` on the board, `/dev/ttyACM0` on the PC). It is built in, so it
  needs no modules. It does not come up if the test kernel dies before userspace sets up the
  gadget.
- **UART** on the GPIO header: pins 8 (TX) and 10 (RX), GND on pin 6, 115200 8N1. This is the
  mini-UART: `ttyS1` with bluetooth-next, `ttyS0` with the downstream kernel. U-Boot and the
  kernel log appear here from the first line.

## Recovering a board that does not boot at all

A broken *good* kernel can only come from `commit` or `rollback`. To recover:

1. Put the SD card in the PC.
2. Copy `bench/prev/zImage` over `zImage`, and `bench/prev/board.dtb` over
   `bcm2835-rpi-zero-w.dtb` on the boot partition.

Or reflash the image (`make flash`).
