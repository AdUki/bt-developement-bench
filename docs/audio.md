# Audio and BlueZ on the board

The board runs bluetoothd from BlueZ master with PipeWire and WirePlumber on top of it: A2DP both
ways, HFP both ends, LE Audio (BAP) where the controller has ISO channels, and the codecs PipeWire
has. The streams, the codec picker and the board's own AVRCP player are all built on PipeWire, so
it is always there. An always-on btmon ring records every HCI packet to `/tmp/btsnoop`, in RAM.

```
btbench-audio status                 # {"stack":"pipewire","services":{"pipewire":"active",...}}
btbench-audio restart                # restart PipeWire and WirePlumber
btbench-audio restart --restart-bt   # and bluetoothd
```

`bench audio restart` and the console's Audio → PipeWire view call the same script.

## The stack's units

- **One target.** `btbench-audio-pipewire.target` wants `pipewire.socket`,
  `pipewire-manager.socket`, `pipewire.service` and `wireplumber.service`.
- **PipeWire's units are `PartOf=` it** (drop-ins in
  `/usr/lib/systemd/system/<unit>.d/50-btbench-audio-pipewire.conf`), so restarting the target
  restarts all of them.
- **Nothing is enabled on its own.** pipewire and wireplumber have
  `SYSTEMD_AUTO_ENABLE = "disable"`. At boot, `btbench-audio-select.service` runs
  `btbench-audio boot`, which starts the target.

`btbench-audio restart` is the cure for a WirePlumber that stopped answering (it has been seen
spinning after Bluetooth links vanished all at once):

1. Disconnects the connected devices that use an audio profile (A2DP, HFP/HSP, ASCS/PACS/BASS),
   so they reconnect to the new PipeWire's endpoints instead of keeping a transport whose owner is
   about to disappear.
2. Runs `systemctl restart btbench-audio-pipewire.target`.
3. With `--restart-bt`, it also restarts bluetoothd, which clears whatever state the old stack
   left on the adapter and the devices.

A device that is not connectable itself (a speaker kit that only pages) has to reconnect from its
side; give PipeWire a few seconds to register its endpoints first, or the first try is refused.

To work with your own endpoint code or BlueZ's `test/` scripts, `systemctl stop
btbench-audio-pipewire.target` for the time: devices would otherwise connect to whichever
registered first. A restart of the target, or a reboot, brings PipeWire back.

To do without audio altogether — a board that is only a BLE remote or a GATT peripheral — switch
the stack off: *Audio on* in Audio → PipeWire, `bench audio off`, or `btbench-audio off` on the
board. That stops PipeWire and WirePlumber (dropping the audio links first) and keeps them stopped
at boot (`AUDIO_STACK=off` in `/data/btbench/audio.env`), until `bench audio on` or the switch.

## Codecs and licensing

| codec | switch | library, license |
|---|---|---|
| SBC, SBC-XQ | always | sbc (LGPL-2.1+) |
| CVSD, mSBC (HFP) | always | sbc |
| LC3 (BAP), LC3-SWB (HFP) | always | liblc3 (Apache-2.0) |
| Opus (A2DP, PipeWire's vendor codec) | always | libopus (BSD) |
| FastStream | always | built in |
| G.722 (ASHA) | always | built in |
| aptX, aptX HD | `BTBENCH_EXTRA_CODECS` (on; `"0"` leaves them out) | libfreeaptx (LGPL-2.1+), recipe in meta-btbench |
| LDAC (encode only) | `BTBENCH_EXTRA_CODECS` (on) | ldacbt / AOSP libldac (Apache-2.0), recipe in meta-btbench |
| AAC | `BTBENCH_AAC = "1"` | fdk-aac, `LICENSE_FLAGS = "commercial"` |

Both switches live in `yocto/meta-btbench/conf/btbench-device.conf`. Run `make image` afterwards.

- **aptX and LDAC are in by default**, from meta-btbench's own recipes (scarthgap's layers do not
  have them). The libraries are free software, but they implement codecs whose owners license
  them for products: a build that ships to someone else may want `BTBENCH_EXTRA_CODECS = "0"`.
- **LDAC can only be sent.** No free decoder exists. Expect an ARM11 to manage LDAC's lower
  bitrates at best, and the 3 Mbaud UART to the BCM43430 limits the higher ones anyway.
- **AAC needs fdk-aac**, which OE flags `commercial`. With `BTBENCH_AAC = "1"`, the distro adds
  `commercial` to `LICENSE_FLAGS_ACCEPTED`. Accepting it is your decision.

- **LC3plus is not in the build.** PipeWire can use it (`lc3plus_hr`), but the only source is
  ETSI's reference code (TS 103 634), licensed solely under ETSI's IPR policy rather than an
  open-source licence, and its build makes a command-line tool, not the library PipeWire links.
  LC3 itself (BAP, and HFP's LC3-SWB) is in every build.

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
- **Priorities.** PipeWire and WirePlumber run at nice -11, and WirePlumber's Lua (a rescan after
  every volume change costs the Zero W ~250 ms) used to starve a btbenchd stream's nice-0 pw-cat
  into xruns. A stream's pump, decoder and pw-cat now run at nice -15; what only records or shows
  runs at nice 10: btbenchd's sampler, D-Bus log, HCI monitor, pw-top, journal view and pw-dumps,
  and btmon (btbench-btsnoop.service). Kernel Bluetooth debug messages (`btbench-btdebug`, the
  `driver` area especially) log hundreds of lines a second while audio streams and take most of
  the core with journald: keep them off unless they are what is being looked at.
- **What is not built.** pipewire-pulse (packaged apart, not installed), JACK, video,
  GStreamer, network audio and any desktop integration.
- **ALSA plugin.** The pipewire-alsa plugin is installed, but PipeWire is *not* made the default
  ALSA device: while it is stopped (for your own endpoint code) there would be no PipeWire to
  answer. Use
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
| `override.bluez5.roles` | `[ a2dp_sink a2dp_source bap_sink bap_source bap_bcast_sink bap_bcast_source asha_sink hfp_hf hfp_ag ]` | every role except HSP (left out as upstream does): A2DP, LE Audio unicast and broadcast, each both ways, and ASHA. Setup turns roles off |
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
sinks the stack has (two Bluetooth speakers at once: `bench play tone 440 A+B`), and/or to a
browser that listens in. One test signal or URL plays at a time, a new one in its place; what
devices send the board can be captured beside it, several at once. It uses the stack's own tools. In mode
`none` there is no Bluetooth audio to reach, but plain ALSA still works, so the loopback card is
there to play into and capture from:

| | PipeWire | none |
|---|---|---|
| play into a sink | `pw-cat --playback --target <node>` (fed a WAV on stdin) | `aplay -D hw:Loopback,0,0` |
| capture a source | `pw-cat --record --target bluez_input.…` (or a sink's monitor) | `arecord -D hw:Loopback,1,0` |
| decode a URL | `mpg123 -s` (MP3, ICY titles; https through its curl helper), else `ffmpeg` | the same |

Each stream runs one pump thread in btbenchd that moves 20 ms chunks from the source through the
gain and the level meter to each sink's stdin (a 16 KiB pipe each: a gain or frequency change is
heard within ~100 ms) and to the listeners. What paces it is whatever is real-time in it: the sink
that takes the chunk first, a capture producing, or, when it plays nowhere (or every sink has
stalled), a clock. A sink behind the others gets what fits and loses the rest, so a speaker that
stops taking audio does not stop the others. In PipeWire the streams show as `btbench-stream-<id>`
in `wpctl status` and `pw-top`, a second sink's as `btbench-stream-<id>-2`.

- **Tunnel.** A capture of one device into a sink of another: a phone playing to the board
  (A2DP sink) relayed to a speaker (the board as A2DP source). In PipeWire the phone's stream is
  a node `bluez_input.<addr>.…` once it plays. WirePlumber does not link it anywhere by itself on
  the bench (`node.autoconnect = false` on `bluez_input.*` in `50-btbench.conf`; Setup's
  "Received audio plays by itself" undoes it): what plays is what was picked. Nor does it link
  what the board sends a device where PipeWire's node for that is a stream (the HFP hands-free
  unit's microphone, an LE Audio acceptor's source, `bluez_output.…`), which would otherwise
  record the default source: the board sends what the route plays there, or nothing. Upstream links every
  one to the default sink, so two phones, a received broadcast and the radio all played at once.
  `bluez5.media-source-role = "input"` would not do: it covers A2DP only, PipeWire decides LE
  Audio's node class itself. A received A2DP node is run unlinked all the same
  (`node.always-process`): a node nobody runs never acquires its transport, and a phone whose
  stream start is not taken stops sending while its app goes on "playing". LE Audio's are not: the
  node of a stream a peer started looks the same as a microphone the board would open itself, and
  run, every connected earbud's microphone would hold a CIS (which also left a BE200 with no room
  to accept another one as peripheral).
- **The route** (`/api/audio/route`, Listen): one input at a time — the board's own stream, or a
  capture the route makes of the device picked, waited for up to 15 s until its node appears — on
  the outputs picked, which are that stream's sinks. Changing the input moves no output; changing
  an output touches only that sink. Two single-channel broadcasts, a left and a right one, are one
  input: two mono `pw-cat --record`, one each channel, interleaved by the pump (`target_right`); the
  left one paces, the right one is padded with silence when short, and cut back to its newest 20 ms
  once more than 200 ms of it waits (what stays queued is an offset between the two). A coordinated set (a pair of earbuds, or an LE Audio device's left and right
  streams) is recorded from and played into PipeWire's combined node of it.
- **Roles that are streams.** Where PipeWire's Bluetooth node is a stream rather than a device —
  what a device sends the board, and what the board sends as HFP hands-free unit or LE Audio
  earbud (`bluez_output.…`, `Stream/Input/Audio`, which PipeWire would fill from the default
  microphone) — pw-cat records from it or plays into it all the same, by name. A call's audio and
  an LE Audio microphone on the gateway side are WirePlumber loopbacks (`bluez_input.AA:BB:…`) of an
  internal node; the endpoint list gives them that node's profile and codec.
- **Rates.** A stream has one rate. PipeWire resamples on both ends; the loopback takes any.
- **Decoders.** `mpg123` (MP3: most internet radio) and `ffmpeg` (everything else: AAC streams,
  HLS, FLAC/WAV/M4A from a media server) are on the image. OE flags ffmpeg `commercial`; the
  distro accepts that flag for ffmpeg alone while `BTBENCH_FFMPEG` is `"1"`, the default —
  `BTBENCH_FFMPEG = "0"` in `btbench-device.conf` leaves it out, and then only MP3 plays (the
  Listen page says so for anything else). A board flashed without them gets them without a
  reflash: `make pkg ARGS=mpg123`, `make pkg ARGS=ffmpeg`.
  Which one a URL gets: its extension when it has one that says (`.mp3`, `.aac`, `.ogg`, ...);
  else (a station's `/aac/96`, `/live`) btbenchd reads the stream's first few KB and its
  Content-Type first: MPEG audio frames go to mpg123, AAC (ADTS), Ogg, FLAC or MP4 to ffmpeg. The
  bytes win over the header, as Shoutcast servers send AAC as `audio/mpeg`.
- **CPU.** The Zero W has one core: a decoder plus pw-cat into an SBC sink is the heaviest kind
  of stream, a generator into a sink the lightest. Watch the console's CPU readout (or Traffic →
  Graphs) while one runs. At most four streams run at once; one that plays nowhere still runs
  until it is stopped.
- **Real time.** The capture and the sink (pw-cat, or arecord/aplay) run SCHED_FIFO 30, the
  decoder at the pump's nice -15. pw-cat moves each graph cycle (512 frames, 10.7 ms) in its main
  loop: at a nice value the Zero W had it late about once a second, and PipeWire dropped that cycle
  (a click, and a listening browser's buffer running dry minutes later).

## LE Audio broadcasts (Auracast)

PipeWire's own way to broadcast is a fixed list of BIGs in WirePlumber's config
(`bluez5.bcast_source.config`), read once when WirePlumber starts. The bench does without it:
btbenchd calls `SetConfiguration` on the adapter's broadcast PAC (`/org/bluez/hci0/pac_bcast0`, which
BlueZ makes for PipeWire's broadcast source endpoint) with an LC3 configuration and a BIG's QoS
(BAP's broadcast presets), BlueZ hands PipeWire the transport, and PipeWire makes a sink of it
(`bluez_output.00_00_00_00_00_00.N.1`). The BIG goes on air when something plays into the sink and
off when the transport is cleared (`ClearConfiguration`), so a broadcast is started and stopped like
a stream (`/api/le-audio`, Audio → LE Audio). The kernel puts the BIG on the first advertising set it
finds free and advertises the Broadcast ID only: there is no Broadcast Name.

Receiving is BlueZ's: while the adapter scans, BAP syncs to a broadcaster's periodic advertising
for a moment, reads its BASE and makes an endpoint and a transport per BIS
(`dev_.../sid0/bis1/fd0`). `MediaTransport1.Select` marks one to receive; PipeWire acquires it, BlueZ
syncs to the BIG, and the audio is a `bluez_input` stream, which the route records when the
broadcast is its input (WirePlumber links it nowhere by itself).

Both need an adapter whose controller has the ISO broadcaster and sync receiver features (`btmgmt
info`: `iso-broadcaster sync-receiver`): the VIM3L's BE200 (with the DDC entry of
btbench-firmware-ibt that switches them on), not the boards' own BCM chips.

What the bench's forks change for it:

- **BlueZ** (`src/device.c`, `profiles/audio/bap.c`): a Broadcast Audio Announcement probes the
  broadcast source profile on a device whose services are resolved too (a bonded LE Audio device
  that broadcasts from its public address was never found), again when the Broadcast ID changes,
  and again once BAP dropped the session of a broadcast that ended (or whose PA sync could not
  start: a stored broadcaster is probed before the adapter is up).
- **BlueZ** (`src/adapter.c`, `src/device.c`): a broadcast that an LE scan of 5 s or more (counted
  from the scan asked for, even when it joins one already running) did not hear has ended: its
  Broadcast Audio Announcement leaves the device's `ServiceData` and its BISes' endpoints and
  transports go. BlueZ keeps the advertising data of a device that stays (a bonded broadcaster), so
  a broadcast that stopped was listed for good, rescan or not. A scan that never ran (the
  controller still busy answers a start late) decides nothing: the next one does.
- **BlueZ** (`profiles/audio/bass.c`): no PAST flag on every BASS peer. The kernel reads it when it
  syncs to that device's broadcast and first waits 20 s for the device to transfer the sync, so
  each BASE read of a connected LE Audio device that also broadcasts (the Realtek) held the
  controller for 20 s, and scans asked for meanwhile never ran. An assistant that wants PAST asks
  for it in its Add Source, where the flag is still set.
- **BlueZ** (`profiles/audio/transport.c`): a broadcast transport keeps its own copy of the
  Broadcast Code. It copied the stream's QoS with the code's pointer, which the setup frees when the
  stream is cleared; a GetManagedObjects in between (btbenchd reads the tree on every change)
  appended freed memory and libdbus aborted bluetoothd. Clearing a broadcast PipeWire still holds
  has more of this (the transport stays half gone, and the next Acquire segfaults), so btbenchd
  clears a broadcast only off air.
- **PipeWire**: upstream's "bluez5: deal with asynchronous device and node removal" (after 1.6.9):
  a broadcast cleared while its sink still held the transport crashed WirePlumber (the SPA device
  kept a pointer to the freed Bluetooth device).

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
  `BLUETOOTHD_NOPLUGIN=` (empty) and stop `btbench-audio-pipewire.target`, or remove `hfp_hf`
  from WirePlumber's roles.

**Receiving A2DP on the Zero W.** The BCM43430A1 shares one radio between Wi-Fi and Bluetooth,
and as the peripheral of a link it gets too little of it: another board streaming aptX HD
(576 kbit/s) to it got about half its packets through, the rest dropped at the sender for want of
credits. As central the same link carries all of it. So the Zero W's controller asks for the central
role on every link it accepts (`BT_CENTRAL=yes` in board.env: btbenchd sets the kernel's link mode,
`hciconfig hci0 lm` says `ACCEPT CENTRAL`); the links it opens itself are central anyway. aptX HD
still lost a packet now and then with the Wi-Fi awake (a ~100-200 ms gap, 0.7-1.3 a minute over
3 min runs); with the Wi-Fi in power save (`WIFI_POWERSAVE=on` in board.env, btbench-wifi sets
it) 0-0.3 a minute. aptX (384 kbit/s) arrived whole in a minute with the Wi-Fi awake.

**LE Audio on the Zero W.** The BCM43430A1 is a Bluetooth 4.1 controller without ISO channels.
To exercise BAP, use a USB dongle that has them (with the gadget off, `BTBENCH_GADGET = "off"`)
or a virtual controller: `btvirt -l2` or a vhci device.

**LE Audio unicast (VIM3L, Intel BE200).** The board sends to an LE Audio earbud or speaker:
pair it over LE (`bench pair`; a device with no classic profile is connected over its LE bearer),
then pick the profile — PipeWire chooses `bap-duplex` when the device also has a source PAC, which
enables its microphone too; `bench codec ADDR bap-sink` for music only — and play into
`bluez_output.ADDR.1`. The board receives from a phone: `bench adv le-audio` (Devices → LE →
*LE Audio receiver* in the console) announces ASCS, PACS and CAS as an earbud does, the phone pairs
with *btbench-…* and, with LE Audio switched on in the device's settings there, streams.
Verified 2026-10-20: the board to an RTL8763EFL (48 kHz stereo LC3, one CIS, 253 kbit/s). The
other way an S25 configures and enables the board's sink ASE (BlueZ accepts the CCID List Android
puts in Enable since `bap: accept CCID List metadata`), but the BE200 as CIS peripheral refuses the
phone's CIS (`PARAMETER_OUT_OF_MANDATORY_RANGE`, never reported to the host: no CIS Request); the
same was seen with a Synaptics central.

**Auracast on the BE200.** Its firmware does broadcast, but linux-firmware's DDC leaves it off:
the controller's LE features come from DDC entry 0x0126, without Isochronous Broadcaster and
Synchronized Receiver, so it has no BIG commands and only PA-syncs. The image's .ddc
(btbench-firmware-ibt) adds an entry with both bits set, as Intel's Windows driver does when
its `EnableLEAudioBroadcast` is on. The kernel applies it only when it loads the firmware, i.e. from
power-up: not after a USB re-enumeration that finds the firmware already running. On a running
adapter, write the entry by hand and power-cycle the hci to re-read the features:
`hcitool -i hci0 cmd 0x3f 0x08b 0a 26 01 ff f9 01 ff be 00 00 00`, then `btmgmt power off` / `on`.
`btmgmt info` then lists `iso-broadcaster sync-receiver`. Restart bluetoothd and WirePlumber so
they register broadcast endpoints. A broadcast source also needs `bluez5.bcast_source.config` in
WirePlumber's `monitor.bluez.properties`, which makes PipeWire create the
`bluez_output.00_00_00_00_00_00.N.1` sink. Verified 2026-10-20: a 48_2_1 broadcast (one BIS, 83
kbit/s) from the board, received by an RTL8763EFL (PA sync, BIG sync, audio playing). The other way
is not verified yet. The Realtek broadcasts from its own public address, and the board, bonded to
that address, treats it as the unicast device and creates no broadcast endpoint.

Two things that look like LE Audio faults and are not: a BE200 whose LE Create CIS answers
*Command Disallowed* after days up was stuck on an earlier CIS; `btmgmt power off` / `on` (an HCI
reset) cleared it. And when the adapter is unplugged or re-enumerated, PipeWire does not register
its LE endpoints with the new one: the board's PACS is empty (no PAC, locations and contexts 0) and
phones find nothing to stream to until WirePlumber restarts.

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
- **Playing.** Playing an entry makes it the board's input (the route, "The route" above): it
  takes the place of whatever the board played, and plays on the outputs picked in Listen's Play
  to. With none on, the Bluetooth sink the player would pick is turned on there (PipeWire's
  default sink when it is a Bluetooth one, else the first Bluetooth sink, else the default sink),
  so a car that pressed play hears it. The folder the entry is in (or the folder played) becomes the now playing list; next and
  previous go round it, and a track that ends (the sweep, a station that drops) is followed by
  the next (repeat one: by itself; repeat all: round again). Shuffle puts the list itself in a
  random order, the track playing first (off: back as listed), so the now playing list a remote
  browses is the order it plays in; repeating all, each round is a new order. Its buttons do what Listen's do on the board's stream: pause holds the stream where it is (a station, a test signal) and play plays it on, stop ends it and leaves nothing playing (no track, no now playing list), whoever stopped it: the remote, or Listen. A station's ICY title
  becomes the track's title (its "Artist - " part the artist), the station its album.
- **Other streams.** Playing nothing itself, the player tells the remote about the board's input
  when it is the board's own (a station, a media server's folder, a test signal picked in Listen),
  wherever it plays, and its buttons act on that stream the same way; audio the board receives from a device is not the board's to pause: its
  track with the artist, album and length the media server gave, the position, and the stream's
  repeat and shuffle, which the remote can change. The remote's buttons act on that stream. A
  URL stream's playlist is then the now playing list the remote browses (items
  `<player>/item/s<stream>_<n>`, the track playing its trackid); playing one of them jumps the
  stream to it. A change to that list (shuffle, which reorders it, tracks added or removed) goes
  to the remote as a changed now playing list (AVRCP NOW_PLAYING_CONTENT_CHANGED), and the remote
  reads it again.
- **Browsing.** The remote can browse the library, the now playing list and search results
  (names, by every word), play an entry and add one to the now playing list. Upstream BlueZ
  rejects these commands in the target role; the bench's fork answers them (the image's BlueZ,
  and `make bluez` from `src/bluez`). With an
  unpatched bluetoothd the player still shows, plays and changes tracks, without the library.
- **Cover art.** A station's cover (the image its ICY StreamUrl names), or a media server's
  track's (its playlist item's `art`), reaches the remote as
  AVRCP 1.6 cover art, the way a phone's reaches the board: btbenchd's BIP responder
  (`obex/cover_art.h`) listens on a dynamic L2CAP PSM in ERTM, the player registers it as
  `ObexPort`, and the fork's target record then offers cover art there (feature bit 8, an
  L2CAP + OBEX entry in its additional protocols). While a controller is connected to the
  responder each track carries an image handle (`bluez:ImgHandle` in the player's Metadata), and
  the controller pulls the image, its properties or its 200×200 thumbnail (made with ffmpeg) by
  it; images are fetched with curl once per URL into `/tmp/btbench-cover`. A controller paired
  before the record changed has it cached: pair again. Upstream BlueZ ignores `ObexPort`, and the
  remote sees no cover.
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
