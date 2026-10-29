# The system: image, storage, networking

What `btbench-base`, `btbench-image` and `btbench-rpi.wks` make of a booted board. The interfaces
other parts rely on are in [contracts.md](contracts.md).

## The SD card

| Partition | Size | Mounted | What |
|---|---|---|---|
| `boot` (FAT) | ≥ 256 MB | `/boot`, **read-only** | firmware, U-Boot, the good kernel; trial kernels in `/boot/bench/{test,prev}`, U-Boot's `uboot.env` |
| `root` (ext4) | packages + 512 MB | `/`, **read-only** | the image; deploys rsync into it, opened for that |
| `data` (ext4) | rest of the card | `/data`, read-write | settings, and nothing else |

The root is found by `PARTUUID=b7be4c4e-02`: the image recipe fixes the disk identifier. `/boot`
and `/data` are mounted by label with `nofail`: a damaged one costs you its function, not the board.

The card is read-only except for settings. A card that is not written does not wear out, and a
power cut (the usual way a bench is switched off) cannot corrupt the root or `/boot`.
- **The root and `/boot` are mounted read-only.** The root uses Yocto's `read-only-rootfs`;
  volatile-binds give `/var/lib`, `/var/cache`, `/var/spool` and `/srv` a copy in RAM, made at
  boot, and `/var/log` is a link into `/var/volatile`. opkg's database is in `/usr/lib/opkg`, on
  the root, so what `make pkg` installs stays known to opkg.
- **`/data` holds settings only, written when one changes:**
  - `btbench/`: Setup's files, `bluetoothd.env`, `btsnoop.env`, the adapter and agent
    (`bluetooth.json`), the HID, player, OBEX and radio settings, `wifi/` (networks), `ssh/`
    (sshd's host keys), `wireplumber-profiles.conf`, and `wireplumber/`: WirePlumber's own state
    (each device's profile, so its codec, and the volumes), bound over `/var/lib/wireplumber`;
  - `btbench/timesync/`: timesyncd's clock, one small write per NTP sync, so a board without a
    network does not start at the image's build date;
  - `bluetooth/`: BlueZ's pairings, bind-mounted over `/var/lib/bluetooth`.
- **A factory reset** (System → Overview, or `POST /api/system/factory-reset`) empties
  `btbench/` but for the host keys, the clock and, unless asked, the Wi-Fi networks, and with
  *Forget paired devices* `bluetooth/` too. btbenchd only leaves `/data/btbench/factory-reset` and
  reboots; btbench-data does the deleting early in the next boot, before bluetoothd or WirePlumber
  run, so nothing writes its files back. Over ssh: `echo pairings > /data/btbench/factory-reset; reboot`.
- **What changes often is in RAM,** and gone at reboot:
  - the journal (`/run/log/journal`, uncompressed, 32 MB);
  - the per-program logs (`/tmp/logs`);
  - the HCI capture ring (`/tmp/btsnoop`, 4 × 8 MB; download a file to keep it);
  - OBEX transfers (`/tmp/btbench/obex`);
  - BlueZ's device cache (each adapter's `cache/`, bound to `/run/btbench/bluetooth-cache`);
  - the rest of `/var`.

Settings that have to be in `/etc` are links to `/data`
(`/etc/wireplumber/wireplumber.conf.d/60-btbench-profiles.conf`), or are read from a fixed path in
`/run` (sshd's `HostKey /run/btbench/ssh/…`, which `btbench-sshkeys` points at `/data/btbench/ssh`,
or at `/run` for that boot if `/data` did not mount).

**Writing the root or `/boot`:** `btbench-rw open / /boot` remounts them read-write, and
`btbench-rw close` remounts them read-only. Every writer goes through it:
- `make deploy-*`, `make pkg` and `make linux-deploy`, through the Makefile's `board_rw`; the
  close runs in an EXIT trap, so a failed deploy closes too;
- `btbench-kernel` (`try`, `commit`, `rollback` open `/boot`; `prune` opens `/`);
- `btbench-btdebug keep` (its modprobe.d file).

Uses are counted, so overlapping ones do not close each other's mounts. `btbench-rw status` shows
the state; `btbench-rw reset` closes everything after a killed deploy, and so does a reboot.

The kernel will not remount read-only while a process still runs a file that a deploy replaced:
rsync renames the new copy over the old one, which lives on, deleted, until its process ends.
The close then names the units (after `make deploy-scripts`, usually `btbench-wifi.service`) and
leaves the root writable. Restart them and run `btbench-rw reset`, or reboot. Deploys that restart
what they replace (bluetooth, PipeWire, btbenchd) are not affected.

A reflash rewrites `/data` too. Deploys and kernel swaps leave it alone.

## First boot

1. `btbench-data` grows the data partition and its filesystem to the end of the card, then creates
   the directories above and binds timesyncd's clock to `/data`. Later boots find nothing to grow.
   `btbench-sshkeys` makes sshd's host keys in `/data/btbench/ssh` the first time.
2. `sshdgenkeys` makes the ssh host keys (a new set with every flash; the Makefile does not check
   them).
3. `btbench-wifi` copies the network from the device conf to `/data/btbench/wifi/`, if one was
   given, and tries it; otherwise it opens the setup AP.

Root logs in with the device conf's password, or with its ssh key (`BTBENCH_SSH_PUBKEY`). The
journal is in RAM, uncompressed (32 MB cap): reading it costs the Zero W a fraction of a second
instead of seconds, and the SD card no writes, but it starts empty at every boot.

## The USB link

Plug the Zero W's **USB** port (not PWR) into the PC. It is one composite USB device:

- **Ethernet**: NCM (`BTBENCH_GADGET=ncm`) or ECM (`ecm`, for hosts without NCM). The board is
  `10.55.0.1` and gives the PC an address from `10.55.0.2–9` over DHCP, with no router and no DNS,
  so the PC keeps its own default route and resolver. On Linux the interface shows up as
  `enx06bb…`. Its MAC comes from the board's serial number, so NetworkManager keeps the same
  "Wired connection" profile across reboots and reflashes; nothing needs configuring. If
  NetworkManager is set to ignore unknown devices, `nmcli device set <iface> managed yes`.
- **Serial console**: `/dev/ttyACM0` on the PC (`picocom /dev/ttyACM0`), a login on `ttyGS0`.
  It works when the network does not, e.g. a trial kernel without a network.

`ssh root@10.55.0.1`, or `ssh root@<hostname>-<mac>.local` (mDNS from systemd-resolved, on the USB link
and Wi-Fi). `<mac>` is the last three bytes of the board's Wi-Fi MAC address (`btbench-257bfc`):
every image has the same `/etc/hostname`, and `btbench-hostname.service` puts the MAC after it at
boot, as a transient hostname, so that several boards on one network each have a name of their
own. `BTBENCH_HOSTNAME_MAC = "0"` in the device conf keeps the plain name. A name given in the
console (System → Name: `btbench-rpi5`) replaces it: it is kept in `/data/btbench/hostname`, taken at
once and at every boot, and emptied it goes back to `btbench-<mac>`; a factory reset clears it. The boards also
announce themselves as DNS-SD services on mDNS — the web console as `_http._tcp` and ssh as
`_ssh._tcp`, under the board's name — so a service browser lists every bench in reach without
knowing its name: `avahi-browse -rt _http._tcp` on a PC, the Bonjour/mDNS browser of a phone.
systemd-resolved publishes them (`/etc/systemd/dnssd/`); there is no Avahi on the image. mDNS is on
for usb0, wlan0 and Ethernet (the VIM3L's port).

`BTBENCH_GADGET=off`, or a board without a device controller, leaves the port alone: on the Zero W
that keeps it a USB host, for a Bluetooth dongle, and the board is reachable over Wi-Fi only.

## Wi-Fi

`btbench-wifi` (service `btbench-wifi`) runs Wi-Fi as either a client or an AP, never both: the
BCM43430 does not do both reliably.

With the default policy `auto`:

1. **Client.** With networks stored, `wpa_supplicant@wlan0` gets 45 s to join one of them and get
   an address.
2. **Setup AP.** With no networks, or none answering, the board scans once, then opens
   `btbench-XXXX` (the last 4 hex digits of its serial; WPA2, password `BTBENCH_AP_PSK`, channel 6).
   It is `10.42.0.1` and runs a DHCP server. Every DNS name resolves to the board, so a phone that
   joins is sent to its page, where you can add a network. The scan from before the AP came up
   is what `btbench-wifi scan` shows while the AP is up.
3. **Retry.** While the AP is up, the board tries the stored networks again every 10 min, and at
   once when one is added. If that fails it comes back to the AP and says why in `last_error`.
4. **Lost network.** A client that has lost its network for 5 min opens the AP.

```
btbench-wifi status                       # JSON: state, policy, ssid, ip, ap, networks, last_error
btbench-wifi scan
btbench-wifi add "Lab WiFi" 'passphrase'  # or no passphrase for an open network
btbench-wifi remove "Lab WiFi"
btbench-wifi mode auto|sta|ap|off         # kept across reboots
```

`sta` keeps trying as a client and never opens the AP. `ap` holds the AP. `off` powers the
interface down, e.g. to measure Bluetooth without Wi-Fi coexistence (stay on the USB link for
that).

The networks are kept in `/data/btbench/wifi/wpa_supplicant-wlan0.conf`, with SSIDs in hex and
keys derived. Use `add` and `remove` rather than editing the file. `country=` follows
`BTBENCH_WIFI_COUNTRY`.

## Bluetooth address and pairings

`btbench-bdaddr` runs before every `bluetoothd` start (it is pulled in by `bluetooth.service`).

- **Pairings.** It bind-mounts `/data/bluetooth` over `/var/lib/bluetooth`.
- **Address.** It checks the controller's address. The BCM43430A1 has no address programmed: it
  reports `43:43:A1:12:1F:AC`, or `AA:AA:AA:AA:AA:AA`, which mainline treats as "unconfigured". In
  either case the script sets `B8:27:EB:xx:xx:xx` with `btmgmt public-addr`, derived from the
  serial number the way Raspberry Pi OS's `bthelper` does. Any other address is left alone.
- **Which controller.** The board's own: the first not on USB, whatever its hciN (a USB adapter can
  probe first and take hci0). `BT_HCI` (board.env) only when every controller is on USB.

## A second adapter

A USB Bluetooth adapter next to the board's own controller is a second adapter of the bench's
(docs/api.md "Two adapters"): the console's Devices page and `bench -a hciN` pick which one to
scan, pair and connect with, and `bench primary` moves the advertisements, the GATT server and the
HID remote to another. Its firmware goes in the board's image (`BTBENCH_BOARD_FIRMWARE`).

- **Intel BE200** (Wi-Fi 7 M.2 card, on a USB adapter on the VIM3L and on an M.2 HAT on the
  Raspberry Pi 5; USB `8087:0036`, Bluetooth 5.4 with LE Audio). btusb + btintel load
  `intel/ibt-0291-0291.sfi` and its `.ddc`: from `btbench-firmware-ibt`, pinned to a
  linux-firmware tag newer than scarthgap's. Without them it stays in its bootloader, an hci with
  address `00:00:00:00:00:00` that never comes up. Booted, `dmesg` says
  `hci1: Firmware timestamp 2026.29 buildtype 1 build 120611`.
  On the Pi 5 its Bluetooth needs the HAT to wire the M.2 slot's USB pins to one of the board's
  USB ports. A PCIe-only HAT shows the Wi-Fi alone (docs/kernel.md "Raspberry Pi 5"). The Wi-Fi
  half is `wlpcie0` (iwlwifi, `btbench-firmware-iwlwifi`), up only when you bring it up.
- **Nordic nRF54L15 DK** running the bench's own controller firmware, over the DK's USB (its
  J-Link's second CDC ACM port carries HCI): see below.

### An nRF54L15 DK as the controller

The BE200's firmware is a blob with LE Audio switched on by a DDC trick; a Nordic nRF54L15 DK
running Zephyr's open-source link layer is a controller whose every line can be read, changed and
rebuilt. The firmware is `tools/nrf-hci`: Zephyr's `hci_uart` sample as it is (from the Zephyr
workspace, not copied) with every feature the bench wants switched on in `prj.conf` — extended
and periodic advertising, PAST both ways, Coded PHY, BIG source and sink, CIS central and
peripheral — plus one source file of its own, `src/addr.c`, which gives the link layer the chip's
factory static address as its public one (Zephyr boots with `00:00:00:00:00:00`, which the
kernel refuses to power). HCI runs as H:4 at 1 Mbaud with RTS/CTS on the DK's `uart20`, which the
on-board J-Link exposes as the second of its two USB serial ports (interface 02).

On this PC (once, `make nrf-setup`: west in a venv, a Zephyr workspace pinned to
`ZEPHYR_VERSION` with only the modules this build needs, the Zephyr SDK's arm toolchain; the
paths are `ZEPHYR_WORKSPACE`, `ZEPHYR_VENV`, `ZEPHYR_SDK`, `JLINK_DIR` in `bench.conf`). SEGGER's
J-Link software is the one download with a licence click: unpack `JLink_Linux_x86_64.deb` with
`dpkg-deb -x` into `JLINK_DIR`, no root needed. Then, with the DK on this PC's USB:

```
make nrf-hci      # build/nrf-hci (first applies tools/nrf-hci/patches to the Zephyr tree)
make nrf-flash    # over the DK's J-Link
make nrf-check    # HCI reset, version, BD_ADDR, LE features, buffer sizes — no root, no attach
```

The bench's own changes to Zephyr's link layer are diffs in `tools/nrf-hci/patches/`, each
opening with why; `make nrf-hci` applies the ones not yet in the workspace's tree (a patch whose
hunks a later one touches would fail that check, so the changes stay in one file). What is in it,
both switched on in `prj.conf`:

- `CONFIG_BT_CTLR_SYNC_ESTAB_WINDOW_US`: a wider receive window for the first PDU of a periodic
  advertising sync. Zephyr listens exactly where the advertiser's SyncInfo offset says, which the
  specification allows, but a StreamSDK stream195x board's Infineon controller sends its AUX_SYNC_IND
  10 ms (one ISO interval of its BIG) later than it announces, so every sync to its Auracast failed
  with 0x3e; with the window the sync takes one event, and the normal window is used from the next.
- `CONFIG_BT_CTLR_SYNC_TIMING_EVENTS`: after every periodic sync (established or failed) and BIG
  sync, a vendor event with where the first PDU landed relative to the announced offset, the
  events listened to and the CRC failures. The monitor decodes them as `Zephyr Periodic Sync
  Timing` and `Zephyr BIG Sync Timing` (`is:vendor`), so a peer with a wrong SyncInfo or BIGInfo is
  read off the capture, with the numbers its firmware's developers need, instead of guessed from a
  0x3e. More of the kind (what the standard events leave out when a peer misbehaves) belong here.

A DK that sits on a board's USB can be reflashed without moving it: SEGGER's `JLinkRemoteServer`
for arm64 (from the same licence-click download, `JLink_Linux_<ver>_arm64.tgz`; only
`JLinkRemoteServerCLExe` and `libjlinkarm.so.*` are needed) goes to the board's `/data/jlink` and
runs there (`LD_LIBRARY_PATH=/data/jlink /data/jlink/JLinkRemoteServerCLExe -Port 19020`, e.g. under
`systemd-run --unit=jlink-remote`); then `make nrf-flash NRF_IP=<board>` flashes through it. The
board's HCI goes away during the flash: `systemctl restart btbench-hci-uart@ttyACMn` afterwards, and
bluetoothd's broadcast probes need `bench audio restart --restart-bt` once the adapter is back.

Moved to a board's USB, the DK is attached by udev (`70-btbench-hci-uart.rules` starts
`btbench-hci-uart@ttyACMn.service`, which runs `btattach -P h4 -S 1000000` on the port and is
bound to it), and bluetoothd picks up the new hci as it does any adapter: `bench -a hciN`, Devices
→ adapter, `bench primary`. The address is the chip's: a static random one worn as public, with
no OUI behind it. Zephyr's controller is LE only, so the DK is for LE Audio and LE tests, never
for A2DP or HFP.

## Changing the scripts

The sources are in `tools/target/`. `make deploy-scripts` copies them to the board's `/usr/bin`
without restarting anything; the next `make image` carries them too.
