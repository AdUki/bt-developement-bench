# Interfaces between the pieces

Who provides what to whom. Every piece of the bench is written against this page; change it here
first when an interface moves.

## Ownership (directories)

| Area | Owns |
|---|---|
| system | `yocto/meta-btbench/recipes-core/{images,btbench-base,base-files}/`, `recipes-core/packagegroups/packagegroup-btbench-tools.bb`, `yocto/meta-btbench/wic/`, `tools/target/{btbench-wifi,btbench-gadget,btbench-bdaddr,btbench-data}`, `mk/system.mk`, `docs/system.md` |
| audio + BlueZ | `recipes-connectivity/bluez5/`, `recipes-multimedia/{pipewire,wireplumber,libfreeaptx,ldacbt,btbench-audio,packagegroups}/`, `tools/target/{btbench-audio,btbench-btsnoop}`, `tools/dev/{bbvars,clangd-gen,upstream}`, `mk/dev.mk`, `docs/{audio,dev,upstream}.md` |
| kernel | `recipes-kernel/`, `dynamic-layers/raspberrypi/recipes-{kernel,bsp}/`, `dynamic-layers/meson/recipes-bsp/`, `boards/dts/`, `boards/*.cfg`, `tools/dev/mkdtb`, `tools/target/btbench-kernel`, `tools/target/btbench-btdebug`, `mk/kernel.mk`, `docs/kernel.md` |
| btbenchd | `app/` (except `app/src/hci/`, `app/tests/hci/`), `recipes-btbench/`, `tools/bench`, `tools/fake-bluez/`, `mk/app.mk`, `docs/{api,web}.md` |
| HCI monitor | `app/src/hci/`, `app/tests/hci/`, `docs/monitor.md` |

## Image composition

`btbench-image` installs: `packagegroup-btbench-tools` (system), `packagegroup-btbench-audio`
(audio + BlueZ: bluez5 and its tools, pipewire, wireplumber, btbench-audio,
btbench-btsnoop), `packagegroup-btbench-kernel` (kernel: kernel modules, btbench-kernel,
libubootenv-bin), `btbench-base` (system), `btbenchd` (btbenchd).

## Build-time variables (auto.conf / btbench-device.conf)

`BTBENCH_BOARD`, `BTBENCH_KERNEL` (next|rpi), `BTBENCH_KARCH`, `BTBENCH_KIMAGE`,
`BTBENCH_KDEFCONFIG`, `BTBENCH_DTBS`, `BTBENCH_DTS_EXTRA` (absolute path or empty),
`BTBENCH_KFRAGMENT_BOARD` (the board's kernel fragment, `boards/<board>.cfg`: absolute path or empty),
`BTBENCH_TRY_METHOD`, `BTBENCH_GADGET_UDC`, `BTBENCH_BT_HCI`, `BTBENCH_BT_CENTRAL` (yes|empty), `BTBENCH_WIFI_POWERSAVE` (on|empty),
`BTBENCH_ROOT` (the repo checkout),
`BTBENCH_{BLUEZ,PIPEWIRE,WIREPLUMBER,LINUX}_{SRCREV,PV}` (yocto/conf/versions.conf).

Board conf (`yocto/conf/boards/<board>.conf`): `BTBENCH_WKS` (the card layout in `wic/`),
`BTBENCH_BOARD_FIRMWARE` (packages the image installs for the board's radios when the BSP has
none), `BTBENCH_CMDLINE` (the kernel command line, where U-Boot's boot.scr sets it: Amlogic).

Device: `BTBENCH_HOSTNAME`, `BTBENCH_ROOT_PASSWORD`, `BTBENCH_SSH_PUBKEY` (path on the build
host), `BTBENCH_WIFI_{SSID,PSK,COUNTRY}`, `BTBENCH_AP_PSK`, `BTBENCH_GADGET` (ecm|ncm|off),
`BTBENCH_EXTRA_CODECS` (0|1), `BTBENCH_AAC` (0|1),
`BTBENCH_HTTP_PORT`, `BTBENCH_FFMPEG` (0|1: ffmpeg on the image, for streams that are not MP3).

Recipes that ship files from the repo (tools/target, tools/dev, boards/dts) reach them with
`FILESEXTRAPATHS:prepend := "${BTBENCH_ROOT}/tools/target:"` (or the dir they need) and list them
in `SRC_URI` as `file://...`, so bitbake tracks their checksums.

## On the board

- `/etc/btbench/board.env` — shell `KEY=VALUE`: `BOARD KERNEL KARCH KIMAGE DTBS TRY_METHOD GADGET_UDC BT_HCI
  BT_CENTRAL WIFI_POWERSAVE` (`BT_CENTRAL=yes`: btbenchd makes the board's own controller central on the
  links it accepts; `WIFI_POWERSAVE=on`: btbench-wifi leaves the Wi-Fi in power save, else off).
- `/etc/btbench/device.env` — `HOSTNAME AP_PSK WIFI_COUNTRY GADGET HTTP_PORT`.
- `/data` — ext4 partition (label `data`), mounted at boot, grown to fill the card at first boot.
  - `/data/btbench/bluetoothd.env` — `BLUETOOTHD_ARGS="-d ..."` (no `-n`: under systemd it only duplicates
    journal lines; the System page's) and `BLUETOOTHD_NOPLUGIN` — Setup's: `--noplugin=<plugins off>`
    (`hfp` by default: BlueZ's own HFP plugin would fight PipeWire for the HF role) and, once a
    main.conf setting is changed, `--configfile=/data/btbench/main.conf`. Both are read by the
    bluetooth.service drop-in; each writer replaces its own variable and keeps the other lines.
  - `/data/btbench/setup.json` — Setup's state: `{plugins_off, roles_on, obex_off, settings}`.
  - `/data/btbench/main.conf` — written by btbenchd from the image's `/etc/bluetooth/main.conf` with
    Setup's settings applied (the image's file is never edited); absent while none is set.
  - `/data/btbench/wireplumber-profiles.conf` — written by btbenchd: the Bluetooth roles
    (`override.bluez5.roles`) and Setup's WirePlumber settings, on top of the image's
    `50-btbench.conf`. WirePlumber reads it through `/etc/wireplumber/wireplumber.conf.d/60-btbench-profiles.conf`,
    a link to it (btbench-base; the root is read-only); btbench-data seeds it with a comment.
  - `/tmp/logs/` — btbench-logd (btbench-logs.service): the journal as one file per program, in that
    program's own format (for clog, `app/clog`): `bluez.log`, `pipewire.log`, `wireplumber.log`,
    `wpasupplicant.log`, `btbenchd.log`, `kernel.log`, `system.log` (the rest); each rotated once
    (`.1`) at 2 MiB; `btsnoop` → `/tmp/btsnoop`. RAM (tmpfs), gone at reboot, like the journal.
    `bluez-dbus.log` is btbenchd's, not logd's: BlueZ's D-Bus traffic, a message a line (dbus_log.h),
    rotated the same way.
  - `/data/btbench/hostname` — the board's name given in the console (`PUT /api/system/hostname`),
    one line, a DNS label; btbench-hostname makes it the hostname, with no MAC after it.
  - `/data/btbench/btsnoop.env` — `ROTATE_SIZE`, `ROTATE_COUNT` for the btmon ring.
  - `/data/btbench/radio.json` — btbenchd's radio station list, `{"stations":[{"name","url"}]}`.
  - `/data/btbench/route.json` — the route's outputs (Listen's "Play to"), `{"outputs":[{"type",
    "target"}]}` as a stream's sinks; the input is not kept.
  - `/data/btbench/hid.json` — the HID host's settings, `{"media_keys": true}`.
  - `/data/btbench/hid-remote.json` — the HID remote's, `{"enabled": true, "name": "btbench remote"}`.
  - `/data/btbench/player.json` — whether btbenchd offers its AVRCP player, and the repeat and
    shuffle of its own now playing list: `{"enabled": true, "repeat": "off", "shuffle": false}`.
  - `/data/btbench/bluetooth.json` — btbenchd's agent and what `PUT /api/bluetooth` set on each
    adapter, by hci name: `{"agent": "auto", "agent_capability": "KeyboardDisplay", "adapters":
    {"hci0": {"powered", "alias", "pairable", "discoverable", "discoverable_timeout_s"}},
    "primary": ""}` (adapter keys only once set). Applied to an adapter each time BlueZ shows it: at boot, after a bluetoothd
    restart. A discoverable with a timeout is kept as off.
  - `/data/btbench/obex.json` — the OBEX server's push policy, `{"policy": "auto|ask|reject"}`.
  - `/data/btbench/wireplumber/` — WirePlumber's state (the profile picked per device, routes,
    volumes), bound over `/var/lib/wireplumber` by btbench-data. A device removed in BlueZ
    (`DELETE /api/bluetooth/devices/{addr}`) loses its entry in `default-profile` (btbenchd, through
    WirePlumber's `default-profile` metadata).
  - `/data/btbench/factory-reset` — written by `POST /api/system/factory-reset`; at the next boot
    btbench-data empties `/data/btbench` except `ssh/`, `timesync/` and (unless the file says
    `wifi`) `wifi/`, and `/data/bluetooth` when it says `pairings`, then removes it.
  - `/tmp/btbench/obex/` — OBEX files and obexd's root, in RAM (btbenchd `--volatile-dir`): `in/` (received, pulled; `in/cover/` cover art), `out/` (to send), `shared/`.
  - `/data/btbench/ssh/` — sshd's host keys (btbench-sshkeys); sshd reads them through `/run/btbench/ssh`.
  - `/data/btbench/timesync/` — systemd-timesyncd's clock, bound over `/var/lib/systemd/timesync` (btbench-data).
  - `/data/btbench/wifi/` — wpa_supplicant networks.
  - `/data/bluetooth/` — persisted `/var/lib/bluetooth` (bonds survive reflashes of the rootfs? no: of deploys and kernel swaps; a reflash rewrites /data too).
  - `/tmp/btsnoop/` — the always-on btmon ring, in RAM (`hci-<YYYYmmdd-HHMMSS>.btsnoop`, newest is being written).
    btbenchd writes `<name>.ctx` beside each new file: the context it began with (the links,
    L2CAP/AVDTP signalling and ATT discovery still in force), btsnoop datalink 2001.
- `/` and `/boot` (the FAT boot partition) — mounted read-only; `btbench-rw open|close MOUNT...`
  remounts them for a writer (docs/system.md "The SD card"). `/data` holds settings only.
- `/run/btbench/` — runtime state (`wifi.json`, `wifi-scan.json`, `ssh` → the host keys,
  `bluetooth-cache/<adapter>` bound over each adapter's BlueZ cache).
- `/run/btbench-rw/` — btbench-rw's counts.
- Network: USB gadget `usb0` = `10.55.0.1/24` (DHCP server for the PC), Wi-Fi AP `10.42.0.1/24`,
  mDNS `<hostname>-<mac>.local` (btbench-hostname: the last three bytes of wlan0's MAC after
  `/etc/hostname`, a transient hostname; `HOSTNAME_MAC=0` in device.env keeps it plain; a name in
  `/data/btbench/hostname` replaces both), with the
console and ssh announced as DNS-SD `_http._tcp` and `_ssh._tcp` (`/etc/systemd/dnssd/`).
- `wlan0` is always the board's own Wi-Fi. A PCIe Wi-Fi card (iwlwifi: the rpi5's BE200) is renamed
  `wlpcie0` as it appears (btbench-base's `70-btbench-pcie-wifi.link`), and nothing configures it.

## Target scripts (`/usr/bin`, sources in `tools/target/`)

All print JSON on stdout for `status`/`scan`, exit non-zero with a one-line reason on stderr on
failure. btbenchd and `bench` call them; they must not need a terminal.

### btbench-audio
- `btbench-audio status` → `{"stack":"pipewire","services":{"pipewire":"active","wireplumber":"active"}}`
- `btbench-audio restart [--restart-bt]` — drop the audio links, restart btbench-audio-pipewire.target (and bluetoothd)
- `btbench-audio boot` — start the stack (btbench-audio-select.service)
- `btbench-audio get` → `pipewire` (kept for older callers)

### btbench-wifi
- `btbench-wifi status` → `{"state":"sta|ap|connecting|off","policy":"auto|sta|ap|off","ssid":"x"|null,"ip":"a.b.c.d"|null,"ap":{"ssid":"btbench-1a2b","ip":"10.42.0.1"},"networks":["x","y"],"last_error":null|"..."}`
- `btbench-wifi scan` → `[{"ssid":"x","signal_dbm":-52,"freq":2437,"security":"wpa2|wpa3|open|wep"}]` (cached results while in AP mode)
- `btbench-wifi add <ssid> [<psk>]`, `btbench-wifi remove <ssid>`, `btbench-wifi mode <auto|sta|ap|off>`
- `btbench-wifi run` — the state machine (btbench-wifi.service)

### btbench-kernel
- `btbench-kernel status` → `{"running":"7.3.0-rc2-btbench","slot":"good|test","method":"uboot-oneshot","pending_try":false,"good":{"version":"..."},"test":{"present":true,"version":"..."},"prev":{"present":false,"version":null}}`
- `btbench-kernel try` (reboots), `commit`, `rollback`
- Trial files: `/boot/bench/test/<KIMAGE>` (zImage/Image), `/boot/bench/test/board.dtb` (optional),
  modules in `/lib/modules/<version>/`. `/boot/bench/prev/` the same layout.

### btbench-btdebug
- `btbench-btdebug status` → `{"available":true,"flags":"pf","areas":[{"name":"l2cap","label":"L2CAP","on":0,"total":201,"kept":false}, ...]}`:
  per area, its dynamic-debug call sites and how many print; `total` 0 when its module is not loaded
- `btbench-btdebug on AREA...`, `off AREA...` (areas: `hci mgmt l2cap smp iso sco rfcomm bnep hidp driver all`),
  `keep [AREA...]` (on from boot: `/etc/modprobe.d/btbench-btdebug.conf`, `dyndbg` options per module;
  no area removes it)
- Needs `CONFIG_DYNAMIC_DEBUG` and debugfs (the `bt-debug.cfg` kernel fragment); the messages go to the
  kernel log.

## btbenchd

- Binary `/usr/bin/btbenchd`, unit `btbenchd.service`, www in `/usr/share/btbenchd/www`, runs as root.
- Flags: `--port N` `--www DIR` `--data-dir DIR` (settings, default `/data/btbench`)
  `--volatile-dir DIR` (OBEX transfers, default `/tmp/btbench`) `--capture-dir DIR` (default
  `/tmp/btsnoop`) `--no-bluetooth`
  `--hci-replay FILE.btsnoop` (feed the HCI monitor from a capture, paced in real time).
- WebSocket `/api/ws?topics=a,b` — messages `{"topic":"...","data":{...}}`.
- The route (`/api/audio/route`, `app/src/audio/route.h`) plays one input on the outputs picked;
  it relies on WirePlumber linking no received Bluetooth audio by itself (`node.autoconnect =
  false` on `bluez_input.*`: the image's `50-btbench.conf`, Setup's `Node.node.autoconnect`).
- OBEX (`/api/obex`, `app/src/obex/`): btbenchd runs `/usr/libexec/bluetooth/obexd -n -s -r
  /tmp/btbench/obex` on first use (bluez5-obex; `-s` puts `org.bluez.obex` on the system bus,
  which BlueZ's `obex.conf` allows root), unless the name is owned already.
- Audio streams (`/api/audio/streams`, `app/src/audio/`) run, from PATH: `pw-cat` and `pw-dump`
  (pipewire-tools, with `PIPEWIRE_RUNTIME_DIR=/run/pipewire` from the unit), `aplay`/`arecord`
  (alsa-utils-aplay), `mpg123` (packagegroup-btbench-audio), `ffmpeg` (only with
  `BTBENCH_FFMPEG = "1"`), `curl`. A missing one fails only the streams that need it.
- PipeWire's timing (`/api/audio/pipewire`, the `pipewire` topic) runs `pw-top -b`
  (pipewire-tools), through `stdbuf -oL` (coreutils) when it is there so its table is not held in
  a pipe buffer; only while someone subscribes.

### HCI monitor API (HCI monitor area; btbenchd wires it in)

The module is `app/src/hci/`, a CMake subdirectory building the static library `btb_hci`, with one
entry point:

```cpp
namespace btb::hci {
struct Options { std::string replay_file; std::string capture_dir = "/tmp/btsnoop";
                 std::string capture_unit = "btbench-btsnoop.service"; };
using Publish = std::function<void(const std::string &topic, const nlohmann::json &data)>;
class Monitor;  // owns the monitor socket (or replay) thread
std::unique_ptr<Monitor> start(const Options &, Publish);
void register_routes(httplib::Server &, Monitor &);
}
```

Routes:
- `GET /api/hci/stats` — latest 1 s window:
  `{"ts":<ms>,"window_ms":1000,"adapters":[{"index":0,"name":"hci0","addr":"..","acl_mtu":1021,"acl_pkts":8,"le_mtu":251,"le_pkts":8,"iso_mtu":0,"iso_pkts":0}],"conns":[{"index":0,"handle":11,"type":"acl|le|sco|esco|cis|bis","peer":"AA:BB:..","tx_bps":0,"rx_bps":0,"tx_pps":0,"rx_pps":0,"in_flight":0,"credits":8,"lat_ms":{"min":0,"avg":0,"p50":0,"p95":0,"max":0},"channels":[{"scid":64,"dcid":64,"psm":25,"name":"AVDTP media","tx_bps":0,"rx_bps":0,"lat_ms":{...},"avdtp":{"codec":"SBC","config":"48000 Hz, joint stereo, 8 subbands, bitpool 53","state":"streaming","rtp_lost":0,"rtp_jitter_ms":0.0,"frames_per_packet":7}}]}]}`
- `GET /api/hci/history?index=0&handle=11&seconds=300` — `{"t":[ms..],"tx_bps":[],"rx_bps":[],"lat_p50":[],"lat_p95":[],"in_flight":[]}`
- `GET /api/hci/latency?index=0&handle=11` — `{"bucket_ms":1,"counts":[...]}` (since connect)
- `GET /api/hci/events?since=<seq>` — `[{"seq":1,"ts":<ms>,"index":0,"handle":11,"kind":"conn|disconn|avdtp|l2cap|mark|...","text":"..."}]` (ring of 500)
- `POST /api/hci/mark` `{"text":"..."}` — written to the kernel's logging channel (shows in btmon/btsnoop/Wireshark)
- `GET /api/hci/live.pcap` — chunked pcap, linktype 254 (LINUX_BT_MONITOR), live from now on
- `GET /api/capture` — `{"running":true,"unit":"btbench-btsnoop.service","dir":"/tmp/btsnoop","files":[{"name":"hci-20261009-120000.btsnoop","size":123,"mtime":<ms>,"active":true}]}`; `active`: the file being written (the newest while the unit runs, or the monitor's recording of a replay)
- `PUT /api/capture` `{"running":false}`; `GET|DELETE /api/capture/files/<name>`; `GET /api/capture/files/<name>/analyze` (text: `btmon -a`)
- Packet view of a file, the one being written included (docs/monitor.md "Packet view" has the filter syntax and full shapes). Each answers within a 400 ms budget; `"complete":false` + `"next"` mean "repeat the request, it continues":
  - `GET /api/capture/files/<name>/packets?filter=&start=0&count=200` (or `at_t=SECONDS` / `at_n=FRAME`; `wait=MS`: a tail request, an empty page is held until a match is appended, the file goes or MS pass, 30 s at most, 150 ms of batching after the first match, at most 4 waiting at once else 503) — `{"total","start","complete","next","scanned","packets_in_file","indexed_bytes","file_size","datalink","t0_ms","filter","newer"?,"packets":[{"n","ts_ms","t","index","dir":"tx|rx|","type":"cmd|evt|acl|sco|iso|index|log|mgmt","proto":"att|..."|null,"handle","cid","psm","len","lat_ms","summary","err"?,"mark"?,"cont"?}]}` (`newer`: a newer capture's name, where the live end is after a rotation)
  - `GET /api/capture/files/<name>/packets/<n>` — one packet as above plus `"fields":[{"name","value","off","len","children":[...]}]` and `"hex"`
  - `GET /api/capture/files/<name>/graph?filter=&bucket_ms=&points=&from=&to=&conn=INDEX:HANDLE` — `{"from","to","bucket_ms","buckets","matches","complete","filtered":{"pkts":[],"tx_bytes":[],"rx_bytes":[]},"conns":[{"index","handle","type","peer","tx_bytes","rx_bytes",...}],"conn":{"index","handle","tx_bps":[],"rx_bps":[],"lat_p50":[],"lat_p95":[],"lat_max":[]}|null}`
  - `GET /api/capture/files/<name>/stats?filter=` — `{"total":{"pkts","bytes","tx","rx","err"},"marks","cont","span_s","protos":[{"proto",...}],"types":[{"type",...}],"links":[{"index","handle","type","peer",...}],"latency":{...}|null,"complete",...}`
  - `GET /api/capture/files/<name>/export?filter=&context=0|1` — btsnoop (datalink 2001) of the matches; header `X-Packets`; 409 until the filter is complete
- WebSocket topics: `hci.stats` (the stats object, 1 Hz while subscribed), `hci.event` (one event).

### btbench-btsnoop.service (audio + BlueZ area)

Runs `/usr/bin/btbench-btsnoop` (source `tools/target/btbench-btsnoop`, audio + BlueZ area): a
ring of btmon captures in RAM, `/tmp/btsnoop/hci-<YYYYmmdd-HHMMSS>.btsnoop`. It starts `btmon -w` on a
new file, and when the current one passes `ROTATE_SIZE` (8 MiB) it starts the next btmon before
stopping the old one (no gap; a few packets land in both), then deletes the oldest beyond
`ROTATE_COUNT` (4: 32 MB of RAM at most). Upstream btmon has no rotation of its own (`--write-rotate-*` is a local
patch, not in bluez.git). Enabled by default, `Restart=on-failure`.
