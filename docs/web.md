# The web console

btbenchd serves a console at `http://10.55.0.1/` (the USB gadget link), `http://<hostname>.local/`,
or `http://localhost:8080/` with `make run`. Plain HTML/JS/CSS from `app/www/`, no build step:
`make deploy-www` copies it to the board and a reload shows it. Light or dark follows the system.
The REST and WebSocket API behind every button is in [api.md](api.md) (also served at `/api`).

The console is seven areas — **Listen**, **Devices**, **Audio**, **LE**, **Files**, **Traffic**,
**System** —
in a rail on the left (a bar at the bottom on a phone). Each area has a few views in the bar at
the top of the page; an area reopens on the view last used in it, and the address names the view
(`#audio/play`, `#packets`), so it can be bookmarked. Settings that are set once (the adapter's
name and agent, scan options, an advertisement's intervals, a stream's rate) sit behind a
*Settings*, *Options* or *More fields* disclosure, and raw values (configuration bytes, object
paths, capability hex) behind *Details* or in a tooltip.

On a desktop the page scrolls only where it has to. A view built around one long list — *Packets*,
*D-Bus*, *Logs*, an open capture — fills the window and only the list scrolls; *Listen* on a wide
screen (from 1400 px) shows Now playing and the library side by side, each column scrolling on its
own. The cards worth reading big (the library, the packet tables, the D-Bus and log views, a
link's graphs, each chart in *Graphs*) have a full-screen button; Esc puts them back. On a very
wide screen (1700 px) the pages are allowed wider.

The foot of the rail (the top bar on a phone) shows the board's name, the adapter (on / visible /
scanning; *slow to answer*, in amber, when BlueZ missed a read — the devices stay as it last listed
them), the WebSocket (live / offline; it reconnects by itself), temperature, CPU and — in red
— undervoltage, which on a Zero W corrupts the Bluetooth UART long before anything else notices.

The console keeps one WebSocket and subscribes only to what the view on screen shows, plus the
pairing question (`bt.request`) and the health readout (`system`). A view in the background, or a
console nobody looks at, costs the board nothing — except Graphs while *Record in background* is on.

## Pairing questions

When BlueZ asks the agent something (numeric comparison, a PIN, a passkey, a just-works or
profile authorization), a dialog comes up on whichever view is open, with the time left before
BlueZ gives up. With *Pairing questions* on **answer automatically** (Devices → Settings) only a
passkey to type is asked; on **ask me here** every question is. `bench agent` answers the same
questions from a terminal.

## Areas and views

**Listen** — the console as a music player, for using the board rather than testing it. Two
views. *Now playing* shows the source chosen: a connected phone (its AVRCP player — track,
cover art, progress, previous / play / pause / next / stop, repeat and shuffle when the player has
them, the link's volume) or a stream the board plays itself (a radio station, with the cover or
logo it sends and its description; tracks from a UPnP media server with their artist, album and
length, and repeat and shuffle for a playlist; a test signal, with its level, which only stops; or
anything started in Audio → Streams). Chips switch between them when there are several: one per
phone and one per stream the board plays. Above the title it says which way the music flows, the
same way for both: *Receiving · Bluetooth* from a device (and whether its audio is streaming into
the board, or only its player is known), or *Playing from the board* · Internet radio, Media
server, Test signal or Audio input; then the path, source → this board → where it plays. *Library*, a page of its own so long folders get the screen, has
*Up next* first — the phone's now-playing list, or the board's queue; a click plays that track —
then the phone's own folders and search (for a browsable player), the radio stations, the media servers
on the LAN, and *Test signals* (tones, a left/right check, a sweep, pink and white noise, silence,
and a tone of any frequency and level): a station plays on its own, a track plays with the rest of
its folder after it as the queue, and **+** adds a track to the end of the queue. A station or
tracks take the place of the URL stream shown, on its output; a test signal is a stream of its own
beside it, on the output chosen. Under the stations a stream's URL adds
it, named as the stream calls itself, and **×** on a station removes it. Media servers show their covers: a folder of
albums is a grid of them (title, artist, year), an album opens with its cover, artist, year, genre,
track count and length, *Play all* and *Add all*, and its tracks by number with their format
(`FLAC 24/96`, `MP3 44.1 kHz`); a track without a cover of its own shows its album's in Now
playing. Covers load from the server itself, so a browser on another network than the server
shows the plain icons. On a wide screen both are on screen side by side, so the two view buttons are not shown. A
phone shows here as soon as it is connected, before it has played anything: its play button is
how the music is started from the console. A bar at the bottom of the library keeps the
track playing, play/pause and next in reach. Every source has the same *Play to* — none, the
board's default output, the loopback card or a Bluetooth speaker, each a button — and, apart from
that, *Listen in this browser*, and each stream has its own: a tone can play on one speaker while
the radio plays on another. The board keeps playing with the page closed. A phone's music always
plays on the default output (PipeWire links it there); picking a speaker sends it on there too (a
capture of it, a tunnel), and listening in this browser starts within a second while the phone
plays into the board — one that does not is asked once to play, never paused. Both change what
plays at once: an output picked moves the stream there (the
station and the queue carry on), and the line under the buttons says what the board is doing —
asking, opening the output, waiting for the Bluetooth link to stream, *Playing on … · codec* — or
why it could not (a speaker not connected, one that went away, one that takes no audio). The volume slider sets what a person means: the Bluetooth speaker's
own volume when the link has one, else the stream's level, or this browser's.


**Devices** — the adapter bar. With two adapters (the board's own and a USB one) it starts with a
picker (`hci0 ★ | hci1`, ★ the primary, a hover says the maker, Bluetooth version and USB): the bar,
the scan and the device list are then that adapter's, the choice is remembered in the browser, and
*Make primary* moves advertising, the GATT server and the HID remote to it. Then: powered, visible,
pairable, and under *Settings* the name others
see (the board's hostname, `btbench-<mac>`, until one is typed; emptied, it goes back to that), how long it stays visible, how pairing questions are answered (automatically, or asked here)
and the IO capability (which decides the pairing method a peer gets). *Scan* finds devices; its
*Options* are every filter BlueZ has: the transport (dual: LE and BR/EDR, LE only, BR/EDR only),
when to stop, a signal filter (RSSI at least, or path loss at most), services by UUID, a name or
address prefix, every advertisement or only changed ones, the board visible while it scans, and
connecting to what the prefix matches; a filter the adapter's BlueZ does not take is greyed out,
and the options are remembered in the browser. While a scan runs its filter is spelled out beside
the button. A device is one row: a light (green connected, pale paired, amber busy), its name,
address, kind, signal (bars and dBm, from the scan) and its transports (BR/EDR, LE; lit where
connected), and the one action its state calls for — pair, connect, disconnect, or cancel a
pairing (one button of one width, which shows the work in progress itself); a connected device also
shows the profiles it has up (A2DP, AVRCP, HFP, HID, PAN, GATT; lit while streaming). One of the
board's own adapters, seen advertising by the other, is tagged *this board · hciN*. Rows keep their
place while they change (a connect, a pairing, a new RSSI) and only a row that changed is redrawn,
with a short highlight when its state does; new devices go last, and the list is sorted again
(connected, paired, then by name) when the view is opened or the filter or adapter changes. A click on the row opens the rest: each transport with its own state (connected,
bonded or paired, connectable) and its own connect or disconnect (whose progress and failure show
on that transport's line, not the device's), which transport a dual-mode device
connects on first, why it last disconnected; *Profiles*: each one connected — an A2DP link with
its direction, codec and state, AVRCP and its player, HID, PAN, GATT with its service count, the
battery level, and from PipeWire the profile its audio is on (the only place HFP shows: PipeWire's
own HFP does not go through BlueZ); for a connected device its live link (RSSI and TX
power per transport, read from the controller every two seconds); class, appearance, advertised TX
power, services by name, manufacturer and service data, advertising flags; trust and remove (click twice); connect or
disconnect one profile by UUID, and pair with a PIN for legacy devices; and shortcuts that open
the GATT explorer, a file session or the AVDTP probe on that device. *Input devices*: the
Bluetooth keyboards, remotes, mice and game pads connected to the board (BlueZ makes each a kernel
input device): their report maps read out (Consumer Control, Keyboard, Mouse, … and the reports'
sizes; the raw map under a disclosure), the last key pressed and a live log of presses, and
*Media keys control what plays*: a remote's play/pause, next, previous and volume keys drive the
connected phone's player, as with a speaker.

**LE** — *GATT explorer*: pick a device, connect (a dual-mode device such as a speaker connects
and disconnects its LE link only, so a classic connection carrying audio stays), and its services
appear by handle, folded when there are more than four (each says how many characteristics it
has; what you open stays open across a reload). A characteristic is one line: its value (as text
too when it is a string) and read, write (hex, or `text:…`; request / command / reliable as the
flags allow) and notify; its descriptors read the same way. Every value change lands in the
notification log; below it, the *advertisers in range*, strongest first,
with what each advertisement carried. *Advertise*: a form for `LEAdvertisingManager1`
(type, name, UUIDs, discoverable; under *More fields* manufacturer and service data as `id:hex`,
appearance, includes, intervals, TX power, timeout); several at once, each with its state (an advertisement whose timeout ran out shows
"released"). *Remote control*: the board as a Bluetooth LE remote (HID over GATT). Switched on, it advertises
under its name; pair it from a phone, TV or computer like any remote. The pad has power, mute,
a d-pad with OK, back, home, menu, previous / play–pause / next, and volume and search rockers;
a key stays down while it is held (a volume key repeats on the host as with a real remote), and
with the pad focused the keyboard's arrows, Enter, Escape, Space and + / − press its keys. *Type*
sends text as key presses; *Battery* sets the level hosts show. *GATT server*: an application as JSON (the Example button loads one with a
heart-rate counter, a battery level and an echo characteristic), its live values, and a log of
what remote devices read, wrote and subscribed to.

**Audio** — four views, from the links to the audio server. *Bluetooth*: a card per connected
audio device — each audio link on it, which way it goes (*Receiving*: the device plays into the
board; *Sending*: the board plays to it), its codec in words, its state (streaming or idle), the
sink's reported delay and the AVRCP absolute volume, and *Listen here* on a receiving link; the
PipeWire profile, which is the codec choice (send with SBC, SBC-XQ, AAC, aptX, LDAC, Opus — what
the build has — both ways, receive, or HFP with CVSD/mSBC; the link renegotiates on a change);
folded, the device's remote stream endpoints with their capabilities, and the raw configuration
and paths. Under the cards the *AVDTP probe*: any device's own DISCOVER / GET_ALL_CAPABILITIES
answer, connected or not, every stream endpoint with its role, codec and other capabilities.
*Players*: AVRCP only — each connected device's player with its track (title, artist, album,
genre, track n of m), a progress bar that moves between the phone's updates, its cover art (AVRCP
1.6, pulled over BIP) when the phone offers it, previous / rewind / play–pause / stop / fast
forward / next (the seeks run while the button is held), repeat (off, all, one) and shuffle when
the player offers them, the link's volume, *Listen here* and, folded for a browsable player, a
browser: the now-playing list, its files, search, and *play* or *add* per item. Under the phones,
*This board's player*: what a car or speaker the board plays to sees (its track, play state and
the adapters it is offered on), its controls, *Offer it* (on or off), and folded, its library and
now-playing list. *Streams*: what the board plays itself. *Output for new streams*, where every
stream started there begins — the connected Bluetooth sinks by name and codec first, PipeWire's
default output, the other sinks, or nowhere — with the codec picker for a Bluetooth one and *also
listen in this browser*; *Running*, each stream with its own output (*to*: moved there at once,
the source carrying on, so two streams play to two devices) and under it, in Listen's words, what
the board does about it — asking, opening it, waiting for the Bluetooth link to stream, *Playing
on … · codec*, or why not — then its level meters (RMS bar, peak mark, per channel), the radio
title, Pause (and Previous/Next for a playlist) for a URL stream, a live gain slider and, for a
tone, its frequency and level, listening adding the buffered time, a volume and a spectrum (the
console keeps listening when you change views); then *New stream*: a test signal (a tone, its own
frequency on the right channel if wanted, a sweep, white/pink/brown noise, silence) or audio from a
device (capture — with a speaker as the output, a tunnel from one device to another) or what an
output plays (its monitor), with rate, channels, gain and the decoder under *Format and gain*; and
*Internet radio and URLs*: the station list (edit it as `name | url` lines) and a URL field, each
played to the output or listened to. A URL plays at 44.1 kHz unless a rate is chosen (a UPnP track
from Listen plays at its own rate): decoding to another rate makes the decoder resample, at three
times the CPU on a Zero W. Media servers are browsed in Listen → Library. What plays to a Bluetooth
output shows on the device's display through the board's player, and the device's buttons pause,
skip and stop it. *PipeWire*: the services, *Audio on* (the whole stack off and on, kept across reboots: off is
Bluetooth without audio, for a board that is only an LE remote or a GATT peripheral), a Restart
(with a confirmation, optionally restarting bluetoothd), and the graph from pw-top.

**Files** — OBEX, three views. *Share* is the way a phone shares files: drop files (or choose
them), pick a paired device (the ones without Object Push are greyed out), *Send* — the board
uploads them, opens an Object Push session and sends them one after the other, each with its
progress, "waiting for … to accept" while the device asks its user, and a tick when it is done.
Beside it *Received*: what devices sent the board, newest first, images as thumbnails, with
download and delete; *Receive files* switches the board's OBEX server (obexd) on or off, *Incoming*
says whether a push is saved at once, asked about (the question comes up right there, Accept or
Decline) or refused. The tester's views follow, under the obexd bar (start/stop and the same
policy; an asked push with a name to save it as). *Sessions*: a new client session to a paired
device (Object Push, File Transfer, Phonebook, Messages, cover art) and a card per session with its
operations: send a file from `out/`, pull or exchange a business card; browse the device's
folders, get, put, mkdir, delete; select a phonebook, list, search, pull one or all; list message
folders and messages, fetch one; an image's properties, thumbnail or full image, and the transfers
with progress (pause, resume, cancel). *On the board*: the files — `out/` (upload what to send),
`in/` (download what arrived), `shared/`.

**Traffic** — five views, in the order of looking closer: *Links*, *Packets*, *D-Bus*, *Graphs*,
*Captures*.

*Links*: the HCI monitor's connections: throughput, TX latency p50/p95/max (HCI send to
Number Of Completed Packets), packets in flight against the controller's credits, RTP loss. Click
one for two minutes of throughput and latency graphs and, under a disclosure, the latency
histogram since it connected and its L2CAP channels with the AVDTP stream's state, codec and RTP
loss/jitter. Below: the event log (connections, channels, AVDTP signalling) and **Mark**, which
writes a note into btmon, the btsnoop ring and live captures.

*Packets*: the packets as they happen, filtered on the
board with the Capture viewer's filter box and quick filters (the same syntax). The table follows
the newest rows unless you scroll up, keeps the last 1000, and marks where the board dropped
packets (the page fell behind) or skipped matches (more than it sends: the newest are kept).
Changing the filter re-filters the 2048 packets the board still holds. Click a row for its decoded
fields and hex dump; *Pause* stops the stream (the rows stay clickable while the board still holds
them), *Open as capture* opens the file btmon is writing in the capture viewer with the same
filter. The stream runs only while this view is on screen and the browser tab is visible. Under
the table, the command for a live capture in Wireshark on your PC.

*D-Bus*: BlueZ's D-Bus traffic as it happens — the calls to BlueZ and obexd with their arguments,
the replies with the call they answer and how long it took, errors in red, the signals
(`PropertiesChanged` read as what changed: `Device1 RSSI=-58`). It opens with the last messages the
board kept and follows the new ones; filter by words (`!word` hides), by kind, and with *show
polling* btbenchd's re-reading of the object tree every few seconds. Click a message for its
arguments in full. The board writes everything to `bluez-dbus.log`, downloadable here and open in
System → Logs, whether or not this view is open.

*Graphs*: live, scrolling graphs on one time axis (30 s, 2 min, 5 min or 15 min): per connection the
throughput (TX solid, RX dashed), TX latency p50 / p95 / max, packets in flight against the
controller's free credits, and for an AVDTP media stream its RTP packets, losses and jitter;
the audio streams' levels; the board's CPU and temperature. Each connection (stream, the board)
has one colour; click it in the legend to hide it, or pick one connection. Each chart's latest
values stand in its title, by the full-screen button. Hovering shows the values at that instant on
every chart. Pause freezes the picture (the data keeps coming), CSV
downloads the samples. *Record in background* (on by default, remembered per browser) keeps the
graphs' topics subscribed while another view is on screen, so the 15 minutes before an issue are
there when you come back; it keeps pw-top running on the board, so switch it off to leave the board
idle. Off, the samples are collected only while the view is open, and the last five minutes of a
connection come from the monitor's history when it opens.

*Captures*: the always-on btsnoop ring: start/stop recording, the files (the newest is being
written), open, download, delete, and *Summary* (`btmon -a` on the board). *Open* opens a file in the packet
viewer:
- a filter box (`proto:att handle:64 lat>20 "read request"`; the syntax is under the box and in
  [monitor.md](monitor.md#filter-syntax)) and quick filters: HCI commands/events, L2CAP signalling,
  ATT, SMP, AVDTP, media, AVRCP, RFCOMM, MGMT (what bluetoothd asks the kernel, by name), errors,
  marks and logs, slow TX. They combine: a click adds one or takes it away, and the ones picked are
  joined with `||` (ATT and media: `proto:att || proto:rtp`); *all* clears them, and a time range
  dragged on the graph goes into each;
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

**System** — the board itself, in eight views: its state and logs, then how it is set up.
*Overview*: health, versions of BlueZ, PipeWire, WirePlumber and the
kernel, the stack's services with restart buttons, bluetoothd's command line under a
disclosure (e.g. `-d -E`; saved to `/data/btbench/bluetoothd.env` and applied by restarting
bluetooth), the log files to download (`clog` reads them on the board), reboot, and under a
disclosure a factory reset (every setting back to the image's, optionally the paired devices and
the Wi-Fi networks too; it reboots, and clears the console's own preferences in this browser).
*Logs*: one viewer for the journal of a unit or of the kernel and the files in `/tmp/logs` (each
program's own, `bluez-dbus.log` among them), live with *follow*; a debug build's source paths are
shown from the source tree on (`src/adapter.c`). *Tools*: the jobs runner (l2ping, l2test, iperf3,
btmgmt, wpctl and the other allowlisted tools, with live output and kill). *Kernel* also has the
kernel's Bluetooth debug messages by area (HCI core, management, L2CAP, SMP, ISO, SCO, RFCOMM,
BNEP, HIDP, the controller drivers; each says how many of its messages print, *keep from boot*,
*all off*, *show in the journal*).

How the stack is configured (these four views were an area of their own, *Setup*, and their
old addresses still work). *Profiles*: every profile the board offers, on and off
by group — Audio (A2DP sink and source, HFP HF and AG, HSP, BlueZ's own HFP), LE Audio (unicast
sink and source, broadcast sink and source), Control (AVRCP), Input (HID host, HID over GATT host,
the HID remote), Network (PAN), Files (the OBEX servers) and Device info. *Bluetooth settings*: BlueZ's
main.conf — the radio (classic and LE, classic only, LE only), Secure Connections, privacy,
re-pairing, fast connectable, the classic page and supervision timeouts, the LE connection and
advertising intervals, latency and supervision timeout, the GATT cache, key size, MTU and EATT
channels, and the policy plugin's reconnects — each with *Default* to leave the image's value (and
what that value is, when main.conf says). Above them the adapters, with what the kernel says each
can do; the kernel's experimental features are a list of those this BlueZ can set, each with the
adapters the kernel offers it on. Every other key of main.conf is folded under its section with
BlueZ's own comment as help.
*Audio settings*: WirePlumber's Bluetooth settings — the codecs offered, SBC-XQ, mSBC, hardware volume,
the HFP backend, the default rate, LDAC quality, AAC's bitrate mode, switching to the headset
profile when recording, remembering profiles, and any other Bluetooth setting WirePlumber's schema
has. Codecs and settings for codecs whose PipeWire plugin is not installed are left out. These
lists follow the installed kernel, BlueZ, PipeWire and WirePlumber (docs/api.md, Setup).
*Plugins*: every plugin this bluetoothd was built with, on its own (policy, autopair, the LE Audio
services, …). Changes collect until *Apply*: the bar says how many there
are and what applying restarts (bluetoothd, WirePlumber, obexd), since a restart drops every
connection.

*Wi-Fi*: state (client, setup AP, connecting, off), networks in range, add a network, the saved
ones, and the mode (auto / client / setup AP / off). The same page is served alone at `/wifi`:
when the board has no network it runs its setup AP, and a phone that joins it is sent there.
Adding a network over the setup AP drops that link; the page says so, and the board falls back to
the AP if joining fails.

*Kernel*: the good / test / previous kernel slots, and *try* (one trial boot of the test kernel,
falling back to the good one on a panic, hang or power cycle), *commit* and *roll back*, each
behind a confirmation.


On a PC (`make run`, `make pc`) the board-only parts — the audio stack's restart, Wi-Fi, kernel, reboot — say
that they run on the board only.
