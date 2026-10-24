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
| `pipewire` | PipeWire's graph timing, as `GET /api/audio/pipewire` | once a second, from `pw-top -b`, which runs only while someone subscribes |
| `job.<id>` | `{stream: "out\|err", line}`, then `{exit, killed}` | a job's output |
| `hci.stats` | the body of `GET /api/hci/stats` | 1 Hz |
| `hci.event` | one entry of `GET /api/hci/events` | per event |
| `bluez.dbus` | `{entries: [...]}`, entries as `GET /api/bluetooth/dbus` | BlueZ's D-Bus messages, in batches at most 4 Hz |

## Bluetooth

### `GET /api/bluetooth`

```json
{"available": true, "error": "", "stale": false,
 "adapter": {"name": "hci0", "path": "/org/bluez/hci0", "address": "B8:27:EB:50:7B:22", "address_type": "public",
             "alias": "btbench", "powered": true, "discoverable": false, "discoverable_timeout_s": 180,
             "pairable": true, "discovering": false, "roles": ["central", "peripheral"],
             "uuids": [{"uuid": "110b", "name": "Audio Sink"}],
             "adv": {"supported_instances": 4, "active_instances": 0, "supported_includes": ["tx-power"],
                     "supported_secondary_channels": ["1M"]},
             "manufacturer": 15, "manufacturer_name": "Broadcom", "version": "4.2",
             "system_name": "btbench", "connectable": true, "power_state": "on", "pairable_timeout_s": 0,
             "class": "0x6c0000", "class_major": "miscellaneous",
             "modalias": "usb:v1D6Bp0246d0557", "modalias_name": "BlueZ 5.87",
             "experimental": [{"uuid": "6fbaf188-05e0-496a-9885-d6ddfdb4e03e", "name": "ISO socket"}],
             "primary": true, "usb": false,
             "discovery": { ...as "discovery" below, this adapter's... }},
 "adapters": [ ...every adapter, the same shape... ],
 "devices": [ ...see below... ],
 "request": null,
 "agent": {"policy": "auto", "capability": "KeyboardDisplay", "registered": true},
 "discovery": {"on": false, "ours": false,
               "filter": {"transport": "auto", "rssi": null, "pathloss": null, "duplicate_data": false,
                          "uuids": [], "pattern": null, "discoverable": false, "auto_connect": false},
               "filters": ["UUIDs", "RSSI", "Pathloss", "Transport", "DuplicateData", "Discoverable",
                           "Pattern", "AutoConnect"],
               "until_s": null}}
```

`adapter` is the primary adapter, `discovery` its scan. `manufacturer` is the controller maker's
Bluetooth company ID (`manufacturer_name` for the common ones), `version` its HCI version; `usb`
says the controller is on USB (a dongle, an M.2 card on an adapter) rather than the board's own.
`system_name` is BlueZ's `Name` (the alias's default), `modalias` the Device ID bluetoothd gives
the adapter and `modalias_name` who it names (`BlueZ 5.87` for BlueZ's own, a company for a
`bluetooth:` source, else empty); `connectable`, `power_state` and `modalias` are null where
BlueZ does not have them. `experimental` is bluetoothd's enabled experimental features (`name`
empty for one btbenchd does not know).

**Two adapters.** With more than one adapter (the VIM3L's own BCM4359 and an Intel BE200 on its
USB, say) every one is the bench's: the device list has each adapter's devices (a device seen by
both is there twice, once per adapter, each with its own pairing and connection), each adapter
scans on its own, and the agent answers for all of them (a request's `adapter` says which one is
asked). One is the **primary**: the advertisements, the local GATT server and the HID remote
run on it, and a scan or a device request that names no adapter goes to it. It is the board's own
controller (the first not on USB, hci0 among those) unless `PUT /api/bluetooth {"primary": ...}`
chose another; that choice is kept by address, and while that adapter is not there (unplugged) the
board's own stands in. btbenchd started with `--bt-adapter` (a PC) has that one only.

Everything kept per adapter is kept by its address, not its hciN: which controller gets hci0
depends on the order they probe in, and a USB one can come first. On a board, a USB adapter's
default alias is the board's name with the end of its address (`btbench-712d0b-5e2e`), so a phone
that scans tells the two apart.

A device is named by its address and, where it matters, `?adapter=hciN` (or the adapter's
address) on every `/api/bluetooth/devices/...` and `/api/gatt/...` route: that adapter's object of
the device, or a 404. Without it, the primary's, else the first adapter's that has it.
`available` turns false only when BlueZ is not on the bus (or has no adapter); a read of its objects
that goes unanswered (bluetoothd busy, a Zero W scanning a crowded room) leaves the last answer in
place with `stale: true` until the next one. A device is:

```json
{"path": "/org/bluez/hci0/dev_C0_FF_EE_00_00_01", "address": "C0:FF:EE:00:00:01", "address_type": "random",
 "name": "Bench HRM", "alias": "Bench HRM", "adapter": "hci0", "icon": "",
 "paired": false, "bonded": false, "trusted": false, "blocked": false, "connected": true,
 "services_resolved": true, "legacy_pairing": false, "cable_pairing": false, "rssi": -61, "tx_power": 4,
 "modalias": null, "modalias_name": "", "security_level": 1, "encryption_type": 2,
 "class": null, "class_major": "", "appearance": "0x0341", "appearance_name": "Heart Rate Sensor",
 "uuids": [{"uuid": "180d", "name": "Heart Rate"}],
 "manufacturer_data": {"0x0059": "0102"}, "service_data": {"180d": "48"}, "advertising_flags": "06",
 "preferred_bearer": null,
 "bearers": {"bredr": null,
             "le": {"connected": true, "paired": false, "bonded": false, "connectable": true}},
 "profiles": [{"id": "gatt", "name": "GATT", "detail": "4 services", "state": "connected"}],
 "busy": "", "busy_on": "", "error": "", "error_on": "",
 "last_disconnect": {"bearer": "le", "reason": "remote", "message": "Connection terminated by remote host",
                     "at_ms": 1791661005562}}
```

A device that is a member of a coordinated set (CSIP: a pair of LE Audio earbuds) has `set`, the
set's object path (`/org/bluez/hci0/set_…`, BlueZ's `DeviceSet1`), and `set_rank`, its rank in it;
both `null` otherwise (a device in several sets shows the first). The top level has `sets`:
`[{"path": "/org/bluez/hci0/set_1", "adapter": "/org/bluez/hci0", "size": 2, "auto_connect": true,
"devices": ["AA:BB:…", "CC:DD:…"]}]`, the selected adapter's only when one is selected.

`security_level` (0 none, 1 low, 2 medium, 3 high, 4 FIPS) and `encryption_type` (0 none, 1 E0,
2 AES-CCM) are BlueZ's experimental `SecurityLevel` and `EncryptionType`, null without them;
`modalias` is the device's Device ID, as the adapter's.
`class` is the Class of Device (BR/EDR; `class_major` its major class in words), `appearance` the GAP
Appearance (LE; `appearance_name` its category), each null when the device sent none, and `icon`
BlueZ's `Icon`, a freedesktop icon name it derives from either (`phone`, `audio-headset`, …; empty
when it has none).
`rssi` (and the advertising data) are only there while BlueZ has them, i.e. during and shortly after
a scan; a connected device's live link RSSI is `GET .../link` (below). `bearers` are BlueZ's
experimental `org.bluez.Bearer.BREDR1` and `.LE1`, one per transport the device was seen on (null
for the other): a dual-mode device has both, each with its own connection and pairing state.
`preferred_bearer` is the transport a dual-mode device connects on first (`last-used`, `bredr`,
`le`, `last-seen`; null for a single-mode one). `last_disconnect` is the last time one of its
bearers went down and why (BlueZ's `Disconnected` signal: `unknown`, `timeout`, `local`, `remote`,
`authentication`, `suspend`), kept until btbenchd restarts. `busy` is `pairing`, `connecting`,
`disconnecting` or `removing` while one of those runs, and `busy_on` what it is about: a bearer
(`le`, `bredr`), a profile's full UUID, or empty for the whole device; `error` is the last
operation's failure in BlueZ's words, kept until the next one, `error_on` likewise.
`profiles` (empty unless connected) is what the device has up, from its BlueZ objects: `a2dp` and
`le-audio` per media transport (`detail` says `receiving` or `sending` — the board's side — and the
codec; `state` is the transport's `idle`, `pending` or `active`), `avrcp` (its player's name),
`hid`, `pan`, `gatt` (the service count) and `battery` (the level). HFP is not among them: PipeWire's
own HFP does not go through BlueZ (`GET /api/audio/devices` has the profile PipeWire uses). Sorted: connected, paired, then
by alias.

### `GET /api/bluetooth/dbus?since=0&limit=500`

BlueZ's D-Bus traffic: every method call, reply, error and signal to or from `org.bluez` and
`org.bluez.obex` on the system bus, as btbenchd's monitor connection sees it
(`org.freedesktop.DBus.Monitoring.BecomeMonitor`; not started with `--no-bluetooth`). It always
runs; the last 2000 are kept for this route and every one goes to `/tmp/logs/bluez-dbus.log`
(rotated once at 2 MiB, readable in the log viewer). The bus's own messages (NameAcquired, BlueZ's
AddMatch) are left out.

`{"running": true, "error": "", "seq": 412, "file": "/tmp/logs/bluez-dbus.log", "entries": [...]}`:
the entries after `since`, oldest first, at most `limit` (1..2000, the newest). An entry:

```json
{"seq": 411, "ts": 1791715514331, "type": "call|return|error|signal",
 "sender": ":1.112", "sender_name": "btbenchd", "dest": "org.bluez", "dest_name": "bluetoothd",
 "path": "/org/bluez/hci0", "iface": "org.bluez.Adapter1", "member": "StartDiscovery", "sig": "",
 "args": [], "cut": false}
```

`*_name` is the program behind the bus name (`/proc/<pid>/comm`). A reply or error has `call` (the
member it answers) and `latency_ms`, an error `error_name` and `error`, and no `path`/`member`.
`args` is the message's arguments as JSON; arguments longer than 1500 bytes are a string cut with
`…` (`cut: true`), and the reply to `GetManagedObjects` (the whole tree, which btbenchd re-reads every
5 s) is `"N objects"`.

### `PUT /api/bluetooth`

Any of: `powered`, `alias`, `discoverable`, `discoverable_timeout_s`, `pairable` (on `adapter`,
e.g. `"hci1"` or its address; default the primary), `primary` (an adapter, hciN or its address, to
make the primary; `""` back to the board's own; 409 with `--bt-adapter`), `agent` (`"auto"` or
`"ask"`), `agent_capability` (`DisplayOnly`, `DisplayYesNo`, `KeyboardOnly`, `NoInputNoOutput`,
`KeyboardDisplay`). Answers with
the new state. What is set is kept in `/data/btbench/bluetooth.json` and put back on the adapter
whenever BlueZ shows it (a reboot, a bluetoothd restart), since BlueZ powers adapters on at start
and forgets Pairable; a discoverable with a timeout is kept as off. On a board, an adapter whose
alias was never set here is given the board's hostname (`btbench-257bfc`), so boards in range of
each other show up under names of their own; `"alias": ""` goes back to that (BlueZ's `Name`, which
stays `btbench`, is not changed).

The agent is BlueZ's default agent. With `auto` it accepts every pairing and every service
authorization by itself, the way a device with no screen would, and only a passkey to type goes to
the operator. With `ask` every question goes to the operator: the console's pairing dialog,
`bench agent`, or `POST /api/bluetooth/request`. The capability decides the pairing method BlueZ
negotiates (a new capability re-registers the agent).

### `POST /api/bluetooth/scan`

`{"on": true, "adapter": "hci1", "transport": "auto|le|bredr", "rssi": -70, "duplicate_data": true,
"uuids": ["180d"], "pattern": "", "seconds": 30}` — discovery on `adapter` (default the primary)
with that filter (`SetDiscoveryFilter`) for `seconds` (0: until stopped). Each adapter scans on its
own, with a filter of its own. `{"on": false, "adapter": ...}` stops that adapter's and clears its
filter; `{"on": false}` alone stops every scan started here. Every key of BlueZ's filter:

| key | what |
|---|---|
| `transport` | `auto` (dual: LE scan and BR/EDR inquiry interleaved, as the adapter allows), `le`, `bredr` |
| `rssi` | only devices at least this strong (-127..20 dBm) |
| `pathloss` | only devices whose path loss (advertised TX power minus RSSI) is at most this (0..137 dB); either this or `rssi`, never both |
| `uuids` | only devices offering one of these services (any UUID form) |
| `pattern` | only devices whose name or address starts with this; `""` matches every device (and lists non-discoverable ones) |
| `duplicate_data` | report every advertisement, not only changed ones (default true) |
| `discoverable` | make the adapter visible while it scans |
| `auto_connect` | connect to what `pattern` matches, as it is found (needs `pattern`) |

An adapter's `discovery.filters` lists the keys its BlueZ takes (`GetDiscoveryFilters`). A filter BlueZ
refuses fails the scan with its reason (400 for one btbenchd refuses itself: rssi with pathloss,
auto_connect without a pattern, values out of range).

### Devices

| route | body | what |
|---|---|---|
| `GET /api/bluetooth/devices` | | the device list |
| `GET /api/bluetooth/devices/{addr}` | | one device, plus `gatt_services`, `media_endpoints`, `media_transports` |
| `POST /api/bluetooth/devices/{addr}/pair` | `{"pin": "1234"}` (optional) | `Pair()`; the PIN answers a legacy device's `RequestPinCode` |
| `POST .../cancel-pairing` | | `CancelPairing()` |
| `POST .../connect` | `{"uuid": "110b"}` or `{"bearer": "le\|bredr"}` (optional) | `Connect()`, `ConnectProfile(uuid)`, or that bearer's `Connect()`: one transport of a dual-mode device. A plain connect of a dual-mode device with a classic audio, HID or PAN profile is its BR/EDR bearer's `Connect()`: `Device1.Connect` follows `PreferredBearer` ("last-used"), and a speaker last used over LE came up on LE alone |
| `POST .../disconnect` | `{"uuid": "110b"}` or `{"bearer": "le\|bredr"}` (optional) | `Disconnect()`, `DisconnectProfile(uuid)`, or that bearer's `Disconnect()` (which also cancels its `Connect()` in progress) |
| `GET .../link` | | a connected device's link as the controller measures it, per connected bearer: `{"bredr": {rssi, rssi_relative, tx_power, max_tx_power} \| {error} \| null, "le": ...}`, through `btmgmt conn-info` (mgmt Get Connection Information), asked when called. BR/EDR's `rssi` is relative (`rssi_relative`: dB from the controller's golden receive power range, 0 inside it), LE's in dBm; a null value is one the controller does not know |
| `POST .../trust`, `.../untrust`, `.../block`, `.../unblock` | | the `Trusted` / `Blocked` property |
| `PUT /api/bluetooth/devices/{addr}` | any of `alias`, `trusted`, `blocked`, `wake_allowed`, `preferred_bearer` (`last-used`, `bredr`, `le`, `last-seen`; effective once disconnected) | properties; answers with the device |
| `DELETE /api/bluetooth/devices/{addr}` | | `RemoveDevice()`: forget it, bond and all; `busy` is `removing` until it is gone |

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
is the same bytes as text: always for a characteristic the spec defines as a UTF-8 string (Device
Name, the Device Information strings, a track title, a User Description), otherwise when there are
at least two printable bytes (a lone byte is far more often a number: a battery level of 90 is
"Z"). Reads and `gatt.notify` carry `text` the same way.

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

`GET /api/media` — the remote stream endpoints, the transports and the AVRCP players (they are
BlueZ's objects, whoever registered the local endpoints). Transports and players also carry
`device_name`, the device's alias:

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
raw element as hex. `delay_ms` is the sink's reported delay (A2DP delay reporting), `null` without;
`program_info` an LE Audio transport's Program_Info metadata (what a broadcast plays), `null`
without; `location` an LE Audio transport's audio location (the bitmask of BAP's Audio Locations:
1 front left, 2 front right, 3 both), `null` on A2DP.

`PUT /api/media/transport` `{"path": "<transport>", "volume": 0..127}` — AVRCP absolute volume;
on an LE Audio transport 0..255, the Volume Control Service's volume setting.

### AVRCP players

A connected phone (or anything that plays into the board) shows its AVRCP target as a player —
BlueZ's `MediaPlayer1`, whichever audio stack runs:

```json
{"path": "/org/bluez/hci0/dev_5C_E9_1E_22_40_01/player0", "device": "...", "address": "5C:E9:1E:22:40:01",
 "name": "Music", "type": "Audio", "subtype": "Audio Book", "status": "playing",
 "track": {"title": "...", "artist": "...", "album": "...", "genre": "...", "track_number": 3,
           "number_of_tracks": 12, "duration_ms": 180000, "img_handle": ""},
 "position_ms": 42000, "repeat": "off", "shuffle": "alltracks", "equalizer": null, "scan": null,
 "browsable": true, "searchable": false, "playlist_changes": 0, "transport": ".../sep1/fd0",
 "folder": {"name": "/NowPlaying", "items": 12},
 "items": [{"path": ".../player0/NowPlaying/item1", "name": "...", "type": "audio", "folder_type": "",
            "playable": true, "metadata": { ...as track... }}]}
```

`status` is BlueZ's: `playing`, `paused`, `stopped`, `forward-seek`, `reverse-seek`, `error`.
A setting the player does not offer is `null`; an unknown number (all ones on the wire) is `null`
or a `duration_ms` of 0. `position_ms` is as of the last update the phone sent (on play, pause,
seek and track changes): count on from there while it plays. `transport` is the same device's
transport, for its volume. `playlist_changes` counts the times the player said its now playing
list changed (AVRCP's NOW_PLAYING_CONTENT_CHANGED, BlueZ's `Playlist` sent again): a list read
before is stale once it moves. `items` are what browsing listed (see below), in listing order.

| route | body | what |
|---|---|---|
| `POST /api/media/players/control` | `{"path", "action"}` | `play`, `pause`, `stop`, `next`, `previous`, `fast-forward`, `rewind` (a seek runs until `play` or `release`), `release`, or `press:<key>` / `hold:<key>` with an AV/C operation id (`0x44` play, `0x4b` forward, ...). Waits for the phone: 403/502 with BlueZ's error when it refuses |
| `PUT /api/media/players` | `{"path"}` + any of `repeat` (`off\|singletrack\|alltracks\|group`), `shuffle` (`off\|alltracks\|group`), `equalizer` (`off\|on`), `scan` | the player's settings |
| `POST /api/media/players/browse` | `{"path", "folder": "<item path>"}` (folder optional) | `ChangeFolder` when given, then `ListItems`: `{"items": [{path, name, type, folder_type, playable, title, artist, album, duration_ms}]}`. The items also appear under the player from then on |
| `POST /api/media/items` | `{"path": "<item>", "action": "play\|add"}` | play an item now, or add it to the now-playing list |
| `POST /api/media/players/search` | `{"path", "value"}` | `MediaFolder1.Search` (a `searchable` player), answered with the result folder's listing: `{"items", "folder", "result"}` |
| `GET /api/media/players/cover?path=<player>[&full=1]` | | the current track's cover art (`track.img_handle`), pulled over BIP from the player's `obex_port` (AVRCP 1.6 cover art) through obexd and cached per handle for as long as that BIP connection lasts (a phone hands out the same handles again on its next one): the 200×200 thumbnail, or with `full=1` the native image. 404 when the player offers none. A track without a handle: the BIP channel is opened (a phone sends handles only while one is) and the answer is `{"image": false, "channel": "opened"}`, or `"open"` when it already was (the track has no cover) |

Browsing folders: `POST /api/media/players/browse` with `"folder": "<player>/NowPlaying"`, `"<player>/Filesystem"`,
or a folder item's path. Its answer also has `folder`, the name the player gives the folder now listed.

### The board's player

The board's own AVRCP player: what a car, speaker or headset the board plays to sees as the
source's player, with its track and controls, and a library it can browse (docs/audio.md "The
board's player"). btbenchd registers it with BlueZ (`org.bluez.Media1.RegisterPlayer`, object
`/org/btbench/player`) on every adapter while it is enabled.

`GET /api/media/local-player`:

```json
{"enabled": true, "path": "/org/btbench/player", "registered": ["/org/bluez/hci0"], "error": "",
 "status": "playing", "track": {"id": 2, "Title": "Sine 440 Hz", "Artist": "",
 "Album": "Test signals", "Genre": "Test signal", "Number": 2, "NumberOfTracks": 9},
 "repeat": "off", "shuffle": false, "position_ms": 12400,
 "position": {"index": 1, "count": 9}, "queue": [{"id": 1, "name": "Sine 1 kHz"}, ...],
 "stream": 3, "mirror": null, "sink": "bluez_output.0C_32_3A_D6_C3_44.1",
 "library": [{"id": 0, "parent": null, "name": "Test signals", "type": "folder", "playable": true},
             {"id": 1, "parent": 0, "name": "Sine 1 kHz", "type": "audio"}, ...]}
```

`status` is `stopped`, `playing` or `paused`; `track` uses MediaItem1's metadata names (a radio's
`Title` is its ICY title once the station sends one); `queue` is the now playing list, and
`position` where in it the track is. `stream` is the audio stream that plays it
(`/api/audio/streams/{id}`), `sink` the endpoint it plays into: the Bluetooth sink (PipeWire's
default sink if that is one, else the first), or PipeWire's default sink when there is none. It
is a local stream (Audio streams): playing takes the place of what the board plays of its own on
that sink, and a test signal or URL started elsewhere on it ends the player's (it is then stopped).
`registered` lists the adapters BlueZ took it on; `error` says why the last play or registration
failed. `library` is the whole tree, ids are stable while btbenchd runs. `position_ms` is how far
into the track it is (paused time not counted); `repeat` (`off`, `track`, `all`) and `shuffle` are
the player's MPRIS `LoopStatus` and `Shuffle`, which BlueZ offers the remote as AVRCP's repeat and
shuffle settings. "btbench" is the player's name (`Identity`), never a track's artist.

BlueZ counts the position for the remote itself: from 0 at each new track, with a clock running
while the status is Playing. The player sends MPRIS `Seeked` when the track's real position is more
than 1.5 s from that count (a stream joined midway, a pause the count missed), and a track's
`Duration` (`mpris:length`) when it has one, which the remote needs for a progress bar.

While the player plays nothing itself, it tells remotes what another stream plays to a Bluetooth
sink (one of its sinks, or PipeWire's default sink when that is a Bluetooth one; the newest if several):
`mirror` is that stream's id, `track` its title — a radio's ICY title (split into `Artist` and
`Title` at its " - ") with the station as `Album`; a playlist's item with the `Artist`, `Album` and
`Duration` its playlist entry carries, and `Number`/`NumberOfTracks`; a test signal by name — and
`status` playing or paused. A remote's buttons then act on that stream: pause and play pause a URL
stream (they end a test signal, which has no pause), stop ends it, next and previous move through
its playlist, and repeat and shuffle are the stream's own (`source.repeat`, `source.shuffle`).

| route | body | what |
|---|---|---|
| `PUT /api/media/local-player` | any of `{"enabled": bool}`, `{"repeat": "off\|track\|all"}`, `{"shuffle": bool}` | offer the player to devices or not (kept in `/data/btbench/player.json`, on by default); repeat and shuffle of the stream it tells about, else of its now playing list (kept in the same file; a test signal has none: 400); answered with the state |
| `POST /api/media/local-player` | `{"action": "play\|pause\|stop\|next\|previous"}` | the controls a remote has; `play` with nothing chosen plays the test signals |
|  |  | Both answer with the state at once; starting or stopping the stream follows within a second (the next GET shows it) |
| `POST /api/media/local-player` | `{"play": <id>}` or `{"enqueue": <id>}` | play a library entry (a folder: its tracks; a track: in its folder, so next and previous go through it), or add it to the now playing list |

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

## LE Audio broadcasts (Auracast)

Both ways, over BlueZ's BAP plugin and PipeWire. Unicast LE Audio (earbuds, speakers) needs nothing
of its own: a connected device is a PipeWire sink or source like an A2DP one, its links are in
`GET /api/media` (transport UUIDs `2bc9`/`2bcb`: the board's sink or source PAC) and its codec is a
profile (`bap-sink`, `bap-duplex`).

`GET /api/le-audio`:

```json
{"adapters": [{"adapter": "hci0", "address": "44:A3:BB:36:5E:2E", "name": "btbench-vim3l",
               "broadcaster": true, "receiver": true, "pac": "/org/bluez/hci0/pac_bcast0", "discovering": false}],
 "broadcasts": [{"path": "/org/bluez/hci0/pac_bcast0/fd1", "adapter": "hci0", "uuid": "1852", "big": 0, "bis": 1,
                 "state": "active", "codec": {...LC3, as in /api/media, with channels...},
                 "qos": {"big": 0, "bis": 1, "interval_us": 10000, "sdu": 200, "rtn": 4, "latency_ms": 20,
                         "phy": "2M", "framed": false, "presentation_delay_us": 40000, "sync_factor": 2},
                 "encrypted": false, "node": "bluez_output.00_00_00_00_00_00.6381.1"}],
 "sources": [{"address": "84:17:15:2D:00:20", "device": "/org/bluez/hci0/dev_84_17_15_2D_00_20", "adapter": "hci0",
              "name": "RTL8763E", "rssi": -60, "broadcast_id": "A327AC", "encrypted": false, "paired": true,
              "connected": true,
              "streams": [{"path": ".../dev_84_17_15_2D_00_20/sid0/bis1/fd0", "endpoint": ".../sid0/bis1",
                           "big": 0, "bis": 1, "state": "idle", "codec": {...}, "location": 3,
                           "metadata": "0302040002052003", "program_info": null, "encrypted": false,
                           "node": null}]}]}
```

`adapters`: which can broadcast (`broadcaster`: the controller's ISO broadcaster) and receive
(`receiver`: sync receiver), from mgmt; `pac` is the adapter's broadcast endpoint, there once
PipeWire registered the LE Audio broadcast source role (`null` without it). `broadcasts`: the
board's own, a BIG each (one BIS here); `state` is the transport's: `idle` (configured, off air:
nothing plays into it), `active` (on air), `pending`/`broadcasting` on the way. `node` is the
PipeWire sink to play into (an Audio streams sink, `null` until PipeWire made it). `sources`: the
broadcasters in range, strongest first (`rssi` null when not heard lately; one an LE scan did not
hear goes, see audio.md): a device with a
Broadcast Audio Announcement (its `broadcast_id`; `name` is the Broadcast Name when it sends one),
and its `streams` once BlueZ read its BASE — which it does on its own while the adapter scans, by
syncing to the periodic advertising for a moment. `location` is the BIS's audio location (bits:
front left 1, front right 2, ...), `metadata` the BASE's as hex and `program_info` its
Program_Info (what plays, `null` when the broadcaster says nothing); `encrypted` from the BIGInfo or
the Public Broadcast Announcement. A stream's `state`: `idle`, `broadcasting` (selected, being
synced), `active` (received: `node` is PipeWire's capture of it, which plays to the default
output). A Broadcast Assistant's sources (BASS, `src_...` objects) are left out: the assistant
drives them.

| route | what |
|---|---|
| `POST /api/le-audio/broadcasts` | `{"adapter": "", "preset": "48_2_1", "channels": 2, "code": "", "presentation_delay_us": 40000}` — a new BIG of one BIS on `adapter` (default the primary), with the next free BIG handle: 201 `{"big", "pac"}` once PipeWire has its node. `preset` is BAP's broadcast configuration `8_1_1` … `48_6_2` (rate, 7.5/10 ms frames, low latency `_1` or high reliability `_2`; 44.1 kHz is not offered), `channels` 2 (front left + right in the BIS) or 1 (front left, or front right with `"location": "right"`: a left and a right broadcast are one stereo programme for a receiver that takes both), `code` a Broadcast Code (text of up to 16 bytes, or `0x` and 32 hex digits) which encrypts it. It goes on air while something plays into its `node`. 409 when the adapter cannot broadcast or has no broadcast endpoint |
| `DELETE /api/le-audio/broadcasts/{big}?adapter=` | the BIG's streams go (`ClearConfiguration`), and its node with them. Only off air (`state` `idle`), else 409: stop what plays into it first, and PipeWire lets the broadcast go some 6 s later (BlueZ clearing a stream PipeWire still holds left its transport half gone, and the next Acquire crashed bluetoothd) |
| `PUT /api/le-audio/streams` | `{"path": "<a source's stream>", "receive": true, "code": "", "links": ["<the source's other streams>"]}` — `true` selects it (`MediaTransport1.Select`): PipeWire acquires it and BlueZ syncs to the BIG; `code` first sets an encrypted one's Broadcast Code; `links` are received with it in the same BIG sync (`MediaTransport1.Links`, then each selected): a BIG is synced once, with the BISes selected by then, so a BIS selected on its own after the first is never synced. `false` lets it go (`Unselect`). Nothing is retried: a stream that does not sync stays as BlueZ has it, and another `true` tries again |

Broadcasts are found by scanning (`POST /api/bluetooth/scan` with `transport` `le`) on an adapter
that can receive. A broadcaster bonded with the board (an LE Audio earbud or speaker that also
broadcasts) is found like any other: the bench's BlueZ probes the announcement on a device whose
services are resolved too. When a broadcast ends (its streams gone) BlueZ drops what it read, and
the next announcement is read again.

The board's broadcast carries no Broadcast Name: the kernel advertises it with its Broadcast ID
alone, so receivers list it by the adapter's address (or the ID).

## Audio streams

A stream plays a **source** into **sinks** and/or to listeners: a test signal, an internet radio
or UPnP URL, or one device's audio, into Bluetooth speakers (or the loopback card), heard in a
browser, or both. Each stream is one pump thread in btbenchd and the stack's own tools as child
processes (`pw-cat` in PipeWire mode, one per sink; `aplay`/`arecord` for plain ALSA devices such
as the loopback card; `mpg123` or `ffmpeg` for URLs). With PipeWire stopped only the ALSA devices
are there to play into or capture from. At most 4 run at once.

One **local** stream plays to an output at a time: a test signal or a URL (what the board plays of
its own). A new one takes the place of those playing to any of its sinks, and of those with no sink
(only browsers hear them), which are stopped and forgotten as `DELETE` would; it does not inherit
their sinks, the request names them. Local streams on other outputs play on beside it (a broadcast's
programme and music on a speaker). Captures (what devices send the board, two phones at once say)
are not local: any number of them run beside it.

`POST /api/audio/streams` (201):

```json
{"source": {"type": "tone", "freq": 1000, "freq_right": null, "level_db": -12},
 "sinks": [{"type": "pipewire", "target": "bluez_output.F8_DF_15_0A_11_3C.1"},
           {"type": "pipewire", "target": "bluez_output.54_27_1E_EE_E9_8A.1"}],
 "rate": 48000, "channels": 2, "gain_db": 0, "latency_ms": 60, "label": ""}
```

| source `type` | fields |
|---|---|
| `tone` | `freq` Hz, `freq_right` (the right channel on its own frequency, or `null`), `level_db` (peak dBFS) |
| `sweep` | `from`, `to` Hz, `seconds`, `log` (default true), `repeat` (default true), `level_db` |
| `noise` | `color`: `white`, `pink` or `brown`; `level_db` is the RMS (default -20) |
| `silence` | — |
| `url` | `url` (http/https: a stream, a file, a `.pls`/`.m3u` playlist — its first entry is played — or a UPnP item), `decoder` `auto\|mpg123\|ffmpeg` (auto: mpg123 for MP3 when installed, ffmpeg for the rest — AAC, Ogg, FLAC; a URL whose extension does not say is told by its first bytes, docs/audio.md), `title` (a name for the label), `playlist`: `[{url, title, artist, album, duration_ms, art}, ...]` (up to 500; all but `url` optional, what a media server said about the track, for the board's player to tell remotes; `art` the cover's http(s) URL) played one after another — `url` is its first entry, given or not; without one it is `[{url, title}]`; `repeat` `off\|track\|all` (default off) and `shuffle` (the list put in a random order, the first first) |
| `capture` | `backend` `pipewire\|alsa`, `target` (a source endpoint id; "" the default source), `monitor: true` (PipeWire: capture what goes *to* the sink `target`), `title` |

`sinks` is a list of up to 4 sinks, each played the same audio: `[]` (listeners only),
`{"type": "pipewire", "target": ""}` (a node name, "" the default sink) or `{"type": "alsa",
"target": "hw:Loopback,0,0"}` (an ALSA device string, `hw:`/`plughw:`): the ids of `GET
/api/audio/endpoints`. Each is played once (a repeat is dropped). `"sink": {...} | null` instead
names one, as before there were several. A *tunnel* from one device to others is a `capture` of
the first into sinks of the others.

The sink that takes each chunk first is the stream's clock. A sink behind the others (still
opening, a Bluetooth link that stopped streaming, one that takes its audio in bigger periods) is
given what it has room for, up to 200 ms waiting beyond its pipe once it plays, and loses the rest:
it never holds the others or the listeners. Each sink adds its own latency, so two speakers are
not sample-aligned. `rate` is one of 8000 … 96000;
frequencies are clamped to the rate's Nyquist, `gain_db` to -60..+20.

The answer, and each entry of `GET /api/audio/streams` (`{"streams": [...], "max": 4}`, newest
first), is the spec as normalised plus:

```json
{"id": 3, "label": "tone 1 kHz → bluez_output.F8_DF_15_0A_11_3C.1 + bluez_output.54_27_1E_EE_E9_8A.1",
 "node": "btbench-stream-3", "sink": {"type": "pipewire", "target": "bluez_output.F8_DF_15_0A_11_3C.1"},
 "sink_req": 0, "sink_seq": 0,
 "sinks_status": [{"type": "pipewire", "target": "bluez_output.F8_DF_15_0A_11_3C.1", "state": "playing",
                   "error": "", "since_ms": 1791557330240}, {...}],
 "state": "running", "error": "", "started_ms": 1791557330192, "ended_ms": null, "seconds": 12.3,
 "listeners": 1, "level": {"peak_db": [-12.0, -12.0], "rms_db": [-15.0, -15.0], "clipped": 0},
 "meta": {"name": "Groove Salad: ...", "title": "Artist - Song", "url": "<the URL played>",
          "art": "https://somafm.com/logos/512/groovesalad512.jpg", "description": ""},
 "track": 0, "track_seconds": 12.3, "paused": false}
```

`state`: `starting`, `running`, `ended` (a file or sweep finished), `failed` (`error` has the
tool's last words: a refused URL, none of the sinks it started with would open), `stopped`. A
stream that plays nowhere and that nobody listens to runs on until it is stopped. `level`
is the last 250 ms after the gain. `sink` is the first of `sinks` (`null` with none), for
clients that read one. `sinks_status` is each sink as the pump has it, in the order of `sinks`, then
the ones lost since the last `PUT {"sinks"}`: `{type, target, state, error, since_ms}`, `state` one
of `starting` (open, not yet taking audio), `playing` (has taken half a second), `stalled` (has
taken nothing for 2 s: a Bluetooth link that stopped streaming; the stream plays on without it, on
the other sinks or its own clock, and it plays again when it takes audio), `failed` (it would not
open, or its pw-cat ended — a speaker gone, a codec change that made its node anew: `error` is what
pw-cat said; it leaves `sinks`, and the stream goes on with the others, or without one). Nothing is opened again by itself: a sink lost is added again by hand (`PUT
{"sinks"}`), and a capture whose source went away ends the stream. `sink_status` is the
first of `sinks_status` with `seq` (`state` `none` with no sink), as before there were several. A named PipeWire sink is that one only
(`node.dont-fallback`, `node.dont-reconnect`): never the default output instead. `meta` carries an ICY stream's station name and current title (from the decoder, or —
when it reports none, as ffmpeg does not — asked of the server every 15 s); `meta.url` is the stream a playlist pointed to.
`meta.art` is the cover the station sends with its title (ICY `StreamUrl`: an image, or a
now-playing JSON such as Bauer's eventdata, whose image is looked up), often its logo, `""` for
none; `meta.description` its `icy-description` header. `node` is
the PipeWire stream's name (`wpctl status`, `pw-top`): the first sink's; each other sink's has a
number after it (`btbench-stream-3-2`). A URL stream's `source.playlist` is in the order it
plays, each item with an `id` the stream gave it; `track` is the position of the one playing
(`meta.title` is that item's title), `track_seconds` how much of it has played (paused time not
counted); the stream ends after the last track, unless it repeats. Shuffled, the list itself is in
a random order, the track playing first; shuffle off puts it back as listed, tracks added since
included. With repeat `track` a track that ends plays again (next still moves on); with `all` the
last is followed by the first (shuffled: the list in a new order) and previous on the first goes
to the last.

| route | what |
|---|---|
| `GET /api/audio/streams/{id}` | one stream |
| `PUT /api/audio/streams/{id}` | `{"gain_db": -6}` at any time; `{"source": {"freq": 440, "freq_right": null, "level_db": -20}}` for a tone, `{"source": {"level_db": ...}}` for noise — live, without a click. A URL stream: `{"next": true}`, `{"previous": true}`, `{"track": 3}`, `{"enqueue": [{url, title, ...}]}` (appended to the playlist; with `"at": "next"`, right after the track playing), `{"remove": <item id>}` (any track but the one playing), `{"repeat": "all"}`, `{"shuffle": true}` (the list in a random order, the track playing first; `false`: back as listed), `{"paused": true}` (silence goes to the sink, which stays open, and the decoder waits). Any stream: `{"sinks": [...]}` (or one `{"sink": {...} \| null}`) changes its sinks without stopping the source: the ones kept play on untouched, new ones open, the others close; `[]` leaves its listeners only. The answer's `sink_req` numbers the request, and `sink_seq` says which one `sinks_status` answers |
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
first. Without it (`alsa`: PipeWire stopped or not answering) they are the loopback card's two sides when
snd-aloop is loaded (`hw:Loopback,0,0` to play into, `hw:Loopback,1,0` to capture), and nothing
(`none`) otherwise. `kind`: `bluetooth`, `loopback`, `hardware`, `virtual`. An LE Audio broadcast's
node (PipeWire makes one per BIS) has its BlueZ transport as `transport` (`null` on the others) and
says what it is: `Auracast broadcast` (the board's own, `address` null) or `Auracast from
AA:BB:...` (one received); PipeWire's own name for it is a made-up `00 00 00 00 00 00.3986`.

### Bluetooth codecs

PipeWire picks a Bluetooth device's codec with its profile: `a2dp-sink-sbc_xq` sends A2DP with
SBC-XQ, `a2dp-duplex-opus_05` runs Opus both ways, `audio-gateway` receives the device's A2DP (and
is its HFP gateway), `headset-head-unit` is HFP with mSBC. Choosing a profile is choosing the
codec (as in a desktop's Bluetooth sound settings). PipeWire switches the codec only while the
device does not also send to the board: on a duplex link (`a2dp-duplex-*`, two boards that both
have the A2DP sink and source roles on) picking a profile tears the nodes down and switches
nothing. For two boards, turn the A2DP source role off on the one that receives (Setup → Profiles,
WirePlumber's `bluez5.roles`) and pair them again: the link is then one way, and its codec
switches. The switch makes the device's nodes anew: a stream playing to the device loses that
sink (`failed`, "defined target not found") and plays on its others, and a capture of the device's
audio ends; add the sink again, or start the capture again, once the new node is there.

`GET /api/audio/devices` — `{"backend": "pipewire", "error": "", "devices": [{"id": 38, "name":
"bluez_card.A4_B1_C1_0B_28_E4", "label": "SMI-PC", "address": "A4:B1:C1:0B:28:E4", "active":
{...a profile...}, "profiles": [{"index": 131074, "name": "a2dp-sink-sbc_xq", "description":
"High Fidelity Playback (A2DP Sink, codec SBC-XQ)", "available": true, "kind": "a2dp", "codec":
"SBC-XQ"}]}]}`. `kind`: `a2dp` (send), `a2dp-duplex`, `gateway` (receive), `headset`, `bap`,
`off`, `other`. The list depends on what is connected: the receive profiles appear once the device
has opened its stream to the board, the send ones once the board's A2DP source is connected.

`PUT /api/audio/devices/{id}/profile` `{"profile": "a2dp-sink-sbc_xq"}` (or the index) switches
it (`wpctl set-profile`) and answers with the device once the link has renegotiated (a few
seconds; it waits up to about ten); `switch_error` says so when it has not by then. With PipeWire stopped the list is
empty and says why.

### UPnP media servers

| route | what |
|---|---|
| `GET /api/audio/upnp/servers?search=1` | SSDP search of the LAN (~2.5 s; `timeout_ms`): `{servers: [{id, name, manufacturer, model, location, icon}], searched_ms, error}`; without `search`, the last search's |
| `GET /api/audio/upnp/browse?server=<id>&object=0&start=0&count=100` | ContentDirectory `Browse` of a container (`0` the root): `{containers: [{id, title, child_count, class, artist, genre, year, thumb, art}], items: [{id, title, artist, album, class, genre, year, track, duration_s, url, mime, size, rate, bits, channels, thumb, art}], returned, total, object, server}`. `art` and `thumb` are the cover's URLs on the server (`upnp:albumArtURI`, the largest and the smallest DLNA size, or an image `<res>`; "" when none); a container's `artist` is its album artist; `rate`, `bits`, `channels` are the audio resource's, null where the server does not say. 502 with the server's own fault ("No such object (UPnP error 701)") |

An item's `url` is played as a `url` source (FLAC, WAV, AAC and the like need ffmpeg on the
board; MP3 plays with mpg123). `tools/fake-upnp/server.py` is a media server to try it with.

### Radio stations

`GET /api/audio/radio` — `{"stations": [{"name", "url"}], "saved": false}` (a starter list until
one is saved); `PUT /api/audio/radio` `{"stations": [...]}` saves them to
`/data/btbench/radio.json`. A station without a `name` gets the one its stream gives itself
(`icy-name`), else its URL's host: asking the stream takes a second or so per station.

## OBEX

btbenchd drives BlueZ's obexd: started on first use with `--system-bus` (or adopted when something
already owns `org.bluez.obex`), stopped with btbenchd. Files live under `/tmp/btbench/obex`,
in RAM and gone at reboot (obexd's root too: it writes nowhere else; the SD card is kept for
settings): `out/` what is sent, `in/` what is received or
pulled, `shared/` anything put there for others. A device browsing the board over FTP sees all
three.

`GET /api/obex` — `{running, managed, owner, error, root, agent: {policy, registered}, request,
sessions: [{id, path, destination, target, channel, psm}], transfers: [{id, session, status,
name, filename, size, transferred, direction}], files: {in, out, shared}, now_ms}`. A file is
`{name, size, mtime}` (ms); `now_ms` is the board's clock, to say "how long ago" by (a board
without a network may be hours off).
`PUT /api/obex` `{"running": true|false, "policy": "auto|ask|reject"}`: start/stop obexd, and what
the board's OBEX server does with an incoming push (`auto` saves it into `in/`; `ask` holds it as
`request` for `POST /api/obex/request` `{"id", "accept", "name"}`, 55 s). The policy is kept in
`/data/btbench/obex.json`.

`POST /api/obex/sessions` `{"address", "target": "opp|ftp|pbap|map|bip-avrcp", "channel"|"psm", "source"}`
(201) opens a client session (`source`: the local adapter's address, else the one the kernel routes to); `DELETE /api/obex/sessions/{id}` closes it.
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
| `GET /api/capture/files/{name}/packets?filter=&start=0&count=200` | its packets, filtered and paged: `{total, start, complete, next, packets_in_file, packets: [{n, t, ts_ms, dir, type, proto, handle, cid, psm, len, lat_ms, summary}]}`. `at_t=SECONDS` or `at_n=FRAME` start the page there. Filter terms: `type:`, `dir:`, `proto:`, `handle:`, `cid:`, `psm:`, `index:`, `opcode:`, `evt:`, `subevt:`, `is:err\|mark\|cont\|lat`, `len>N`, `lat>MS`, `t>=S t<S`, `n<=N`, `addr:BDADDR`, `!term`, `a\|b`, `a b \|\| c` (either group); other words search the summaries |
| `GET /api/capture/files/{name}/packets/{n}` | one packet decoded: the same object plus `fields` (a tree of `{name, value, off, len, children}`) and `hex` |
| `GET /api/capture/files/{name}/graph?filter=&bucket_ms=&points=&conn=INDEX:HANDLE` | per time bucket, the filtered packets and bytes, and one link's TX/RX bit/s and TX latency p50/p95/max; the list of links |
| `GET /api/capture/files/{name}/stats?filter=` | what the filter matches, counted: `{total: {pkts, bytes, tx, rx, err}, marks, cont, span_s, protos: [{proto, ...}], types: [{type, ...}], links: [{index, handle, type, peer, ...}], latency: {n, avg_ms, p50_ms, p95_ms, max_ms}, complete}` |
| `GET /api/capture/files/{name}/export?filter=&context=1` | the matching packets as a btsnoop file (datalink 2001); `context=1` adds what btmon needs to decode them. 409 until the filter has run over the whole file |

The packet routes work against a time budget (400 ms a request on the board): an answer with
`"complete": false` is partial (a big file still being indexed, or a text filter still searching),
and repeating the same request continues where it stopped. The filter syntax, the shapes in full
and the timings are in `docs/monitor.md` ("Packet view"). The live packet stream classifies
nothing while no stream is open, and is described in `docs/monitor.md` ("Live packets").

They answer 503 with the reason when the monitor socket cannot be opened (no `CAP_NET_RAW`).
`btbenchd --hci-replay FILE.btsnoop` feeds the monitor from a capture instead (`make run
HCI=replay:FILE`).

### PipeWire's graph

`GET /api/audio/pipewire`: the last table of `pw-top -b` (PipeWire's profiler), while someone
follows the `pipewire` topic; `{"running": false, "error": ...}` otherwise. pw-top is a process of
its own, so it runs only while watched (the Graphs tab, Audio → PipeWire).

```json
{"running": true, "ts": 1791620110152, "xruns": 0,
 "drivers": [{"id": 49, "state": "R", "name": "alsa_output.platform-snd_aloop.0.analog-stereo",
              "quantum": 2048, "rate": 48000, "period_ms": 42.667, "format": "S32P 2 48000",
              "busy_ms": 1.4, "wait_ms": 0.115, "busy_q": 0.03, "wait_q": 0.0, "err": 0, "xruns": 0,
              "followers": [{"id": 53, "state": "R", "name": "btbench-stream-1", "latency_ms": 60.0,
                             "busy_ms": 0.42, "wait_ms": 2.4, "busy_q": 0.01, "err": 3, "xruns": 0, ...}]}]}
```

A driver (a sound card, a Bluetooth transport) runs one graph cycle per quantum. `busy_q` is that
cycle's work as a share of the quantum (the DSP load); `period_ms` is quantum ÷ rate, the time the
cycle must fit in. A follower's `latency_ms` is the latency it reports. `err` counts xruns since
pw-top started; `xruns` is how many came since the table before, and the top-level `xruns` is
their sum. States: `R` running, `I` idle, `S` suspended, `C` creating, `E` error, `T`/`t`
transport running/starting. Times are `null` when pw-top did not measure them.

## Audio stack

`GET /api/audio` — `btbench-audio status`:
`{"stack": "pipewire", "enabled": true, "services": {"pipewire": "active", "wireplumber": "active"}}`.

`PUT /api/audio` `{"enabled": false}` — switches the audio stack (PipeWire and WirePlumber) off,
after dropping the links that carry audio, and keeps it off across reboots (`AUDIO_STACK=off` in
`/data/btbench/audio.env`; a factory reset removes it); `true` starts it again. Bluetooth works
without it — LE, GATT, the HID remote, OBEX — but nothing registers A2DP or HFP endpoints. Answers
with the status. While it is off, `POST /api/audio/restart` refuses (500, "the audio stack is off").

`POST /api/audio/restart` `{"restart_bt": false}` (the body is optional) — restarts PipeWire and
WirePlumber, first dropping the links that carry audio so devices reconnect to the new endpoints
(with `restart_bt` bluetoothd is restarted too), and answers with the status.

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

## Setup

Profiles switched on and off, BlueZ's plugins, and the main.conf and WirePlumber settings worth
changing. Each switch is applied in the layer that implements it: bluetoothd's `--noplugin` list,
a main.conf btbenchd writes and passes with `--configfile` (both in `BLUETOOTHD_NOPLUGIN` of
`/data/btbench/bluetoothd.env`), WirePlumber's `bluez5.roles` and settings in
`60-btbench-profiles.conf`, obexd's `-P` (OBEX servers), and the HID remote. On a PC (`--pc`) the
files are written into the data directory and nothing is restarted (`pending: true`).

What there is to set follows the installed stack: the built-in tables give labels and help, and
are narrowed and extended by what the machine says about itself, read on every `GET`:

| source | gives |
|---|---|
| the image's `/etc/bluetooth/main.conf` | every key BlueZ documents there: those not in the table as `type: "text"`, `more: true` (help: BlueZ's comment); `default` (the commented-out example) and `image` (a live line) for all; the UUIDs `KernelExperimental` takes, with their names |
| the kernel (mgmt: Read Index List, Controller Information, Experimental Features) | `controllers` (index, address, supported/current settings, experimental features), `kernel_experimental` (the global ones); each `General.KernelExperimental` option's `note` (where it is offered, where on); a feature the kernel offers that bluetoothd cannot set listed `disabled`; a LE Audio profile's `note` when no adapter reports what it `needs` (`cis-central`, `cis-peripheral`, `iso-broadcaster`, `sync-receiver`) |
| `/usr/share/bluez5/builtin-plugins` (the image's BlueZ build) and `/usr/lib/bluetooth/plugins` | `plugins`: only those this bluetoothd has, unknown ones added; a profile whose plugin is missing `available: false` |
| PipeWire's `spa-0.2/bluez5/libspa-codec-bluez5-*.so` | a codec option without its plugin `missing: true`; a setting for such a codec `available: false`; a plugin the table does not know in the setting's `note` |
| WirePlumber's `wireplumber.conf` (`wireplumber.settings.schema`) and the image's fragments | every `bluetooth.*` setting as `Settings.<name>` with its `default`; `image`: what the image's fragments set (`monitor.bluez.properties`, `wireplumber.settings`) |

`sources` says which were read. A source that cannot be read leaves its table as built.

`GET /api/setup`:

```json
{"profiles": [{"id": "a2dp_source", "label": "A2DP source", "group": "Audio", "help": "...", "on": true,
               "layers": ["pipewire"], "plugins": [], "roles": ["a2dp_source"], "obex": []}, ...],
 "plugins": [{"name": "policy", "label": "Policy", "help": "...", "on": true}, ...],
 "settings": [{"key": "LE.MinConnectionInterval", "label": "Connection interval, min", "help": "...",
               "type": "number", "file": "main.conf", "min": 7.5, "max": 4000, "unit": "ms", "step": 1.25,
               "default": null, "value": null}, ...],
 "controllers": [{"index": 0, "address": "44:A3:BB:36:5E:2E", "supported": ["br/edr", "le", "cis-central", ...],
                  "current": [...], "experimental": [{"uuid": "6fbaf188-...", "flags": 1}]}],
 "kernel_experimental": [{"uuid": "6fbaf188-05e0-496a-9885-d6ddfdb4e03e", "flags": 1}],
 "sources": {"controllers": "mgmt", "bluez_plugins": "...", "codecs": "...", "wireplumber_schema": "..."},
 "roles": ["a2dp_sink", "a2dp_source", "bap_sink", "bap_source", "bap_bcast_sink", "bap_bcast_source",
           "asha_sink", "hfp_hf", "hfp_ag", "hsp_hs", "hsp_ag"],
 "apply": true, "files": {"main_conf": null, "bluetoothd_env": "...", "wireplumber": "..."}}
```

Profiles: A2DP sink/source, HFP HF/AG, HSP HS/AG, LE Audio unicast sink/source and broadcast
sink/source, ASHA (WirePlumber roles; ASHA with BlueZ's asha plugin), BlueZ's own HFP, AVRCP, HID host (classic), HID over GATT host,
PAN, battery levels (BlueZ plugins), the OBEX servers (Object Push, File Transfer, Phonebook,
Message Access) and the HID remote. By default all are on except HSP,
BlueZ's HFP and the remote, as the image has them.

Settings (`value` null: BlueZ's or the image's default): `General.ControllerMode`
`dual|bredr|le`, `General.SecureConnections` `on|off|only`, `General.Privacy`,
`General.JustWorksRepairing`, `General.FastConnectable`, `General.AlwaysPairable`,
`General.NameResolving`, `General.Experimental`, `General.KernelExperimental` (a list of the
kernel features' UUIDs; empty writes `false`), `BR.PageTimeout`
and `BR.LinkSupervisionTimeout` (ms), `LE.MinConnectionInterval` / `LE.MaxConnectionInterval`
(ms, 7.5..4000), `LE.ConnectionLatency`, `LE.ConnectionSupervisionTimeout` (ms),
`LE.Min/MaxAdvertisementInterval` (ms), `GATT.Cache`, `GATT.KeySize`, `GATT.ExchangeMTU`,
`GATT.Channels` (EATT), `GATT.Client`, `Policy.ReconnectAttempts`, `Policy.AutoEnable`,
`Policy.ResumeDelay`; and WirePlumber's: `Monitor.bluez5.codecs` (a list), `Monitor.bluez5.enable-sbc-xq`,
`Monitor.bluez5.enable-msbc`, `Monitor.bluez5.enable-hw-volume`, `Monitor.bluez5.hfphsp-backend`,
`Monitor.bluez5.default.rate`, `Monitor.bluez5.dummy-avrcp-player`, `Device.bluez5.a2dp.ldac.quality`,
`Device.bluez5.a2dp.aac.bitratemode`, `Device.bluez5.auto-connect` and `Device.bluez5.hw-volume` (lists
of roles), `Node.bluez5.media-source-role` `playback|input`, `Node.node.autoconnect` (whether received audio
is linked to the default output by itself; the image says no), `Node.bluez5.decode-buffer.latency`
(samples, 0: PipeWire's own), `Settings.bluetooth.autoswitch-to-headset-profile`,
`Settings.bluetooth.use-persistent-storage`, `Settings.device.restore-profile`; and
those the sources above add (main.conf's other keys, WirePlumber's other `bluetooth.*` settings).
Times are given in ms and written in BlueZ's units (1.25 ms, 10 ms, 0.625 ms slots).

`PUT /api/setup` `{"profiles": {"a2dp_source": false}, "plugins": {"policy": false},
"settings": {"General.ControllerMode": "le", "GATT.Cache": null}}` — checked as a whole (400 and
nothing changed when one part is wrong), saved, and applied: WirePlumber restarted when its
fragment changed, then bluetooth.service when its arguments or main.conf did, obexd when its
servers did. A restart drops every connection. Answers with the state plus `restarted`
(`["wireplumber.service", "bluetooth.service", "obexd"]`) and `pending`.

## System

| route | what |
|---|---|
| `GET /api/system` | `{hostname, ips, uptime_s, load1, cpu_pct, temp_c, mem: {total_kb, used_kb, available_kb}, power: {available, under_voltage, seen}, versions: {bluetoothd, pipewire, wireplumber, btmon, kernel, os}, services: {unit: state}, bluetoothd_args, bluetoothd_noplugin, data_dir, board, device}` |
| `GET /api/system/health` | the first part only (what the `system` topic carries) |
| `GET /api/system/services` | `{unit: "active\|inactive\|failed\|..."}` for bluetooth, pipewire, wireplumber, btbench-btsnoop, btbench-wifi, btbenchd |
| `POST /api/system/services/{unit}/restart` | restart one of those (not btbenchd itself) |
| `GET /api/system/bluetoothd-args` | `{args, noplugin, file}` |
| `PUT /api/system/bluetoothd-args` `{"args": "-d -E", "restart": true}` | writes `BLUETOOTHD_ARGS` in `/data/btbench/bluetoothd.env` (other lines, `BLUETOOTHD_NOPLUGIN`, are kept) and restarts bluetooth.service |
| `GET /api/system/journal?unit=bluetooth&lines=200` | `{unit, entries: [{ts, prio, unit, ident, pid, msg}]}`, oldest first; `unit=kernel` is the kernel's log (`journalctl -k`) |
| `GET /api/system/logs` | `{dir, files: [{name, size, mtime_ms, link}]}`: btbench-logd's files (`/tmp/logs`, `--logs-dir`); `link` is a symbolic link's target (`btsnoop`) |
| `GET /api/system/logs/{name}?tail=2000&download=1` | one file as `text/plain`, or its last `tail` lines; `download=1` as an attachment. `X-Size` is its size |
| `GET /api/system/logs/view?src=j:UNIT\|j:kernel\|f:NAME&lines=1000&q=&prio=7&before=&after=` | the log viewer's reading (see below) |
| `GET /api/system/logs/{name}?from=OFFSET` | what was appended after `OFFSET`, in whole lines, for following a file; `X-Size` is the offset to ask from next, `X-Reset: 1` that the file was rotated or more than 1 MiB came, and the text starts afresh |
| `GET /api/system/btdebug` | the kernel's Bluetooth debug messages by area: `btbench-btdebug status` (docs/contracts.md); 503 on a PC |
| `PUT /api/system/btdebug` `{"on": ["l2cap", "smp"], "off": ["all"], "keep": ["l2cap"]}` | any of them, applied off → on → keep; `keep` replaces the areas on from boot (`[]`: none). Answers with the status |
| `PUT /api/system/journal` `{"unit": "pipewire"}` | the unit the `journal` topic follows (default bluetooth) |
| `POST /api/system/reboot` | answers, then reboots |
| `POST /api/system/factory-reset` `{"pairings": true, "wifi": false}` | every setting back to the image's (both keys optional, defaults shown): answers, then reboots, and btbench-data does the reset at that boot before anything starts (docs/contracts.md, `/data/btbench/factory-reset`). The debug areas kept from boot go at once. `pairings` forgets the paired devices, `wifi` the networks (the image's first one comes back). sshd's host keys stay |

`power` is the raspberrypi-hwmon undervoltage alarm (`in0_lcrit_alarm`): undervoltage corrupts the
Bluetooth UART long before anything else notices, so `seen` is latched since btbenchd started.
`board` and `device` are `/etc/btbench/board.env` and `device.env`. Reboot, factory reset and
restarts answer 403 on a PC (`btbenchd --pc`).

### The log viewer's route

`GET /api/system/logs/view` reads the journal (`src=j:bluetooth.service`, `j:kernel`) or one of
btbench-logd's files (`src=f:bluez.log`) on the board, as the same entries, newest last:

```json
{"src": "f:bluez.log", "entries": [{"ts": 1791620110152, "prio": 7, "who": "bluetoothd", "pid": 22948,
  "msg": "src/device.c:device_add_connection() Device 0C:32:3A:D6:C3:44 is already connected"}],
 "before": "1840221", "end": 2097245, "reset": false, "scanned": 1000, "limited": false}
```

- Entries: `ts` (ms; 0 for a file line in no known format), `prio` (0–7), and when known `who`
  (the program), `pid`, `tid` ("pid.tid", nsdk format), `topic` (PipeWire's and WirePlumber's) and
  `msg`. A file line is parsed back from the format btbench-logd wrote it in.
- `lines` (1–20000) is how many to return. They are the newest that match, read from the end.
- `q` is the filter: words that must all appear (case-insensitive), `!word` that must not,
  `/regex/` (POSIX extended, with JavaScript's `\d`, `\s` and `(?:`; case-insensitive unless it has a
  capital), and `"a phrase"`. `prio` is the lowest priority kept.
- `before` (from an earlier answer) continues older than that answer's oldest entry; `before` is
  `null` when there is nothing older. A journal search stops after 1.5 s and says `limited`:
  ask again with its `before` to search further back.
- `after=OFFSET` (files) returns what was appended after `end`, for following. `reset` means the
  file was rotated and the entries start afresh.
- The answer is gzipped (level 1) for a client that accepts it; its type is
  `application/vnd.btbench.logs+json`.

The journal is read from the files being written first (in RAM: the image's journald keeps it in
`/run`, uncompressed); older files are opened only when those run out. On a Zero W: a file's 1000
lines in about 0.1 s and its 20 000 in about 1 s, the journal's 1000 entries in about 0.3 s
(libsystemd's own pace, about 0.2 ms an entry). The console asks for 1000 first and fetches the
rest of what was asked for in chunks after.

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
