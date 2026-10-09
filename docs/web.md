# The web console

btbenchd serves a console at `http://10.55.0.1/` (the USB gadget link), `http://<hostname>.local/`,
or `http://localhost:8080/` with `make run`. Plain HTML/JS/CSS from `app/www/`, no build step:
`make deploy-www` copies it to the board and a reload shows it. Light or dark follows the system.
The REST and WebSocket API behind every button is in [api.md](api.md) (also served at `/api`).

The header shows the board's name, the adapter (on / visible / scanning), the WebSocket (live /
offline; it reconnects by itself), temperature, CPU and — in red — undervoltage, which on a Zero W
corrupts the Bluetooth UART long before anything else notices.

The console keeps one WebSocket and subscribes only to what the visible tab shows, plus the pairing
question (`bt.request`) and the header's health line (`system`). A tab in the background, or a
console nobody looks at, costs the board nothing.

## Pairing questions

When BlueZ asks the agent something (numeric comparison, a PIN, a passkey, a just-works or
profile authorization), a dialog comes up on whichever tab is open, with the time left before
BlueZ gives up. With the agent on **auto** (Devices tab) only a passkey to type is asked; on
**ask** every question is. `bench agent` answers the same questions from a terminal.

## Tabs

**Devices** — the adapter (powered, discoverable and its timeout, pairable, alias), the agent's
policy and IO capability (which decides the pairing method a peer gets), discovery with a
transport (auto / le / bredr), an RSSI floor and a duration. Each device shows its state and
actions: pair (a PIN field in its details for legacy devices), cancel pairing, connect, disconnect,
trust, remove (click twice). Its name opens the details: UUIDs with names, class, appearance, TX
power, manufacturer and service data, advertising flags, and connect/disconnect of one profile by
UUID.

**LE** — *Advertisers in range*: what each LE device's advertisement carried. *GATT explorer*:
pick a device, connect, and its services, characteristics and descriptors appear by handle with
read, write (hex, or `text:…`; request / command / reliable as the flags allow) and notify; every
value change lands in the notification log. *Advertising*: a form for `LEAdvertisingManager1`
(name, UUIDs, manufacturer and service data as `id:hex`, appearance, intervals, TX power, timeout,
includes); several at once, each with its state (an advertisement whose timeout ran out shows
"released"). *Local GATT server*: an application as JSON (the Example button loads one with a
heart-rate counter, a battery level and an echo characteristic), its live values, and a log of
what remote devices read, wrote and subscribed to.

**Audio** — the audio stack (PipeWire / BlueALSA / none) with a confirmation, optionally
restarting bluetoothd; then BlueZ's view, the same in every mode: transports (codec decoded, raw
configuration, state, delay, an AVRCP absolute-volume slider), the remote stream endpoints with
their capabilities, and AVRCP players.

**Monitor** — the HCI monitor's connections: throughput, TX latency p50/p95/max (HCI send to
Number Of Completed Packets), packets in flight against the controller's credits, RTP loss. Click
one for two minutes of throughput and latency graphs, the latency histogram since it connected, and
its L2CAP channels with the AVDTP stream's state, codec and RTP loss/jitter. Below: the event log
(connections, channels, AVDTP signalling), a **Mark** button that writes a note into btmon, the
btsnoop ring and live captures, and the command for a live capture in Wireshark on your PC.

**Capture** — the always-on btsnoop ring: start/stop, the files (the newest is being written),
view, download, delete, and `btmon -a` analysis on the board. *View* opens a file in the packet
viewer:
- a filter box (`proto:att handle:64 lat>20 "read request"`; the syntax is under the box and in
  [monitor.md](monitor.md#filter-syntax)) and quick filters: HCI commands/events, L2CAP signalling,
  ATT, SMP, AVDTP, media, AVRCP, RFCOMM, errors, marks and logs, slow TX;
- a graph strip of the whole capture: packets per time bucket of the filtered set, and for a chosen
  connection its TX/RX throughput and TX latency p50/p95. Drag across it to keep only that time
  range (it becomes `t>=… t<…` in the filter, and the graph zooms to it); click to jump the table
  there. The band shows which part of the capture is on screen;
- the packet table: frame number, time since the first packet, direction, handle, protocol,
  length and summary (TX latency for completed TX packets), coloured by kind and direction, errors
  in red, marks highlighted. Only the rows on screen are fetched, so a 300 000-packet file scrolls
  like a small one. ↑ ↓, Page Up/Down, Home and End move the selection;
- the selected packet decoded: a tree of fields, and the hex dump with offsets. Hovering a field
  highlights its bytes.

On the board a big file is indexed, and a text filter searched, a slice at a time: the status line
shows the progress and the totals grow until done. For the file btmon is writing, *follow* keeps
reading what is appended (and scrolls with it while the table is at the bottom).

**Wi-Fi** — state (client, setup AP, connecting, off), networks in range, add a network, the saved
ones, and the mode (auto / client / setup AP / off). The same page is served alone at `/wifi`:
when the board has no network it runs its setup AP, and a phone that joins it is sent there.
Adding a network over the setup AP drops that link; the page says so, and the board falls back to
the AP if joining fails.

**Kernel** — the good / test / previous kernel slots, and *try* (one trial boot of the test kernel,
falling back to the good one on a panic, hang or power cycle), *commit* and *roll back*, each
behind a confirmation.

**System** — health, versions of BlueZ, PipeWire, WirePlumber, BlueALSA and the kernel, the stack's
services with restart buttons, bluetoothd's command line (e.g. `-d -E`; saved to
`/data/btbench/bluetoothd.env` and applied by restarting bluetooth), the journal of a unit (the
last 200 lines, and live with *follow*), the jobs runner (l2ping, l2test, iperf3, btmgmt, wpctl
and the other allowlisted tools, with live output and kill), and reboot.

On a PC (`make run`, `make pc`) the board-only parts — audio mode, Wi-Fi, kernel, reboot — say
that they run on the board only.
