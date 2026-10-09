# Audio and BlueZ on the board

The board runs bluetoothd from BlueZ master and one of three audio setups on top of it. You pick
the setup at runtime; nothing needs a rebuild. An always-on btmon ring records every HCI packet
to `/data/btsnoop`.

| mode | what runs | use it for |
|---|---|---|
| `pipewire` (default) | PipeWire system-wide, WirePlumber | A2DP both ways, HFP both ends, LE Audio (BAP), the codecs PipeWire has |
| `bluealsa` | BlueALSA (`bluealsa`, plus `bluealsa-aplay` by hand) | A2DP and HFP with ALSA PCMs per link, a much smaller stack |
| `none` | bluetoothd alone | `bluetoothctl endpoint/transport`, BlueZ's `test/` scripts, your own endpoint code |

```
btbench-audio get                      # pipewire
btbench-audio status                   # {"mode":"pipewire","modes":[...],"services":{...}}
btbench-audio set bluealsa             # switch; remembered across reboots
btbench-audio set none --restart-bt    # switch and restart bluetoothd too
```

`bench audio <mode>` and the web UI's Audio tab call the same script.

## How the modes switch

PipeWire and BlueALSA both register A2DP endpoints and HFP profiles with bluetoothd. Running
both at once would leave devices connecting to whichever registered first, so the modes exclude
each other through systemd:

- **One target per mode.** `btbench-audio-pipewire.target` wants `pipewire.socket`,
  `pipewire-manager.socket`, `pipewire.service` and `wireplumber.service`.
  `btbench-audio-bluealsa.target` wants `bluealsa.service`. `btbench-audio-none.target` wants
  nothing.
- **The targets conflict with each other.** Starting one stops the other two.
- **The services are `PartOf=` their target** (drop-ins in
  `/usr/lib/systemd/system/<unit>.d/50-btbench-audio-*.conf`). Stopping a target stops its stack.
  The sockets are included, so a PipeWire client cannot socket-activate PipeWire in another mode.
- **Ordering.** `bluealsa.service` is ordered `After=` PipeWire and WirePlumber. On a switch,
  systemd therefore stops the old stack before it starts the new one, so the two never overlap.
- **Nothing is enabled on its own.** pipewire, wireplumber and bluealsa have
  `SYSTEMD_AUTO_ENABLE = "disable"`. At boot, `btbench-audio-select.service` runs
  `btbench-audio boot`, which starts the stored mode's target.

`btbench-audio set <mode>`:

1. Writes the mode to `/data/btbench/audio-mode`.
2. Disconnects the connected devices that use an audio profile (A2DP, HFP/HSP, ASCS/PACS/BASS),
   so they reconnect to the new stack's endpoints. It does not leave a transport whose owner is
   about to disappear.
3. Runs `systemctl start btbench-audio-<mode>.target`.
4. With `--restart-bt`, it also restarts bluetoothd. bluetoothd drops a stack's endpoints by
   itself when the stack leaves D-Bus. A restart also clears whatever state that stack left on
   the adapter and the devices.

**Where the mode comes from at boot:**

1. `/data/btbench/audio-mode`
2. `AUDIO_MODE` in `/etc/btbench/device.env`
3. The image's default, `BTBENCH_AUDIO_MODE` in `btbench-device.conf`

## Codecs and licensing

| codec | PipeWire | BlueALSA | switch | library, license |
|---|---|---|---|---|
| SBC, SBC-XQ | yes | yes (SBC) | always | sbc (LGPL-2.1+) |
| CVSD, mSBC (HFP) | yes | yes | always | sbc; spandsp (LGPL-2.1) for BlueALSA's mSBC |
| LC3 (BAP), LC3-SWB (HFP) | yes | LC3-SWB | always | liblc3 (Apache-2.0) |
| Opus (A2DP, PipeWire's vendor codec) | yes | yes | always | libopus (BSD) |
| FastStream | yes | yes | always | built in |
| G.722 (ASHA) | yes | no | always | built in |
| aptX, aptX HD | yes | yes | `BTBENCH_EXTRA_CODECS = "1"` | libfreeaptx (LGPL-2.1+), recipe in meta-btbench |
| LDAC (encode only) | yes | yes | `BTBENCH_EXTRA_CODECS = "1"` | ldacbt / AOSP libldac (Apache-2.0), recipe in meta-btbench |
| AAC | yes | yes | `BTBENCH_AAC = "1"` | fdk-aac, `LICENSE_FLAGS = "commercial"` |

Both switches live in `yocto/meta-btbench/conf/btbench-device.conf`. Run `make image` afterwards.

- **aptX and LDAC are opt-in** because scarthgap's layers do not have them. The libraries
  implement codecs whose owners license them for products. Building them for a bench on your
  desk is your call, which is why they are off by default.
- **LDAC can only be sent.** No free decoder exists. Expect an ARM11 to manage LDAC's lower
  bitrates at best, and the 3 Mbaud UART to the BCM43430 limits the higher ones anyway.
- **AAC needs fdk-aac**, which OE flags `commercial`. With `BTBENCH_AAC = "1"`, the distro adds
  `commercial` to `LICENSE_FLAGS_ACCEPTED`. Accepting it is your decision.

**Which codecs are offered at runtime:**

- **PipeWire.** WirePlumber's `bluez5.codecs` list (below). Delete a name to stop offering that
  codec.
- **BlueALSA.** SBC, CVSD, mSBC and LC3-SWB are on by default. The rest are enabled by name with
  `-c` on its command line (`SYSTEMD_BLUEALSA_ARGS` in the bluealsa bbappend). Only the codecs
  that were built get a `-c`, because an unknown name stops the daemon. To try something else
  on the board, use `systemctl edit bluealsa` with an `ExecStart=` override.

## PipeWire without a desktop

There is no user session on the board, so PipeWire runs as a **system service**. It uses
upstream's system units (`-Dsystemd-system-service=enabled`):

- **The daemon.** `pipewire.service` runs as user `pipewire` (groups `audio`, `video`). Its
  sockets are in `/run/pipewire`: `pipewire-0`, and `pipewire-0-manager` for session managers
  and tools.
- **The session manager.** `wireplumber.service` runs as the same user with profile
  `main-systemwide`. A drop-in gives it `/var/lib/wireplumber` (`StateDirectory=`,
  `XDG_STATE_HOME`), so the default devices, profiles and volumes survive a restart. The home
  directory of `pipewire` is `/`, where it could not write.
- **Root's tools.** `/etc/profile.d/btbench-pw.sh` exports `PIPEWIRE_RUNTIME_DIR=/run/pipewire`.
  `wpctl status`, `pw-top`, `pw-dump`, `pw-cli` and `pw-cat` in a root shell therefore reach the
  system daemon. A program started some other way (a systemd unit, btbenchd's job runner) needs
  the same variable.
- **D-Bus.** BlueZ's policy lets anyone call `org.bluez`, and lets root (bluetoothd) call back
  into the `pipewire` user's `MediaEndpoint1`/`Profile1` objects. No extra policy is needed.
- **Real-time.** The units grant `CAP_SYS_NICE`, so `module-rt` gets SCHED_FIFO without RTKit.
- **What is not built.** pipewire-pulse (packaged apart, not installed), JACK, video,
  GStreamer, network audio and any desktop integration.
- **ALSA plugin.** The pipewire-alsa plugin is installed, but PipeWire is *not* made the default
  ALSA device: in the other modes there would be no PipeWire to answer. Use
  `aplay -D pipewire file.wav`.
- **Sample rates.** `/etc/pipewire/pipewire.conf.d/50-btbench.conf` allows the graph to run at
  44.1 kHz as well as 48 kHz. Most A2DP links run at 44.1 kHz. Resampling them to 48 kHz on the
  ARM11 costs more CPU than SBC itself, and it adds its own latency to whatever you are
  measuring.

### WirePlumber's Bluetooth settings

The drop-in is `/etc/wireplumber/wireplumber.conf.d/50-btbench.conf`:

| setting | value | why |
|---|---|---|
| `support.logind`, `monitor.bluez.seat-monitoring` | `disabled` (profiles `main` and `main-systemwide`) | no logind seat on a headless board. Seat monitoring would keep the BlueZ monitor waiting for one forever |
| `bluez5.roles` | `[ a2dp_sink a2dp_source bap_sink bap_source hfp_hf hfp_ag ]` | every role except HSP, as upstream does |
| `bluez5.codecs` | every codec name PipeWire 1.6 knows | names that were not built are ignored. Trim the list to force a codec |
| `bluez5.hfphsp-backend` | `native` | PipeWire's own HFP; no oFono or hsphfpd on the image |

WirePlumber merges fragments: objects key by key, arrays by **concatenation**. To replace one of
these lists from another fragment, name the section `override.monitor.bluez.properties`.

## snd-aloop: the board's sound card

The Zero W has no analog audio under the mainline kernel. Its sound card is the ALSA loopback
(`CONFIG_SND_ALOOP`, loaded at boot through `/etc/modules-load.d/snd-aloop.conf`). What is
played into one side can be captured from the other:

| play to | capture from |
|---|---|
| `hw:Loopback,0,N` | `hw:Loopback,1,N` |
| `hw:Loopback,1,N` | `hw:Loopback,0,N` |

**In PipeWire**, the loopback card is the only ALSA device. That makes it the default sink and
source until a Bluetooth device appears. WirePlumber ranks Bluetooth nodes higher, so a
connected headset becomes the default, as on a desktop. Use `wpctl set-default <id>` to pin one.

```
# A phone plays into the board (A2DP sink): record what arrives
arecord -D hw:Loopback,1,0 -f S16_LE -r 44100 -c 2 phone.wav

# The board plays to a speaker (A2DP source): play a file to the Bluetooth sink
pw-play --target bluez_output.AA_BB_CC_DD_EE_FF.1 test.wav     # or: wpctl status for the name

# BlueALSA mode: the PCMs are per link
bluealsa-aplay -L                                     # list them
aplay -D bluealsa:DEV=AA:BB:CC:DD:EE:FF,PROFILE=a2dp test.wav
bluealsa-aplay AA:BB:CC:DD:EE:FF                      # phone → hw:Loopback (pass -D)
```

## bluetoothd

- **Version.** BlueZ comes from bluez.git at `BTBENCH_BLUEZ_SRCREV` (`yocto/conf/versions.conf`),
  the revision `src/bluez` starts from (see `docs/dev.md`).
- **What the image adds.** The deprecated `hci*` tools, the testers (`mgmt-tester`,
  `l2cap-tester`, `iso-tester`, ...), `btvirt`, `btpclient`, every uninstalled tool
  (`bluez5-noinst-tools`), `btmgmt` (its own package) and the Python scripts in
  `/usr/lib/bluez/test` (`bluez5-testtools`).

**`main.conf`** is upstream's file, with three settings turned on:

| setting | why |
|---|---|
| `Experimental = true` | BAP, BASS, VCP, MICP, CSIP, MCP, TMAP, GMAP and ASHA are experimental plugins. Without this there is no LE Audio |
| `KernelExperimental = 6fbaf188-05e0-496a-9885-d6ddfdb4e03e` | the kernel creates ISO sockets only with this feature set |
| `JustWorksRepairing = confirm` | after a reflash the phone still holds a key the board has lost. The agent is asked instead of the phone being refused |

**Arguments.** bluetoothd's arguments come from `/data/btbench/bluetoothd.env`, through the
drop-in `/usr/lib/systemd/system/bluetooth.service.d/10-btbench.conf`:

```
BLUETOOTHD_ARGS="-d"                   # debug logging; -E, -K, -d <file pattern> ...
BLUETOOTHD_NOPLUGIN="--noplugin=hfp"   # the default
```

Run `systemctl restart bluetooth` after editing (`bench bt-args "-d"` does both).

- **`-n` is not needed.** bluetoothd does not fork under systemd either way. `-n` only copies
  every log line to stderr, so each line shows up twice in the journal.
- **BlueZ's own `hfp` plugin is not loaded.** It is an experimental HFP Hands-Free with its own
  SDP record and RFCOMM server, so `Experimental = true` would load it. It would then compete
  with PipeWire's native HFP and BlueALSA's `hfp-hf` for the same role. To work on it, set
  `BLUETOOTHD_NOPLUGIN=` (empty) and use audio mode `none`, or remove `hfp_hf` from
  WirePlumber's roles.

**LE Audio on the Zero W.** The BCM43430A1 is a Bluetooth 4.1 controller without ISO channels.
To exercise BAP, use a USB dongle that has them (with the gadget off, `BTBENCH_GADGET = "off"`)
or a virtual controller: `btvirt -l2` or a vhci device.

## The capture ring

`btbench-btsnoop.service` is enabled by default. It runs `/usr/bin/btbench-btsnoop`, a loop
around `btmon -w`:

- **Files.** Each one is `/data/btsnoop/hci-<YYYYmmdd-HHMMSS>.btsnoop`. The newest is the one
  being written.
- **Rotation.** When the current file passes `ROTATE_SIZE`, a new btmon starts on the next file
  and the old one is stopped a second later. There is no gap: a few packets land in both files.
  Upstream btmon cannot rotate by itself.
- **Pruning.** Only `ROTATE_COUNT` files are kept, oldest deleted first.
- **Settings.** Put them in `/data/btbench/btsnoop.env`:

  | variable | default | |
  |---|---|---|
  | `ROTATE_SIZE` | `16M` | bytes, or with a K/M/G suffix |
  | `ROTATE_COUNT` | `8` | files kept, including the one being written |
  | `CHECK_INTERVAL` | `5` | seconds between size checks |
  | `BTSNOOP_DIR` | `/data/btsnoop` | |
  | `BTMON_ARGS` | | extra btmon options, e.g. `-i 0` |

- **Using it.** btbenchd lists, downloads, deletes and analyses the files (`/api/capture`). Open
  them with `btmon -r`, `btmon -a` or Wireshark. A live view is `curl -sN
  http://<board>/api/hci/live.pcap | wireshark -k -i -`.

## Files

| what | where |
|---|---|
| BlueZ recipe | `yocto/meta-btbench/recipes-connectivity/bluez5/bluez5_git.bb` (requires poky's `bluez5.inc`) |
| bluetoothd drop-in | `recipes-connectivity/bluez5/bluez5/10-btbench.conf` |
| PipeWire, WirePlumber | `recipes-multimedia/{pipewire,wireplumber}/*_git.bb` and their config fragments |
| BlueALSA | `recipes-multimedia/bluealsa/bluealsa_%.bbappend` (4.3.1, codecs, args) |
| codec libraries | `recipes-multimedia/{libfreeaptx,ldacbt}/` |
| modes, capture ring | `recipes-multimedia/btbench-audio/` (units, drop-ins), `tools/target/btbench-{audio,btsnoop}` |
| everything for the image | `recipes-multimedia/packagegroups/packagegroup-btbench-audio.bb` |
