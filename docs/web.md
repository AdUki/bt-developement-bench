# The web console

btbenchd serves a console at `http://10.55.0.1/` (the USB gadget link), `http://<hostname>.local/`,
or `http://localhost:8080/` with `make run`. Plain HTML/JS/CSS from `app/www/`, no build step:
`make deploy-www` copies it to the board and a reload shows it. Light or dark follows the system.
The REST and WebSocket API behind every button is in [api.md](api.md) (also served at `/api`).

The console is six areas — **Devices**, **Audio**, **LE**, **Files**, **Traffic**, **System** —
in a rail on the left (a bar at the bottom on a phone). Each area has a few views in the bar at
the top of the page; an area reopens on the view last used in it, and the address names the view
(`#audio/route`, `#packets`), so it can be bookmarked. Settings that are set once (the adapter's
name and agent, scan options, an advertisement's intervals, a stream's rate) sit behind a
*Settings*, *Options* or *More fields* disclosure, and raw values (configuration bytes, object
paths, capability hex) behind *Details* or in a tooltip.

On a desktop the page scrolls only where it has to. A view built around one long list — *Packets*,
*D-Bus*, *Logs*, an open capture — fills the window and only the list scrolls; *Audio* on a wide
screen (from 1400 px) shows Players and the route or the library side by side, each column scrolling on its
own. The cards worth reading big (the library, the packet tables, the D-Bus and log views, a
link's graphs, each chart in *Graphs*) have a full-screen button; Esc puts them back. On a very
wide screen (1700 px) the pages are allowed wider.

The foot of the rail (the top bar on a phone) shows the board's name (also the browser tab's
title, so several boards' tabs are told apart; System → Name renames it), the adapter (on / visible /
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
the button. A device is one row: a light (green connected, pale paired, amber busy), an icon of what it is,
its name,
address, kind, signal (bars and dBm, from the scan) and its transports (BR/EDR, LE; lit where
connected), and the one action its state calls for — pair, connect, disconnect, or cancel a
pairing (one button of one width, which shows the work in progress itself); a connected device also
shows the profiles it has up (A2DP, AVRCP, HFP, HID, PAN, GATT; lit while streaming). One of the
board's own adapters, seen advertising by the other, is tagged *this board · hciN*. The icon is a
computer, a phone, a wearable (a watch), a speaker, headphones (headsets, earbuds, hearing aids), a
TV, an input device (keyboards, mice, remotes, game pads) or, for anything else, the Bluetooth
sign; it is decided by the Class of Device (major and minor class) first, then the LE Appearance,
then BlueZ's own `Icon`, then the services listed (HID or HID over GATT: input; Hearing Access:
headphones; A2DP sink, ASCS or BASS: speaker), and a hover says which it came from. The same icon
marks the device in Files → Share and in Audio → Route. Rows keep their
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
the GATT explorer or a file session on that device. *Input devices*: the
Bluetooth keyboards, remotes, mice and game pads connected to the board (BlueZ makes each a kernel
input device): their report maps read out (Consumer Control, Keyboard, Mouse, … and the reports'
sizes; the raw map under a disclosure), the last key pressed and a live log of presses, and
*Media keys control what plays*: a remote's play/pause, next, previous and volume keys drive the
connected phone's player, as with a speaker.

**Audio** — the board as a player and as the audio bench, in five views. The board plays one
**input** at a time on any **outputs** picked: the route (`/api/audio/route`), the same for every
browser and kept by the board with the page closed. *Players* is what plays and what next,
*Route* where it comes from and where it goes, *Library* what to play; *LE Audio* is Auracast and
*PipeWire* the audio server.

*Players* shows the input, or another player picked in its picker: the board's own stream (while it
is the input or loaded), then each device with a player, the input first and marked *input*; a
device with several players (Spotify and the phone's own music app, an LE Audio device's media
control) has each of them under it, and the device itself shows the one playing. A player that is
not the input says *Not the input*, its keys act on the phone, and *Make it the input* (or ▶ on the
board's loaded stream) makes it the route's input; a new input in Route shows that one again.
What it shows: a connected phone (its AVRCP player — track, cover art, progress,
previous / play / pause / next / stop, repeat and shuffle when the player has them, the link's
volume), a stream the board plays
itself (a radio station, with the cover or logo it sends and its description; tracks from a UPnP
media server with their artist, album and length, and repeat and shuffle for a playlist; a test
signal, with its level, which only stops), or Bluetooth audio with no player to show (an Auracast
broadcast — ▶ receives it again, ■ lets it go and leaves no input; an LE Audio source; a call's
audio over HFP). Above the title it says which way the music flows: *Receiving · Bluetooth*,
*Receiving · Auracast* or *Receiving · Call audio* from a device (and whether its audio streams into
the board, the board is waiting for it, or why it failed), or *Playing from the board* · Internet
radio, Media server or Test signal; then the path, source → this board → where it plays. The
volume slider sets what a person means: the Bluetooth speaker's own volume when the link has one
and the input plays on that one alone, else the stream's level (all its outputs at once), or this
browser's.

Under it, while something plays, *Up next*: the phone's now-playing list (read again when its
shuffle changes or the phone says the list changed), or the board's queue with each track's artist and length and **×** to take a track
out; shuffled, it is in the order it plays; a click plays that track.

*Route* is the inputs and the outputs side by side (one under the other on a phone). *Inputs*: *This board* — what the library
loaded (a station, the queue, a test signal, paused while a device is the input), or nothing — and
every Bluetooth input, one row a device and a kind of audio (a phone's A2DP and its HFP call audio
are two rows; a coordinated set, a pair of earbuds, is one), with its codec, what a broadcast plays,
the players in it and its state (streaming, connected but not streaming, in range, receiving).
Tapping a row makes it the input; the one before goes quiet — a phone is left alone (not paused, not
disconnected), the board's playlist pauses with its queue kept, a test signal stops. A broadcast not
received yet is received first, a phone that is not playing is asked once to play. The input row
says when the board waits for the device's audio (15 s at most) and why it failed; nothing is
retried by itself, tapping it again is the retry. The row is lit only while its audio plays:
picked and waiting for the device it shows the dots, and failed or not sending it is not lit.
*Disconnect* on the input's row leaves the board with no input (the device stays connected). A broadcast that carries one channel offers
*+ as left* / *+ as right* on the other channel's broadcasts while it is the input: the two are then
one stereo input. On a board whose adapter can receive broadcasts, the broadcasts are a section of
their own, *Auracast*, with *Scan* (dots while it scans; an LE scan of 30 s, as in LE Audio): a broadcaster heard
whose streams BlueZ has not read yet is listed as *reading its streams…* until it can be picked, and only while it is heard (a broadcaster that stopped leaves BlueZ its last announcement). Under a Bluetooth row are the
device's links that way — what kind (A2DP, LE Audio · CIS, Auracast · BIS), the codec in words, the
delay the sink reports, streaming or idle — with the link's volume (AVRCP's absolute volume, or LE
Audio's Volume Control), the PipeWire profile (the board's own codec choice: on an input it is the
way back, so it is called *Profile* there), and under *Details* the device's stream endpoints with
their capabilities and the links' raw configuration and paths; a phone that is not the input has
its player's previous / play / pause / next there. The input that plays carries the stream's level
meters (RMS bar, peak mark, per channel), its gain, and for a tone its frequency and level, live.

*Outputs* is one list, each with a switch, on or off by itself: *This browser* (this browser
listens to the input: its own choice, remembered by the browser), the board's own sound card if it
has one (not the loopback card, which nobody hears), each Bluetooth speaker or pair of earbuds (a
set is one row), and the board's Auracast broadcast — one row for its BISes (a left and a right one
too): on with none, it starts a stereo broadcast (48_2_1), and turned off it ends it. The device
the input comes from is shown but cannot be turned on (it would get its own audio back), nor can
Auracast while the input is a broadcast, and picking such an input takes those outputs off. Up to
four of the board's at once; they are the board's, stay when the input changes, and change what
plays at once: an output tapped joins (or leaves) the ones playing, which carry on untouched. A
row is lit only once its output plays: while it opens and its Bluetooth link starts (BlueZ's
Acquire) it shows the dots, and one that would not open, takes no audio or whose link does not
stream within 8 s is not lit (one tapped on here that fails is taken off the outputs again;
tapping it is the retry). The row says what the board does about it — *asking…*, *opening…*,
*waiting for its link*, *playing · SBC 48 kHz*, *lost: why*, *not connected* — and nothing is
retried on its own: a speaker lost is tapped again. A Bluetooth speaker's row has its link,
volume, codec picker (the link renegotiates on a change, the output is lost a moment and plays
again) and *Details* as an input's has. *This browser* on has its volume and, folded, a
*Spectrum* of what it hears with the time buffered (opened once, the browser's audio goes through
Web Audio for the rest of the page's life). Under the outputs, folded, *This
board's player*: what a car or speaker the board plays to sees over AVRCP (its track, play state
and the adapters it is offered on), its controls, *Offer it* (on or off), and folded, its library
and now-playing list; read while the fold is open.

*Library*, a page of its own so long folders get the screen: the phone's own folders and search
(for a browsable player), the radio
stations, the media servers on the LAN, and *Test signals* (tones, a left/right check, a sweep,
pink and white noise, silence, and under them any of them made to measure: a tone with its own
frequency on the right channel if wanted, a sweep's range, length, logarithmic and repeat, white,
pink or brown noise, the level, and under *Format and gain* the rate, channels and gain): a station plays on its own,
a track plays with the rest of its folder after it as the queue, **+** adds a track to the end of
the queue and *Play next* right after the track playing, and ▶ on a media server's folder plays all
of it (its subfolders' tracks too, up to 500). Whatever the library plays becomes the input. Under
the stations a stream's URL adds it, named as the stream calls itself, **×** on a station
removes it, and a URL can be played once without keeping it, with the decoder picked (auto,
mpg123, ffmpeg); a URL plays at 44.1 kHz, as nearly every stream is (decoding to another rate
makes the decoder resample, at three times the CPU on a Zero W). Media servers show their covers: a folder of albums is a grid of them (title, artist,
year), an album opens with its cover, artist, year, genre, track count and length, *Play all*,
*Play next* and *Add all*, and its tracks by number with their format (`FLAC 24/96`, `MP3 44.1 kHz`);
a track without a cover of its own shows its album's in Players. Covers load from the server
itself, so a browser on another network than the server shows the plain icons.

On a wide screen (from 1400 px) Players stays on the left and Route or Library is beside it
(Library until one is picked), so Players has no view button there. On a narrow one a bar at
the bottom of Route and Library keeps the track playing, play/pause and next in reach.

*LE Audio*: Auracast both ways, on the adapter that can (a picker when two can; the bar says
whether it broadcasts, receives or both). *Broadcast from the board* starts a broadcast: the
quality (BAP's broadcast configurations, 48 kHz 10 ms low latency first), stereo (one BIS), left +
right (two BISes) or mono, and a
Broadcast Code to encrypt it (empty: anyone listens), the presentation delay under *Options*. The
board sends one broadcast at a time: *Start broadcast* is off while there is one. Each BIS is a row: BIG and BIS, *on air*, *silent* (it transmits only while
something plays into it) or *ending…*, its codec, SDU, bitrate, retransmissions, latency and PHY, what plays into it,
and *Stop broadcast*. A broadcast started is one of the route's outputs at once (what the board plays
goes on air), and one taken off them ends: *Stop broadcast* (or turning it off in Route)
stops anything else playing into it and the daemon clears it once PipeWire let it go off air, a
few seconds later. So does one left from before that is not an output and plays nothing. The board's broadcast carries no Broadcast Name, so receivers list it by address.
*Broadcasts in range* scans (LE, 30 s) and shows each broadcaster as a card: its name (the
Broadcast Name), address, Broadcast ID, signal, encrypted and paired; then its streams, one per
BIS, once BlueZ has read them (location, codec, state, and the Program_Info the broadcaster sends,
in bold). *Listen* syncs to one — a Broadcast Code
field comes first on an encrypted one — and makes it the route's input, played on the route's outputs
(the board's own broadcasts are taken off them); *syncing…* until the BIG is up, *receiving* then,
and *Stop*, which ends it as the input too. To hear it here, turn on *This browser*.
Nothing is retried: a stream that does not sync stays as it is, and *Listen* again is the retry.

*PipeWire*: the audio server. Its services, *Audio on* (the whole stack off and on, kept across
reboots: off is Bluetooth without audio, for a board that is only an LE remote or a GATT
peripheral), a Restart (with a confirmation, optionally restarting bluetoothd). *Clock*: what
PipeWire's settings ask for — forced quantum and rate, quantum, its minimum and maximum, rate,
the allowed rates — each a field of its own with *Default*, and above them the quantum and rate
the running drivers cycle at (pw-top's); they last until PipeWire restarts. *Defaults*: the
default sink and source (where an application that names no output plays; the board's own
streams name theirs), and the one chosen when it is not there. *Nodes*: every node of the graph
by kind — sinks, sources, playing and recording streams, drivers — with its state, media class,
format, latency, what drives it, the application, its volume (wpctl's scale) and *Mute*, what it
is linked to and from with the channels, and its props under a fold; *Reload* reads it again
(it is read when the view opens, after each change made here, and when the board's audio links or
streams change). *Bluetooth cards*: each device's PipeWire profile, the same codec picker as in
Route. *Timing*: the graph from pw-top, a second at a time while the view is open.

**LE** — *GATT explorer*: pick a device, connect (a dual-mode device such as a speaker connects
and disconnects its LE link only, so a classic connection carrying audio stays), and its services
appear by handle, folded when there are more than four (each says how many characteristics it
has; what you open stays open across a reload). A characteristic is one line: its value (as text
too when it is a string) and read, write (hex, or `text:…`; request / command / reliable as the
flags allow) and notify; its descriptors read the same way. Every value change lands in the
notification log; below it, the *advertisers in range*, strongest first,
with what each advertisement carried. *Advertise*: a form for `LEAdvertisingManager1`
(type, name, UUIDs, discoverable; under *More fields* manufacturer and service data as `id:hex`,
appearance, includes, intervals, TX power, timeout), and *LE Audio receiver*, which fills it in
as an earbud announces itself (ASCS, PACS, CAS, 100-150 ms) so phones stream to the board; several at once, each with its state (an advertisement whose timeout ran out shows
"released"). *Remote control*: the board as a Bluetooth LE remote (HID over GATT). Switched on, it advertises
under its name; pair it from a phone, TV or computer like any remote. The pad has power, mute,
a d-pad with OK, back, home, menu, previous / play–pause / next, and volume and search rockers;
a key stays down while it is held (a volume key repeats on the host as with a real remote), and
with the pad focused the keyboard's arrows, Enter, Escape, Space and + / − press its keys. *Type*
sends text as key presses; *Battery* sets the level hosts show. *GATT server*: an application as JSON (the Example button loads one with a
heart-rate counter, a battery level and an echo characteristic), its live values, and a log of
what remote devices read, wrote and subscribed to.

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

**Traffic** — six views, in the order of looking closer: *Links*, *Packets*, *D-Bus*, *Logs*,
*Graphs*, *Captures*.

*Links*: the HCI monitor's connections: throughput, TX latency p50/p95/max (HCI send to
Number Of Completed Packets), packets in flight against the controller's credits, RTP loss. Click
one for two minutes of throughput and latency graphs and, under a disclosure, the latency
histogram since it connected and its L2CAP channels with the AVDTP stream's state, codec and RTP
loss/jitter. Below: the event log (connections, channels, AVDTP signalling) and **Mark**, which
writes a note into btmon, the btsnoop ring and live captures.

*Packets*: the packets, captured and live, as one list: the file btmon is writing, opened at its
end and growing with it, or any file of the ring picked from the box at the top (the one being
written is marked). *Follow* is on while the table is at the end: new packets are fetched as
they are written and the table stays at the bottom; scrolling up switches it off (nothing is
fetched then), scrolling back to the end or ticking it switches it on. At a rotation the view
moves to the new file. Everything below is the same for an old file and for the live end:
- a filter box (`proto:att handle:64 lat>20 "read request"`; the syntax opens under *Filter syntax*
  beside *Statistics*, and is in
  [monitor.md](monitor.md#filter-syntax)) and quick filters: HCI commands/events, vendor
  commands and events, advertising (advertising, periodic and BIGInfo reports, inquiry results,
  MGMT Device Found), ACL, L2CAP signalling, ATT, SMP, AVDTP, media, AVRCP, RFCOMM, SCO, ISO, MGMT
  (what bluetoothd asks the kernel, by name), errors, marks and logs, slow TX. They combine: a click
  shows one's packets or takes it away again, and the ones shown are joined with `||` (ATT and
  media: `proto:att || proto:rtp`); a right click (or shift-click) hides one's packets instead,
  struck through, its `!term` in every group (`proto:att !is:adv || type:iso !is:adv`), and again
  shows them. *all* clears them, and a time range dragged on the graph goes into each;
- a graph strip of the whole file: packets per time bucket of the filtered set, and for a chosen
  connection its TX/RX throughput and TX latency p50/p95. Drag across it to keep only that time
  range (it becomes `t>=… t<…` in the filter, and the graph zooms to it); click to jump the table
  there. The band shows which part of the file is on screen;
- the packet table: frame number, time since the first packet (or time of day, or since the row
  above), direction, handle, protocol, length and summary (TX latency for completed TX packets),
  coloured by kind and direction, errors in red, marks highlighted. Only the rows on screen are
  fetched, so a 300 000-packet file scrolls like a small one. ↑ ↓, Page Up/Down, Home and End
  move the selection. Ctrl-click adds or drops a packet, shift-click (or shift with the arrows)
  picks a range of up to 5000; a right click copies the picked rows as shown, columns lined up,
  or only their summaries, and filters what they are in or out (one packet: its command or event,
  protocol or channel, and link; several: their kinds together); Ctrl+C copies the rows;
- the selected packet decoded: a tree of fields, and the hex dump with offsets. Hovering a field
  highlights its bytes; the terms under the summary add themselves to the filter on a click
  (shift: negated; a right click offers both). Lines of the tree are picked the same way (click,
  ctrl, shift); a right click copies text selected with the mouse, the picked lines (indented as in
  the tree), all the fields, the hex dump or the bytes as plain hex, and filters the packet in or
  out like the list's; Ctrl+C copies the picked lines. An ISO packet names its BIS or CIS and its
  codec configuration, and its SDU is cut into codec frames per channel
  ([monitor.md](monitor.md#iso-data));
- the separator between the table and the detail drags (or ← → when focused, a double click
  puts it back), and *filter* and *graph* in the head put away the filter (box, quick filters,
  syntax) and the graph strip for a taller table; this browser remembers both;
- *Statistics*: what the filter matches, by protocol and by link, with the TX latency percentiles;
  *Save matches* downloads them as a btsnoop file (*with context*: with what btmon needs to decode
  them).

On the board a big file is indexed, and a text filter searched, a slice at a time: the status line
shows the progress and the totals grow until done. Under the table, the command for a live
capture in Wireshark on your PC.

*D-Bus*: BlueZ's D-Bus traffic as it happens — the calls to BlueZ and obexd with their arguments,
the replies with the call they answer and how long it took, errors in red, the signals
(`PropertiesChanged` read as what changed: `Device1 RSSI=-58`). It opens with the last messages the
board kept and follows the new ones; filter by words (`!word` hides), by kind, and with *show
polling* btbenchd's re-reading of the object tree every few seconds. Click a message for its
arguments in full. The board writes everything to `bluez-dbus.log`, downloadable here and open in
*Logs*, whether or not this view is open.

*Logs*: one viewer for the journal of a unit or of the kernel and the files in `/tmp/logs` (each
program's own, `bluez-dbus.log` among them), live with *follow*; a debug build's source paths are
shown from the source tree on (`src/adapter.c`). System's log files and *show in the journal* open
here.

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
written), *Open* (the file in *Packets*), download, delete, and *Summary* (`btmon -a` on the
board).

**System** — the board itself, in eight views: its state and tools, then how it is set up.
*Overview*: the board's name (e.g. `btbench-rpi5`; the hostname, mDNS name and adapter alias
follow it, and emptied it is `btbench-<mac>` again), health, versions of BlueZ, PipeWire, WirePlumber and the
kernel, the stack's services with restart buttons, bluetoothd's command line under a
disclosure (e.g. `-d -E`; saved to `/data/btbench/bluetoothd.env` and applied by restarting
bluetooth), the log files to download (`clog` reads them on the board), reboot, and under a
disclosure a factory reset (every setting back to the image's, optionally the paired devices and
the Wi-Fi networks too; it reboots, and clears the console's own preferences in this browser).
*Tools*: the jobs runner (l2ping, l2test, iperf3,
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
