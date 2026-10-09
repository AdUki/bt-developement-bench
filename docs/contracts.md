# Interfaces between the pieces

Who provides what to whom. Every piece of the bench is written against this page; change it here
first when an interface moves.

## Ownership (directories)

| Area | Owns |
|---|---|
| system | `yocto/meta-btbench/recipes-core/{images,btbench-base,base-files}/`, `recipes-core/packagegroups/packagegroup-btbench-tools.bb`, `yocto/meta-btbench/wic/`, `tools/target/{btbench-wifi,btbench-gadget,btbench-bdaddr,btbench-data}`, `mk/system.mk`, `docs/system.md` |
| audio + BlueZ | `recipes-connectivity/bluez5/`, `recipes-multimedia/{pipewire,wireplumber,bluealsa,libfreeaptx,ldacbt,btbench-audio,packagegroups}/`, `tools/target/{btbench-audio,btbench-btsnoop}`, `tools/dev/{bbvars,clangd-gen}`, `mk/dev.mk`, `docs/{audio,dev}.md` |
| kernel | `recipes-kernel/`, `dynamic-layers/raspberrypi/recipes-{kernel,bsp}/`, `boards/dts/`, `tools/dev/mkdtb`, `tools/target/btbench-kernel`, `mk/kernel.mk`, `docs/kernel.md` |
| btbenchd | `app/` (except `app/src/hci/`, `app/tests/hci/`), `recipes-btbench/`, `tools/bench`, `tools/fake-bluez/`, `mk/app.mk`, `docs/{api,web}.md` |
| HCI monitor | `app/src/hci/`, `app/tests/hci/`, `docs/monitor.md` |

## Image composition

`btbench-image` installs: `packagegroup-btbench-tools` (system), `packagegroup-btbench-audio`
(audio + BlueZ: bluez5 and its tools, pipewire, wireplumber, bluealsa, btbench-audio,
btbench-btsnoop), `packagegroup-btbench-kernel` (kernel: kernel modules, btbench-kernel,
libubootenv-bin), `btbench-base` (system), `btbenchd` (btbenchd).

## Build-time variables (auto.conf / btbench-device.conf)

`BTBENCH_BOARD`, `BTBENCH_KERNEL` (next|rpi), `BTBENCH_KARCH`, `BTBENCH_KIMAGE`,
`BTBENCH_KDEFCONFIG`, `BTBENCH_DTBS`, `BTBENCH_DTS_EXTRA` (absolute path or empty),
`BTBENCH_TRY_METHOD`, `BTBENCH_GADGET_UDC`, `BTBENCH_BT_HCI`, `BTBENCH_ROOT` (the repo checkout),
`BTBENCH_{BLUEZ,PIPEWIRE,WIREPLUMBER,LINUX}_{SRCREV,PV}` (yocto/conf/versions.conf).

Device: `BTBENCH_HOSTNAME`, `BTBENCH_ROOT_PASSWORD`, `BTBENCH_SSH_PUBKEY` (path on the build
host), `BTBENCH_WIFI_{SSID,PSK,COUNTRY}`, `BTBENCH_AP_PSK`, `BTBENCH_GADGET` (ecm|ncm|off),
`BTBENCH_AUDIO_MODE` (pipewire|bluealsa|none), `BTBENCH_EXTRA_CODECS` (0|1), `BTBENCH_AAC` (0|1),
`BTBENCH_HTTP_PORT`.

Recipes that ship files from the repo (tools/target, tools/dev, boards/dts) reach them with
`FILESEXTRAPATHS:prepend := "${BTBENCH_ROOT}/tools/target:"` (or the dir they need) and list them
in `SRC_URI` as `file://...`, so bitbake tracks their checksums.

## On the board

- `/etc/btbench/board.env` — shell `KEY=VALUE`: `BOARD KERNEL KARCH KIMAGE DTBS TRY_METHOD GADGET_UDC BT_HCI`.
- `/etc/btbench/device.env` — `HOSTNAME AP_PSK WIFI_COUNTRY GADGET AUDIO_MODE HTTP_PORT`.
- `/data` — ext4 partition (label `data`), mounted at boot, grown to fill the card at first boot.
  - `/data/btbench/audio-mode` — one word.
  - `/data/btbench/bluetoothd.env` — `BLUETOOTHD_ARGS="-d ..."` (no `-n`: under systemd it only duplicates
    journal lines) and `BLUETOOTHD_NOPLUGIN="hfp"` (BlueZ's own HFP plugin would fight PipeWire/BlueALSA
    for the HF role), both read by the bluetooth.service drop-in. Writers replace one, keep the other.
  - `/data/btbench/btsnoop.env` — `ROTATE_SIZE`, `ROTATE_COUNT` for the btmon ring.
  - `/data/btbench/wifi/` — wpa_supplicant networks.
  - `/data/bluetooth/` — persisted `/var/lib/bluetooth` (bonds survive reflashes of the rootfs? no: of deploys and kernel swaps; a reflash rewrites /data too).
  - `/data/btsnoop/` — the always-on btmon ring (`hci-<YYYYmmdd-HHMMSS>.btsnoop`, newest is being written).
- `/boot` — the FAT boot partition, mounted read-write.
- `/run/btbench/` — runtime state (`wifi.json`, `wifi-scan.json`).
- Network: USB gadget `usb0` = `10.55.0.1/24` (DHCP server for the PC), Wi-Fi AP `10.42.0.1/24`,
  mDNS `<hostname>.local`.

## Target scripts (`/usr/bin`, sources in `tools/target/`)

All print JSON on stdout for `status`/`scan`, exit non-zero with a one-line reason on stderr on
failure. btbenchd and `bench` call them; they must not need a terminal.

### btbench-audio
- `btbench-audio get` → `pipewire` | `bluealsa` | `none`
- `btbench-audio status` → `{"mode":"pipewire","modes":["pipewire","bluealsa","none"],"services":{"pipewire":"active","wireplumber":"active","bluealsa":"inactive"}}`
- `btbench-audio set <mode> [--restart-bt]`
- `btbench-audio boot` — start the stored mode (btbench-audio-select.service)

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

## btbenchd

- Binary `/usr/bin/btbenchd`, unit `btbenchd.service`, www in `/usr/share/btbenchd/www`, runs as root.
- Flags: `--port N` `--www DIR` `--data-dir DIR` (default `/data/btbench`) `--no-bluetooth`
  `--hci-replay FILE.btsnoop` (feed the HCI monitor from a capture, paced in real time).
- WebSocket `/api/ws?topics=a,b` — messages `{"topic":"...","data":{...}}`.

### HCI monitor API (HCI monitor area; btbenchd wires it in)

The module is `app/src/hci/`, a CMake subdirectory building the static library `btb_hci`, with one
entry point:

```cpp
namespace btb::hci {
struct Options { std::string replay_file; std::string capture_dir = "/data/btsnoop";
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
- `GET /api/capture` — `{"running":true,"unit":"btbench-btsnoop.service","dir":"/data/btsnoop","files":[{"name":"hci-20261009-120000.btsnoop","size":123,"mtime":<ms>,"active":true}]}`
- `PUT /api/capture` `{"running":false}`; `GET|DELETE /api/capture/files/<name>`; `GET /api/capture/files/<name>/analyze` (text: `btmon -a`)
- WebSocket topics: `hci.stats` (the stats object, 1 Hz while subscribed), `hci.event` (one event).

### btbench-btsnoop.service (audio + BlueZ area)

Runs `/usr/bin/btbench-btsnoop` (source `tools/target/btbench-btsnoop`, audio + BlueZ area): a
ring of btmon captures, `/data/btsnoop/hci-<YYYYmmdd-HHMMSS>.btsnoop`. It starts `btmon -w` on a
new file, and when the current one passes `ROTATE_SIZE` (16 MiB) it starts the next btmon before
stopping the old one (no gap; a few packets land in both), then deletes the oldest beyond
`ROTATE_COUNT` (8). Upstream btmon has no rotation of its own (`--write-rotate-*` is a local
patch, not in bluez.git). Enabled by default, `Restart=on-failure`.
