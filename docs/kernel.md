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
| `next` (default) | bluetooth-next `master`, from the bench's fork with its fixes | `linux-btbench` | `src/linux` | `bcm2835_defconfig` | `broadcom/bcm2835-rpi-zero-w.dtb` + `boards/dts/rpi0w-btbench.dtsi` |
| `rpi` | raspberrypi/linux `rpi-6.12.y` | `linux-raspberrypi` (meta-raspberrypi) | `src/linux-rpi` | `bcmrpi_defconfig` | `broadcom/bcm2708-rpi-zero-w.dtb` |

**Why bluetooth-next.** This is where the Bluetooth stack you develop against lives. The image's
kernel and your dev tree come from the same tree, so the board runs what you build and its
modules match it.

**The pinned revision.** The image's revision is pinned in `yocto/conf/versions.conf`
(`BTBENCH_LINUX_SRCREV`). bluetooth-next is rebased onto net-next and is sometimes broken, so the
pin only moves when you move it, with `make bump-versions` (it takes `src/linux`'s HEAD). To bake
whatever `src/linux` holds into the image without changing the pin, use
`make image SRC=local` (externalsrc). The recipe never writes into that tree.

**Configuration.** The board's upstream defconfig, plus two fragments in
`yocto/meta-btbench/recipes-kernel/linux/files/`: `btbench.cfg` (what the bench needs) and
`bt-debug.cfg` (Bluetooth logging switched on at runtime: debugfs, dynamic debug, the Bluetooth
debugfs, printk timestamps, a 256 KiB log buffer; see "Kernel Bluetooth logs"). The same
fragments are used by `linux-btbench` (`BTBENCH_KERNEL_FRAGMENTS`), the `linux-raspberrypi`
bbappend and `make linux`, followed by the board's own fragment, `boards/<board>.cfg`
(`KFRAGMENT_BOARD`; `boards/rpi.cfg` for both Raspberry Pis: their watchdog, the undervoltage
alarm, no HDMI audio). `btbench.cfg` switches on:

- **Bluetooth.** Built as modules, so a rebuilt `bluetooth.ko` can be reloaded without a reboot.
  It covers hci_uart over serdev with the Broadcom protocol (the onboard BCM43430A1), btusb for
  dongles, vhci for btvirt and the BlueZ testers, `BT_DEBUGFS`, and the MSFT and AOSP extensions.
- **Tracing.** Dynamic debug, ftrace (function and graph tracers), kprobe and uprobe events.
- **Recovery.** `IKCONFIG_PROC`, `PANIC_TIMEOUT=10`, the watchdog core (the SoC's driver, built
  in, is in the board fragment), pstore/ramoops.
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
make src-linux            # once: src/linux, the submodule (bluetooth-next and the bench's fixes)
make sdk                  # once per image: the cross compiler comes from the image's SDK
make linux                # build: build/rpi0w/linux-next, installed into build/rpi0w/stage/linux-next
make linux-test           # copy it to the test slot, boot it once, report what came up
make linux-commit         # keep it
```

**`make src-linux`.** `src/linux` is a submodule of the repository, the bench's fork
(`github.com/AdUki/linux`) on its branch `bench`: bluetooth-next at the image's pin plus the
bench's fixes as commits (the dwc2 gadget one). `make src-linux` checks it out, shallow (one
commit, not the history), at the commit the repository records, which is the image's kernel. A
tree already there is left alone. See docs/dev.md "The forks" for changes, pushing and bumping.

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

- `ls /sys/fs/pstore` (dmesg of the crashed or hung boot, `console-ramoops-0`). The journal is
  in RAM, so the failed boot's is gone: pstore is what is left of it.

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

## Kernel Bluetooth logs

Every `BT_DBG` / `pr_debug` in `net/bluetooth` and `drivers/bluetooth` is compiled in and off
(`bt-debug.cfg`), and switched at runtime through dynamic debug — by area, without a rebuild or a
reboot. The messages, with the function's name, go to the kernel log (`dmesg`, `journalctl -k`).

```bash
make bt-debug AREAS="l2cap smp"     # on, then follow the kernel log (Ctrl-C; they stay on)
make bt-debug STOP=l2cap            # one off
make bt-debug AREAS=smp KEEP=1      # on now and from boot
make bt-debug-off                   # all off, none kept
bench btdebug on mgmt; bench logs kernel -f
btbench-btdebug status              # on the board: per area, call sites and how many print
```

Areas: `hci` (core, events, connections, hci_sync, sockets), `mgmt`, `l2cap`, `smp`, `iso`, `sco`,
`rfcomm`, `bnep`, `hidp`, `driver` (hci_uart, btbcm, btusb, btrtl, btintel), `all`. The console has
the same switches in System → Kernel, and its journal shows "Kernel log". *Keep from boot*
writes `dyndbg` options to `/etc/modprobe.d/btbench-btdebug.conf`, applied as the modules load.

They are chatty: an A2DP stream with `l2cap` on logs thousands of lines a second, enough to load
the Zero W's one core and change the timing being looked at. Switch on the area in question, not
`all`. For anything finer (one function, one line) write to
`/sys/kernel/debug/dynamic_debug/control` directly: `echo 'func l2cap_recv_frame +pfl' > …`.

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

## Khadas VIM3L (`BOARD=vim3l`)

A second SoC family: Amlogic S905D3 (four Cortex-A55, arm64, 2 GB), on meta-meson's
`khadas-vim3l` machine. Only `KERNEL=next`. It has not been booted by the bench yet.

| | |
|---|---|
| defconfig | arm64 `defconfig` + `boards/vim3l.cfg` (every other SoC family off, the Amlogic watchdog and PWMs built in, no HDMI sound card) |
| device tree | `amlogic/meson-sm1-khadas-vim3l.dtb`, mainline as it is: the USB-C port is already a peripheral |
| Bluetooth | BCM4359 (AP6398S) on UART_A with RTS/CTS, `brcm,bcm43438-bt`, hci_uart over serdev |
| Wi-Fi | the same module on SDIO, brcmfmac |
| gadget | dwc2, `ff400000.usb`, on the USB-C port that also powers the board |
| console | UART_AO on the 40-pin header: pin 17 GND, 18 RX, 19 TX, 115200 8N1 (`ttyAML0`) |

**Firmware.** Neither the Wi-Fi firmware nor the Bluetooth patch is in linux-firmware.
`btbench-firmware-vim3l` installs them from LibreELEC's collection: brcmfmac asks for
`brcm/brcmfmac4359-sdio.khadas,vim3l.{bin,txt}`, btbcm for `brcm/BCM4359C0.khadas,vim3l.hcd` and
then `brcm/BCM4359C0.hcd`.

**The boot chain.** The SoC's ROM loads Amlogic's signed BL2 and BL3x, which start mainline
U-Boot (2024.01, `khadas-vim3l_defconfig`). meta-meson's `amlogic-boot-fip` packs them into
`u-boot.bin.sd.bin`, which its image class writes into the card's first megabyte, around the
partition table (`wic/btbench-meson.wks`; the boot partition starts at 4 MB). U-Boot's distro
boot finds `boot.scr` on the FAT partition. There is no firmware to set a device tree and command
line, so the script (`dynamic-layers/meson/recipes-bsp/btbench-boot-scr/files/boot.cmd.in`) loads
`Image` and `meson-sm1-khadas-vim3l.dtb` itself and sets the command line from the board conf
(`BTBENCH_CMDLINE`). U-Boot writes the board's serial number (from the eFuse MAC) into the tree it
boots.

**Booting from the SD card.** The ROM tries the SPI flash (when the board's MCU says to), then
the eMMC, and only then the SD card. A VIM3L that has a boot loader in either runs that one first,
and it has to find the card's `boot.scr` by itself, and then kernel trials do not work (its
environment is not the card's `uboot.env`). The serial console says which one runs: the bench's
announces `U-Boot 2024.01 ... khadas-vim3l`. To have the ROM reach the card, set the boot mode to
eMMC (Khadas' OOWOW menu, or `kbi bootmode w emmc` in Khadas' U-Boot) and erase the eMMC's boot
loader (`dd if=/dev/zero of=/dev/mmcblkN bs=1M count=4` from a running system, N being the eMMC).

**Kernel trials** work as on the Raspberry Pis (`TRY_METHOD=uboot-oneshot`): the bench's U-Boot
config keeps the environment in `/boot/uboot.env` (`dynamic-layers/meson/recipes-bsp/u-boot/`;
U-Boot numbers the SD card mmc 1), and `boot.scr` clears `bench_try` before it boots the test
slot. Two differences:

- **No watchdog in U-Boot.** It has no driver for the SoC's watchdog, so a test kernel that hangs
  before the kernel's own driver and systemd take the watchdog over is not reset: power-cycle the
  board. The next boot runs the good kernel.
- **No pstore.** The board has no ramoops region in its device tree yet.

The good slot is `/boot/Image` and `/boot/meson-sm1-khadas-vim3l.dtb`.

