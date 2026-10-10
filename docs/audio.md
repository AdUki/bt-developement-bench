# Audio and BlueZ on the board

The board runs bluetoothd from BlueZ master and either PipeWire or nothing on top of it. You pick
the setup at runtime; nothing needs a rebuild. An always-on btmon ring records every HCI packet
to `/tmp/btsnoop`, in RAM.

| mode | what runs | use it for |
|---|---|---|
| `pipewire` (default) | PipeWire system-wide, WirePlumber | A2DP both ways, HFP both ends, LE Audio (BAP), the codecs PipeWire has |
| `none` | bluetoothd alone | `bluetoothctl endpoint/transport`, BlueZ's `test/` scripts, your own endpoint code |

```
btbench-audio get                      # pipewire
btbench-audio status                   # {"mode":"pipewire","modes":[...],"services":{...}}
btbench-audio set none                 # switch; remembered across reboots
btbench-audio set pipewire --restart-bt  # switch and restart bluetoothd too
```

`bench audio <mode>` and the console's Audio → Stack view call the same script.

## How the modes switch

PipeWire registers A2DP endpoints and HFP profiles with bluetoothd. In mode `none` your own
endpoint code or BlueZ's test scripts register theirs instead, and with PipeWire still there
devices would connect to whichever registered first, so the modes exclude each other through
systemd:

- **One target per mode.** `btbench-audio-pipewire.target` wants `pipewire.socket`,
  `pipewire-manager.socket`, `pipewire.service` and `wireplumber.service`.
  `btbench-audio-none.target` wants nothing.
- **The targets conflict with each other.** Starting one stops the other.
- **PipeWire's units are `PartOf=` its target** (drop-ins in
  `/usr/lib/systemd/system/<unit>.d/50-btbench-audio-pipewire.conf`). Stopping the target stops
  PipeWire. The sockets are included, so a PipeWire client cannot socket-activate PipeWire in
  mode `none`.
- **Nothing is enabled on its own.** pipewire and wireplumber have
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

| codec | switch | library, license |
|---|---|---|
| SBC, SBC-XQ | always | sbc (LGPL-2.1+) |
| CVSD, mSBC (HFP) | always | sbc |
| LC3 (BAP), LC3-SWB (HFP) | always | liblc3 (Apache-2.0) |
| Opus (A2DP, PipeWire's vendor codec) | always | libopus (BSD) |
| FastStream | always | built in |
| G.722 (ASHA) | always | built in |
| aptX, aptX HD | `BTBENCH_EXTRA_CODECS = "1"` | libfreeaptx (LGPL-2.1+), recipe in meta-btbench |
| LDAC (encode only) | `BTBENCH_EXTRA_CODECS = "1"` | ldacbt / AOSP libldac (Apache-2.0), recipe in meta-btbench |
| AAC | `BTBENCH_AAC = "1"` | fdk-aac, `LICENSE_FLAGS = "commercial"` |

Both switches live in `yocto/meta-btbench/conf/btbench-device.conf`. Run `make image` afterwards.

- **aptX and LDAC are opt-in** because scarthgap's layers do not have them. The libraries
  implement codecs whose owners license them for products. Building them for a bench on your
  desk is your call, which is why they are off by default.
- **LDAC can only be sent.** No free decoder exists. Expect an ARM11 to manage LDAC's lower
  bitrates at best, and the 3 Mbaud UART to the BCM43430 limits the higher ones anyway.
- **AAC needs fdk-aac**, which OE flags `commercial`. With `BTBENCH_AAC = "1"`, the distro adds
  `commercial` to `LICENSE_FLAGS_ACCEPTED`. Accepting it is your decision.

**Which codecs are offered at runtime:** WirePlumber's `bluez5.codecs` list (below). Delete a
name to stop offering that codec.

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
  ALSA device: in mode `none` there would be no PipeWire to answer. Use
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
| `bluetooth.autoswitch-to-headset-profile` | `false` | upstream switches a device to HFP when anything records from it; on the bench every recorder is a measurement, and the switch would drop the A2DP stream being captured. Profiles (codecs) are chosen on purpose: the console's codec picker, `bench codec`, `wpctl set-profile` |

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
```

## Streams: playing to and listening from devices

btbenchd plays and captures audio itself (`/api/audio/streams`, the console's Audio → Play and
listen, `bench play`): a test signal, an internet radio or UPnP URL, or what one device sends, into any
sink the stack has, and/or to a browser that listens in. It uses the stack's own tools. In mode
`none` there is no Bluetooth audio to reach, but plain ALSA still works, so the loopback card is
there to play into and capture from:

| | PipeWire | none |
|---|---|---|
| play into a sink | `pw-cat --playback --target <node>` (fed a WAV on stdin) | `aplay -D hw:Loopback,0,0` |
| capture a source | `pw-cat --record --target bluez_input.…` (or a sink's monitor) | `arecord -D hw:Loopback,1,0` |
| decode a URL | `mpg123 -s` (MP3, ICY titles; https through its curl helper), else `ffmpeg` | the same |

Each stream runs one pump thread in btbenchd that moves 20 ms chunks from the source through the
gain and the level meter to the sink's stdin (a 16 KiB pipe: a gain or frequency change is heard
within ~100 ms) and to the listeners. What paces it is whatever is real-time in it: the sink
consuming, a capture producing, or, when it plays nowhere, a clock. In PipeWire the streams show
as `btbench-stream-<id>` in `wpctl status` and `pw-top`.

- **Tunnel.** A capture of one device into a sink of another: a phone playing to the board
  (A2DP sink) relayed to a speaker (the board as A2DP source). In PipeWire the phone's stream is
  a node `bluez_input.<addr>.…` once it plays; WirePlumber also links it to the default sink
  (the loopback card) as usual.
- **Rates.** A stream has one rate. PipeWire resamples on both ends; the loopback takes any.
- **Decoders.** `mpg123` (MP3: most internet radio) and `ffmpeg` (everything else: AAC streams,
  HLS, FLAC/WAV/M4A from a media server) are on the image. OE flags ffmpeg `commercial`; the
  distro accepts that flag for ffmpeg alone while `BTBENCH_FFMPEG` is `"1"`, the default —
  `BTBENCH_FFMPEG = "0"` in `btbench-device.conf` leaves it out, and then only MP3 plays (the
  Listen page says so for anything else). A board flashed without them gets them without a
  reflash: `make pkg ARGS=mpg123`, `make pkg ARGS=ffmpeg`.
- **CPU.** The Zero W has one core: a decoder plus pw-cat into an SBC sink is the heaviest kind
  of stream, a generator into a sink the lightest. Watch the console's CPU readout (or Traffic →
  Graphs) while one runs. At most four streams run at once, and one that plays nowhere ends after
  30 s with nobody listening.

## bluetoothd

- **Version.** BlueZ comes from the fork at `BTBENCH_BLUEZ_SRCREV` (`yocto/conf/versions.conf`),
  the revision `src/bluez` starts from (see `docs/dev.md`).
- **The bench's fork.** The recipe fetches from `github.com/AdUki/bluez`, whose branch `bench`
  has the bench's commits on top of upstream (docs/dev.md "The forks"): AVRCP browsing in the
  target role, for the board's player (below).
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
  with PipeWire's native HFP for the same role. To work on it, set
  `BLUETOOTHD_NOPLUGIN=` (empty) and use audio mode `none`, or remove `hfp_hf` from
  WirePlumber's roles.

**LE Audio on the Zero W.** The BCM43430A1 is a Bluetooth 4.1 controller without ISO channels.
To exercise BAP, use a USB dongle that has them (with the gadget off, `BTBENCH_GADGET = "off"`)
or a virtual controller: `btvirt -l2` or a vhci device.

## The board's player

When the board plays to something (a car, a speaker, headphones: the board as A2DP source), that
device is the AVRCP controller and the board the target: it shows the source's player, its track
and state, and sends play, pause, next and previous. btbenchd registers a player of its own for
that, `/org/btbench/player`, with BlueZ on every adapter (`org.bluez.Media1.RegisterPlayer`), in
place of the stand-in PipeWire can register (`bluez5.dummy-avrcp-player`, off by default), which
has a play state and nothing else.

- **Its library.** *Test signals*: sines at 100 Hz, 440 Hz, 1 kHz and 10 kHz, 1 kHz left with
  2 kHz right, a 10 s sweep from 20 Hz to 20 kHz (a track that ends, so the remote sees the next
  one start), pink and white noise, silence, and a *Levels* folder (1 kHz from 0 to −60 dBFS).
  *Radio*: the stations of the Listen page (`/data/btbench/radio.json`, read at start).
- **Playing.** Playing an entry starts an audio stream (above) into the Bluetooth sink: PipeWire's
  default sink when it is a Bluetooth one, else the first Bluetooth sink, else the default sink.
  The folder the entry is in (or the folder played) becomes the now playing list; next and
  previous go round it, and a track that ends (the sweep, a station that drops) is followed by
  the next. Pause and stop end the stream; play starts the track again. A station's ICY title
  becomes the track's title, the station its album.
- **Browsing.** The remote can browse the library, the now playing list and search results
  (names, by every word), play an entry and add one to the now playing list. Upstream BlueZ
  rejects these commands in the target role; the bench's fork answers them (the image's BlueZ,
  and `make bluez` from `src/bluez`). With an
  unpatched bluetoothd the player still shows, plays and changes tracks, without the library.
- **Off.** `PUT /api/media/local-player {"enabled": false}` (the console's *Offer it*) unregisters
  it, kept in `/data/btbench/player.json`. BlueZ's target then has no player, as in upstream's
  default.

To try it without a car: two virtual controllers, `btvirt -l2` on the board, are two more
adapters for bluetoothd (hci1, hci2), and the player is registered on both. Power both on, pair
hci1 with hci2 (`btmgmt --index 1 pair -c 3 <hci2's address>`), connect AVRCP from hci1
(`ConnectProfile` 0000110e-…), and hci1's device object gets `avrcp/player0`: the board's player
as a controller sees it, browsed with its `org.bluez.MediaFolder1` (`ChangeFolder`, `ListItems`,
`Search`) and played with an item's `Play`. The virtual devices offer A2DP too, and WirePlumber
was seen spinning on them (pw-dump timing out) until it was restarted: remove them
(`pkill btvirt`) and `systemctl restart wireplumber` when done.

## The capture ring

`btbench-btsnoop.service` is enabled by default. It runs `/usr/bin/btbench-btsnoop`, a loop
around `btmon -w`:

- **Files.** Each one is `/tmp/btsnoop/hci-<YYYYmmdd-HHMMSS>.btsnoop`. The newest is the one
  being written. They are in RAM and gone at reboot: the SD card is kept for settings
  (docs/system.md "The SD card"). Download one to keep it.
- **Rotation.** When the current file passes `ROTATE_SIZE`, a new btmon starts on the next file
  and the old one is stopped a second later. There is no gap: a few packets land in both files.
  Upstream btmon cannot rotate by itself.
- **Pruning.** Only `ROTATE_COUNT` files are kept, oldest deleted first.
- **Settings.** Put them in `/data/btbench/btsnoop.env`:

  | variable | default | |
  |---|---|---|
  | `ROTATE_SIZE` | `8M` | bytes, or with a K/M/G suffix |
  | `ROTATE_COUNT` | `4` | files kept, including the one being written (32 MB of RAM by default) |
  | `CHECK_INTERVAL` | `5` | seconds between size checks |
  | `BTSNOOP_DIR` | `/tmp/btsnoop` | `/data/...` puts the ring back on the SD card |
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
| codec libraries | `recipes-multimedia/{libfreeaptx,ldacbt}/` |
| modes, capture ring | `recipes-multimedia/btbench-audio/` (units, drop-ins), `tools/target/btbench-{audio,btsnoop}` |
| everything for the image | `recipes-multimedia/packagegroups/packagegroup-btbench-audio.bb` |
