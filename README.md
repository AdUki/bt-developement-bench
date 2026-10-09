# bt-developement-bench

A Bluetooth development bench on a Yocto (scarthgap) image: a board you push BlueZ, PipeWire,
WirePlumber and kernel builds to in seconds, with a web console and a CLI to drive Bluetooth and
watch the HCI traffic live. Built for a Raspberry Pi Zero W; other boards are a `boards/<name>.mk`
plus a `yocto/conf/boards/<name>.conf` away.

- **BlueZ** from git (pinned in `yocto/conf/versions.conf`), with every tool and test script.
- **Audio stack switchable on the board:** PipeWire + WirePlumber, BlueALSA, or bluetoothd alone
  (`bench audio <mode>`).
- **Kernel:** bluetooth-next by default (image and dev tree), linux-raspberrypi for comparison
  (`KERNEL=rpi`). Trial boots fall back to the good kernel on their own.
- **Monitoring:** an always-on btsnoop ring, live per-link/per-channel throughput, TX latency
  (HCI → Number of Completed Packets), controller buffer occupancy, AVDTP state and RTP loss, and
  a live pcap stream into Wireshark.
- **Control:** web console (`http://10.55.0.1`) and `tools/bench`: pair, connect, GATT explorer,
  advertising, local GATT server, audio mode, Wi-Fi, kernel slots, logs.
- **Links:** USB gadget (network + serial console) on the data micro-USB port; Wi-Fi with a setup
  AP (`btbench-XXXX`) when no network answers.

## Quick start

```bash
make host-deps          # Yocto host packages (Ubuntu/Debian)
make configure          # hostname, root password, ssh key, Wi-Fi, caches
make image              # first build: a few hours on ARMv6, then sstate
make flash DISK=/dev/sdX
```

Plug the Zero W's **USB** (data) port into the PC. The board is `10.55.0.1`, the PC gets an
address by DHCP, and `btbench.local` answers over mDNS. Then:

```bash
make ssh                         # root shell (your ssh key)
tools/bench status               # the CLI; BENCH=host to point it elsewhere
xdg-open http://10.55.0.1        # the web console
```

## The inner loop

```bash
make sdk                         # once per image: the cross toolchain + sysroot
make src                         # src/{bluez,pipewire,wireplumber,linux}: worktrees of ~/projects/*
make bluez && make deploy-bluez  # build with the image's options, rsync, restart bluetooth
make pipewire wireplumber && make deploy
make linux && make linux-test    # boot the new kernel once; make linux-commit to keep it
make gdb PROC=bluetoothd
make clangd                      # compile_commands.json is already in every src tree
```

`make` alone lists every target.

## Docs

| | |
|---|---|
| [docs/dev.md](docs/dev.md) | the dev loop: src trees, builds, deploys, gdb, clangd, version pinning |
| [docs/kernel.md](docs/kernel.md) | bluetooth-next vs linux-raspberrypi, trial/commit/rollback, recovery |
| [docs/audio.md](docs/audio.md) | audio modes, codecs and licensing, headless PipeWire, bluetoothd args |
| [docs/monitor.md](docs/monitor.md) | what the HCI monitor measures and how; Wireshark live capture |
| [docs/system.md](docs/system.md) | SD card layout, first boot, USB link, Wi-Fi and the setup AP |
| [docs/web.md](docs/web.md) / [docs/api.md](docs/api.md) | the web console and the REST/WebSocket API |
| [docs/contracts.md](docs/contracts.md) | the interfaces between the pieces |

## Layout

```
Makefile, mk/*.mk         every target (yocto, system, dev, kernel, app)
bench.conf.sample         per-machine defaults (BOARD, KERNEL, TARGET, *_UPSTREAM)
boards/                   board descriptions + device-tree additions
yocto/conf/               bblayers/local templates, versions.conf, per-board bitbake settings
yocto/meta-btbench/       the layer
app/                      btbenchd (C++17) and its web console
tools/bench               the CLI
tools/target/             scripts installed on the board (audio, wifi, kernel, gadget, ...)
tools/dev/                helpers for the dev targets
tools/fake-bluez/         a fake BlueZ for running btbenchd on a desk (make run BT=fake)
```
