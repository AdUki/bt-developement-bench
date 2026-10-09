# HCI monitor

btbenchd watches every HCI packet between the host and the controller and turns them into numbers
for the Monitor tab and `bench mon`. It shows throughput per link and per L2CAP channel, how long
the controller holds our packets, controller buffer use, AVDTP stream state with the negotiated
codec, and RTP loss and jitter. It does this without btmon running and without touching the
traffic.

It also gives access to the always-on btmon capture ring (`/data/btsnoop`), shows what is inside
those files (a filtered packet list, decoded packets, graphs), and can stream a live pcap to
Wireshark.

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
| `GET /api/capture/files/<name>/packets` | the packet list, filtered and paged: see [Packet view](#packet-view) |
| `GET /api/capture/files/<name>/packets/<n>` | one packet decoded field by field, with its bytes |
| `GET /api/capture/files/<name>/graph` | time series over the capture, for the graph strip |

`<name>` must be a bare file name ending in `.btsnoop`: no slashes, no leading dot.

Note that `analyze` occupies an HTTP worker while btmon runs.

## Packet view

The Capture tab can open any file of the ring (or any btsnoop file put in `/data/btsnoop`) and
show what is inside: a packet list with a filter, one packet's decoded fields and hex dump, and
graphs over the whole capture. The code is `packet_view.{h,cpp}` (index, filter, routes' bodies)
and `dissect.cpp` (summaries and fields). All three datalinks are read: 2001 (btmon), 1002 (H4)
and 1001.

### The index

The first request for a file scans it once into an index of **32 bytes per packet**: file
offset, timestamp, controller index, monitor opcode, direction, length, the handle, the L2CAP CID
and PSM, the HCI opcode or event code, the protocol, flags (error, mark, continuation fragment)
and the TX latency. Everything a structured filter term or a graph needs is in it, so neither
reads the file again. Only a page of summaries (200 packets) or one packet's detail is decoded.

While scanning, the indexer keeps the same bookkeeping as the live decoder, cut down to what the
index needs:
- **links**, from their connection events, or from their first data packet;
- **L2CAP channels**, from Connection Request/Response, LE and enhanced credit-based connections
  and Disconnection Response. That maps a dynamic CID to its PSM and protocol, so `proto:avdtp`
  or `psm:25` work on channels that carry no protocol header of their own. As in the decoder, the
  first PSM 25 channel of a link is AVDTP signalling and later ones are media (`proto:rtp`).
  Continuation fragments inherit their start fragment's channel;
- **TX latency**: the HCI TX → NOCP FIFO per link, exactly as [above](#tx-latency-hci-tx--number-of-completed-packets).
  Every completed TX packet carries its latency, so `lat>20` finds the slow ones and the detail
  shows it.

Indexes are cached for the **two** most recently used files. A cached index is checked on every
request: if the file was replaced (a new inode) or shrank, it is rebuilt. If it grew — the file
btmon is writing — it is **extended from where it stopped**, including a record cut in half by
btmon the last time. At most 1 000 000 packets are indexed per file (32 MB); a ring file holds
50 000–320 000.

### Time budget and partial answers

The board has three HTTP workers, so no request may hold one for seconds. Each request may spend
**400 ms** indexing and filtering. If that is not enough, the answer says so:

- `"complete": false`: the file is not fully indexed yet (`indexed_bytes` < `file_size`), or a
  text filter has not looked at every packet yet (`scanned` < `packets_in_file`);
- `"next"`: the frame number the scan continues from.

The client repeats the same request, and each repetition picks up where the last one stopped:
filter results are cached per file and per filter string (the four most recent). The matches
found so far are already valid, because matches are only ever appended. `total` grows until
`complete` is `true`. The console does this by itself and shows the progress; `bench cap packets`
waits for it.

| On this PC (Ryzen laptop) | A2DP file, 16 MiB, 49 500 packets | LE scan file, 16 MiB, 318 600 packets |
|---|---|---|
| index the whole file | 19 ms | 59 ms |
| structured filter (`proto:rtp lat>30`), first page | 8 ms | — |
| text filter over every packet | 30–60 ms | 300 ms |
| graph | 2 ms | 4 ms |

A Zero W is 10–20 times slower. Indexing a full ring file there takes about 0.3–1.2 s, so the
first page arrives after at most one budget (under 1 s), and the totals complete over the next one
to three requests. A text filter over a dense file takes 3–6 s of CPU in total, delivered in
400 ms slices; its first matches show at once if they are common. Structured filters stay well
inside one budget.

### Filter syntax

Terms are separated by spaces, and all of them must match. `!term` negates a term, and `|` means
"or" inside a value.

| Term | Matches |
|---|---|
| `type:cmd\|evt\|acl\|sco\|iso\|index\|log\|mgmt\|diag` | the kind of packet; `log` is system notes and user logging (bluetoothd, marks) |
| `dir:tx\|rx` | host → controller, or back. Commands are TX and events RX; notes have no direction |
| `proto:hci\|l2cap\|att\|smp\|sdp\|rfcomm\|avdtp\|rtp\|avctp\|bnep\|hid\|sco\|iso` | the topmost protocol the index knows. `l2cap` is the signalling channels and channels of unknown protocol |
| `handle:11` | ACL/SCO/ISO data of that handle, and the commands and events about it (Disconnect, Encryption Change, NOCP, ...) |
| `cid:0x40`, `psm:25`, `index:0` | the L2CAP channel as carried in the packet, its PSM, the controller |
| `opcode:0x0406` | an HCI command, and the Command Complete/Status that answer it |
| `evt:0x13`, `subevt:0x02` | an HCI event code; an LE Meta subevent |
| `is:err\|mark\|cont\|lat` | a failure (non-zero status, ATT error, L2CAP reject or refusal, SMP Pairing Failed, AVDTP reject, SDP error, RFCOMM DM, AV/C rejected); a btbench mark; an L2CAP continuation fragment; a TX packet with a latency |
| `len>100`, `len<10`, `len:27` | the HCI packet length. Also `>=`, `<=` and `=` |
| `t>=12.5 t<30` | seconds since the first packet. The graph zooms to the range |
| `n<=500` | the frame number |
| `lat>20` | TX latency in ms. Packets without one match no `lat` term |
| any other word, `"a phrase"` | a case-insensitive substring of the summary |

Numbers can be decimal or `0x` hex. A word with a colon that is not one of the fields above, like
a BD_ADDR, is text. An unknown value (`type:foo`) or a bad number answers 400 with the reason.

Structured terms are answered from the index. Text terms decode the summary of every packet that
passed the structured terms, so `proto:att "write request"` is much faster than `"write request"`
alone.

### What is decoded

Each packet has a **summary** (the list's one line, which the text filter searches) and a
**field tree** with byte ranges into the packet, for the detail pane:

| Layer | What |
|---|---|
| HCI commands | 225 by name; parameters of connection management, scan/advertising enables, LE create connection/update/encryption, sniff mode, name/class/scan writes, pairing replies; the handle of every per-connection command |
| HCI events | 55 events and 29 LE subevents by name, with status (the error by name). Command Complete/Status name their command; Command Complete decodes the common return values (version, address, name, buffer sizes, RSSI, key size). Connection, disconnection (reason), encryption, mode change, remote features/version/name, IO capability/user confirmation, NOCP, inquiry results and LE advertising reports (address, RSSI, name from the AD data; the AD structures in the detail) |
| ACL / L2CAP | handle, packet boundary, length; CID, PSM; fragments. Signalling: every command by name, with its CIDs, PSM, result, configuration options (MTU, mode, FCS) and LE/enhanced credit-based parameters |
| ATT | every opcode; handles, offsets, values (with text when printable), UUIDs of the read-by-type/group requests, find information, error responses (request, handle and error by name) |
| SMP | every code; pairing request/response (IO capability, auth requirements, key size), Pairing Failed reason, identity address |
| SDP | PDU type, transaction, the UUIDs of a search pattern, record handles, error codes |
| RFCOMM | frame type, DLCI, P/F, credits, multiplexer commands (PN, MSC, ...), and the data as text when it is text (the AT commands of HFP) |
| AVDTP | signalling: signal, message type, SEIDs, the capability list with the codec decoded (the monitor's `decode_media_codec`), DISCOVER's SEPs, delay reports, rejects with their error. Media: the RTP header, and SBC's frame count and first frame header (recognised by its syncword) |
| AVCTP / AVRCP | the AV/C frame type; PASS THROUGH operations; vendor-dependent PDUs by name (RegisterNotification's event, SetAbsoluteVolume's volume, ...); browsing PDUs by name |
| SCO / ISO | handle, packet status; ISO sequence number, SDU length, timestamp |
| Monitor notices | new index (bus, address, name), index info, system notes, user logging (`ident: message`), management channel open/close/commands/events |

### Routes

`GET /api/capture/files/<name>/packets?filter=&start=0&count=200` (also `at_t=SECONDS`: start at
the first match at or after that time; `at_n=N`: at the match holding frame N or the next one;
`count` at most 1000):

```json
{"name": "hci-20261009-120000.btsnoop", "filter": "proto:rtp lat>100",
 "total": 92, "start": 0, "complete": true, "next": null, "scanned": 49549,
 "packets_in_file": 49549, "indexed_bytes": 16757713, "file_size": 16757713, "datalink": 2001,
 "t0_ms": 1760000000000.0,
 "packets": [{"n": 1137, "ts_ms": 1760000032284.542, "t": 32.284542, "index": 0, "dir": "tx",
              "type": "acl", "proto": "rtp", "handle": 11, "cid": 115, "psm": 25, "len": 629,
              "lat_ms": 116.181, "summary": "RTP seq 1340 ts 302848, SBC 7 frames, bitpool 53"}]}
```

`n` is the 1-based frame number in the file, as btmon and Wireshark count it. `t` is seconds since
the first packet, `ts_ms` the capture's own clock. `index`, `handle`, `cid`, `psm`, `proto` and
`lat_ms` are `null` when they do not apply. `"err": true`, `"mark": true` and `"cont": true` appear
on the packets they describe.

`GET /api/capture/files/<name>/packets/<n>`: the same object, plus `fields` and `hex`. `fields` is
a tree of `{"name", "value", "off", "len", "children": [...]}` nodes. `off`/`len` are byte ranges
into `hex`, the HCI packet as the monitor carries it (no H4 type byte). The first node, `Frame`,
gives the time, controller, monitor opcode, length and TX latency. 404 for a frame that is not in
the file (or not indexed yet: then the message says to try again).

`GET /api/capture/files/<name>/graph?filter=&bucket_ms=&points=&from=&to=&conn=INDEX:HANDLE`:

```json
{"from": 0.0, "to": 501.244, "bucket_ms": 500, "buckets": 1003, "matches": 49549, "total": 49549,
 "complete": true, "filtered": {"pkts": [...], "tx_bytes": [...], "rx_bytes": [...]},
 "conns": [{"index": 0, "handle": 11, "type": "acl", "peer": "AA:BB:CC:DD:EE:FF",
            "tx_bytes": 15284272, "rx_bytes": 553, "tx_pkts": 24493, "rx_pkts": 39, "lat_n": 24492,
            "first_t": 25.424, "last_t": 501.244}],
 "conn": {"index": 0, "handle": 11, "tx_bps": [...], "rx_bps": [...],
          "lat_p50": [...], "lat_p95": [...], "lat_max": [...]}}
```

- `filtered`: per bucket, the packets that match the filter, and their bytes by direction.
- `conns`: every link seen, busiest first, at most 32.
- `conn`: one link's throughput (HCI payload bit/s, as the live monitor counts it) and TX latency
  per bucket. The link is the one `conn` names (`HANDLE` alone means controller 0), or the busiest
  one. Latency is `null` in buckets with no completion; a sample is put in the bucket of its TX.
- The range is `from`/`to` (seconds since the first packet), else the filter's `t` terms, else the
  whole capture. Without `bucket_ms`, the width is the smallest of 1/2/5 × 10ⁿ ms (then 15 s, 30 s,
  minutes, hours) that keeps at most `points` buckets (default and maximum 2000). The console asks
  for one bucket per 2 px of its canvas.

Every error is `{"error": "..."}`: 400 for a bad name or filter, 404 for a missing file or packet,
415 for a file that is not a btsnoop capture.

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

- **Header decoding only, live.** ATT, SMP, AVCTP, RFCOMM, SDP and EATT are counted and named
  by the live monitor, not parsed. The [packet view](#packet-view) of a capture file does decode
  them.
- **The packet view decodes one fragment at a time.** An L2CAP SDU split over several ACL packets
  shows its first fragment decoded (marked `[got/total]`) and the rest as continuations; only
  signalling is reassembled, for the channel tracking. A capture that starts with links already up
  has no PSM for their channels (`proto:l2cap`), as in the live monitor. Latency filters on the
  file being written can miss the last few TX packets: their NOCP was not in the file yet when
  that filter was first scanned.
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
  `hci_capture`, `hci_replay` and `hci_packets`. `hci_replay` writes btsnoop files and drives the
  whole module through its HTTP routes on a loopback server. `hci_packets` covers the packet view:
  the index (and its growth while a file is written), the filter language, the summaries and
  field trees of the common packet kinds, the graph's buckets and the packet routes, including
  partial answers under a zero budget.
- `hci_snoop_summary FILE.btsnoop` (built next to the tests) runs any capture through the decoder.
  It prints the events and each link's busiest second, which makes it the quick way to check the
  decoder against a real trace. Its latency figures should agree with `btmon -a`.
