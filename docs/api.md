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
| `obex` | the body of `GET /api/obex` | on a change (a session, a transfer's progress, an incoming push), at most 4 Hz |
| `audio.streams` | the body of `GET /api/audio/streams` (levels, titles) | 4 Hz while a stream runs, once more when the last ends |
| `gatt.server` | `{op: "read\|write\|notify-on\|notify-off", path, uuid, value, offset, device}` | a remote device using the local GATT server (or the HID remote's) |
| `hid.input` | `{address, device, event, type: "key\|rel\|abs", code, name, value, ts}` | an input event of a Bluetooth keyboard, remote or mouse connected to the board (`value` 1 down, 0 up, 2 repeat) |
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

Any service, characteristic or descriptor may give a fixed `"handle"` (1..0xffff, a number or
`"0x0400"`): a service's is its declaration, a characteristic's its value (its declaration is
the handle before), a descriptor's its own; a notifying characteristic's CCC, which BlueZ adds,
takes the handle after its value. BlueZ refuses an application whose handles collide or do not
fit. Without handles BlueZ allocates them, the same way each time for the same definition.

`GET /api/le/gatt-server` — `{"defined", "state": "none|pending|registering|registered|failed",
"error", "path", "app", "values": [{path, uuid, value, text, notifying}]}`;
`GET /api/le/gatt-server/example` — the example above; `DELETE /api/le/gatt-server` unregisters it.
Remote reads and writes are reported on the `gatt.server` topic.

### HID remote (HID over GATT, device role)

The board as a Bluetooth LE remote control: a HID service (Report Map with a Consumer Control
report, ID 1, and a boot-layout keyboard report, ID 2; HID Information, Control Point, Protocol
Mode), Battery and Device Information (PnP ID), registered as a second GATT application under
`/org/btbench/hid` at fixed handles (HID 0x0400–0x0410 with the reports' values at 0x040a and
0x040e, Battery 0x0420, Device Information 0x0430: a bonded host's cached database stays valid), and an advertisement (appearance 0x0180 *Remote Control*, the HID UUID, its
name) — both only while it is on. Hosts pair before they read the report map (it is
encrypted-read), like with any HID device.

| route | what |
|---|---|
| `GET /api/le/hid` | `{enabled, name, gatt: {state, error}, advertisement: {id, state, error}, subscribed: {consumer, keyboard}, battery, last: {key, action, ts}, keys: {consumer: [...], keyboard: [...]}}`; `subscribed` says a host turned on the report's notifications |
| `PUT /api/le/hid` `{"enabled": true, "name": "btbench remote"}` | on (409 with BlueZ's reason if it refused the service or the advertisement), `{"enabled": false}` off — kept in `/data/btbench/hid-remote.json`, so it is back on after a restart, and advertised again when BlueZ drops the advertisement (an adapter power cycle); `{"battery": 0..100}` the level hosts show |
| `POST /api/le/hid/key` `{"key": "volume_up", "action": "tap", "hold_ms": 80}` | `action` `tap` (press, hold, release), `down` or `up`. `key`: a name from `keys` — `play_pause`, `next`, `previous`, `volume_up`, `mute`, `home`, `back`, `menu`, `power`, …; `up`, `down`, `left`, `right`, `enter`, `escape`, … — one character (`a`, `7`), or a raw usage `consumer:0xcd` / `keyboard:0x28`. A key held down more than 10 s is released by itself; 409 while the remote is off |
| `POST /api/le/hid/type` `{"text": "hello"}` | types printable ASCII (US layout), up to 256 characters |

### HID host: input devices on the board

BlueZ's input plugin (HoG for LE, HIDP for classic) makes a paired keyboard, remote or mouse a
kernel input device; these routes show them and what they send. The event devices are read only
while someone subscribes to `hid.input`, or with `media_keys` on.

| route | what |
|---|---|
| `GET /api/hid` | `{media_keys, reading, devices: [{address, name, adapter, vendor, product, version, hid, nodes: [{event, name, hidraw}], report_descriptor (hex), summary: {applications: [{page, usage, name, report_ids}], reports: [{id, input_bits, output_bits, feature_bits}], items, bytes}}], recent: [the last 100 events]}` |
| `PUT /api/hid` `{"media_keys": true}` | a connected device's play/pause, stop, next, previous and volume keys drive the connected phone's AVRCP player (the playing one first) and its link volume; kept in `/data/btbench/hid.json`, on by default |

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
 "players": [ ...see AVRCP below... ]}
```

Decoded: SBC, AAC, aptX/aptX HD, LDAC (rates and modes), other vendor codecs by name, and LC3
configurations (BAP LTVs: rate, frame duration, allocation, octets per frame, bitrate); always the
raw element as hex. `delay_ms` is the sink's reported delay (A2DP delay reporting), `null` without.

`PUT /api/media/transport` `{"path": "<transport>", "volume": 0..127}` — AVRCP absolute volume.

### AVRCP players

A connected phone (or anything that plays into the board) shows its AVRCP target as a player —
BlueZ's `MediaPlayer1`, whichever audio stack runs:

```json
{"path": "/org/bluez/hci0/dev_5C_E9_1E_22_40_01/player0", "device": "...", "address": "5C:E9:1E:22:40:01",
 "name": "Music", "type": "Audio", "subtype": "Audio Book", "status": "playing",
 "track": {"title": "...", "artist": "...", "album": "...", "genre": "...", "track_number": 3,
           "number_of_tracks": 12, "duration_ms": 180000, "img_handle": ""},
 "position_ms": 42000, "repeat": "off", "shuffle": "alltracks", "equalizer": null, "scan": null,
 "browsable": true, "searchable": false, "transport": ".../sep1/fd0",
 "folder": {"name": "/NowPlaying", "items": 12},
 "items": [{"path": ".../player0/NowPlaying/item1", "name": "...", "type": "audio", "folder_type": "",
            "playable": true, "metadata": { ...as track... }}]}
```

`status` is BlueZ's: `playing`, `paused`, `stopped`, `forward-seek`, `reverse-seek`, `error`.
A setting the player does not offer is `null`; an unknown number (all ones on the wire) is `null`
or a `duration_ms` of 0. `position_ms` is as of the last update the phone sent (on play, pause,
seek and track changes): count on from there while it plays. `transport` is the same device's
transport, for its volume. `items` are what browsing listed (see below), in listing order.

| route | body | what |
|---|---|---|
| `POST /api/media/players/control` | `{"path", "action"}` | `play`, `pause`, `stop`, `next`, `previous`, `fast-forward`, `rewind` (a seek runs until `play` or `release`), `release`, or `press:<key>` / `hold:<key>` with an AV/C operation id (`0x44` play, `0x4b` forward, ...). Waits for the phone: 403/502 with BlueZ's error when it refuses |
| `PUT /api/media/players` | `{"path"}` + any of `repeat` (`off\|singletrack\|alltracks\|group`), `shuffle` (`off\|alltracks\|group`), `equalizer` (`off\|on`), `scan` | the player's settings |
| `POST /api/media/players/browse` | `{"path", "folder": "<item path>"}` (folder optional) | `ChangeFolder` when given, then `ListItems`: `{"items": [{path, name, type, folder_type, playable, title, artist, album, duration_ms}]}`. The items also appear under the player from then on |
| `POST /api/media/items` | `{"path": "<item>", "action": "play\|add"}` | play an item now, or add it to the now-playing list |
| `POST /api/media/players/search` | `{"path", "value"}` | `MediaFolder1.Search` (a `searchable` player), answered with the result folder's listing: `{"items", "folder", "result"}` |
| `GET /api/media/players/cover?path=<player>[&full=1]` | | the current track's cover art (`track.img_handle`), pulled over BIP from the player's `obex_port` (AVRCP 1.6 cover art) through obexd and cached per handle: the 200×200 thumbnail, or with `full=1` the native image. 404 when the player offers none |

Browsing folders: `POST /api/media/players/browse` with `"folder": "<player>/NowPlaying"`, `"<player>/Filesystem"`,
or a folder item's path. Its answer also has `folder`, the name the player gives the folder now listed.

### AVDTP browsing

`POST /api/media/avdtp/{addr}?timeout_s=15` asks the device itself for its stream endpoints, over
an AVDTP signalling channel of btbenchd's own (it pages the device if needed): DISCOVER, then
GET_ALL_CAPABILITIES (GET_CAPABILITIES for AVDTP 1.0) of every SEP.

```json
{"address": "A4:B1:C1:0B:28:E4", "took_ms": 683,
 "seps": [{"seid": 6, "role": "sink", "media_type": "audio", "in_use": false,
           "codec": {"name": "SBC", "summary": "..."},
           "capabilities": [{"category": 1, "name": "media transport", "hex": ""},
                            {"category": 7, "name": "media codec", "hex": "0000ffff0235", "media_type": 0,
                             "codec_type": 0, "codec": {...}},
                            {"category": 8, "name": "delay reporting", "hex": ""}]}]}
```

It needs no audio stack and shows what BlueZ's endpoint objects do not (SEPs in use, every
capability, a device not connected over A2DP). Devices take one signalling channel at a time:
while A2DP is up the probe is refused (409) and `GET /api/media`'s endpoints are BlueZ's view.

## Audio streams

A stream plays a **source** into a **sink** and/or to listeners: a test signal, an internet radio
or UPnP URL, or one device's audio, into a Bluetooth speaker (or the loopback card), heard in a
browser, or both. Each stream is one pump thread in btbenchd and the stack's own tools as child
processes (`pw-cat` in PipeWire mode, `aplay`/`arecord` for plain ALSA devices such as the
loopback card, `mpg123` or `ffmpeg` for URLs). In audio mode `none` only the ALSA devices are
there to play into or capture from. At most 4 run at once.

`POST /api/audio/streams` (201):

```json
{"source": {"type": "tone", "freq": 1000, "freq_right": null, "level_db": -12},
 "sink": {"type": "pipewire", "target": "bluez_output.F8_DF_15_0A_11_3C.1"},
 "rate": 48000, "channels": 2, "gain_db": 0, "latency_ms": 60, "label": ""}
```

| source `type` | fields |
|---|---|
| `tone` | `freq` Hz, `freq_right` (the right channel on its own frequency, or `null`), `level_db` (peak dBFS) |
| `sweep` | `from`, `to` Hz, `seconds`, `log` (default true), `repeat` (default true), `level_db` |
| `noise` | `color`: `white`, `pink` or `brown`; `level_db` is the RMS (default -20) |
| `silence` | — |
| `url` | `url` (http/https: a stream, a file, a `.pls`/`.m3u` playlist — its first entry is played — or a UPnP item), `decoder` `auto\|mpg123\|ffmpeg` (auto: mpg123 for MP3 when installed, ffmpeg for the rest), `title` (a name for the label), `playlist`: `[{url, title}, ...]` (up to 500) played one after another — `url` is its first entry, given or not; without one it is `[{url, title}]` |
| `capture` | `backend` `pipewire\|alsa`, `target` (a source endpoint id; "" the default source), `monitor: true` (PipeWire: capture what goes *to* the sink `target`), `title` |

`sink` is `null` (listeners only), or `{"type": "pipewire", "target": ""}` (a node name, "" the
default sink) or `{"type": "alsa", "target": "hw:Loopback,0,0"}` (an ALSA device string,
`hw:`/`plughw:`): the ids of `GET /api/audio/endpoints`. A *tunnel* from one device to
another is a `capture` of the first into a sink of the second. `rate` is one of 8000 … 96000;
frequencies are clamped to the rate's Nyquist, `gain_db` to -60..+20.

The answer, and each entry of `GET /api/audio/streams` (`{"streams": [...], "max": 4}`, newest
first), is the spec as normalised plus:

```json
{"id": 3, "label": "tone 1 kHz → bluez_output.F8_DF_15_0A_11_3C.1", "node": "btbench-stream-3",
 "state": "running", "error": "", "started_ms": 1791557330192, "ended_ms": null, "seconds": 12.3,
 "listeners": 1, "level": {"peak_db": [-12.0, -12.0], "rms_db": [-15.0, -15.0], "clipped": 0},
 "meta": {"name": "Groove Salad: ...", "title": "Artist - Song", "url": "<the URL played>"},
 "track": 0, "track_seconds": 12.3, "paused": false}
```

`state`: `starting`, `running`, `ended` (a file or sweep finished; a stream that plays nowhere and
was not listened to for 30 s ends too, saying so in `error`), `failed` (`error` has the tool's
last words: a refused URL, a sink that went away), `stopped`. `level` is the last 250 ms after the
gain. `meta` carries an ICY stream's station name and current title (from the decoder, or —
when it reports none, as ffmpeg does not — asked of the server every 15 s); `meta.url` is the stream a playlist pointed to. `node` is
the PipeWire stream's name (`wpctl status`, `pw-top`). A URL stream's `track` is its position in
`source.playlist` (`meta.title` is that item's title), `track_seconds` how much of it has played;
the stream ends after the last track.

| route | what |
|---|---|
| `GET /api/audio/streams/{id}` | one stream |
| `PUT /api/audio/streams/{id}` | `{"gain_db": -6}` at any time; `{"source": {"freq": 440, "freq_right": null, "level_db": -20}}` for a tone, `{"source": {"level_db": ...}}` for noise — live, without a click. A URL stream: `{"next": true}`, `{"previous": true}`, `{"track": 3}`, `{"enqueue": [{url, title}]}` (appended to the playlist), `{"paused": true}` (silence goes to the sink, which stays open, and the decoder waits) |
| `DELETE /api/audio/streams/{id}` | stop it (and forget it) |
| `GET /api/audio/streams/{id}/listen` | the stream as an endless WAV (`audio/wav`, chunked): `curl -sN http://<board>/api/audio/streams/3/listen \| aplay`. `?mono=1` halves a stereo stream's bandwidth. A listener that falls behind loses its oldest second; 3 at most per stream |

### Endpoints

`GET /api/audio/endpoints` (`?fresh=1` skips the 2 s cache) — where streams play to and capture
from:

```json
{"backend": "pipewire", "error": "",
 "sinks": [{"id": "bluez_output.F8_DF_15_0A_11_3C.1", "backend": "pipewire", "direction": "sink",
            "label": "JBL Flip 5", "kind": "bluetooth", "address": "F8:DF:15:0A:11:3C",
            "profile": "a2dp-sink", "codec": "sbc", "serial": "120", "state": "running",
            "default": true, "rate": null, "channels": null}],
 "sources": [ ...the same shape... ],
 "tools": {"pw-cat": true, "pw-dump": true, "aplay": true, "arecord": true, "mpg123": true, "ffmpeg": false, "curl": true}}
```

With PipeWire running (`backend` `pipewire`) they are its audio nodes (`pw-dump`), Bluetooth
first. Without it (`alsa`, audio mode `none`) they are the loopback card's two sides when
snd-aloop is loaded (`hw:Loopback,0,0` to play into, `hw:Loopback,1,0` to capture), and nothing
(`none`) otherwise. `kind`: `bluetooth`, `loopback`, `hardware`, `virtual`.

### Bluetooth codecs

PipeWire picks a Bluetooth device's codec with its profile: `a2dp-sink-sbc_xq` sends A2DP with
SBC-XQ, `a2dp-duplex-opus_05` runs Opus both ways, `audio-gateway` receives the device's A2DP (and
is its HFP gateway), `headset-head-unit` is HFP with mSBC. Choosing a profile is choosing the
codec (as in a desktop's Bluetooth sound settings).

`GET /api/audio/devices` — `{"backend": "pipewire", "error": "", "devices": [{"id": 38, "name":
"bluez_card.A4_B1_C1_0B_28_E4", "label": "SMI-PC", "address": "A4:B1:C1:0B:28:E4", "active":
{...a profile...}, "profiles": [{"index": 131074, "name": "a2dp-sink-sbc_xq", "description":
"High Fidelity Playback (A2DP Sink, codec SBC-XQ)", "available": true, "kind": "a2dp", "codec":
"SBC-XQ"}]}]}`. `kind`: `a2dp` (send), `a2dp-duplex`, `gateway` (receive), `headset`, `bap`,
`off`, `other`. The list depends on what is connected: the receive profiles appear once the device
has opened its stream to the board, the send ones once the board's A2DP source is connected.

`PUT /api/audio/devices/{id}/profile` `{"profile": "a2dp-sink-sbc_xq"}` (or the index) switches
it (`wpctl set-profile`) and answers with the device once the link has renegotiated (a few
seconds; it waits up to about ten); `switch_error` says so when it has not by then. Without PipeWire (mode `none`)
the list is empty and says why.

### UPnP media servers

| route | what |
|---|---|
| `GET /api/audio/upnp/servers?search=1` | SSDP search of the LAN (~2.5 s; `timeout_ms`): `{servers: [{id, name, manufacturer, model, location, icon}], searched_ms, error}`; without `search`, the last search's |
| `GET /api/audio/upnp/browse?server=<id>&object=0&start=0&count=100` | ContentDirectory `Browse` of a container (`0` the root): `{containers: [{id, title, child_count, class}], items: [{id, title, artist, album, class, duration_s, url, mime, size, art}], returned, total, object, server}`. 502 with the server's own fault ("No such object (UPnP error 701)") |

An item's `url` is played as a `url` source (FLAC, WAV, AAC and the like need ffmpeg on the
board; MP3 plays with mpg123). `tools/fake-upnp/server.py` is a media server to try it with.

### Radio stations

`GET /api/audio/radio` — `{"stations": [{"name", "url"}], "saved": false}` (a starter list until
one is saved); `PUT /api/audio/radio` `{"stations": [...]}` saves them to
`/data/btbench/radio.json`.

## OBEX

btbenchd drives BlueZ's obexd: started on first use with `--system-bus` (or adopted when something
already owns `org.bluez.obex`), stopped with btbenchd. Files live under `/data/btbench/obex`
(obexd's root too: it writes nowhere else): `out/` what is sent, `in/` what is received or
pulled, `shared/` anything put there for others. A device browsing the board over FTP sees all
three.

`GET /api/obex` — `{running, managed, owner, error, root, agent: {policy, registered}, request,
sessions: [{id, path, destination, target, channel, psm}], transfers: [{id, session, status,
name, filename, size, transferred, direction}], files: {in, out, shared}}`.
`PUT /api/obex` `{"running": true|false, "policy": "auto|ask|reject"}`: start/stop obexd, and what
the board's OBEX server does with an incoming push (`auto` saves it into `in/`; `ask` holds it as
`request` for `POST /api/obex/request` `{"id", "accept", "name"}`, 55 s).

`POST /api/obex/sessions` `{"address", "target": "opp|ftp|pbap|map|bip-avrcp", "channel"|"psm"}`
(201) opens a client session; `DELETE /api/obex/sessions/{id}` closes it.
`POST /api/obex/sessions/{id}` `{"op": ..., ...}` runs one operation:

| target | op | arguments | answer |
|---|---|---|---|
| opp | `send`, `pull_card`, `exchange` | `file` (a name in `out/`) for send/exchange | the transfer |
| ftp | `list` | | `{entries: [{name, type, size, modified}]}` |
| ftp | `cd`, `mkdir`, `delete` | `name` (`..` up, `/` root for cd) | |
| ftp | `get` / `put` | `name` (remote) / `file` (in `out/`) and `name` | the transfer (get lands in `in/`) |
| ftp | `copy`, `move` | `name`, `to` | |
| pbap | `select` | `location` (`int`, `sim1`), `phonebook` (`pb`, `ich`, `och`, `mch`, `cch`, `spd`, `fav`) | |
| pbap | `size`, `list`, `search`, `pull`, `pull_all` | `field`/`value` (search), `vcard` (pull), `format`, `order`, `max` | `{size}`, `{vcards: [{vcard, name}]}`, or the transfer |
| map | `folder`, `list_folders`, `list`, `get` | `name` (folder), `message` (a listed path), `attachment` | `{folders}`, `{messages: [{path, subject, sender, timestamp, read, ...}]}`, or `{transfer, file}` |
| bip-avrcp | `properties`, `thumbnail`, `get` | `handle` | `{properties: [...]}`, or the transfer |

`POST /api/obex/transfers/{id}` `{"action": "cancel|suspend|resume"}`. Files:
`GET /api/obex/files/{in|out|shared}`, `POST /api/obex/files/{out|shared}?name=x` (the body is the
file, up to 32 MiB), `GET`/`DELETE /api/obex/files/{folder}/{name}`.

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
| `GET /api/hci/live?filter=&since=SEQ` | live packets, filtered on the board (the capture filter syntax), as a chunked NDJSON stream: one packet per line in the capture packet list's shape plus `seq`, and `{"status": {seq, cursor, scanned, sent, dropped, skipped, ...}}` lines (first, whenever `dropped`/`skipped` grow, and at least every 2 s). Batched every 150 ms, at most 200 packet lines a batch (more matches: the newest are sent, the rest `skipped`; a client that falls behind loses the oldest: `dropped`). `since=SEQ` starts after that packet, `since=0` with everything the 2048-packet ring holds; without it, from now. At most 4 streams (503) |
| `GET /api/hci/live/{seq}` | one packet of the live ring decoded: the packet object plus `fields` and `hex`, as for a capture file; 404 once the ring moved past it |
| `GET /api/capture` | `{running, unit, dir, files: [{name, size, mtime, active}]}` — the always-on btsnoop ring |
| `PUT /api/capture` `{"running": false}` | stop or start `btbench-btsnoop.service` |
| `GET` / `DELETE /api/capture/files/{name}` | download or delete one file |
| `GET /api/capture/files/{name}/analyze` | `btmon -a` of it, as text |
| `GET /api/capture/files/{name}/packets?filter=&start=0&count=200` | its packets, filtered and paged: `{total, start, complete, next, packets_in_file, packets: [{n, t, ts_ms, dir, type, proto, handle, cid, psm, len, lat_ms, summary}]}`. `at_t=SECONDS` or `at_n=FRAME` start the page there. Filter terms: `type:`, `dir:`, `proto:`, `handle:`, `cid:`, `psm:`, `index:`, `opcode:`, `evt:`, `subevt:`, `is:err\|mark\|cont\|lat`, `len>N`, `lat>MS`, `t>=S t<S`, `n<=N`, `!term`, `a\|b`; other words search the summaries |
| `GET /api/capture/files/{name}/packets/{n}` | one packet decoded: the same object plus `fields` (a tree of `{name, value, off, len, children}`) and `hex` |
| `GET /api/capture/files/{name}/graph?filter=&bucket_ms=&points=&conn=INDEX:HANDLE` | per time bucket, the filtered packets and bytes, and one link's TX/RX bit/s and TX latency p50/p95/max; the list of links |

The packet routes work against a time budget (400 ms a request on the board): an answer with
`"complete": false` is partial (a big file still being indexed, or a text filter still searching),
and repeating the same request continues where it stopped. The filter syntax, the shapes in full
and the timings are in `docs/monitor.md` ("Packet view"). The live packet stream classifies
nothing while no stream is open, and is described in `docs/monitor.md` ("Live packets").

They answer 503 with the reason when the monitor socket cannot be opened (no `CAP_NET_RAW`).
`btbenchd --hci-replay FILE.btsnoop` feeds the monitor from a capture instead (`make run
HCI=replay:FILE`).

## Audio mode

`GET /api/audio` — `btbench-audio status`:
`{"mode": "pipewire", "modes": ["pipewire", "none"], "services": {"pipewire": "active", "wireplumber": "active"}}`.

`PUT /api/audio` `{"mode": "none", "restart_bt": false}` — `pipewire` or `none`: starts or stops
PipeWire and WirePlumber (with `restart_bt` bluetoothd is restarted too, so no endpoint PipeWire
registered lingers) and answers with the new status.

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
| `GET /api/system` | `{hostname, ips, uptime_s, load1, cpu_pct, temp_c, mem: {total_kb, used_kb, available_kb}, power: {available, under_voltage, seen}, versions: {bluetoothd, pipewire, wireplumber, btmon, kernel, os}, services: {unit: state}, bluetoothd_args, bluetoothd_noplugin, data_dir, board, device}` |
| `GET /api/system/health` | the first part only (what the `system` topic carries) |
| `GET /api/system/services` | `{unit: "active\|inactive\|failed\|..."}` for bluetooth, pipewire, wireplumber, btbench-btsnoop, btbench-wifi, btbenchd |
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
`pw-dump`, `pw-cli`, `hcitool`, `hciconfig`. Nothing else, and no
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
