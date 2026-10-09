# HCI monitor

btbenchd watches every HCI packet between the host and the controller and turns them into numbers
for the Monitor tab and `bench mon`. It shows throughput per link and per L2CAP channel, how long
the controller holds our packets, controller buffer use, AVDTP stream state with the negotiated
codec, and RTP loss and jitter. It does this without btmon running and without touching the
traffic.

It also gives access to the always-on btmon capture ring (`/data/btsnoop`), and can stream a live
pcap to Wireshark.

The code is in `app/src/hci/` (the static library `btb_hci`) and the tests are in
`app/tests/hci/`. The HTTP shapes are in [contracts.md](contracts.md#hci-monitor-api-hci-monitor-area-btbenchd-wires-it-in).

## Where the packets come from

- **Live:** the kernel's HCI monitor channel. This is the socket btmon reads:
  `AF_BLUETOOTH`/`BTPROTO_HCI` bound to `HCI_DEV_NONE` + `HCI_CHANNEL_MONITOR`. It sees every
  controller, every packet in both directions, and the index add/remove/open/close notices.
  - `SO_TIMESTAMP` gives each packet the kernel's receive time. Latency is therefore not skewed by
    when btbenchd gets scheduled, and on a Zero W that matters: one ARM11 core is shared with
    bluetoothd and PipeWire.
  - The socket buffer is 1 MiB. If it still overflows, the kernel's count of dropped packets is
    reported as `source.kernel_drops`.
  - Opening the socket needs `CAP_NET_RAW` (btbenchd runs as root). Without it, every
    `/api/hci/*` route answers **503** with the reason, while `/api/capture*` keeps working.
- **Replay:** `btbenchd --hci-replay FILE.btsnoop` feeds the same decoder from a capture, paced in
  real time. Two formats are read:
  - datalink 2001: what `btmon -w` and the capture ring write;
  - datalink 1002: H4, from Android and many vendor stacks. It is mapped to controller 0.

  When the file ends, the last partial window is closed and the stats stay as they were.
  `source.done` becomes `true`.

## What is measured

Everything is grouped into **1 s windows**:
- `GET /api/hci/stats` and the `hci.stats` WebSocket topic carry the latest window;
- `/api/hci/history` keeps the last 300 windows per connection, for the graphs;
- `/api/hci/latency` holds the TX latency histogram since the connection was made.

### Links

A link is created by its connection event:

| Link type | Created by |
|---|---|
| ACL | Connection Complete |
| SCO / eSCO | (Synchronous) Connection Complete |
| LE | LE (Enhanced) Connection Complete v1/v2 |
| CIS | CIS Established v1/v2 |
| BIS | Create BIG Complete / BIG Sync Established |

A link is removed by:
- Disconnection Complete;
- BIG Terminate or BIG Sync Lost;
- the controller closing.

A removed link is still reported in the window it ended in, marked `"connected": false`, and is
gone from the next window. Its events stay in the events ring.

If the trace starts while a link is already up, the link is created from its first data packet,
and its event says so. The peer address is unknown in that case. A link seen this way is assumed
to be BR/EDR until one of the LE fixed channels (4, 5, 6) shows up on it.

### Throughput

`tx_bps`/`rx_bps` count the HCI payload bytes per second: the ACL data after the 4-byte ACL header,
which includes the L2CAP header, and the SCO/ISO data after their headers. `tx_pps`/`rx_pps` count
HCI packets, which means fragments, not L2CAP SDUs. Channel numbers count the same bytes, split
by L2CAP channel. Continuation fragments are counted against the channel their start fragment
named.

### TX latency (HCI TX → Number Of Completed Packets)

Every packet the host sends on a link (ACL, ISO, and SCO when Synchronous Flow Control is on) is
timestamped and placed in that link's FIFO, which holds up to 128 entries. Each Number Of Completed
Packets (NOCP) event retires the oldest entries of that handle, one per completed packet. The
difference between the two timestamps is the latency sample. Each sample goes into:
- the link's histogram for the window and its histogram since connect;
- the histogram of the L2CAP channel the packet belonged to, because each FIFO entry remembers its
  channel.

This is the same accounting as `btmon -a` (bluez `monitor/analyze.c`).

What the number means: **the time a packet spends in the controller's buffer.** It runs from the
moment the host hands the packet over until the controller reports that buffer free again. For
ACL, that happens when the peer's baseband acknowledged the packet or it was flushed. It does not
include anything above HCI (PipeWire, the socket queue, the kernel's own L2CAP queue). It also
does not include the air time after the ACK.

On a healthy A2DP link it is a few ms. It grows when the radio is struggling: retransmissions, a
peer in sniff, or Wi-Fi coex taking the antenna on the Zero W's shared BCM43430.

The histogram has 1 ms buckets from 0 to 500 ms, plus an overflow bucket. Percentiles are read from
the histogram (nearest rank, reported at the bucket's midpoint and clamped to the exact min/max),
so no samples are kept or sorted. `min`, `max` and `avg` are exact. `lat_ms.n` is the number of
samples. When `n` is 0 the other fields are 0, and in `/history` they are `null`.

A completion that matches nothing counts as `nocp_unmatched`. That happens for packets sent before
the trace started. If the FIFO overflows, the oldest entry is dropped and counted as
`fifo_overflow`. An overflow means NOCP events are missing. In both cases the next few samples can
be off by a packet.

### In flight vs credits: the A2DP stall signal

- **`in_flight`** is how many of this link's packets the controller holds right now: sent, not
  yet completed. `in_flight_max` is the highest it reached during the window.
- **`credits`** is how many more packets the host may send into that link's buffer **pool**. It is
  the pool size the controller announced minus everything in flight on every link sharing that
  pool. `credits_min` is the lowest it reached during the window.

The pools come from the controller's buffer-size replies:

| Pool | Size from | Links |
|---|---|---|
| `acl` | Read Buffer Size `acl_pkts` | BR/EDR ACL, and LE too if the controller has no LE buffers |
| `le` | LE Read Buffer Size `le_pkts` | LE |
| `iso` | LE Read Buffer Size v2 `iso_pkts` | CIS, BIS |
| `sco` | Read Buffer Size `sco_pkts` | SCO/eSCO, only with Synchronous Flow Control (otherwise `none`: no NOCP comes, nothing is queued) |

**`credits_min` at 0 means the host had to stop sending.** Every buffer is full and the kernel is
queueing. If this happens on the A2DP link while the media channel's latency climbs, the radio
cannot keep up and audio will glitch, even though nothing above HCI did anything wrong. If
`credits` stays high while `rtp_lost` grows on TX, the loss is above HCI: the source dropped
packets.

The buffer sizes are read from the controller's init. If btbenchd starts after the controller was
initialised (the usual case on the board), the BR/EDR numbers are fetched with `HCIGETDEVINFO`
instead. LE and ISO buffer counts have no such ioctl, so they stay unknown (0, credits `null`)
until the controller is re-initialised, for example by `btmgmt power off/on` or a
bluetoothd restart.

### L2CAP channels

Fixed channels are named by CID:

| CID | Name |
|---|---|
| 1 | L2CAP signalling |
| 2 | connectionless |
| 4 | ATT |
| 5 | LE signalling |
| 6 | SMP |
| 7 | SMP (BR/EDR) |

Dynamic channels are learned from the signalling on CID 1 and CID 5:
- Connection Request/Response;
- LE Credit Based Connection Request/Response;
- Enhanced Credit Based Connection Request/Response (EATT, and ECRED on BR/EDR);
- Disconnection Response.

Each channel carries both ends' CIDs and the PSM. In the JSON, **`scid` is ours** (the local
endpoint, which RX packets carry) and **`dcid` is the peer's** (which TX packets carry), the same
view as `l2test` and BlueZ's socket options.

The CIDs inside the signalling are from the sender's point of view:
- a request's Source CID is the requester's endpoint;
- a response's Destination CID is the responder's endpoint.

So the CID that is "ours" depends on which way the command went. The decoder resolves this per
direction, and an LE/ECRED response is matched to its request by the signal identifier.

PSMs are named: SDP, RFCOMM, BNEP, HID, AVCTP, AVCTP browsing, AVDTP, ATT, EATT. Other PSMs show
as `PSM 0x....`. A channel that was opened before the trace started is counted under its CID with
`psm` 0.

Signalling messages split over several ACL fragments are reassembled, up to 1 KiB. Other channels
are not reassembled: only the header in the first fragment is read.

### AVDTP and A2DP

On a link, **the first L2CAP channel on PSM 0x19 is AVDTP signalling.** Every later PSM 0x19
channel is the **transport (media) channel** of the stream that was just OPENed (AVDTP §5.4.6). If
no OPEN is pending, it belongs to the first stream that has no transport yet.

Signalling is decoded in full. Each message produces an `avdtp` event:

| Signal | Decoded content |
|---|---|
| DISCOVER | the SEP list |
| GET_(ALL_)CAPABILITIES | codec capabilities, delay reporting |
| SET_CONFIGURATION | the codec configuration, see below |
| RECONFIGURE | the new configuration |
| OPEN, START, SUSPEND, CLOSE, ABORT | state changes |
| DELAY_REPORT | the reported value, in ms |
| any reject | the error code |

SET_CONFIGURATION codec configurations:
- **SBC:** rate, channel mode, blocks, subbands, allocation, bitpool range.
- **AAC:** object type, rate, channels, bitrate, VBR.
- **aptX, aptX HD, aptX LL, FastStream:** rate and channel mode.
- **LDAC:** rate and channel mode.
- **Opus (Google):** rate, channels, frame duration.
- **LC3plus** and other vendor codecs are named by vendor and codec ID.

Commands are matched to their responses by transaction label and direction. The state of each
stream moves only on an **accept**: configured → open → streaming ⇄ suspended, and the stream is
removed on CLOSE or ABORT. A stream is identified by our SEID (`lseid`) and the peer's (`rseid`).
A command's ACP SEID names the SEP of whoever receives it.

Messages fragmented at the AVDTP layer (start/continue/end packets) are named in the events but
not decoded. In practice only very long capability lists use them.

### RTP on the media channel

Each media packet's RTP header (sequence number, timestamp, SSRC) is read on whichever side sends
it. TX is ours (A2DP source); RX is the peer's.

- **`rtp_lost`** counts the gaps in the sequence numbers during this window. `rtp_lost_total`
  counts them since the stream started.
  - Duplicates and late packets count neither as lost nor as progress.
  - A new SSRC restarts the count.
  - Packets lost **on TX** never reached HCI: the source (PipeWire, bluealsa) dropped them.
  - Packets lost **on RX** were lost before the peer's controller sent them.
- **`rtp_jitter_ms`** is the RFC 3550 interarrival jitter, J += (|D| − J)/16. The HCI timestamp is
  used as the arrival time, and the codec's sample rate as the RTP clock.
  - On TX it measures how evenly the source hands packets to the kernel, so scheduling hiccups on
    the Zero W show up here. On RX it measures how evenly the peer's packets arrive.
  - It is updated only between consecutive sequence numbers. Steps over 1 s are ignored: they
    mean a source restarted its clock, or a vendor snoop log that only keeps some media packets.
  - It is `null` when the rate is unknown.
- **`frames_per_packet`**: for SBC, the frame count from the media payload header, averaged over
  the window.
- aptX, aptX LL and FastStream have no RTP header and get no RTP statistics.

### Events

`/api/hci/events` keeps a ring of the last 500 events, and `hci.event` pushes each one as it
happens. Each event has a `kind`:

| Kind | What |
|---|---|
| `index` | controller added, removed, up or down |
| `conn` | a link was connected, or a connection attempt failed |
| `disconn` | a link was disconnected, with the reason |
| `l2cap` | a channel was opened, closed or refused, with its PSM and both CIDs |
| `avdtp` | an AVDTP signal, as above |
| `mark` | a mark |

Events that carry structured detail have a `data` object, for example the codec and config of
an `avdtp` event.

## Marks

`POST /api/hci/mark {"text":"…"}` (`bench mark "…"`) writes a user-logging frame to the kernel's HCI
logging channel (`HCI_CHANNEL_LOGGING`), with ident `btbench`. The frame format is the one in
bluez `src/shared/log.c`. Because the kernel delivers it to every monitor, the mark shows up:
- in btmon, as `btbench: …`;
- in the btsnoop ring;
- in a live Wireshark capture;
- in the events ring.

The answer `{"ok":true,"seq":N,"logged":true}` says whether the kernel took the frame. It never
does in replay mode, but the mark still goes into the events ring.

Marks found in a replayed capture show up as `mark` events too. bluetoothd's own log lines on that
channel are ignored.

## Live capture in Wireshark

```sh
curl -sN http://10.55.0.1/api/hci/live.pcap | wireshark -k -i -
# or keep a file:
curl -sN http://10.55.0.1/api/hci/live.pcap > live.pcap
```

The stream is a pcap with linktype 254, `LINUX_BT_MONITOR`. Each record is the 4-byte pseudo-header
(adapter index, monitor opcode; both big-endian) followed by the monitor payload, so Wireshark
decodes it like a btmon capture, marks included. It starts from the moment of connection.

- Each stream has a 1 MiB queue. If the client falls behind (slow Wi-Fi), whole 64 KiB chunks
  are dropped, oldest first. The count is logged when the stream ends.
- The board's HTTP server has 3 worker threads, and **each open stream holds one** for as long as
  the client stays connected. So at most 2 streams are allowed; a third gets 503.
- A stream ends when the client disconnects (noticed within 1 s even when there is no traffic) or
  when btbenchd stops.

## The capture ring (`/data/btsnoop`)

`btbench-btsnoop.service` (source `tools/target/btbench-btsnoop`) keeps btmon writing
`/data/btsnoop/hci-<YYYYmmdd-HHMMSS>.btsnoop` at all times:
- it rotates at 16 MiB;
- it keeps 8 files;
- the newest file is the one being written.

btbenchd does not write these files. It only manages them:

| Route | What it does |
|---|---|
| `GET /api/capture` | the unit state and the files, newest first. `active` marks the file btmon is writing: the newest one, while the unit runs |
| `PUT /api/capture {"running":false}` | `systemctl stop` (or `start`) of the unit. Runs with fork/exec and a 15 s timeout, never through a shell |
| `GET /api/capture/files/<name>` | downloads the file. If it is the active file, you get what was written so far; a cut-off last record reads as the end of the file |
| `DELETE /api/capture/files/<name>` | deletes the file. Refused (409) for the active file |
| `GET /api/capture/files/<name>/analyze` | the text output of `btmon -a`. 30 s timeout (504 with the partial output), output capped at 4 MiB |

`<name>` must be a bare file name ending in `.btsnoop`: no slashes, no leading dot.

Note that `analyze` occupies an HTTP worker while btmon runs.

## Threads and cost

- **One reader thread** blocks in `poll()` on the monitor socket.
  - It drains up to 64 packets per wakeup and decodes headers only.
  - It wakes just after each whole second to close the window, even when there is no traffic.
  - The 1 Hz `hci.stats` and the per-event `hci.event` are published from this thread, outside
    the stats lock.
- **One mutex** protects the decoder. The HTTP handlers take it briefly to copy JSON out.
- **No allocation per packet** on the data paths (ACL/SCO/ISO data, NOCP). Allocation happens:
  - when a link, channel, stream or event is created;
  - once a second, when the window's JSON is built;
  - every 64 KiB of traffic per live pcap stream.
- **ARMv6 has no 64-bit atomics**, so none are used. Shared state is under the mutex, and the only
  atomic is a 32-bit subscriber count.
- **Speed:** a 46 000-packet capture decodes in about 0.1 s on a desktop.

## Limits

- **Header decoding only.** ATT, SMP, AVCTP, RFCOMM, SDP and EATT are counted and named, not
  parsed.
- **AVDTP media detection relies on seeing the channel open.**
  - If btbenchd starts while a stream is already up, the media channel shows as `CID 0x....`,
    with no RTP stats, until the next reconnect.
  - AVDTP over LE, and A2DP signalling split across AVDTP fragments, are not decoded.
- **Latency is controller residency**, not end-to-end audio latency.
  - For SCO without flow control there is no NOCP, so there is no latency.
  - For BIS there is no peer ACK: NOCP means the controller scheduled the packet.
- **Controller buffer counts** for LE and ISO are unknown when the controller was initialised
  before btbenchd started (see above).
- **`index`/`handle` identify a link only while it is up**, because handles are reused.
  `/history` and `/latency` return 404 once the link has been dropped.
- **Clock steps.** The wall clock on a board without an RTC steps when NTP syncs. A backwards
  step re-anchors the windows; a long gap closes at most 300 empty windows. A NOCP that lands
  "before" its TX gives no sample.

## Development

- `app/tests/hci/`: synthetic packet tests, run by `make test`/ctest as `hci_decode`, `hci_pcap`,
  `hci_capture` and `hci_replay`. `hci_replay` writes btsnoop files and drives the whole module
  through its HTTP routes on a loopback server.
- `hci_snoop_summary FILE.btsnoop` (built next to the tests) runs any capture through the decoder.
  It prints the events and each link's busiest second, which makes it the quick way to check the
  decoder against a real trace. Its latency figures should agree with `btmon -a`.
