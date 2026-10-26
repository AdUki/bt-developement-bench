# HCI monitor

btbenchd watches every HCI packet between the host and the controller and turns them into numbers
for the console's Traffic area and `bench mon`. It shows throughput per link and per L2CAP channel,
how long the controller holds our packets, controller buffer use, AVDTP stream state with the
negotiated codec, and RTP loss and jitter. It does this without btmon running and without touching the
traffic.

It also gives access to the always-on btmon capture ring (`/tmp/btsnoop`, in RAM) and shows what is
inside its files as one packet list: a filtered list, decoded packets, graphs, and for the file
being written a live end that grows as btmon writes it, for as long as someone watches. It can
also stream a live pcap to Wireshark.

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

  A replay has no btmon writing the ring, so the monitor records what it replays into the capture
  directory itself, as `hci-<start time>.btsnoop`, a record per replayed packet as it is due. While
  it does, `/api/capture` marks that file `active`, and the [packet view](#packet-view) follows it
  as it follows btmon's file on a board. The directory must exist (`make run` creates it); the
  recording stays when the replay ends. (`Options::replay_record`, on in the daemon, off in tests.)

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
  - Packets lost **on TX** never reached HCI: the source (PipeWire) dropped them.
  - Packets lost **on RX** were lost before the peer's controller sent them.
- **`rtp_jitter_ms`** is the RFC 3550 interarrival jitter, J += (|D| − J)/16. The HCI timestamp is
  used as the arrival time, and the codec's sample rate as the RTP clock.
  - On TX it measures how evenly the source hands packets to the kernel, so scheduling hiccups on
    the Zero W show up here. On RX it measures how evenly the peer's packets arrive.
  - It is updated only between consecutive sequence numbers. Steps over 1 s are ignored: they
    mean a source restarted its clock, or a vendor snoop log that only keeps some media packets.
  - It is `null` when the rate is unknown, and in a second with no media packet (a paused or
    suspended stream): the estimate would only repeat the last one.
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

## A controller that stops answering

The monitor also notices a controller that was sent something (a command, data) and has sent
nothing back for 5 s. btbenchd then asks it Read Local Version over a raw HCI socket — a peer out
of range leaves data unanswered too, but its controller still answers that — and one that gives no
answer within 5 s is reset: its driver is unbound and bound again in sysfs (`hci/watchdog.cpp`),
with a mark saying so in the capture. BlueZ then shows the adapter afresh, the bench restores its
settings and calls its devices back. At most once a minute per controller, five times per run.

Seen on the Zero W's BCM43430 (UART, Raspberry Pi OS's BCM43430A1.hcd) under aptX HD to a sink:
its radio keeps the link up, but from one moment it sends the host nothing — the UART's receive
counter and interrupts stop, CTS stays asserted and the host's bytes still go out — so every
command timed out (adapter settings fail as "Authentication Failed", connects as no answer) and
the kernel only noticed minutes later, or never. Without the reset only a reboot brought it back.

The cause was bytes lost on their way to the controller: once it answered an "Unknown HCI
Command" 0xed59 that the host never sent — the bytes 01 59 ed of an RTP header it had just been
sent, read as a command after it lost its place in the H4 stream — and wedged 180 ms later.
Mainline's PL011 driver lacks the Raspberry Pi kernel's fixes for the BCM2835's TX FIFO (a
character written to a full FIFO is dropped, and a busy Zero W fills it ~1300 times a second):
the bench's kernel carries them (src/linux, `serial: amba-pl011: ...`), with
`cts-event-workaround` on uart0 (boards/dts/rpi0w-btbench.dtsi). The reset stays as the net for
whatever else stops a controller.

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

## The capture ring (`/tmp/btsnoop`)

`btbench-btsnoop.service` (source `tools/target/btbench-btsnoop`) keeps btmon writing
`/tmp/btsnoop/hci-<YYYYmmdd-HHMMSS>.btsnoop` at all times:
- it rotates at 8 MiB;
- it keeps 4 files, 32 MB of RAM at most (`ROTATE_SIZE`, `ROTATE_COUNT` in
  `/data/btbench/btsnoop.env`);
- the newest file is the one being written. At a rotation the next btmon starts on the new file
  before the old one is stopped, so a few packets land in both.

The ring is in RAM: a capture is written all the time, and the SD card is kept for settings
(docs/system.md "The SD card"). It is gone at reboot; download a file to keep it.

btbenchd does not write these files. It manages them, and writes one small file beside each new
one: its [context](#context), `<name>.ctx`.

| Route | What it does |
|---|---|
| `GET /api/capture` | the unit state and the files, newest first. `active` marks the file being written: the newest one while the unit runs, or the one the monitor records while [replaying](#where-the-packets-come-from) |
| `PUT /api/capture {"running":false}` | `systemctl stop` (or `start`) of the unit. Runs with fork/exec and a 15 s timeout, never through a shell |
| `GET /api/capture/files/<name>` | downloads the file. If it is the active file, you get what was written so far; a cut-off last record reads as the end of the file |
| `DELETE /api/capture/files/<name>` | deletes the file and its `.ctx`. Refused (409) for the active file |
| `GET /api/capture/files/<name>/analyze` | the text output of `btmon -a`. 30 s timeout (504 with the partial output), output capped at 4 MiB |
| `GET /api/capture/files/<name>/packets` | the packet list, filtered and paged, and with `wait=` the live end of the file being written: see [Packet view](#packet-view) |
| `GET /api/capture/files/<name>/packets/<n>` | one packet decoded field by field, with its bytes |
| `GET /api/capture/files/<name>/graph` | time series over the capture, for the graph strip |
| `GET /api/capture/files/<name>/stats` | what a filter matches, counted by protocol, packet kind and link |
| `GET /api/capture/files/<name>/export` | the packets a filter matches, as a btsnoop file |

`<name>` must be a bare file name ending in `.btsnoop`: no slashes, no leading dot.

Note that `analyze` occupies an HTTP worker while btmon runs.

## Packet view

Traffic → Packets shows a file of the ring (or any btsnoop file put in `/tmp/btsnoop`): a packet
list with a filter, one packet's decoded fields and hex dump, and graphs over the whole capture.
It opens on the file being written, at its end, and keeps growing with it ([The live end](#the-live-end));
Traffic → Captures opens any older file in the same view. One model for old and new packets: a
packet is a frame of its file, numbered as btmon and Wireshark count it, filtered, summarised and
decoded by the same code whether it was written an hour ago or just now. The code is
`packet_view.{h,cpp}` (index, filter, routes' bodies) and `dissect.cpp` (summaries and fields).
All three datalinks are read: 2001 (btmon), 1002 (H4) and 1001.

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

### Context

Only the first file of the ring starts at the beginning. The others start mid-session: the phone
connected, its AVRCP and A2DP channels opened, its GATT database was discovered, maybe hours and
several files earlier. On its own such a file cannot say that CID 0x0076 is AVRCP browsing, which
codec a stream carries, or which characteristic a handle is, and neither can btmon.

So btbenchd's reader thread, which sees every packet anyway, keeps a **context journal**
(`context.{h,cpp}`): the packets that explain later ones, for as long as they still apply.

| Kept | Until |
|---|---|
| New/Open/Close Index, Index Info (the controllers) | Delete Index |
| connection complete events: BR/EDR, SCO/eSCO, LE, CIS | the link's Disconnection Complete, a new connection on its handle, or its controller's Close Index (a power-off, a bluetoothd restart: the kernel ends every link without a Disconnection Complete) |
| L2CAP signalling, fragments included (which PSM and mode a channel is) | the link goes; the newest 400 per link |
| AVDTP signalling (which codec a stream carries) | the link goes; the newest 200 per link |
| ATT discovery: Exchange MTU, Find Information, Read By (Group) Type, their errors (btmon's att.c names characteristics from them) | the link goes; the newest 1500 per link |
| ISO setup ([ISO data](#iso-data)): LE Create BIG (and its test form) and its Complete, LE BIG Create Sync and BIG Sync Established, LE Set CIG Parameters (and its test form) and its Command Complete, LE Create CIS, LE CIS Request, LE Setup ISO Data Path, LE Set Periodic Advertising Data | its controller's Close Index; the newest 64 per controller |
| the newest periodic advertising report of each sync train that carries a BASE (a train repeats it every interval: only a different one replaces it) | its controller's Close Index |
| ASE states notified on a link (Codec Configured to Disabling), by their characteristic or their layout | the link goes; the newest 32 per link |

The journal classifies packets with the indexer's `Classifier`, so it knows the channels the
same way. It costs one classification per packet, and a few hundred KiB at most for a link that
stays up for days. It runs only when the capture directory exists; btbenchd watches that directory
with inotify.

**When btmon starts a ring file**, btbenchd writes the journal beside it as `<name>.ctx`: a
btsnoop file (datalink 2001), records in the order they happened, through a temporary file and a
rename. Context files whose capture is gone are deleted at the next rotation.

**When btbenchd starts** (a deploy or a restart, mid-session), the journal is seeded before the
live stream is read. It replays the ring files, oldest first, from the newest file that has a
context file (that file's context included) to the file being written. Normally that is one
file, about 0.4 s on the Zero W. The first start on a ring without context files reads all of
them: about 3 s for eight full files. The live decoder is then fed the journal too, so a link
open before the start keeps its peer, its named channels and its AVDTP stream (RTP loss and
jitter in Graphs) instead of reappearing "already connected" with bare CIDs. The context keeps
no Number Of Completed Packets, so after it the decoder forgets what it counted as in flight:
otherwise those packets would stay in flight and the first live completions would match them,
hours late.

**The index reads `<name>.ctx` first**, then resets the per-link counts, so a file's links count
only its own traffic. Links known only from the context, with nothing in the file, are left out
of the graph's list. An export with context puts the same records first.

Without a context file (a capture from elsewhere, a link older than every ring file), two
protocols are still recognised by their payload. The channel is then remembered with its likely
PSM:
- A2DP media, by its RTP header;
- AVRCP, by its AVCTP header (PID 0x110e or 0x110c), plain on the control channel, and behind
  an ERTM control field on the browsing channel.

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

### The live end

The file btmon is writing is a file like the others, indexed further on each request from where
the last one stopped. What makes it live is a **tail request**: the same list request, from the
first match the client does not have yet, with `wait=MS`. When that page would be empty and the
index and the filter are complete, the request does not answer at once: it waits for the file to
grow, and answers as soon as a new match is in it. The client shows the page, asks again from
its new end, and so on: packets are sent only when asked for, at the pace the client takes them,
and nothing at all happens for a view that is not open.

- **Waking up.** The waiting request watches the file with inotify (`IN_MODIFY`: btmon writes
  one record per `write()`), and its directory for a new capture. The watch exists only while
  the request waits; the reader thread is not involved, and no timer polls the file.
- **A burst is one page.** A tail answer that is not a full page goes out `kTailBatchMs`
  (150 ms) after its first match was in hand, whether that match was there when asked or arrived
  during the wait, with everything that came meanwhile. A client at the live end asks again the
  moment it has an answer, so without this a busy link would cost one request per packet; with
  it, A2DP (700 packets/s) is 5–7 requests a second of about 100 packets, and on a quiet link a
  packet shows within 150 ms.
- **Falling behind is not losing.** A client that reads slower than the file grows just has a
  longer list ahead of it: `total` grows faster than its position, the rows are in the file, and
  it can jump to the end. Nothing is dropped and nothing is skipped: the ring file is the buffer.
- **Rotation.** When btmon starts the next file, the request answers at once with `"newer":
  "<name>"` (the directory watch saw it), and a tail request on a file that is no longer the
  newest in the directory is answered the same way without waiting (the client was away over a
  rotation). The client moves to that file and continues at its start: its numbering is the new
  file's. The overlap of a rotation (a few packets in both files) is shown as such.
- **Nothing to say.** When nothing matching arrives within `wait` (at most `kMaxWaitMs`, 30 s),
  the answer is the empty page, and the client asks again. A file nobody writes (recording
  stopped, a replay that ended) costs one idle request per `wait`.
- **Deleted under the request:** it answers with what it has; the next request gets 404.
- **Workers.** A waiting request holds one of the HTTP workers, like a live pcap stream. At most
  `kMaxWaiters` (4) wait at once; a fifth gets 503. The pool grows for them (webserver.cpp).
- **Latencies at the live end.** A TX packet's `lat_ms` is `null` until its NOCP is written; the
  index fills it in when it indexes that NOCP, so the detail (and a later page) shows it, while
  the page that carried the packet does not. A `lat` filter therefore sees a TX packet only on
  a scan that already has its NOCP; the tail's own matches can miss the last few.

The console tails while its table is at the end and the page is visible, and stops (the request
is aborted) when the table is scrolled up or the view is hidden. `bench mon packets` is the same
loop in a shell.

### Filter syntax

Terms are separated by spaces, and all of them must match. `!term` negates a term, and `|` means
"or" inside a value. `||` on its own between groups of terms matches either group:
`proto:att dir:tx || is:err`.

| Term | Matches |
|---|---|
| `type:cmd\|evt\|acl\|sco\|iso\|index\|log\|mgmt\|diag` | the kind of packet; `log` is system notes and user logging (bluetoothd, marks) |
| `dir:tx\|rx` | host → controller, or back. Commands are TX and events RX; notes have no direction |
| `proto:hci\|l2cap\|att\|smp\|sdp\|rfcomm\|avdtp\|rtp\|avctp\|bnep\|hid\|sco\|iso` | the topmost protocol the index knows. `l2cap` is the signalling channels and channels of unknown protocol |
| `handle:11` | ACL/SCO/ISO data of that handle, and the commands and events about it (Disconnect, Encryption Change, NOCP, ...) |
| `cid:0x40`, `psm:25`, `index:0` | the L2CAP channel as carried in the packet, its PSM, the controller |
| `opcode:0x0406` | an HCI command, and the Command Complete/Status that answer it |
| `evt:0x13`, `subevt:0x02` | an HCI event code; an LE Meta subevent |
| `is:err\|mark\|cont\|lat\|vendor\|adv` | a failure (non-zero status, ATT error, L2CAP reject or refusal, SMP Pairing Failed, AVDTP reject, SDP error, RFCOMM DM, AV/C rejected); a btbench mark; an L2CAP continuation fragment; a TX packet with a latency; a vendor command (OGF 0x3f) with its Command Complete/Status, or a vendor event (0xff); an advertising report (legacy, directed, extended, periodic v1 and v2), BIGInfo report, scan request received, inquiry result (all three forms) or MGMT Device Found (and Advertisement Monitor Device Found): what scanning and periodic sync report, often most of a capture |
| `len>100`, `len<10`, `len:27` | the HCI packet length. Also `>=`, `<=` and `=` |
| `t>=12.5 t<30` | seconds since the first packet. The graph zooms to the range |
| `n<=500` | the frame number |
| `lat>20` | TX latency in ms. Packets without one match no `lat` term |
| `addr:AA:BB:CC:DD:EE:FF` | a peer: the packets of its links (from their connection events or the file's context), and packets whose summary names it (advertising reports, connection events). Its first bytes are enough: `addr:0c:32:3a` |
| any other word, `"a phrase"` | a case-insensitive substring of the summary |

Numbers can be decimal or `0x` hex. A word with a colon that is not one of the fields above, like
a bare BD_ADDR, is text. So is anything in quotes, `"proto:att"` or `"||"` included. An unknown
value (`type:foo`), a bad number, a malformed address or a `||` with nothing on one side answers
400 with the reason. The answer's `filter` is the filter as it was understood, and it parses back
to the same filter.

Structured terms are answered from the index. Text terms (and `addr:`, for the summaries that name
the peer) decode the summary of every packet that passed the structured terms, so
`proto:att "write request"` is much faster than `"write request"` alone.

In the console, a packet's detail lists its terms under the summary (`handle:12`, `addr:...`,
`proto:avctp`, `cid:0x0076`, `opcode:0x0406`, ...). A click adds one to the filter, and a shift-click
adds it negated; a term goes into every `||` group. The statistics' protocols and links work the
same way. A right click on a packet, in the list or the detail, filters what it is in or out: its
command or event, its protocol or channel, its link (an MGMT packet by its name, a quoted phrase,
as the index keeps no MGMT opcodes); several picked packets, their kinds together.

### What is decoded

Each packet has a **summary** (the list's one line, which the text filter searches) and a
**field tree** with byte ranges into the packet, for the detail pane. Where a layout is decoded,
every field of it is in the tree; bytes a layout does not cover (a newer spec's extra fields, a
vendor command) follow as **Parameters** (nothing decoded) or **Undecoded** (the tail), so no byte
is hidden and none is shown twice:

| Layer | What |
|---|---|
| HCI commands | 264 by name, with the parameters of the ones bluetoothd, PipeWire and btbenchd send: connection management (create, accept, reject, disconnect, packet types, role), pairing replies (link key, PIN, IO capability with its authentication requirement, passkey), synchronous connections (setup/accept, enhanced setup with each side's coding format, bandwidths, data paths, latency, packet types and retransmission effort), link policy and sniff (subrating), the controller set-up writes (event masks, timeouts, scan activity, page/inquiry scan type, inquiry mode, voice setting, flush and supervision timeouts, IAC LAPs, host buffer size, LE host support, authenticated payload timeout), name/class/scan/EIR writes. LE: event mask (the subevents by name), legacy and extended advertising (parameters with the properties, intervals, channel map, address types, filter policy, TX power, PHYs, SID and scan request notifications; data and scan response with their operation and AD structures; enable with its sets), periodic advertising (parameters, data, enable), scanning (legacy and extended per PHY), (extended) create connection per initiating PHY, connection update and parameter replies, encryption and LTK replies, data length, PHYs, resolving list and privacy mode, periodic sync (create, terminate, receive enable, transfer and its parameters), isochronous channels (CIG parameters and their test form with every CIS, create/accept/reject CIS, remove CIG, create BIG and its test form, terminate BIG, BIG create sync with its BISes, terminate sync, ISO data path set-up with its codec and removal, peer SCA, ISO TX sync, link quality), host features (the bit by name), power reporting and path loss, subrating; the handle of every per-connection command |
| HCI events | 55 events and 42 LE subevents by name, with status (the error by name). Command Complete/Status name their command; Command Complete decodes the return values of the commands people read the answer of: versions (the Bluetooth version and manufacturer by name), supported commands (how many), LMP features per page and LE features by name, local codecs (v1 and v2 with their transports), codec capabilities and controller delay, buffer sizes, address, name, class, voice setting, scan and inquiry settings, TX power levels, RSSI, link quality, AFH map, clock, key size, list sizes, data lengths, PHY, selected advertising TX power, advertising sets, CIG IDs and CIS handles, ISO TX sync and link quality, and the address or handle a reply is about. Connection, disconnection (reason), encryption, mode change, remote features (by name)/version/name, QoS and flow specification, packet type and synchronous connection changes (air mode by name), sniff subrating, link keys (type by name), IO capability/user confirmation/keypress, remote host features, NOCP, inquiry results. LE: (enhanced) connection complete with the RPAs and clock accuracy, connection update, remote features by name, PHY update, data length, channel selection, advertising set terminated, scan request received, legacy, directed and extended advertising reports (event type with the data status, PHYs, SID, TX power, periodic interval, direct address), periodic sync established (v2 too), periodic advertising reports, sync lost and sync transfer received, CIS established (v2 too: delays, latencies, PHYs, NSE, BN, FT, PDUs, ISO interval), CIS request, BIG complete, terminate and sync established/lost (with every BIS handle), BIGInfo reports, peer SCA, path loss, TX power reporting, subrate change. The summaries carry what matters: `LE CIS Established handle 2304 ISO interval 7.50 ms, latency 10.0/10.0 ms`, `LE Set CIG Parameters complete CIG 0 CIS 2304,2305` |
| Vendor commands | Intel's, on a controller whose Index Info (or Read Local Version Information) says Intel; on any other they stay `Vendor 0xfcXX` with their bytes, since Broadcom and Realtek give the same opcodes other meanings. Named as btmon names them, plus the ones btintel sends that btmon leaves unnamed (Set DSBR, Read Offload Use Cases, Read FSEQ Status). Decoded: the firmware download (Secure Send fragments, Reset with its boot address, Write Secure Boot Params), Read Version (the legacy layout and every TLV: CNVi/CNVr, the variant by name, image type, build, time stamp, locks, OTP address), Read Secure Boot Params, DDC Config Write (each entry, entry 0x0126 as LE features by name: where the BE200 keeps Auracast off), Set Event Mask, Manufacturer Mode, Write BD Address, Read Supported Features, Read FSEQ Status, Memory Write, PPAG Enable, and the Microsoft extension on 0xfc1e (subcommand, features, event prefix, RSSI monitors, advertisement monitors with their patterns, UUID, IRK or address). A controller's company is the latest the file said, so the firmware download, which comes before the kernel's Index Info, decodes too. Intel's vendor events (HCI event 0xff) too: Bootup (source, reset type and reason, DDC status), Secure Send Commands Result (the firmware file a controller refused), fatal, debug and system exceptions (module, line, reason), LE Link Established, Scan Status, LMP PDU Trace (the PDU as bytes), SCO Rejected via LMP, PTT Switch Notification, FW Trace String as text; behind the vendor prefix 0x8780 the Microsoft extension's LE Monitor Device (a monitored device in or out of range) and Intel's Extended Telemetry (the ACL and SCO link quality counters by name) |
| Advertising data | every AD structure of EIR, advertising reports and LE Set (Extended/Periodic) Advertising Data: flags and LE role by name, UUID lists (16/32/128-bit, solicitation), names, TX power, class of device, device ID, connection interval range, target addresses, appearance by category, advertising interval, LE device address, URI, LE features, manufacturer data with the company. LE Audio's service data field by field: Broadcast Audio Announcement (Broadcast ID), Basic Audio Announcement (the BASE: presentation delay, each subgroup's codec configuration and metadata, each BIS), ASCS and CAP announcements (type, available contexts, metadata), Public Broadcast Announcement, TMAP and GMAP roles. The summary adds an announcement's words: `LE Set Periodic Advertising Data set 1 BASE 1 BIS, LC3 48 kHz, 10 ms, FL+FR, 100 octets, media` |
| ACL / L2CAP | handle, packet boundary, length; CID, PSM; fragments. Signalling: every command by name, with its CIDs, PSM, result, configuration options (MTU, mode, FCS), information responses (extended features and fixed channels by name) and LE/enhanced credit-based parameters |
| ATT | every opcode; handles, offsets, values (with text when printable), UUIDs of the read-by-type/group requests, find information, find by type value ranges, error responses (request, handle and error by name); Characteristic declarations (properties, value handle, UUID) and Include declarations in Read By Type responses. In the detail, values decode by their characteristic: the console finds the link's ATT discovery before the packet (in the file and its context, `gatt_before`), so a handle reads as `0x0092 (Sink ASE)`, a Read Response as the value its request asked for and a CCC write as the configuration of the characteristic it belongs to. Decoded characteristics: ASCS (ASE states with every state's parameters, the ASE Control Point's operations and responses with response codes and reasons), PACS (PAC records with the LC3 capabilities and metadata, audio locations, available and supported contexts), BASS (Broadcast Receive State, the control point's add/modify/set code/remove source), VCS (volume state, control point, flags), MICS mute, CSIS (SIRK, size, lock, rank), TMAP role, MCS (player name, track title, duration, position, media state, media control point), CCC, GAP and GATT basics (name, appearance, preferred connection parameters, service changed, client and server features, database hash), battery level, PnP ID, device information strings. Without discovery in the capture (a client that cached the handles never runs it, so a board's own ASCS is usually unnamed), ASE states, ASE Control Point operations and PAC records are recognised by layouts whose lengths add up exactly, marked "(by its layout)". The summary stays the same with or without discovery |
| SMP | every code; pairing request/response (IO capability, auth requirements, key size, OOB, the keys each side distributes), Pairing Failed reason, identity address |
| SDP | PDU type, transaction, the search pattern and attribute ID list of requests, record handles, error codes, continuation state. Attribute responses: every record's attributes, named, with their values as text (UUIDs by name); the summary has each record's service class, service name, L2CAP PSM or RFCOMM channel and profile version (`Handsfree Audio Gateway "Hands-Free Voice gateway" channel 13 Handsfree 1.9`). A response split by a continuation state has its complete records decoded in the first part only |
| RFCOMM | frame type, DLCI, P/F, credits, multiplexer commands (PN, MSC, ...), and the data as text when it is text: the AT commands of HFP, with the codec IDs of `AT+BAC`/`+BCS` named (CVSD, mSBC, LC3-SWB), the gains of `+VGS`/`+VGM` and the feature bits of `+BRSF` in the tree. OBEX when the data is an OBEX packet |
| AVDTP | signalling: signal, message type, SEIDs, the capability list with the codec decoded (the monitor's `decode_media_codec`: SBC, MPEG-1,2, AAC with the A2DP 1.4 object types, aptX, aptX HD, aptX LL, aptX Adaptive, FastStream, LDAC, Opus from Google and from PipeWire (both directions, channels, audio location, frame durations, maximum bitrate), LC3plus HR; every field in the tree, and SBC's bitrate at the maximum bitpool), content protection (SCMS-T, DTCP), recovery, header compression, delay reporting, DISCOVER's SEPs, delay reports, rejects with the error by name (AVDTP and A2DP codes) and the failing category or SEID. Media: the RTP header, and SBC's frame count and first frame header (recognised by its syncword) |
| AVCTP / AVRCP | AVCTP fragmentation (start/continue/end); the AV/C frame type; PASS THROUGH operations (every panel operation, vendor-unique next/previous group). Vendor-dependent PDUs with their parameters: GetCapabilities (the events or company IDs), the player application settings (repeat, shuffle, equalizer, scan by value), GetElementAttributes (the attributes asked for; the response's title, artist, album, track number, total, genre, playing time and cover art handle with their text), GetPlayStatus (status, position, length), RegisterNotification with each event's data in interim and changed responses (play status, track UID, position, battery, settings, addressed player, UIDs, volume with its percentage), SetAbsoluteVolume, SetAddressedPlayer, PlayItem, AddToNowPlaying, continuing responses; AVRCP packet types (a response's start is decoded as far as it goes); rejects with their status. Browsing (behind the channel's ERTM control field: I- and S-frames, SAR, SDU length, the FCS checked): SetBrowsedPlayer (items, folder path), GetFolderItems (scope and range; the items: players, folders and media elements with their names and attributes), ChangePath, GetItemAttributes, GetTotalNumberOfItems, Search, GeneralReject. Summaries carry the values: `RegisterNotification CHANGED VOLUME_CHANGED: 87 (69%)`, `GetElementAttributes STABLE: Title "…", Artist "…", Album "…", Track 3, Time 3:25` |
| OBEX | over RFCOMM, and over L2CAP channels of their own (GOEP 2.0: recognised by a good ERTM FCS, the start of a segmented SDU included): opcode or response code, Connect's version and maximum packet length, SetPath flags, every header (Name, Type, Length, Target/Who with the service named — FTP, PBAP, MAP, BIP cover art —, Connection ID, SRM, Body sizes, ...) |
| SCO / ISO | handle, packet status; ISO packet boundary, timestamp flag and timestamp, sequence number, SDU length, packet status (lost or possibly invalid; from the host it is not used). In the detail, an ISO packet's stream and its SDU cut into codec frames: see [ISO data](#iso-data) |
| Monitor notices | new index (bus, address, name), index info, system notes, user logging (`ident: message`), management channel open/close |
| MGMT | every command and event by name (btmon's tables), with the parameters bluetoothd sends and reads: modes, settings, the device a command or event is about; controller info (address, version, manufacturer, supported and current settings, class, names), index lists, versions, supported commands and events, UUIDs added and removed (16-bit ones by name) with the class they lead to, class and name changes, IO capability, device ID, privacy, discovery, devices added and their flags, key loads (how many), new keys, connection information (RSSI, TX power), connected (flags, EIR), found (RSSI, EIR), new connection parameters, security level changes, advertising instances and features, controller capabilities, experimental features (by name, the active ones in the summary), advertisement monitors |

### ISO data

An ISO packet says only its handle, sequence number and SDU length. Which BIS or CIS the handle is,
and how its SDUs are cut into codec frames, was said once, when the stream was set up. For the one
ISO packet the detail shows, `iso_before()` (`iso_stream.{h,cpp}`) feeds what set it up, from the
file's context and then the file before the packet, to an `IsoTracker`:

- **A BIS of a broadcaster**: LE Create BIG names the BIG's advertising set, LE Create BIG Complete
  its BIS handles (BIS 1, 2, ... in order), and the newest LE Set Periodic Advertising Data of
  that set carries the BASE (fragments put together).
- **A BIS of a receiver**: LE BIG Create Sync names the sync handle and the BIS indices asked for,
  BIG Sync Established their handles in that order, and the newest periodic advertising report of
  that train with a complete BASE (found backwards from the packet; the context's otherwise).
- **A CIS**: LE Set CIG Parameters names the CIS IDs and its Command Complete their handles (a
  central), or LE CIS Request gives the CIG and CIS ID of a handle (a peripheral); LE Create CIS
  or the request names its ACL link. On that link, the ASE states the server notified: Codec
  Configured carries the codec configuration, QoS Configured to Disabling the CIG and CIS. Of the
  ASEs on one CIS, the one in the packet's direction: the peer's sink ASE for what this side sends,
  its source ASE for what it receives (the other way round when this side is the server), as ATT
  discovery named the characteristics; without discovery, the ASE whose configuration makes an SDU
  of the packet's size.
- LE Setup ISO Data Path, in the packet's direction: the data path and its coding format.

The BASE's codec configuration is the subgroup's, with the BIS's own entries over it (BAP 3.7.2.2).
The detail adds a **Stream** node (`BIS 1 of BIG 0`, the codec, sampling frequency, frame
duration, channel allocation, octets per codec frame, frame blocks per SDU, the frame that
configured it and the data path) and, when the SDU is complete and its size is what the
configuration describes (blocks × channels × octets), cuts it into its frames: per block, one
frame per channel in the order of the allocation's bits (BAP 4.3), `LC3 frame FL`, `LC3 frame FR`.
A fragment of an SDU, or an SDU of another size, stays bytes. Without its setup in the file or its
context the stream says so. The summary is the list's either way.

The setup packets are found from the index alone and are few; a CIS reads the ATT packets of its
ACL link, and a receiver's BIS the periodic reports newest first until one of its train.

### Routes

`GET /api/capture/files/<name>/packets?filter=&start=0&count=200` (also `at_t=SECONDS`: start at
the first match at or after that time; `at_n=N`: at the match holding frame N or the next one;
`count` at most 1000; `wait=MS`: a [tail request](#the-live-end), waiting up to MS for a match
when the page is empty):

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
`lat_ms` are `null` when they do not apply. `"err": true`, `"mark": true`, `"cont": true`,
`"vendor": true` and `"adv": true` appear on the packets they describe (the `is:` terms), and
`opcode`, `evt` and `subevt` on the commands and events that have them (the filter's terms of the
same names). A tail request's answer carries `"newer": "<name>"` when a newer
capture exists in the directory (the live end moved there); its `packets` are then whatever the
page had, usually none.

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

`GET /api/capture/files/<name>/stats?filter=`: the matches counted, from the index (no decoding
beyond what the filter needs):

```json
{"filter": "", "total": {"pkts": 12627, "bytes": 5963679, "tx": 4050, "rx": 8566, "err": 1},
 "marks": 0, "cont": 111, "span_s": 188.906,
 "protos": [{"proto": "rtp", "pkts": 10014, "bytes": 5859942, "tx": 3738, "rx": 6276, "err": 0}],
 "types": [{"type": "acl", "pkts": 10429, ...}],
 "links": [{"index": 0, "handle": 12, "type": "acl", "peer": "0C:32:3A:D6:C3:44", "pkts": 6799, ...}],
 "latency": {"n": 3943, "avg_ms": 268.022, "p50_ms": 259.5, "p95_ms": 916.458, "max_ms": 916.458},
 "complete": true, "packets_in_file": 12627, ...}
```

Busiest first; `proto` is `"other"` for packets with none (notes, index records); `latency` is
`null` without TX latencies. It has the same progress fields as the list.

`GET /api/capture/files/<name>/export?filter=&context=1`: the matches as a btsnoop file of
datalink 2001, whatever the source's datalink, named `<name>-filtered.btsnoop` (`-copy` without
a filter); `X-Packets` is how many. With `context=1`, the file's context and the context packets
among the matches come along, in order, so `btmon -r` and Wireshark decode the matches as they
would in the whole capture. 409 while the filter is still being worked out (list until
`complete` first).

The detail (`packets/<n>`) also carries `opcode`, `evt`, `subevt` where they apply, and the link's
`peer`: the terms the console offers.

Every error is `{"error": "..."}`: 400 for a bad name or filter, 404 for a missing file or packet,
415 for a file that is not a btsnoop capture.

## Threads and cost

- **One reader thread** blocks in `poll()` on the monitor socket.
  - It drains up to 64 packets per wakeup and decodes headers only.
  - It wakes just after each whole second to close the window, even when there is no traffic.
  - The 1 Hz `hci.stats` and the per-event `hci.event` are published from this thread, outside
    the stats lock.
- **One mutex** protects the decoder. The HTTP handlers take it briefly to copy JSON out.
- **The packet view** costs the reader thread nothing: it reads the ring's files, and the live
  end is a request waiting on inotify ([The live end](#the-live-end)), both on the HTTP worker
  that asked. The reader thread only keeps the context journal (one classification per packet)
  when the capture directory exists.
- **No allocation per packet** on the data paths (ACL/SCO/ISO data, NOCP). Allocation happens:
  - when a link, channel, stream or event is created;
  - once a second, when the window's JSON is built;
  - every 64 KiB of traffic per live pcap stream.
- **ARMv6 has no 64-bit atomics**, so none are used. Shared state is under the mutexes, and the
  only atomics are the 32-bit subscriber count of the live pcap streams and the count of tail
  requests waiting.
- **Speed:** a 46 000-packet capture decodes in about 0.1 s on a desktop.

## Limits

- **Header decoding only, live.** ATT, SMP, AVCTP, RFCOMM, SDP and EATT are counted and named
  by the live monitor, not parsed. The [packet view](#packet-view) of a capture file does decode
  them.
- **The packet view decodes one fragment at a time.** An L2CAP SDU split over several ACL packets
  shows its first fragment decoded (marked `[got/total]`) and the rest as continuations; only
  signalling is reassembled, for the channel tracking. A capture that starts with links already up
  and no [context](#context) knows their channels only where the payload says (RTP, AVCTP); the
  rest are `proto:l2cap`, as in the live monitor. Latency filters on the
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
- **Frame numbers and `t` are a file's.** At a rotation the live end moves to the next file and
  both start again, and the packets of the overlap are in both files. Frame numbers are what
  btmon and Wireshark show for that file, which is the point; a time of day is in `ts_ms`.
- **The live end shows what btmon wrote**, nothing else: with the capture stopped there is no
  live end, and the monitor's own statistics (which read the kernel socket) still run.
- **Clock steps.** The wall clock on a board without an RTC steps when NTP syncs. A backwards
  step re-anchors the windows; a long gap closes at most 300 empty windows. A NOCP that lands
  "before" its TX gives no sample.

## Development

- `app/tests/hci/`: synthetic packet tests, run by `make test`/ctest as `hci_decode`, `hci_pcap`,
  `hci_capture`, `hci_replay`, `hci_packets` and `hci_dissect`. `hci_replay` writes btsnoop files and drives the
  whole module through its HTTP routes on a loopback server, the recording of a paced replay and
  a tail request following it included. `hci_packets` covers the packet view:
  the index (and its growth while a file is written), the filter language, the summaries and
  field trees of the common packet kinds, ISO streams by their setup (in the file and through the
  context), the `is:vendor` and `is:adv` flags, the graph's buckets and the packet routes, including
  partial answers under a zero budget, and the live end: a tail request woken by records
  appended to the file (a burst as one page, a filter's matches only), by a new file in the
  directory and by the file's deletion, its timeout, and the limit on waiting requests.
- `hci_snoop_summary FILE.btsnoop` (built next to the tests) runs any capture through the decoder.
  It prints the events and each link's busiest second, which makes it the quick way to check the
  decoder against a real trace. Its latency figures should agree with `btmon -a`.
- `hci_dissect FILE.btsnoop [FILTER] [--detail] [--hex]` (built next to the tests) prints the packet
  view of a capture: each matching packet's number, protocol and summary, with `--detail` its field
  tree and with `--hex` its bytes. The way to compare the dissector with `btmon -r` on a real
  trace. `hci_dissect` (the ctest) checks the dissector on single packets, most of them taken from
  the boards' captures: AVDTP codecs and errors, AVRCP control and browsing, SDP records, AT
  commands and OBEX.
