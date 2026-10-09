# fake-bluez

A fake BlueZ on a private D-Bus bus. It lets btbenchd's Bluetooth side and the console's Devices,
LE and Audio pages be driven on a desk machine without a radio, a phone, or any risk to the desk
machine's own Bluetooth: nothing here touches the real system bus.

```sh
make run BT=fake                       # btbenchd with the fake as its "system bus"
tools/fake-bluez/run                   # just the fake, until Ctrl-C
tools/fake-bluez/run CMD ARGS...       # CMD with DBUS_SYSTEM_BUS_ADDRESS on the fake
tools/fake-bluez/run python3 tools/fake-bluez/test_fake.py   # the fake's own self-test
```

`FAKE_BLUEZ_ADAPTERS=2 tools/fake-bluez/run ...` gives it two adapters, as on a desktop with a USB
dongle: the scripted world is in range of hci1, and hci0 (44:A3:BB:36:5E:2E) has nothing in range.

`run` starts a `dbus-daemon` on a socket in a private temporary directory, starts
`fake_bluez.py` on it, and stops both when it exits. sd-bus and libdbus both read
`DBUS_SYSTEM_BUS_ADDRESS`, so a client under `run` finds the fake where it expects the real
thing. The address is also written to `$XDG_RUNTIME_DIR/fake-bluez.addr` for `ctl`.
Needs `dbus-daemon`, `busctl` and python3 with `dbus` and `gi` (python3-dbus, python3-gi).

## What it fakes

`ObjectManager` on `/`, `AgentManager1` on `/org/bluez`, and on `/org/bluez/hci0`: `Adapter1`,
`Media1`, `LEAdvertisingManager1` and `GattManager1`; `Device1` per device, and `Properties` with
`PropertiesChanged` on all of them. Slow operations answer late, like the real ones (pair ~1 s,
connect ~1.5 s, a page timeout 3 s, a GATT read 50 ms), and errors carry BlueZ's own names and
messages.

| device | address | pairs by | when connected |
|---|---|---|---|
| Galaxy Buds | A0:B1:C2:D3:E4:F5 | paired and trusted at start | A2DP sink: SBC + AAC endpoints, SBC 48 kHz transport |
| JBL Flip 5 | F8:DF:15:0A:11:3C | just works: no question | A2DP sink, 48 kHz |
| Pixel 7 | 5C:E9:1E:22:40:01 | `RequestConfirmation` | A2DP source: transport active 2 s later, an AVRCP player |
| Old Speaker | 00:1A:7D:DA:71:13 | `RequestPinCode`, wants `1234` | A2DP sink, 44.1 kHz |
| MX Keys | C7:3B:52:10:9A:1E | `DisplayPasskey` | none; `Connect` is profile-unavailable |
| Bench HRM | C0:FF:EE:00:00:01 (random) | just works | GATT: Heart Rate, Battery, Device Information, an echo service |
| LE-only beacon | D4:CA:6E:00:00:01 | — | none (an iBeacon's manufacturer data) |

All but the Buds appear during a scan, staggered over 4 s, with an RSSI that jitters every 2 s.
The two LE-only devices appear only with an `le` or `auto` filter, the others only without `le`.
RSSI is invalidated when the scan stops, and a device found by a scan is forgotten 30 s later
unless it was paired or connected.

**GATT** (the Bench HRM, resolved 0.5 s after it connects, removed when it disconnects; object
names carry the handles as BlueZ's do):

| handle | what | flags |
|---|---|---|
| 0x000a | Heart Rate service | |
| 0x000b | Heart Rate Measurement — notifies once a second while subscribed | notify |
| 0x000e | Body Sensor Location (01) | read |
| 0x0010 | Battery service; 0x0011 Battery Level (90 %, a percent less every 10 s while subscribed) | read, notify |
| 0x0014 | Device Information: 0x0015 manufacturer, 0x0017 model, 0x0019 firmware | read |
| 0x001b | an echo service (12345678-…-def0); 0x001c reads back and notifies what is written | read, write, write-without-response, notify |

**LE advertising**: `RegisterAdvertisement` reads the advertisement back from the client with
`GetAll` (as BlueZ does), refuses a bad `Type`, counts `ActiveInstances` / `SupportedInstances`
out of 4, and calls `Release` on one whose `Timeout` runs out. **GattManager1**:
`RegisterApplication` reads the application with `GetManagedObjects` and refuses one without a
service, or with a characteristic missing `UUID`/`Service`/`Flags`.

**Media**: an A2DP connection brings `MediaEndpoint1` objects (`…/sep1`, `…/sep2`) and a
`MediaTransport1` (`…/sep1/fd0`) with `Codec`, `Configuration`, `State`, `Delay` (a speaker revises
its delay from 150 to 180 ms 5 s after connecting) and a writable `Volume`. `Acquire` is refused:
there is no audio.

**AVRCP** (the Pixel, once connected): `…/player0` is a `MediaPlayer1` playing a list of four
tracks (title, artist, album, genre, track number, duration). `Play`, `Pause`, `Stop`, `Next`,
`Previous` (to the start of the track after 3 s, else the track before), `FastForward` /
`Rewind` (until `Play` or `Release`) and `Press` with the AV/C keys change its `Status`, `Track`
and `Position`; the position advances while playing and is published every 5 s, as a phone
does. `Repeat` and `Shuffle` are writable (a bad value is `InvalidArgs`). It is also a
`MediaFolder1` (`/NowPlaying`) whose `ListItems` lists the tracks as `MediaItem1` objects
(`…/player0/NowPlaying/itemN`); an item's `Play` jumps to it.

## Playing the other side

```sh
tools/fake-bluez/ctl incoming-pair 5C:E9:1E:22:40:01   # Pixel pairs with us: RequestConfirmation
tools/fake-bluez/ctl incoming-pair F8:DF:15:0A:11:3C   # JBL pairs with us: RequestAuthorization
tools/fake-bluez/ctl incoming-pair 00:1A:7D:DA:71:13   # RequestPinCode; only 1234 succeeds
tools/fake-bluez/ctl display C7:3B:52:10:9A:1E         # DisplayPasskey, digits 0..6, then paired
tools/fake-bluez/ctl drop F8:DF:15:0A:11:3C            # link lost; out of range for 15 s
tools/fake-bluez/ctl start-stream F8:DF:15:0A:11:3C    # its transport goes active / idle
tools/fake-bluez/ctl stop-stream F8:DF:15:0A:11:3C
tools/fake-bluez/ctl advs                              # the registered advertisements, as read
tools/fake-bluez/ctl app-read 2a37                     # a remote device reads, writes or subscribes
tools/fake-bluez/ctl app-write <uuid> 6869             # to a characteristic of the bench's own
tools/fake-bluez/ctl app-subscribe 2a37                # GATT server (values logged by the fake)
tools/fake-bluez/ctl reset                             # back to the initial world
tools/fake-bluez/ctl objects                           # GetManagedObjects
tools/fake-bluez/ctl busctl ...                        # anything else, on the private bus
```

An incoming pairing goes to the default agent. It is refused without asking if the adapter is
not `Pairable`. After pairing, an untrusted device asks `AuthorizeService` for A2DP before it
connects. `incoming-pair` waits for the outcome (up to the agent's 60 s) and prints it. Every
agent call and its answer is logged on the fake's stdout.

## Where it is not BlueZ

- No radio, no audio: `MediaTransport1.Acquire` fails, and nothing reaches an HCI monitor (use
  `make run HCI=replay:FILE.btsnoop` for the Monitor page).
- Pairing never leaves `Connected` true by itself; BlueZ briefly holds an ACL link after `Pair()`.
- An incoming just-works pairing always asks `RequestAuthorization`; BlueZ skips that for some
  agent capabilities.
- Discovery filters are not merged across clients: the last one set wins.
- The GATT database of a bonded device is not cached across connections.
- `/org/bluez` has no `ProfileManager1` methods.
