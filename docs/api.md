# btbenchd HTTP API

btbenchd serves the bench's web console and this API on port 80 of the board (`BTBENCH_HTTP_PORT`;
8080 with `make run`). Every body is JSON; every error is `{"error": "..."}` with a 4xx/5xx status.
Device addresses are `AA:BB:CC:DD:EE:FF` in either case; UUIDs are accepted in any form (`180d`,
`0x180D`, the full 128-bit one) and returned short (`180d`) when they are in the Bluetooth Base
UUID; byte values are lowercase hex without separators (`0a1b`); times are milliseconds since the
epoch unless the name says otherwise. `tools/bench` is a thin client of exactly these routes.

Nothing here is authenticated: keep the board on the USB gadget link or the lab LAN.

Requests that change BlueZ's state return at once; the bus thread carries them out, and the outcome
shows up in the next `GET /api/bluetooth` (a device's `busy` and `error`) or on the `bt` topic.
GATT reads and writes, advertisement registration and the GATT server wait for the answer, because
the answer is the point.

## WebSocket

`GET /api/ws?topics=bt,hci.stats` upgrades to a push-only WebSocket. Every message is
`{"topic": "...", "data": ...}`. A topic pattern is an exact topic, a prefix wildcard (`job.*`,
`hci.*`) or `*`; without `topics` the client gets everything. The client may change its
subscriptions at any time by sending `{"topics": ["bt", "media"]}`. Nothing is computed for a topic
nobody is subscribed to. A client that falls behind loses its oldest messages (32 are queued).

| topic | data | when |
|---|---|---|
| `bt` | the body of `GET /api/bluetooth` | on change, at most 1 Hz; once on connect |
| `bt.request` | the pending agent request, or `null` | at once when it appears or goes |
| `media` | the body of `GET /api/media` | on change, at most 1 Hz; once on connect |
| `gatt.notify` | `{address, path, handle, uuid, value, text, ts}` | every value change BlueZ reports: notifications, indications, and the result of reads |
| `gatt.server` | `{op: "read\|write\|notify-on\|notify-off", path, uuid, value, offset, device}` | a remote device using the local GATT server |
| `system` | the body of `GET /api/system/health` | 1 Hz |
| `journal` | `{ts, prio, unit, ident, pid, msg}` | each new entry of the followed unit (`PUT /api/system/journal`) |
| `job.<id>` | `{stream: "out\|err", line}`, then `{exit, killed}` | a job's output |
| `hci.stats` | the body of `GET /api/hci/stats` | 1 Hz |
| `hci.event` | one entry of `GET /api/hci/events` | per event |

## Bluetooth

### `GET /api/bluetooth`

```json
{"available": true, "error": "",
 "adapter": {"name": "hci0", "path": "/org/bluez/hci0", "address": "B8:27:EB:50:7B:22", "address_type": "public",
             "alias": "btbench", "powered": true, "discoverable": false, "discoverable_timeout_s": 180,
             "pairable": true, "discovering": false, "roles": ["central", "peripheral"],
             "uuids": [{"uuid": "110b", "name": "Audio Sink"}],
             "adv": {"supported_instances": 4, "active_instances": 0, "supported_includes": ["tx-power"],
                     "supported_secondary_channels": ["1M"]}},
 "adapters": [ ...every adapter, the same shape... ],
 "devices": [ ...see below... ],
 "request": null,
 "agent": {"policy": "auto", "capability": "KeyboardDisplay", "registered": true},
 "discovery": {"on": false, "ours": false, "filter": {"transport": "auto", "rssi": null, "duplicate_data": true,
               "uuids": [], "pattern": ""}, "until_s": null}}
```

`adapter` is the primary adapter: hci0, or the one btbenchd was started with (`--bt-adapter`). A
device is:

```json
{"path": "/org/bluez/hci0/dev_C0_FF_EE_00_00_01", "address": "C0:FF:EE:00:00:01", "address_type": "random",
 "name": "Bench HRM", "alias": "Bench HRM", "adapter": "hci0", "icon": "",
 "paired": false, "bonded": false, "trusted": false, "blocked": false, "connected": true,
 "services_resolved": true, "legacy_pairing": false, "rssi": -61, "tx_power": 4,
 "class": null, "class_major": "", "appearance": "0x0341", "appearance_name": "Heart Rate Sensor",
 "uuids": [{"uuid": "180d", "name": "Heart Rate"}],
 "manufacturer_data": {"0x0059": "0102"}, "service_data": {"180d": "48"}, "advertising_flags": "06",
 "busy": "", "error": ""}
```

`rssi` (and the advertising data) are only there while BlueZ has them, i.e. during and shortly after
a scan. `busy` is `pairing`, `connecting` or `disconnecting` while one of those runs; `error` is the
last operation's failure in BlueZ's words, kept until the next one. Sorted: connected, paired, then
by alias.

### `PUT /api/bluetooth`

Any of: `powered`, `alias`, `discoverable`, `discoverable_timeout_s`, `pairable` (on `adapter`,
e.g. `"hci1"`; default the primary), `agent` (`"auto"` or `"ask"`), `agent_capability`
(`DisplayOnly`, `DisplayYesNo`, `KeyboardOnly`, `NoInputNoOutput`, `KeyboardDisplay`). Answers with
the new state.

The agent is BlueZ's default agent. With `auto` it accepts every pairing and every service
authorization by itself, the way a device with no screen would, and only a passkey to type goes to
the operator. With `ask` every question goes to the operator: the console's pairing dialog,
`bench agent`, or `POST /api/bluetooth/request`. The capability decides the pairing method BlueZ
negotiates (a new capability re-registers the agent).

### `POST /api/bluetooth/scan`

`{"on": true, "transport": "auto|le|bredr", "rssi": -70, "duplicate_data": true, "uuids": ["180d"],
"pattern": "", "seconds": 30}` — discovery with that filter (`SetDiscoveryFilter`) for `seconds`
(0: until stopped). `{"on": false}` stops it and clears the filter.

### Devices

| route | body | what |
|---|---|---|
| `GET /api/bluetooth/devices` | | the device list |
| `GET /api/bluetooth/devices/{addr}` | | one device, plus `gatt_services`, `media_endpoints`, `media_transports` |
| `POST /api/bluetooth/devices/{addr}/pair` | `{"pin": "1234"}` (optional) | `Pair()`; the PIN answers a legacy device's `RequestPinCode` |
| `POST .../cancel-pairing` | | `CancelPairing()` |
| `POST .../connect` | `{"uuid": "110b"}` (optional) | `Connect()`, or `ConnectProfile(uuid)` |
| `POST .../disconnect` | `{"uuid": "110b"}` (optional) | `Disconnect()`, or `DisconnectProfile(uuid)` |
| `POST .../trust`, `.../untrust`, `.../block`, `.../unblock` | | the `Trusted` / `Blocked` property |
| `PUT /api/bluetooth/devices/{addr}` | any of `alias`, `trusted`, `blocked`, `wake_allowed` | properties; answers with the device |
| `DELETE /api/bluetooth/devices/{addr}` | | `RemoveDevice()`: forget it, bond and all |

404 for an address BlueZ does not know (scan first), 503 when Bluetooth is not running.

### The agent's question

`GET /api/bluetooth/request` — the pending question or `null`:

```json
{"id": 3, "kind": "confirm", "address": "5C:E9:1E:22:40:01", "name": "Pixel 7", "passkey": "001177",
 "uuid": "", "uuid_name": "", "expires_s": 53}
```

`kind`: `confirm` (numeric comparison: both sides show `passkey`), `authorize` (a just-works pairing
the other side started), `service` (`AuthorizeService` for `uuid`), `pin` (a legacy device wants a
PIN), `passkey` (type the code the other side shows), `display` (show `passkey`, to be typed on the
other side: informational, kept until the pairing ends). Unanswered questions are declined after
55 s.

`POST /api/bluetooth/request` `{"id": 3, "accept": true}`, plus `"pin": "1234"` or
`"passkey": "123456"` for those kinds. 409 when the request is gone (answered, withdrawn, expired).

## GATT client

### `GET /api/gatt/{addr}`

The device's GATT database as BlueZ resolved it (connect first; `services_resolved` says when it
is done), sorted by handle:

```json
{"address": "C0:FF:EE:00:00:01", "connected": true, "services_resolved": true,
 "services": [{"path": ".../service000a", "handle": 10, "uuid": "180d", "name": "Heart Rate", "primary": true,
   "characteristics": [{"path": ".../service000a/char000b", "handle": 11, "uuid": "2a37",
     "name": "Heart Rate Measurement", "flags": ["notify"], "value": "0048", "text": "", "notifying": false,
     "mtu": 247, "descriptors": [{"path": ".../desc000d", "handle": 13, "uuid": "2902",
       "name": "Client Characteristic Configuration", "flags": [], "value": "0000", "text": ""}]}]}]}
```

`value` is what BlueZ has cached (the last read or notification), `null` when it has none; `text`
is the same bytes as text when they are printable.

| route | body | what |
|---|---|---|
| `GET /api/gatt/{addr}/{handle}?offset=N` | | `ReadValue` over the air: `{handle, path, value, text}` |
| `PUT /api/gatt/{addr}/{handle}` | `{"value": "hex"}` or `{"text": "..."}`, `"type": "request\|command\|reliable"`, `"offset": N` | `WriteValue` |
| `POST /api/gatt/{addr}/{handle}/notify` | `{"on": true}` | `StartNotify` / `StopNotify`; values arrive on `gatt.notify` |

`handle` is decimal or `0x`-hex, of a characteristic or a descriptor. Errors carry BlueZ's error
name as `dbus_error`: 403 for `NotPermitted`/`NotAuthorized`/`NotSupported`, 409 for `NotConnected`
or `InProgress`, 400 for a bad length or offset, 502 for any other refusal, 504 for no answer
within 20 s.

## LE: advertising and the GATT server

### Advertisements

`POST /api/le/adv` registers an `LEAdvertisement1` (several may run at once, up to the
controller's instances):

```json
{"type": "peripheral", "local_name": "btbench", "service_uuids": ["180d"], "solicit_uuids": [],
 "manufacturer_data": {"0xffff": "0102"}, "service_data": {"fe2c": "00"}, "appearance": "0x0341",
 "tx_power": 4, "discoverable": true, "includes": ["tx-power", "appearance", "local-name", "rsi"],
 "min_interval_ms": 100, "max_interval_ms": 150, "duration_s": 0, "timeout_s": 0, "secondary_channel": "1M"}
```

Every field is optional (`type` defaults to `peripheral`); a field left out is not set at all, so
BlueZ's defaults apply. `type` is `peripheral` or `broadcast`; manufacturer ids and the appearance
are numbers or `0x` strings; intervals are 20..10240 ms. A body that cannot fit an advertisement is
refused with 400 here rather than BlueZ's "Invalid Length". The answer is the instance:

```json
{"id": 1, "path": "/org/btbench/adv1", "state": "active", "error": "", "spec": { ...normalised... }}
```

`state`: `pending` (no BlueZ yet: registered when it appears), `registering`, `active`, `failed`
(409, with BlueZ's reason, e.g. all instances taken), `released` (BlueZ dropped it: its
`timeout_s` ran out). Registered again whenever BlueZ restarts.

`GET /api/le/adv` — `{"instances": [...], "manager": {...the adapter's adv...}}`;
`DELETE /api/le/adv/{id}` removes one, `DELETE /api/le/adv` all.

### Local GATT server

`PUT /api/le/gatt-server` exports a GATT application under `/org/btbench/gatt` and registers it
with `GattManager1` (it replaces the previous one):

```json
{"services": [
  {"uuid": "180d", "primary": true, "characteristics": [
    {"uuid": "2a37", "flags": ["notify"], "value": "0048", "counter": {"period_ms": 1000}},
    {"uuid": "2a38", "flags": ["read"], "value": "01"}]},
  {"uuid": "12345678-1234-5678-1234-56789abcdef0", "characteristics": [
    {"uuid": "12345678-1234-5678-1234-56789abcdef1", "flags": ["read", "write", "notify"], "text": "echo",
     "descriptors": [{"uuid": "2901", "flags": ["read"], "text": "Echo"}]}]}]}
```

`flags` are BlueZ's (`read`, `write`, `write-without-response`, `notify`, `indicate`,
`encrypt-read`, ...). `value` (hex) or `text` is the initial value. A characteristic with
`counter` holds a little-endian counter as wide as its initial value (1..4 bytes), incremented every
`period_ms` and notified while subscribed. Anything written is kept: reads return it, and a
subscribed client is notified of it (an echo). BlueZ adds the CCC descriptors itself (defining
`2902` is refused).

`GET /api/le/gatt-server` — `{"defined", "state": "none|pending|registering|registered|failed",
"error", "path", "app", "values": [{path, uuid, value, text, notifying}]}`;
`GET /api/le/gatt-server/example` — the example above; `DELETE /api/le/gatt-server` unregisters it.
Remote reads and writes are reported on the `gatt.server` topic.

## Media

`GET /api/media` — the remote stream endpoints, the transports and the AVRCP players, in every
audio mode (they are BlueZ's objects, whoever registered the local endpoints):

```json
{"endpoints": [{"path": ".../sep1", "device": "...", "address": "A0:B1:C2:D3:E4:F5", "uuid": "110b",
   "role": "Audio Sink", "codec": {"id": 0, "name": "SBC", "summary": "...", "rate": [48000, 44100]},
   "capabilities": "ffff0235", "delay_reporting": true}],
 "transports": [{"path": ".../sep1/fd0", "device": "...", "address": "A0:B1:C2:D3:E4:F5", "uuid": "110a",
   "profile": "Audio Source", "endpoint": ".../sep1", "state": "active",
   "codec": {"id": 0, "name": "SBC", "rate": 48000, "channel_mode": "joint stereo", "blocks": 16,
             "subbands": 8, "allocation": "loudness", "min_bitpool": 2, "max_bitpool": 53,
             "max_bitrate": 357000,
             "summary": "48000 Hz, joint stereo, 16 blocks, 8 subbands, loudness, bitpool 2-53, ≤357 kbit/s"},
   "configuration": "11150235", "delay_ms": 150.0, "volume": 100}],
 "players": [{"path": "...", "address": "...", "name": "Music", "status": "playing",
   "track": {"title": "...", "artist": "...", "album": "...", "duration_ms": 180000}, "position_ms": 42000}]}
```

Decoded: SBC, AAC, aptX/aptX HD, LDAC (rates and modes), other vendor codecs by name, and LC3
configurations (BAP LTVs: rate, frame duration, allocation, octets per frame, bitrate); always the
raw element as hex. `delay_ms` is the sink's reported delay (A2DP delay reporting), `null` without.

`PUT /api/media/transport` `{"path": "<transport>", "volume": 0..127}` — AVRCP absolute volume.

## HCI monitor and capture

Served by the HCI monitor module; the full description is in `docs/monitor.md` and
`docs/contracts.md`. In short:

| route | what |
|---|---|
| `GET /api/hci/stats` | the latest 1 s window: adapters (buffer sizes), connections with TX/RX bit/s and packets/s, in-flight vs controller credits, TX latency (HCI TX → Number Of Completed Packets) min/avg/p50/p95/max, and per L2CAP channel the same plus AVDTP codec/config/state, RTP loss and jitter |
| `GET /api/hci/history?index=0&handle=11&seconds=300` | `{t, tx_bps, rx_bps, lat_p50, lat_p95, in_flight}` arrays, one point a second |
| `GET /api/hci/latency?index=0&handle=11` | `{bucket_ms: 1, counts: [...]}` since the connection came up |
| `GET /api/hci/events?since=<seq>` | connections, disconnections, AVDTP, L2CAP, marks (a ring of 500) |
| `POST /api/hci/mark` `{"text": "..."}` | a marker on the kernel's logging channel: it shows in btmon, the btsnoop ring and Wireshark |
| `GET /api/hci/live.pcap` | live pcap (linktype 254, LINUX_BT_MONITOR): `curl -sN http://<board>/api/hci/live.pcap \| wireshark -k -i -` |
| `GET /api/capture` | `{running, unit, dir, files: [{name, size, mtime, active}]}` — the always-on btsnoop ring |
| `PUT /api/capture` `{"running": false}` | stop or start `btbench-btsnoop.service` |
| `GET` / `DELETE /api/capture/files/{name}` | download or delete one file |
| `GET /api/capture/files/{name}/analyze` | `btmon -a` of it, as text |

They answer 503 with the reason when the monitor socket cannot be opened (no `CAP_NET_RAW`).
`btbenchd --hci-replay FILE.btsnoop` feeds the monitor from a capture instead (`make run
HCI=replay:FILE`).

## Audio mode

`GET /api/audio` — `btbench-audio status`:
`{"mode": "pipewire", "modes": ["pipewire", "bluealsa", "none"], "services": {"pipewire": "active", ...}}`.

`PUT /api/audio` `{"mode": "bluealsa", "restart_bt": false}` — switches the stack (stops one,
starts the other; with `restart_bt` bluetoothd is restarted too, so no endpoint of the old stack
lingers) and answers with the new status.

The audio, Wi-Fi and kernel routes run the board's target scripts. On a PC, where they are not
installed, they answer 503 saying so.

## Wi-Fi

| route | what |
|---|---|
| `GET /api/wifi` | `btbench-wifi status`: `{state: "sta\|ap\|connecting\|off", policy, ssid, ip, ap: {ssid, ip}, networks: [...], last_error}`, plus `op`: the last change made through the API (`{op, running, ok, error, started_ms, finished_ms}`) |
| `GET /api/wifi/scan` | `[{ssid, signal_dbm, freq, security}]` (cached results while in AP mode) |
| `POST /api/wifi/networks` `{"ssid": "...", "psk": "..."}` | add a network and try it (no `psk`: open) |
| `DELETE /api/wifi/networks?ssid=...` | forget one |
| `PUT /api/wifi/mode` `{"mode": "auto\|sta\|ap\|off"}` | the policy |

Adding a network and changing the mode can take down the link the request came over (the setup AP),
so both answer 202 at once and run in the background; `op` in `GET /api/wifi` tells how it went.

In AP mode the board is a captive portal: a page asked for under any host name that is not the
board's own is redirected to `/wifi`, the Wi-Fi page on its own, which is what a phone's captive
sheet shows.

## Kernel

`GET /api/kernel` — `btbench-kernel status`: `{running, slot: "good|test", method, pending_try,
good: {version}, test: {present, version}, prev: {present, version}}`, plus `op` as for Wi-Fi.

`POST /api/kernel/try` boots the test kernel once: it answers 202 and the board reboots half a
second later. `POST /api/kernel/commit` (test → good, good → prev) and `POST /api/kernel/rollback`
answer with the new status.

## System

| route | what |
|---|---|
| `GET /api/system` | `{hostname, ips, uptime_s, load1, cpu_pct, temp_c, mem: {total_kb, used_kb, available_kb}, power: {available, under_voltage, seen}, versions: {bluetoothd, pipewire, wireplumber, bluealsa, btmon, kernel, os}, services: {unit: state}, bluetoothd_args, bluetoothd_noplugin, data_dir, board, device}` |
| `GET /api/system/health` | the first part only (what the `system` topic carries) |
| `GET /api/system/services` | `{unit: "active\|inactive\|failed\|..."}` for bluetooth, pipewire, wireplumber, bluealsa, btbench-btsnoop, btbench-wifi, btbenchd |
| `POST /api/system/services/{unit}/restart` | restart one of those (not btbenchd itself) |
| `GET /api/system/bluetoothd-args` | `{args, noplugin, file}` |
| `PUT /api/system/bluetoothd-args` `{"args": "-d -E", "restart": true}` | writes `BLUETOOTHD_ARGS` in `/data/btbench/bluetoothd.env` (other lines, `BLUETOOTHD_NOPLUGIN`, are kept) and restarts bluetooth.service |
| `GET /api/system/journal?unit=bluetooth&lines=200` | `{unit, entries: [{ts, prio, unit, ident, pid, msg}]}`, oldest first |
| `PUT /api/system/journal` `{"unit": "pipewire"}` | the unit the `journal` topic follows (default bluetooth) |
| `POST /api/system/reboot` | answers, then reboots |

`power` is the raspberrypi-hwmon undervoltage alarm (`in0_lcrit_alarm`): undervoltage corrupts the
Bluetooth UART long before anything else notices, so `seen` is latched since btbenchd started.
`board` and `device` are `/etc/btbench/board.env` and `device.env`. Reboot and restarts answer 403
on a PC (`btbenchd --pc`).

## Jobs

The test tools, run on the board with their output streamed: `l2test`, `l2ping`, `isotest`,
`rctest`, `scotest`, `btgatt-client`, `btmgmt`, `bluetoothctl`, `avinfo`, `iperf3`, `wpctl`,
`pw-dump`, `pw-cli`, `bluealsactl`, `bluealsa-cli`, `hcitool`, `hciconfig`. Nothing else, and no
shell: the arguments go to the tool as they are. Tools that expect a terminal (an interactive
`bluetoothctl`, `btmgmt` without a command) wait for input that never comes; kill them, or give them
their command on the command line.

| route | what |
|---|---|
| `GET /api/jobs` | `{jobs: [{id, cmd, args, running, exit, killed, started_ms, finished_ms, topic}], allowed: [...]}`, newest first |
| `POST /api/jobs` `{"cmd": "l2ping", "args": ["-c", "3", "AA:BB:CC:DD:EE:FF"], "timeout_s": 0}` | start one (201); `args` may be one string, split like a shell would; at most 4 run at once |
| `GET /api/jobs/{id}` | the job with its `output`: `[{stream, line}]` (the last 64 KiB) |
| `DELETE /api/jobs/{id}` | kill it (its process group) |

## Static files

`/` is the console (`/usr/share/btbenchd/www`), `/wifi` the Wi-Fi page alone, `/api` this
document, `/uuids.json` the UUID names the console and the API share. Files are served with
`Cache-Control: no-cache`, so a `make deploy-www` shows on the next reload.
